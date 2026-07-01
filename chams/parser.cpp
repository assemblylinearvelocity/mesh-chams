#include "parser.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
	constexpr size_t kMaxVerts = 5'000'000;
	constexpr size_t kMaxFaces = 5'000'000;

	bool StartsWith(const std::vector<uint8_t>& d, const char* s)
	{
		const size_t n = std::strlen(s);
		if (d.size() < n) return false;
		return std::memcmp(d.data(), s, n) == 0;
	}

	struct Cursor
	{
		const uint8_t* p;
		const uint8_t* end;

		bool Ok(size_t n) const { return p + n <= end; }

		template <typename T>
		bool Read(T& out)
		{
			if (!Ok(sizeof(T))) return false;
			std::memcpy(&out, p, sizeof(T));
			p += sizeof(T);
			return true;
		}

		bool Skip(size_t n)
		{
			if (!Ok(n)) return false;
			p += n;
			return true;
		}
	};

	bool ParseV1(const std::vector<uint8_t>& data, float scale,
		std::vector<float>& outPos, std::vector<uint32_t>& outIdx)
	{
		const char* base = reinterpret_cast<const char*>(data.data());
		const char* end = base + data.size();

		const char* lineEnd = static_cast<const char*>(std::memchr(base, '\n', data.size()));
		if (!lineEnd) return false;
		const char* p = lineEnd + 1;

		uint32_t faceCount = 0;
		while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
		while (p < end && *p >= '0' && *p <= '9')
		{
			faceCount = faceCount * 10u + uint32_t(*p - '0');
			++p;
		}
		if (faceCount == 0 || faceCount > kMaxFaces) return false;

		const size_t vcount = (size_t)faceCount * 3;
		if (vcount > kMaxVerts) return false;

		outPos.clear();
		outPos.reserve(vcount * 3);
		outIdx.clear();
		outIdx.reserve(vcount);

		auto skipWs = [&]() {
			while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
		};
		auto expect = [&](char c) {
			skipWs();
			if (p < end && *p == c) { ++p; return true; }
			return false;
		};
		auto readFloat = [&](float& f) {
			skipWs();
			char* endp = nullptr;
			f = std::strtof(p, &endp);
			if (!endp || endp == p) return false;
			p = endp;
			return true;
		};
		auto readTriple = [&](float v[3]) {
			if (!expect('[')) return false;
			if (!readFloat(v[0])) return false;
			if (!expect(',')) return false;
			if (!readFloat(v[1])) return false;
			if (!expect(',')) return false;
			if (!readFloat(v[2])) return false;
			if (!expect(']')) return false;
			return true;
		};

		for (uint32_t f = 0; f < faceCount; ++f)
		{
			for (int v = 0; v < 3; ++v)
			{
				float pos[3], norm[3], uv[3];
				if (!readTriple(pos)) return false;
				if (!readTriple(norm)) return false;
				if (!readTriple(uv)) return false;

				outPos.push_back(pos[0] * scale);
				outPos.push_back(pos[1] * scale);
				outPos.push_back(pos[2] * scale);
				outIdx.push_back((uint32_t)(outPos.size() / 3) - 1);
			}
		}
		return true;
	}

	bool ParseV2(Cursor c, std::vector<float>& outPos, std::vector<uint32_t>& outIdx)
	{
		uint16_t headerSize = 0;
		uint8_t vertSize = 0;
		uint8_t faceSize = 0;
		uint32_t numVerts = 0;
		uint32_t numFaces = 0;

		if (!c.Read(headerSize)) return false;
		if (!c.Read(vertSize)) return false;
		if (!c.Read(faceSize)) return false;
		if (!c.Read(numVerts)) return false;
		if (!c.Read(numFaces)) return false;

		if (vertSize < 12 || faceSize != 12) return false;
		if (numVerts == 0 || numVerts > kMaxVerts) return false;
		if (numFaces == 0 || numFaces > kMaxFaces) return false;

		const size_t skip = headerSize > 12 ? size_t(headerSize - 12) : 0;
		if (skip && !c.Skip(skip)) return false;

		outPos.clear();
		outPos.resize((size_t)numVerts * 3);
		for (uint32_t i = 0; i < numVerts; ++i)
		{
			if (!c.Ok(vertSize)) return false;
			float p[3];
			std::memcpy(p, c.p, sizeof(p));
			outPos[i * 3 + 0] = p[0];
			outPos[i * 3 + 1] = p[1];
			outPos[i * 3 + 2] = p[2];
			c.p += vertSize;
		}

		outIdx.clear();
		outIdx.resize((size_t)numFaces * 3);
		for (uint32_t i = 0; i < numFaces; ++i)
		{
			if (!c.Ok(12)) return false;
			uint32_t tri[3];
			std::memcpy(tri, c.p, sizeof(tri));
			c.p += 12;
			if (tri[0] >= numVerts || tri[1] >= numVerts || tri[2] >= numVerts) return false;
			outIdx[i * 3 + 0] = tri[0];
			outIdx[i * 3 + 1] = tri[1];
			outIdx[i * 3 + 2] = tri[2];
		}
		return true;
	}

	bool ParseV3Plus(Cursor c, std::vector<float>& outPos, std::vector<uint32_t>& outIdx)
	{
		uint16_t headerSize = 0;
		uint8_t vertSize = 0;
		uint8_t faceSize = 0;
		uint16_t lodKind = 0;
		uint16_t numLods = 0;
		uint32_t numVerts = 0;
		uint32_t numFaces = 0;

		if (!c.Read(headerSize)) return false;
		if (!c.Read(vertSize)) return false;
		if (!c.Read(faceSize)) return false;
		if (!c.Read(lodKind)) return false;
		if (!c.Read(numLods)) return false;
		if (!c.Read(numVerts)) return false;
		if (!c.Read(numFaces)) return false;

		if (vertSize < 12 || faceSize != 12) return false;
		if (numVerts == 0 || numVerts > kMaxVerts) return false;
		if (numFaces == 0 || numFaces > kMaxFaces) return false;

		const size_t consumed = 16;
		const size_t skip = headerSize > consumed ? size_t(headerSize - consumed) : 0;
		if (skip && !c.Skip(skip)) return false;

		outPos.clear();
		outPos.resize((size_t)numVerts * 3);
		for (uint32_t i = 0; i < numVerts; ++i)
		{
			if (!c.Ok(vertSize)) return false;
			float p[3];
			std::memcpy(p, c.p, sizeof(p));
			outPos[i * 3 + 0] = p[0];
			outPos[i * 3 + 1] = p[1];
			outPos[i * 3 + 2] = p[2];
			c.p += vertSize;
		}

		outIdx.clear();
		outIdx.resize((size_t)numFaces * 3);
		for (uint32_t i = 0; i < numFaces; ++i)
		{
			if (!c.Ok(12)) return false;
			uint32_t tri[3];
			std::memcpy(tri, c.p, sizeof(tri));
			c.p += 12;
			if (tri[0] >= numVerts || tri[1] >= numVerts || tri[2] >= numVerts) return false;
			outIdx[i * 3 + 0] = tri[0];
			outIdx[i * 3 + 1] = tri[1];
			outIdx[i * 3 + 2] = tri[2];
		}
		return true;
	}
}

namespace MeshParser
{
	bool Parse(const std::vector<uint8_t>& data,
		std::vector<float>& outPositions,
		std::vector<uint32_t>& outIndices)
	{
		outPositions.clear();
		outIndices.clear();

		if (data.size() < 13) return false;

		if (StartsWith(data, "version 1.00"))
			return ParseV1(data, 2.f, outPositions, outIndices);
		if (StartsWith(data, "version 1.01"))
			return ParseV1(data, 1.f, outPositions, outIndices);

		auto findBinaryStart = [&](const char* magic) -> const uint8_t* {
			const size_t mlen = std::strlen(magic);
			if (data.size() < mlen) return nullptr;
			if (std::memcmp(data.data(), magic, mlen) != 0) return nullptr;
			size_t i = mlen;
			while (i < data.size() && (data[i] == '\r' || data[i] == '\n')) ++i;
			return data.data() + i;
		};

		Cursor c{};
		c.end = data.data() + data.size();

		if (auto* p = findBinaryStart("version 2.00")) { c.p = p; return ParseV2(c, outPositions, outIndices); }
		if (auto* p = findBinaryStart("version 3.00")) { c.p = p; return ParseV3Plus(c, outPositions, outIndices); }
		if (auto* p = findBinaryStart("version 3.01")) { c.p = p; return ParseV3Plus(c, outPositions, outIndices); }
		if (auto* p = findBinaryStart("version 4.00")) { c.p = p; return ParseV3Plus(c, outPositions, outIndices); }
		if (auto* p = findBinaryStart("version 4.01")) { c.p = p; return ParseV3Plus(c, outPositions, outIndices); }
		if (auto* p = findBinaryStart("version 5.00")) { c.p = p; return ParseV3Plus(c, outPositions, outIndices); }

		return false;
	}
}
