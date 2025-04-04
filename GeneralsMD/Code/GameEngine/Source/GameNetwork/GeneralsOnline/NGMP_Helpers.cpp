#include "GameNetwork/GeneralsOnline/NGMP_include.h"

void NetworkLog(const char* fmt, ...)
{
	char buffer[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, 1024, fmt, args);
	buffer[1024 - 1] = 0;
	va_end(args);

#if defined(GENERALS_ONLINE_BRANCH_JMARSHALL)
	DevConsole.AddLog(buffer);
#endif

	OutputDebugString(buffer);
	OutputDebugString("\n");
}