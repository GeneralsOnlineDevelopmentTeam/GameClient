#include "GameNetwork/GeneralsOnline/CredentialStore.h"
#include "GameNetwork/GeneralsOnline/json.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>

#pragma comment(lib, "Crypt32.lib")

namespace ServiceConnection
{
namespace
{
using Json = nlohmann::json;

std::mutex storeMutex;
std::string (*pathProvider)() = nullptr;
void (*logSink)(const std::string&) = nullptr;
std::atomic<unsigned long> forcedCryptError{0};
// entries that could not be written; they win over the file and use the same layout
Json overlay = Json::object();
bool fileUnreadable = false;

void Log(const std::string& line)
{
	static std::string previous;
	if (previous == line)
		return;
	previous = line;
	if (logSink)
		logSink(line);
}

std::string Describe(const char* operation, unsigned long code)
{
	return std::string("[Connection] Credentials ") + operation + " failed (" + std::to_string(code) + "), kept for this session only";
}

bool Protect(const std::string& plain, std::string& blob, unsigned long& error)
{
	if (const unsigned long forced = forcedCryptError.load())
	{
		error = forced;
		return false;
	}
	DATA_BLOB input{ (DWORD)plain.size(), (BYTE*)plain.data() };
	DATA_BLOB output{};
	if (!CryptProtectData(&input, L"GO Credentials", nullptr, nullptr, nullptr, 0, &output))
	{
		error = GetLastError();
		return false;
	}
	blob.assign((const char*)output.pbData, output.cbData);
	LocalFree(output.pbData);
	return true;
}

bool Unprotect(const std::string& blob, std::string& plain, unsigned long& error)
{
	if (const unsigned long forced = forcedCryptError.load())
	{
		error = forced;
		return false;
	}
	DATA_BLOB input{ (DWORD)blob.size(), (BYTE*)blob.data() };
	DATA_BLOB output{};
	if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, 0, &output))
	{
		error = GetLastError();
		return false;
	}
	plain.assign((const char*)output.pbData, output.cbData);
	SecureZeroMemory(output.pbData, output.cbData);
	LocalFree(output.pbData);
	return true;
}

// the file of current builds holds {"refresh_token": ...}; it becomes the token of the default GO route
void Normalize(Json& doc)
{
	if (!doc.is_object())
		doc = Json::object();
	if (!doc.contains("tokens") || !doc["tokens"].is_object())
		doc["tokens"] = Json::object();
	if (!doc.contains("proxies") || !doc["proxies"].is_object())
		doc["proxies"] = Json::object();
	if (doc.contains("refresh_token"))
	{
		if (doc["refresh_token"].is_string() && !doc["refresh_token"].get<std::string>().empty() && !doc["tokens"].contains("go"))
			doc["tokens"]["go"] = doc["refresh_token"];
		doc.erase("refresh_token");
	}
}

// the stored document; a file that cannot be read counts as empty and is never overwritten
Json ReadFile()
{
	fileUnreadable = false;
	Json doc = Json::object();
	if (!pathProvider)
		return doc;
	const std::string path = pathProvider();
	std::string bytes;
	if (FILE* file = fopen(path.c_str(), "rb"))
	{
		fseek(file, 0, SEEK_END);
		const long size = ftell(file);
		fseek(file, 0, SEEK_SET);
		if (size > 0)
		{
			bytes.resize(size);
			if (fread(bytes.data(), 1, size, file) != (size_t)size)
				bytes.clear();
		}
		fclose(file);
	}
	if (bytes.empty())
		return doc;
	// a file that was never encrypted still loads
	Json parsed = Json::parse(bytes, nullptr, false);
	if (!parsed.is_discarded() && parsed.is_object())
		return parsed;
	std::string plain;
	unsigned long error = 0;
	if (Unprotect(bytes, plain, error))
	{
		parsed = Json::parse(plain, nullptr, false);
		if (!plain.empty())
			SecureZeroMemory(plain.data(), plain.size());
		if (!parsed.is_discarded() && parsed.is_object())
			return parsed;
		error = ERROR_INVALID_DATA;
	}
	fileUnreadable = true;
	Log(Describe("read", error));
	return doc;
}

Json Merged()
{
	Json doc = ReadFile();
	Normalize(doc);
	Json over = overlay;
	Normalize(over);
	for (const char* section : { "tokens", "proxies" })
	{
		for (auto it = over[section].begin(); it != over[section].end(); ++it)
			doc[section][it.key()] = it.value();
	}
	return doc;
}

bool WriteFile(const Json& doc)
{
	if (!pathProvider || fileUnreadable)
		return false;
	std::string plain;
	try
	{
		plain = doc.dump();
	}
	catch (...)
	{
		Log(Describe("write", ERROR_INVALID_DATA));
		return false;
	}
	std::string blob;
	unsigned long error = 0;
	const bool encrypted = Protect(plain, blob, error);
	if (!plain.empty())
		SecureZeroMemory(plain.data(), plain.size());
	if (!encrypted)
	{
		Log(Describe("encrypt", error));
		return false;
	}
	const std::string path = pathProvider();
	const std::string temp = path + ".tmp";
	bool saved = false;
	if (FILE* file = fopen(temp.c_str(), "wb"))
	{
		saved = fwrite(blob.data(), 1, blob.size(), file) == blob.size();
		if (fclose(file) != 0)
			saved = false;
	}
	if (saved)
		saved = MoveFileExA(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
	if (!saved)
	{
		Log(Describe("write", GetLastError()));
		DeleteFileA(temp.c_str());
	}
	return saved;
}

// changes one entry and rewrites the whole file; when that fails the entry stays in the overlay for this session
bool Change(const char* section, const std::string& key, const Json* value)
{
	std::lock_guard<std::mutex> lock(storeMutex);
	Json doc = Merged();
	if (value)
		doc[section][key] = *value;
	else
		doc[section].erase(key);
	if (WriteFile(doc))
	{
		overlay = Json::object();
		return true;
	}
	Normalize(overlay);
	if (value)
		overlay[section][key] = *value;
	else
		overlay[section].erase(key);
	return false;
}

bool Find(const char* section, const std::string& key, Json& value)
{
	std::lock_guard<std::mutex> lock(storeMutex);
	const Json doc = Merged();
	const auto it = doc[section].find(key);
	if (it == doc[section].end())
		return false;
	value = *it;
	return true;
}
}

void ConfigureCredentialStore(std::string (*provider)())
{
	std::lock_guard<std::mutex> lock(storeMutex);
	pathProvider = provider;
}

void SetCredentialLogSink(void (*sink)(const std::string&))
{
	std::lock_guard<std::mutex> lock(storeMutex);
	logSink = sink;
}

void SetCredentialCryptFailureForTest(unsigned long errorCode)
{
	forcedCryptError.store(errorCode);
}

bool SaveProxyLogin(const std::string& proxyKey, const std::string& user, const std::string& password)
{
	if (proxyKey.empty())
		return false;
	const Json value = { { "user", user }, { "password", password } };
	return Change("proxies", proxyKey, &value);
}

bool LoadProxyLogin(const std::string& proxyKey, std::string& user, std::string& password)
{
	Json value;
	if (proxyKey.empty() || !Find("proxies", proxyKey, value) || !value.is_object() || !value.contains("user") || !value["user"].is_string())
		return false;
	user = value["user"].get<std::string>();
	password = value.contains("password") && value["password"].is_string() ? value["password"].get<std::string>() : std::string();
	return !user.empty();
}

bool DeleteProxyLogin(const std::string& proxyKey)
{
	Json value;
	if (proxyKey.empty() || !Find("proxies", proxyKey, value))
		return true;
	return Change("proxies", proxyKey, nullptr);
}

bool SaveRefreshToken(const std::string& serverKey, const std::string& token)
{
	if (serverKey.empty() || token.empty())
		return false;
	const Json value = token;
	return Change("tokens", serverKey, &value);
}

bool LoadRefreshToken(const std::string& serverKey, std::string& token)
{
	Json value;
	if (serverKey.empty() || !Find("tokens", serverKey, value) || !value.is_string())
		return false;
	token = value.get<std::string>();
	return !token.empty();
}

bool DeleteRefreshToken(const std::string& serverKey)
{
	Json value;
	if (serverKey.empty() || !Find("tokens", serverKey, value))
		return true;
	return Change("tokens", serverKey, nullptr);
}
}
