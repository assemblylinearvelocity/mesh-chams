#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace MeshHttp
{
	bool Fetch(const std::string& assetId, std::vector<uint8_t>& out);
	std::string NormalizeId(const std::string& id);
}
