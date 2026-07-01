#include "loader.h"
#include "parser.h"
#include "http.h"
#include "../../memory/memory.h"
#include "../../Roblox/offsets.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <ShlObj.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <unordered_set>

#pragma comment(lib, "shell32.lib")

namespace
{
	constexpr size_t kMaxVertices = 200000;
	constexpr size_t kMaxFaces = 200000;
	constexpr int kMaxWalk = 65536;

	std::atomic<uintptr_t> g_cacheOff{ Offsets::MeshContentProvider::Cache };

	std::string GetCacheDir()
	{
		static std::string cached;
		if (!cached.empty()) return cached;

		char path[MAX_PATH] = { 0 };
		if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path)))
		{
			cached = std::string(path) + "\\Vintage\\meshes\\";
			CreateDirectoryA((std::string(path) + "\\Vintage").c_str(), nullptr);
			CreateDirectoryA(cached.c_str(), nullptr);
		}
		return cached;
	}

	std::string DiskPathFor(const std::string& id)
	{
		const std::string dir = GetCacheDir();
		if (dir.empty()) return "";
		return dir + id + ".mesh";
	}

	bool ReadDiskBlob(const std::string& id, std::vector<uint8_t>& out)
	{
		const std::string path = DiskPathFor(id);
		if (path.empty()) return false;

		HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE) return false;

		LARGE_INTEGER sz{};
		if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 32 * 1024 * 1024)
		{
			CloseHandle(h);
			return false;
		}

		out.resize((size_t)sz.QuadPart);
		DWORD got = 0;
		const BOOL ok = ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr);
		CloseHandle(h);
		if (!ok || got != out.size()) { out.clear(); return false; }
		return true;
	}

	void WriteDiskBlob(const std::string& id, const std::vector<uint8_t>& data)
	{
		const std::string path = DiskPathFor(id);
		if (path.empty()) return;
		HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0,
			nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE) return;
		DWORD written = 0;
		WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr);
		CloseHandle(h);
	}

	bool LooksLikeAssetUrl(const std::string& s)
	{
		if (s.size() < 4 || s.size() > 512) return false;
		for (char c : s) if (c < 0x20 || c > 0x7e) return false;
		if (s.find("rbxasset") != std::string::npos) return true;
		if (s.find("assetdelivery") != std::string::npos) return true;
		if (s.find("/asset/") != std::string::npos) return true;
		if (s.find("?id=") != std::string::npos) return true;
		if (s.find("rbxassetid") != std::string::npos) return true;
		if (s.find("http") == 0) return true;
		bool allDigits = !s.empty();
		for (char c : s)
		{
			if (c < '0' || c > '9') { allDigits = false; break; }
		}
		return allDigits && s.size() >= 5;
	}

	uintptr_t FindMcp(Memory& mem)
	{
		if (!mem.base) return 0;
		uintptr_t fdm = mem.Read<uintptr_t>(mem.base + Offsets::FakeDataModel::Pointer);
		if (!fdm || !mem.IsValid(fdm)) return 0;
		uintptr_t dm = mem.Read<uintptr_t>(fdm + Offsets::FakeDataModel::RealDataModel);
		if (!dm || !mem.IsValid(dm)) return 0;
		return mem.FindChildByClass(dm, "MeshContentProvider");
	}

	struct CacheChain
	{
		uintptr_t cache = 0;
		uintptr_t lru = 0;
		uintptr_t head = 0;
		bool valid = false;
	};

	CacheChain ProbeChain(Memory& mem, uintptr_t mcp, uintptr_t cacheOff)
	{
		CacheChain r{};
		r.cache = mem.Read<uintptr_t>(mcp + cacheOff);
		if (!r.cache || !mem.IsValid(r.cache)) return r;

		r.lru = mem.Read<uintptr_t>(r.cache + Offsets::MeshContentProvider::LRUCache);
		if (!r.lru || !mem.IsValid(r.lru)) return r;

		r.head = mem.Read<uintptr_t>(r.lru + 0x8);
		if (!r.head || !mem.IsValid(r.head)) return r;

		uintptr_t first = mem.Read<uintptr_t>(r.head);
		if (!first || !mem.IsValid(first) || first == r.head) return r;

		std::string asset = mem.ReadString(first + Offsets::MeshContentProvider::AssetID);
		if (!LooksLikeAssetUrl(asset)) return r;

		r.valid = true;
		return r;
	}

	uintptr_t ResolveChainHead(Memory& mem)
	{
		uintptr_t mcp = FindMcp(mem);
		if (!mcp || !mem.IsValid(mcp)) return 0;

		CacheChain ch = ProbeChain(mem, mcp, g_cacheOff.load());
		if (!ch.valid)
		{
			for (uintptr_t off = 0x40; off < 0x400; off += 8)
			{
				CacheChain cand = ProbeChain(mem, mcp, off);
				if (!cand.valid) continue;
				g_cacheOff.store(off);
				ch = cand;
				break;
			}
		}
		return ch.valid ? ch.head : 0;
	}

	uintptr_t FetchMeshDataAddress(Memory& mem, const std::string& wanted)
	{
		uintptr_t head = ResolveChainHead(mem);
		if (!head) return 0;

		std::string wantedNorm = MeshHttp::NormalizeId(wanted);

		uintptr_t node = mem.Read<uintptr_t>(head);
		std::unordered_set<uintptr_t> visited;

		for (int i = 0; i < kMaxWalk && mem.IsValid(node) && node != head; ++i)
		{
			if (!visited.insert(node).second) break;

			std::string raw = mem.ReadString(node + Offsets::MeshContentProvider::AssetID);
			if (!raw.empty())
			{
				std::string norm = MeshHttp::NormalizeId(raw);
				bool match = raw == wanted ||
					(!wantedNorm.empty() && norm == wantedNorm);
				if (match)
				{
					uintptr_t inter = mem.Read<uintptr_t>(node + Offsets::MeshContentProvider::ToMeshData);
					if (!inter || !mem.IsValid(inter)) return 0;
					uintptr_t md = mem.Read<uintptr_t>(inter + Offsets::MeshContentProvider::MeshData);
					if (!md || !mem.IsValid(md)) return 0;
					return md;
				}
			}

			node = mem.Read<uintptr_t>(node);
		}
		return 0;
	}

	bool ExtractGeometryFromMcp(Memory& mem, uintptr_t meshData,
		std::vector<float>& outVerts, std::vector<uint32_t>& outIndices)
	{
		uintptr_t vstart = mem.Read<uintptr_t>(meshData + Offsets::MeshData::VertexStart);
		uintptr_t vend = mem.Read<uintptr_t>(meshData + Offsets::MeshData::VertexEnd);
		uintptr_t fstart = mem.Read<uintptr_t>(meshData + Offsets::MeshData::FaceStart);
		uintptr_t fend = mem.Read<uintptr_t>(meshData + Offsets::MeshData::FaceEnd);

		if (!mem.IsValid(vstart) || !mem.IsValid(vend) || vstart >= vend) return false;
		if (!mem.IsValid(fstart) || !mem.IsValid(fend) || fstart >= fend) return false;

		const size_t vbytes = vend - vstart;
		const size_t fbytes = fend - fstart;
		if (vbytes == 0 || fbytes == 0) return false;

		constexpr size_t kMaxVBytes = kMaxVertices * 64;
		constexpr size_t kMaxFBytes = kMaxFaces * 12;
		if (vbytes > kMaxVBytes || fbytes > kMaxFBytes) return false;

		auto pickStride = [&](uint32_t mi) -> size_t {
			static const size_t strides[] = { 64, 60, 56, 52, 48, 44, 40, 36, 32 };
			const size_t need = (size_t)mi + 1;
			for (size_t s : strides) if (vbytes / s >= need) return s;
			return 0;
		};

		auto tryFormat = [&](size_t indexBytes, std::vector<uint32_t>& outIdx) -> size_t {
			if (indexBytes != 2 && indexBytes != 4) return 0;
			const size_t faceStride = 3 * indexBytes;
			if (fbytes % faceStride != 0) return 0;
			const size_t fcount = fbytes / faceStride;
			if (fcount == 0 || fcount > kMaxFaces) return 0;

			std::vector<uint8_t> rawIdx(fbytes);
			if (!mem.ReadMemory(fstart, rawIdx.data(), fbytes)) return 0;

			std::vector<uint32_t> tmp(fcount * 3);
			uint32_t mi = 0;
			if (indexBytes == 4)
			{
				for (size_t i = 0; i < tmp.size(); ++i)
				{
					uint32_t v;
					std::memcpy(&v, rawIdx.data() + i * 4, 4);
					tmp[i] = v;
					if (v > mi) mi = v;
				}
			}
			else
			{
				for (size_t i = 0; i < tmp.size(); ++i)
				{
					uint16_t v;
					std::memcpy(&v, rawIdx.data() + i * 2, 2);
					tmp[i] = v;
					if ((uint32_t)v > mi) mi = v;
				}
			}
			if ((size_t)mi >= kMaxVertices) return 0;

			const size_t stride = pickStride(mi);
			if (stride == 0) return 0;

			outIdx = std::move(tmp);
			return stride;
		};

		std::vector<uint32_t> indices;
		size_t srcStride = tryFormat(4, indices);
		if (srcStride == 0) srcStride = tryFormat(2, indices);
		if (srcStride == 0) return false;

		uint32_t maxIdx = 0;
		for (uint32_t v : indices) if (v > maxIdx) maxIdx = v;
		const size_t vcount = (size_t)maxIdx + 1;

		std::vector<uint8_t> raw(vbytes);
		if (!mem.ReadMemory(vstart, raw.data(), vbytes)) return false;

		std::vector<float> verts(vcount * 3);
		for (size_t i = 0; i < vcount; ++i)
		{
			float p[3];
			std::memcpy(p, raw.data() + i * srcStride, sizeof(p));
			verts[i * 3 + 0] = p[0];
			verts[i * 3 + 1] = p[1];
			verts[i * 3 + 2] = p[2];
		}

		outVerts = std::move(verts);
		outIndices = std::move(indices);
		return true;
	}

	void ComputeAabb(MeshLoader::Geometry& g)
	{
		float lo[3] = { 3.4e38f, 3.4e38f, 3.4e38f };
		float hi[3] = { -3.4e38f, -3.4e38f, -3.4e38f };
		const size_t vcount = g.verts.size() / 3;
		for (size_t i = 0; i < vcount; ++i)
		{
			const float x = g.verts[i * 3 + 0];
			const float y = g.verts[i * 3 + 1];
			const float z = g.verts[i * 3 + 2];
			if (x < lo[0]) lo[0] = x; if (x > hi[0]) hi[0] = x;
			if (y < lo[1]) lo[1] = y; if (y > hi[1]) hi[1] = y;
			if (z < lo[2]) lo[2] = z; if (z > hi[2]) hi[2] = z;
		}
		for (int k = 0; k < 3; ++k)
		{
			g.aabbMin[k] = lo[k];
			g.aabbMax[k] = hi[k];
			const float ext = hi[k] - lo[k];
			g.aabbSize[k] = (ext > 1e-4f) ? ext : 1.f;
			g.aabbCenter[k] = (lo[k] + hi[k]) * 0.5f;
		}
	}

	void ComputeNormals(MeshLoader::Geometry& g)
	{
		const size_t vcount = g.verts.size() / 3;
		g.normals.assign(vcount * 3, 0.f);

		for (size_t i = 0; i + 2 < g.indices.size(); i += 3)
		{
			const uint32_t i0 = g.indices[i + 0];
			const uint32_t i1 = g.indices[i + 1];
			const uint32_t i2 = g.indices[i + 2];
			if (i0 >= vcount || i1 >= vcount || i2 >= vcount) continue;

			const float ax = g.verts[i1 * 3 + 0] - g.verts[i0 * 3 + 0];
			const float ay = g.verts[i1 * 3 + 1] - g.verts[i0 * 3 + 1];
			const float az = g.verts[i1 * 3 + 2] - g.verts[i0 * 3 + 2];
			const float bx = g.verts[i2 * 3 + 0] - g.verts[i0 * 3 + 0];
			const float by = g.verts[i2 * 3 + 1] - g.verts[i0 * 3 + 1];
			const float bz = g.verts[i2 * 3 + 2] - g.verts[i0 * 3 + 2];

			const float nx = ay * bz - az * by;
			const float ny = az * bx - ax * bz;
			const float nz = ax * by - ay * bx;

			g.normals[i0 * 3 + 0] += nx; g.normals[i0 * 3 + 1] += ny; g.normals[i0 * 3 + 2] += nz;
			g.normals[i1 * 3 + 0] += nx; g.normals[i1 * 3 + 1] += ny; g.normals[i1 * 3 + 2] += nz;
			g.normals[i2 * 3 + 0] += nx; g.normals[i2 * 3 + 1] += ny; g.normals[i2 * 3 + 2] += nz;
		}

		for (size_t i = 0; i < vcount; ++i)
		{
			float* n = &g.normals[i * 3];
			const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
			if (len > 1e-4f)
			{
				n[0] /= len; n[1] /= len; n[2] /= len;
			}
		}
	}

	bool IsNumericId(const std::string& norm)
	{
		if (norm.empty() || norm.size() > 24) return false;
		for (char c : norm) if (c < '0' || c > '9') return false;
		return true;
	}
}

namespace MeshLoader
{
	bool TryLoad(const std::string& meshId, Geometry& out)
	{
		if (meshId.empty()) return false;

		const std::string norm = MeshHttp::NormalizeId(meshId);

		std::vector<uint8_t> blob;
		bool parsed = false;

		if (IsNumericId(norm))
		{
			if (ReadDiskBlob(norm, blob))
			{
				if (MeshParser::Parse(blob, out.verts, out.indices))
				{
					parsed = true;
				}
				else
				{
					DeleteFileA(DiskPathFor(norm).c_str());
					blob.clear();
				}
			}

			if (!parsed && MeshHttp::Fetch(meshId, blob))
			{
				if (MeshParser::Parse(blob, out.verts, out.indices))
				{
					WriteDiskBlob(norm, blob);
					parsed = true;
				}
			}
		}

		if (parsed)
		{
			ComputeNormals(out);
			ComputeAabb(out);
			return true;
		}

		auto& mem = Memory::Get();
		uintptr_t md = FetchMeshDataAddress(mem, meshId);
		if (md && ExtractGeometryFromMcp(mem, md, out.verts, out.indices))
		{
			ComputeNormals(out);
			ComputeAabb(out);
			return true;
		}

		return false;
	}
}
