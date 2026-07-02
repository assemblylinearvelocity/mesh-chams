#include "http.h"
// hi
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <winhttp.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace
{
	bool IsDigit(char c) { return c >= '0' && c <= '9'; }

	bool LooksLikeMesh(const std::vector<uint8_t>& data)
	{
		return data.size() >= 13 && data[0] == 'v' && data[1] == 'e' && data[2] == 'r';
	}

	bool FetchUrl(const wchar_t* host, const std::wstring& path, std::vector<uint8_t>& out, DWORD& status)
	{
		status = 0;
		out.clear();

		HINTERNET hs = WinHttpOpen(
			L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
			WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
			WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (!hs) return false;

		HINTERNET hc = WinHttpConnect(hs, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
		if (!hc) { WinHttpCloseHandle(hs); return false; }

		HINTERNET hr = WinHttpOpenRequest(hc, L"GET", path.c_str(),
			nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
			WINHTTP_FLAG_SECURE);
		if (!hr) { WinHttpCloseHandle(hc); WinHttpCloseHandle(hs); return false; }

		DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;
		WinHttpSetOption(hr, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof(decompress));

		DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
		WinHttpSetOption(hr, WINHTTP_OPTION_REDIRECT_POLICY,
			&redirectPolicy, sizeof(redirectPolicy));

		const wchar_t* headers = L"Accept: */*\r\nAccept-Encoding: gzip, deflate\r\n";

		bool ok = false;
		if (WinHttpSendRequest(hr, headers, (DWORD)-1L,
			WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
			&& WinHttpReceiveResponse(hr, nullptr))
		{
			DWORD statusSize = sizeof(status);
			WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);

			if (status >= 200 && status < 300)
			{
				out.reserve(64 * 1024);
				uint8_t buf[8192];
				DWORD got = 0;
				while (WinHttpReadData(hr, buf, sizeof(buf), &got) && got > 0)
				{
					out.insert(out.end(), buf, buf + got);
					if (out.size() > 32u * 1024 * 1024) break;
				}
				ok = !out.empty();
			}
		}

		WinHttpCloseHandle(hr);
		WinHttpCloseHandle(hc);
		WinHttpCloseHandle(hs);
		return ok;
	}

	bool FetchUrl(const wchar_t* host, const std::wstring& path, std::vector<uint8_t>& out)
	{
		DWORD status = 0;
		return FetchUrl(host, path, out, status);
	}

	bool FetchAbsoluteUrl(const std::string& url, std::vector<uint8_t>& out)
	{
		const std::string httpsPrefix = "https://";
		if (url.size() <= httpsPrefix.size()) return false;
		if (url.compare(0, httpsPrefix.size(), httpsPrefix) != 0) return false;

		const size_t hostStart = httpsPrefix.size();
		const size_t pathStart = url.find('/', hostStart);

		std::wstring host;
		std::wstring path;
		if (pathStart == std::string::npos)
		{
			for (size_t i = hostStart; i < url.size(); ++i) host.push_back((wchar_t)url[i]);
			path = L"/";
		}
		else
		{
			for (size_t i = hostStart; i < pathStart; ++i) host.push_back((wchar_t)url[i]);
			for (size_t i = pathStart; i < url.size(); ++i) path.push_back((wchar_t)url[i]);
		}

		return FetchUrl(host.c_str(), path, out);
	}

	std::string ExtractJsonValue(const std::vector<uint8_t>& body, const char* key)
	{
		const std::string s(body.begin(), body.end());
		const std::string needle = std::string("\"") + key + "\"";
		auto k = s.find(needle);
		if (k == std::string::npos) return {};
		auto colon = s.find(':', k + needle.size());
		if (colon == std::string::npos) return {};
		auto quote = s.find('"', colon + 1);
		if (quote == std::string::npos) return {};
		auto endQuote = s.find('"', quote + 1);
		if (endQuote == std::string::npos) return {};
		return s.substr(quote + 1, endQuote - quote - 1);
	}

	bool TryFetchAndFollow(const wchar_t* host, const std::wstring& path, std::vector<uint8_t>& out)
	{
		DWORD status = 0;
		if (!FetchUrl(host, path, out, status)) return false;
		if (LooksLikeMesh(out)) return true;

		if (out.size() < 4096)
		{
			std::string location = ExtractJsonValue(out, "location");
			if (location.empty()) location = ExtractJsonValue(out, "Location");
			if (!location.empty())
			{
				std::vector<uint8_t> next;
				if (FetchAbsoluteUrl(location, next) && LooksLikeMesh(next))
				{
					out = std::move(next);
					return true;
				}
			}
		}
		return false;
	}
}

namespace MeshHttp
{
	std::string NormalizeId(const std::string& id)
	{
		if (id.empty()) return id;
		std::string best, current;
		for (char c : id)
		{
			if (IsDigit(c)) current.push_back(c);
			else
			{
				if (current.size() > best.size()) best = std::move(current);
				current.clear();
			}
		}
		if (current.size() > best.size()) best = std::move(current);
		return best.size() >= 5 ? best : id;
	}

	bool Fetch(const std::string& assetId, std::vector<uint8_t>& out)
	{
		const std::string id = NormalizeId(assetId);
		if (id.empty()) return false;

		for (char c : id) if (!IsDigit(c)) return false;
		if (id.size() > 24) return false;

		std::wstring v1Path = L"/v1/asset/?id=";
		for (char c : id) v1Path.push_back((wchar_t)c);

		std::wstring v2Path = L"/v2/asset/?id=";
		for (char c : id) v2Path.push_back((wchar_t)c);

		out.clear();
		if (TryFetchAndFollow(L"assetdelivery.roblox.com", v1Path, out)) return true;

		out.clear();
		if (TryFetchAndFollow(L"assetdelivery.roblox.com", v2Path, out)) return true;

		out.clear();
		if (TryFetchAndFollow(L"assetdelivery.roproxy.com", v1Path, out)) return true;

		out.clear();
		if (TryFetchAndFollow(L"assetdelivery.roproxy.com", v2Path, out)) return true;

		out.clear();
		return false;
	}
}
