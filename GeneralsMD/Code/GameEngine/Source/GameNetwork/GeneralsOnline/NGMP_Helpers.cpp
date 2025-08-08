#include "GameNetwork/GeneralsOnline/NGMP_include.h"
#include <chrono>
#include <mutex>
#include <string>
#include <locale>
#include <codecvt>
#include "../OnlineServices_Init.h"

std::string m_strNetworkLogFileName;
std::mutex m_logMutex;

std::string to_utf8(const std::wstring& wstr)
{
	std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
	return converter.to_bytes(wstr);
}

std::wstring from_utf8(const std::string& utf8_str)
{
	std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
	return converter.from_bytes(utf8_str);
}

void NetworkLog(ELogVerbosity logVerbosity, const char* fmt, ...)
{
	if (!NGMP_OnlineServicesManager::Settings.Debug_VerboseLogging())
	{
		if (logVerbosity < g_LogVerbosity)
		{
			return;
		}
	}

	std::scoped_lock scopedLock { m_logMutex };

	if (m_strNetworkLogFileName.empty())
	{
		auto now = std::chrono::system_clock::now();
		auto in_time_t = std::chrono::system_clock::to_time_t(now);


		m_strNetworkLogFileName = std::format("{}\\GeneralsOnlineData\\GeneralsOnline.log", TheGlobalData->getPath_UserData().str());
		/*
			std::stringstream ss;

#if defined(_DEBUG)
		if (IsDebuggerPresent())
		{
			ss << std::put_time(std::localtime(&in_time_t),
				"GeneralsOnline_Debugger_%Y-%m-%d-%H-%M-%S.log");
		}
		else
		{
			ss << std::put_time(std::localtime(&in_time_t),
				"GeneralsOnline_%Y-%m-%d-%H-%M-%S.log");
		}
#else
		ss << "GeneralsOnline.log";
#endif
*/
		std::ofstream overwriteFile(m_strNetworkLogFileName);

		// log start msg
		overwriteFile << std::put_time(std::localtime(&in_time_t), "Log Started at %Y/%m/%d %H:%M") << std::endl;
	}

	auto const time = std::chrono::current_zone()->to_local(std::chrono::system_clock::now());

	char buffer[8192];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, 8192, fmt, args);
	buffer[8192 - 1] = 0;
	va_end(args);

	std::string strLogBuffer = std::format("[{:%Y-%m-%d %T}] {}", time, buffer);

	// TODO_NGMP: Keep open and flush regularly
	std::ofstream logFile;
	logFile.open(m_strNetworkLogFileName, std::ios_base::app);
	logFile << strLogBuffer.c_str() << std::endl;
	logFile.close();

#if defined(GENERALS_ONLINE_BRANCH_JMARSHALL)
	DevConsole.AddLog(strLogBuffer.c_str());
#endif

	OutputDebugString(strLogBuffer.c_str());
	OutputDebugString("\n");
}

std::string Base64Encode(const std::vector<uint8_t>& data)
{
	static const char base64_chars[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
		"abcdefghijklmnopqrstuvwxyz"
		"0123456789+/";

	std::string encoded;
	size_t i = 0;
	uint32_t octet_a, octet_b, octet_c, triple;

	while (i < data.size())
	{
		octet_a = i < data.size() ? data[i++] : 0;
		octet_b = i < data.size() ? data[i++] : 0;
		octet_c = i < data.size() ? data[i++] : 0;

		triple = (octet_a << 16) | (octet_b << 8) | octet_c;

		encoded += base64_chars[(triple >> 18) & 0x3F];
		encoded += base64_chars[(triple >> 12) & 0x3F];
		encoded += (i > data.size() + 1) ? '=' : base64_chars[(triple >> 6) & 0x3F];
		encoded += (i > data.size()) ? '=' : base64_chars[triple & 0x3F];
	}

	return encoded;
}

std::vector<uint8_t> Base64Decode(const std::string& encodedData) {
	static const std::string base64Chars =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
		"abcdefghijklmnopqrstuvwxyz"
		"0123456789+/";

	auto isBase64 = [](unsigned char c) {
		return std::isalnum(c) || (c == '+') || (c == '/');
		};

	std::vector<uint8_t> decodedData;
	int inLen = encodedData.size();
	int i = 0;
	int in_ = 0;
	uint8_t charArray4[4], charArray3[3];

	while (inLen-- && (encodedData[in_] != '=') && isBase64(encodedData[in_])) {
		charArray4[i++] = encodedData[in_]; in_++;
		if (i == 4) {
			for (i = 0; i < 4; i++)
				charArray4[i] = base64Chars.find(charArray4[i]);

			charArray3[0] = (charArray4[0] << 2) + ((charArray4[1] & 0x30) >> 4);
			charArray3[1] = ((charArray4[1] & 0xf) << 4) + ((charArray4[2] & 0x3c) >> 2);
			charArray3[2] = ((charArray4[2] & 0x3) << 6) + charArray4[3];

			for (i = 0; i < 3; i++)
				decodedData.push_back(charArray3[i]);
			i = 0;
		}
	}

	if (i) {
		for (int j = i; j < 4; j++)
			charArray4[j] = 0;

		for (int j = 0; j < 4; j++)
			charArray4[j] = base64Chars.find(charArray4[j]);

		charArray3[0] = (charArray4[0] << 2) + ((charArray4[1] & 0x30) >> 4);
		charArray3[1] = ((charArray4[1] & 0xf) << 4) + ((charArray4[2] & 0x3c) >> 2);
		charArray3[2] = ((charArray4[2] & 0x3) << 6) + charArray4[3];

		for (int j = 0; j < i - 1; j++)
			decodedData.push_back(charArray3[j]);
	}

	return decodedData;
}

int RoundUpLatencyToFrameInterval(int latency, int frameInterval)
{
	if (frameInterval == 0)
		return latency;

	int remainder = latency % frameInterval;
	if (remainder == 0)
		return latency;

	return latency + frameInterval - remainder;
}
int ConvertMSLatencyToFrames(int ms)
{
	ms = RoundUpLatencyToFrameInterval(ms, 1000 / GENERALS_ONLINE_HIGH_FPS_LIMIT);
	return (int)ceil((ms / 1000.f) * (float)GENERALS_ONLINE_HIGH_FPS_LIMIT);
}

int ConvertMSLatencyToGenToolFrames(int ms)
{
	return ConvertMSLatencyToFrames(ms) / GENERALS_ONLINE_HIGH_FPS_FRAME_MULTIPLIER;
}