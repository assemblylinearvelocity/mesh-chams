#include "http.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <winhttp.h>
#include <ShlObj.h>
#include <TlHelp32.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

namespace
{
	bool IsDigit(char c) { return c >= '0' && c <= '9'; }

	HANDLE OpenRobloxProcess()
	{
		HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE) return nullptr;

		PROCESSENTRY32W entry = { sizeof(PROCESSENTRY32W) };
		DWORD pid = 0;
		if (Process32FirstW(snapshot, &entry))
		{
			do
			{
				if (!_wcsicmp(entry.szExeFile, L"RobloxPlayerBeta.exe") ||
					!_wcsicmp(entry.szExeFile, L"RobloxPlayer.exe"))
				{
					pid = entry.th32ProcessID;
					break;
				}
			} while (Process32NextW(snapshot, &entry));
		}
		CloseHandle(snapshot);
		if (!pid) return nullptr;

		return OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
	}

	std::string ScanRobloxCookie()
	{
		HANDLE handle = OpenRobloxProcess();
		if (!handle) return {};

		static const char prefix[] = "_|WARNING:-DO-NOT-SHARE-THIS.";
		const size_t prefixLen = sizeof(prefix) - 1;

		std::vector<uint8_t> buf;
		buf.reserve(4 * 1024 * 1024);
		std::string result;

		MEMORY_BASIC_INFORMATION mbi;
		uintptr_t addr = 0;
		while (VirtualQueryEx(handle, (LPCVOID)addr, &mbi, sizeof(mbi)))
		{
			const uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;

			const bool readable = mbi.State == MEM_COMMIT &&
				(mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY)) != 0 &&
				(mbi.Protect & PAGE_GUARD) == 0 &&
				mbi.Type == MEM_PRIVATE &&
				mbi.RegionSize <= 64u * 1024 * 1024;

			if (readable)
			{
				buf.resize(mbi.RegionSize);
				SIZE_T got = 0;
				if (ReadProcessMemory(handle, mbi.BaseAddress, buf.data(), buf.size(), &got) && got > prefixLen)
				{
					for (size_t i = 0; i + prefixLen < got; ++i)
					{
						if (buf[i] != '_') continue;
						if (std::memcmp(buf.data() + i, prefix, prefixLen) != 0) continue;

						size_t j = i + prefixLen;
						while (j < got && buf[j] >= 0x20 && buf[j] < 0x7f) ++j;
						const size_t len = j - i;
						if (len >= 300 && len < 4096)
						{
							result.assign((const char*)buf.data() + i, len);
							CloseHandle(handle);
							return result;
						}
					}
				}
			}

			addr = regionEnd;
			if (addr <= (uintptr_t)mbi.BaseAddress) break;
		}

		CloseHandle(handle);
		return {};
	}

	std::atomic<bool> g_authScanning{ false };
	std::atomic<bool> g_authReady{ false };
	std::wstring g_authHeaders;
	std::mutex g_authLock;

	std::wstring GetAuthHeaders()
	{
		if (g_authReady.load(std::memory_order_acquire))
		{
			std::lock_guard<std::mutex> g(g_authLock);
			return g_authHeaders;
		}
		return {};
	}

	void StartAuthScan()
	{
		bool expected = false;
		if (!g_authScanning.compare_exchange_strong(expected, true)) return;

		std::thread([] {
			std::string cookie = ScanRobloxCookie();

			if (cookie.empty())
			{
				char path[MAX_PATH] = { 0 };
				if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path)))
				{
					const std::string full = std::string(path) + "\\Vintage\\cookie.txt";
					HANDLE h = CreateFileA(full.c_str(), GENERIC_READ, FILE_SHARE_READ,
						nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
					if (h != INVALID_HANDLE_VALUE)
					{
						char buf[8192] = { 0 };
						DWORD got = 0;
						ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
						CloseHandle(h);
						cookie.assign(buf, got);
						while (!cookie.empty() && (cookie.back() == '\r' || cookie.back() == '\n' || cookie.back() == ' ' || cookie.back() == '\t'))
							cookie.pop_back();
						while (!cookie.empty() && (cookie.front() == ' ' || cookie.front() == '\t'))
							cookie.erase(cookie.begin());
					}
				}
			}

			if (!cookie.empty())
			{
				std::string header = "Accept: */*\r\nAccept-Encoding: gzip, deflate\r\nCookie: .ROBLOSECURITY=";
				header += cookie;
				header += "\r\n";

				std::wstring wheader;
				for (char c : header) wheader.push_back((wchar_t)c);

				std::lock_guard<std::mutex> g(g_authLock);
				g_authHeaders = std::move(wheader);
			}
			g_authReady.store(true, std::memory_order_release);
		}).detach();
	}

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

		const std::wstring authHeaders = GetAuthHeaders();
		const wchar_t* headers = authHeaders.empty()
			? L"Accept: */*\r\nAccept-Encoding: gzip, deflate\r\n"
			: authHeaders.c_str();

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

	bool TryFetchAndFollow(const wchar_t* host, const std::wstring& path, std::vector<uint8_t>& out, const std::string& debugId)
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
	void EnsureAuthLoaded()
	{
		StartAuthScan();
	}

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
		if (TryFetchAndFollow(L"assetdelivery.roblox.com", v1Path, out, "v1-roblox-" + id)) return true;

		out.clear();
		if (TryFetchAndFollow(L"assetdelivery.roblox.com", v2Path, out, "v2-roblox-" + id)) return true;

		out.clear();
		if (TryFetchAndFollow(L"assetdelivery.roproxy.com", v1Path, out, "v1-roproxy-" + id)) return true;

		out.clear();
		if (TryFetchAndFollow(L"assetdelivery.roproxy.com", v2Path, out, "v2-roproxy-" + id)) return true;

		out.clear();
		return false;
	}
}
