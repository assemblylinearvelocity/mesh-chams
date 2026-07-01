#pragma once
#include <cstdint>
#include <vector>

namespace MeshParser
{
	bool Parse(const std::vector<uint8_t>& data,
		std::vector<float>& outPositions,
		std::vector<uint32_t>& outIndices);
}
