#pragma once

#include <string>

// One DPAPI-protected credentials.json holds the GO login token of every server and the proxy logins.
// Every save reads the whole file, changes its own entry and writes the file back, so no entry wipes another.
// When the file cannot be read, decrypted or written, one log line with the error code is written (no secrets), the value
// stays in memory for this session and nothing blocks.
namespace ServiceConnection
{
// the file path is asked for on every use, so it follows the instance of a multi-instance debug build
void ConfigureCredentialStore(std::string (*pathProvider)());
void SetCredentialLogSink(void (*sink)(const std::string&));
// test hook: encryption and decryption fail with this error code, 0 turns it off
void SetCredentialCryptFailureForTest(unsigned long errorCode);

// returns false when the value could not be saved to the file; it is then kept for this session
bool SaveProxyLogin(const std::string& proxyKey, const std::string& user, const std::string& password);
bool LoadProxyLogin(const std::string& proxyKey, std::string& user, std::string& password);
bool DeleteProxyLogin(const std::string& proxyKey);

bool SaveRefreshToken(const std::string& serverKey, const std::string& token);
bool LoadRefreshToken(const std::string& serverKey, std::string& token);
bool DeleteRefreshToken(const std::string& serverKey);
}
