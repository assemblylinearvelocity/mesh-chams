#pragma once
#include <d3d11.h>
#include <string>

namespace MeshCache
{
	struct GpuMesh
	{
		ID3D11Buffer* vb = nullptr;
		ID3D11Buffer* ib = nullptr;
		unsigned indexCount = 0;
		float meshSize[3] = { 1.f, 1.f, 1.f };
		bool ready = false;
	};

	void Start();
	void Stop();

	GpuMesh* TryGet(const std::string& meshId, ID3D11Device* device);

	void InvalidateAll();
}
