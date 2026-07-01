#include "cache.h"
#include "loader.h"
#include "http.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
	constexpr int kWorkers = 2;
	constexpr std::chrono::milliseconds kFailCooldown{ 5 * 60 * 1000 };

	struct Vertex
	{
		float pos[3];
		float normal[3];
	};

	struct Entry
	{
		MeshCache::GpuMesh mesh{};
		std::vector<Vertex> pendingVerts;
		std::vector<uint32_t> pendingIndices;
		float meshSize[3] = { 1.f, 1.f, 1.f };
		bool dataReady = false;
		bool uploaded = false;
	};

	std::mutex g_lock;
	std::unordered_map<std::string, std::unique_ptr<Entry>> g_cache;
	std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_failed;
	std::unordered_set<std::string> g_queued;
	std::deque<std::string> g_queue;
	std::condition_variable g_cv;

	std::atomic<bool> g_running{ false };
	std::vector<std::thread> g_workers;

	bool UploadToGpu(Entry& e, ID3D11Device* device)
	{
		if (e.uploaded || !e.dataReady || e.pendingVerts.empty() || e.pendingIndices.empty())
			return e.uploaded;

		D3D11_BUFFER_DESC vbd{};
		vbd.Usage = D3D11_USAGE_IMMUTABLE;
		vbd.ByteWidth = (UINT)(e.pendingVerts.size() * sizeof(Vertex));
		vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		D3D11_SUBRESOURCE_DATA vinit{ e.pendingVerts.data() };
		if (FAILED(device->CreateBuffer(&vbd, &vinit, &e.mesh.vb))) return false;

		D3D11_BUFFER_DESC ibd{};
		ibd.Usage = D3D11_USAGE_IMMUTABLE;
		ibd.ByteWidth = (UINT)(e.pendingIndices.size() * sizeof(uint32_t));
		ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
		D3D11_SUBRESOURCE_DATA iinit{ e.pendingIndices.data() };
		if (FAILED(device->CreateBuffer(&ibd, &iinit, &e.mesh.ib))) return false;

		e.mesh.indexCount = (unsigned)e.pendingIndices.size();
		e.mesh.meshSize[0] = e.meshSize[0];
		e.mesh.meshSize[1] = e.meshSize[1];
		e.mesh.meshSize[2] = e.meshSize[2];
		e.mesh.ready = true;
		e.uploaded = true;

		e.pendingVerts.clear();
		e.pendingVerts.shrink_to_fit();
		e.pendingIndices.clear();
		e.pendingIndices.shrink_to_fit();
		return true;
	}

	bool PopPending(std::string& out)
	{
		std::unique_lock<std::mutex> lk(g_lock);
		g_cv.wait(lk, [] { return !g_running.load() || !g_queue.empty(); });
		if (!g_running.load()) return false;
		out = std::move(g_queue.front());
		g_queue.pop_front();
		g_queued.erase(out);
		return true;
	}

	void WorkerLoop()
	{
		while (g_running.load())
		{
			std::string id;
			if (!PopPending(id)) break;

			{
				std::lock_guard<std::mutex> lk(g_lock);
				auto it = g_cache.find(id);
				if (it != g_cache.end() && it->second && it->second->dataReady) continue;
			}

			MeshLoader::Geometry geom;
			bool ok = MeshLoader::TryLoad(id, geom);

			std::lock_guard<std::mutex> lk(g_lock);
			if (!ok)
			{
				g_failed[id] = std::chrono::steady_clock::now() + kFailCooldown;
				continue;
			}

			auto e = std::make_unique<Entry>();
			e->meshSize[0] = geom.aabbSize[0];
			e->meshSize[1] = geom.aabbSize[1];
			e->meshSize[2] = geom.aabbSize[2];

			const size_t vcount = geom.verts.size() / 3;
			e->pendingVerts.resize(vcount);
			for (size_t i = 0; i < vcount; ++i)
			{
				e->pendingVerts[i].pos[0] = geom.verts[i * 3 + 0];
				e->pendingVerts[i].pos[1] = geom.verts[i * 3 + 1];
				e->pendingVerts[i].pos[2] = geom.verts[i * 3 + 2];
				if (geom.normals.size() == geom.verts.size())
				{
					e->pendingVerts[i].normal[0] = geom.normals[i * 3 + 0];
					e->pendingVerts[i].normal[1] = geom.normals[i * 3 + 1];
					e->pendingVerts[i].normal[2] = geom.normals[i * 3 + 2];
				}
				else
				{
					e->pendingVerts[i].normal[0] = 0.f;
					e->pendingVerts[i].normal[1] = 0.f;
					e->pendingVerts[i].normal[2] = 0.f;
				}
			}
			e->pendingIndices = std::move(geom.indices);
			e->dataReady = true;
			g_cache[id] = std::move(e);
			g_failed.erase(id);
		}
	}

	void ScheduleLocked(const std::string& id)
	{
		if (g_queued.count(id)) return;
		auto it = g_cache.find(id);
		if (it != g_cache.end() && it->second && it->second->dataReady) return;

		const auto now = std::chrono::steady_clock::now();
		auto fit = g_failed.find(id);
		if (fit != g_failed.end())
		{
			if (now < fit->second) return;
			g_failed.erase(fit);
		}

		g_queued.insert(id);
		g_queue.push_back(id);
		g_cv.notify_one();
	}

	void EnsureStarted()
	{
		if (g_running.load(std::memory_order_relaxed)) return;
		bool expected = false;
		if (!g_running.compare_exchange_strong(expected, true)) return;

		{
			std::lock_guard<std::mutex> lk(g_lock);
			g_failed.clear();
		}

		for (int i = 0; i < kWorkers; ++i)
		{
			g_workers.emplace_back(WorkerLoop);
		}
	}
}

namespace MeshCache
{
	void Start()
	{
		EnsureStarted();
	}

	void Stop()
	{
		if (!g_running.exchange(false)) return;
		g_cv.notify_all();
		for (auto& t : g_workers)
		{
			if (t.joinable()) t.join();
		}
		g_workers.clear();
	}

	GpuMesh* TryGet(const std::string& meshId, ID3D11Device* device)
	{
		if (meshId.empty() || !device) return nullptr;
		EnsureStarted();

		std::lock_guard<std::mutex> lk(g_lock);
		auto it = g_cache.find(meshId);
		if (it != g_cache.end() && it->second && it->second->dataReady)
		{
			if (!it->second->uploaded)
				UploadToGpu(*it->second, device);
			return it->second->mesh.ready ? &it->second->mesh : nullptr;
		}

		ScheduleLocked(meshId);
		return nullptr;
	}

	void InvalidateAll()
	{
		std::lock_guard<std::mutex> lk(g_lock);
		for (auto& [id, e] : g_cache)
		{
			if (e && e->mesh.vb) { e->mesh.vb->Release(); e->mesh.vb = nullptr; }
			if (e && e->mesh.ib) { e->mesh.ib->Release(); e->mesh.ib = nullptr; }
		}
		g_cache.clear();
		g_failed.clear();
		g_queued.clear();
		g_queue.clear();
	}
}
