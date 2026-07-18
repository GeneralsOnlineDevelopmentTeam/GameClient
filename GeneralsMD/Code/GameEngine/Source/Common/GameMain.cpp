/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// GameMain.cpp
// The main entry point for the game
// Author: Michael S. Booth, April 2001

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/FramePacer.h"
#include "Common/GameEngine.h"
#include "Common/ReplaySimulation.h"

#include <cstdarg>	// TheSuperHackers @debug 18/07/2026 va_list for traceLog
#include <cstdio>	// TheSuperHackers @debug 18/07/2026 fopen/vfprintf for traceLog

// TheSuperHackers @debug 18/07/2026 Startup-chain tracing — audio investigation.
static FILE* g_traceLogFile_gamemain = nullptr;
static void traceLog(const char* fmt, ...)
{
	__try {
		if (!g_traceLogFile_gamemain) {
			g_traceLogFile_gamemain = fopen("genovly_debug.log", "a");
		}
		if (g_traceLogFile_gamemain) {
			va_list args;
			va_start(args, fmt);
			vfprintf(g_traceLogFile_gamemain, fmt, args);
			va_end(args);
			fflush(g_traceLogFile_gamemain);
		}
	}
	__except(EXCEPTION_EXECUTE_HANDLER) {
	}
}


/**
 * This is the entry point for the game system.
 */
Int GameMain()
{
	int exitcode = 0;
	// initialize the game engine using factory function
	traceLog("TRACE: GameMain entry\n");
	TheFramePacer = new FramePacer();
	TheFramePacer->enableFramesPerSecondLimit(TRUE);
	TheGameEngine = CreateGameEngine();
	traceLog("TRACE: engine created TheGameEngine=%p\n", (void*)TheGameEngine);
	traceLog("TRACE: calling TheGameEngine->init()\n");
	TheGameEngine->init();
	traceLog("TRACE: TheGameEngine->init() returned\n");

	if (TheGlobalData->m_exportStats && (!TheGlobalData->m_headless || TheGlobalData->m_simulateReplays.empty()))
	{
		printf("ERROR: -exportStats requires headless replay mode (-headless -replay <file>).\n");
		fflush(stdout);
		exitcode = 1;
	}
	else if (!TheGlobalData->m_simulateReplays.empty())
	{
		exitcode = ReplaySimulation::simulateReplays(TheGlobalData->m_simulateReplays, TheGlobalData->m_simulateReplayJobs);
	}
	else
	{
		// run it
		TheGameEngine->execute();
	}

	// since execute() returned, we are exiting the game
	delete TheFramePacer;
	TheFramePacer = nullptr;
	delete TheGameEngine;
	TheGameEngine = nullptr;

	return exitcode;
}

