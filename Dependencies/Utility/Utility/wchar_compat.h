/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 TheSuperHackers
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

// This file contains WCHAR and related macros for compatibility with non-windows platforms.
#pragma once

// WCHAR
typedef wchar_t WCHAR;
typedef const WCHAR* LPCWSTR;
typedef WCHAR* LPWSTR;

#define _wcsicmp wcscasecmp
#define wcsicmp wcscasecmp

// MultiByteToWideChar
#define CP_ACP 0
#define MultiByteToWideChar(cp, flags, mbstr, cb, wcstr, cch) mbstowcs(wcstr, mbstr, cch)

// WideCharToMultiByte replacement:
// The real Windows API supports a "query mode" where mbstr==nullptr and cb==0, which returns
// the required buffer size. wcstombs(nullptr, wcstr, 0) is unreliable on some CRT
// implementations (e.g. Windows CRT via MinGW), so we use an inline function instead.
inline int WideCharToMultiByte(unsigned int /*cp*/, unsigned long /*flags*/,
                               const wchar_t* wcstr, int /*cch*/,
                               char* mbstr, int cb,
                               const char* /*defchar*/, int* /*used*/)
{
    if (!wcstr)
        return 0;
    if (!mbstr || cb == 0)
    {
        // Query mode: return the required buffer size (number of bytes including null terminator)
        size_t len = wcslen(wcstr);
        return static_cast<int>(len + 1);
    }
    size_t converted = wcstombs(mbstr, wcstr, static_cast<size_t>(cb));
    if (converted == static_cast<size_t>(-1))
        return 0;
    return static_cast<int>(converted);
}

