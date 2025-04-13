#include "GameNetwork/GeneralsOnline/NGMP_include.h"
#include <chrono>

std::string m_strNetworkLogFileName;

void NetworkLog(const char* fmt, ...)
{
	if (m_strNetworkLogFileName.empty())
	{
		auto now = std::chrono::system_clock::now();
		auto in_time_t = std::chrono::system_clock::to_time_t(now);

		std::stringstream ss;

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
		m_strNetworkLogFileName = ss.str();
		std::ofstream overwriteFile(m_strNetworkLogFileName);

		// log start msg
		overwriteFile << std::put_time(std::localtime(&in_time_t), "Log Started at %Y/%m/%d %H:%M") << std::endl;
	}

	char buffer[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, 1024, fmt, args);
	buffer[1024 - 1] = 0;
	va_end(args);

	// TODO_NGMP: Keep open and flush regularly
	std::ofstream logFile;
	logFile.open(m_strNetworkLogFileName, std::ios_base::app);
	logFile << buffer << std::endl;
	logFile.close();

#if defined(GENERALS_ONLINE_BRANCH_JMARSHALL)
	DevConsole.AddLog(buffer);
#endif

	OutputDebugString(buffer);
	OutputDebugString("\n");
}