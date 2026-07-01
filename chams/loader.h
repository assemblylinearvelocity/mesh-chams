#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace MeshLoader
{
	struct Geometry
	{
		std::vector<float> verts;
		std::vector<float> normals;
		std::vector<uint32_t> indices;
		float aabbMin[3];
		float aabbMax[3];
		float aabbSize[3];
		float aabbCenter[3];
	};

	bool TryLoad(const std::string& meshId, Geometry& out);
}
