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

#include <cstring>

// WCHAR
typedef wchar_t WCHAR;
typedef const WCHAR* LPCWSTR;
typedef WCHAR* LPWSTR;

#define _wcsicmp wcscasecmp
#define wcsicmp wcscasecmp

// Safe wrapper for wcstombs that handles null pointers
inline size_t safe_wcstombs(char* mbstr, const wchar_t* wcstr, size_t count)
{
    // If source is null, return 0 and optionally set destination to empty string
    if (wcstr == nullptr)
    {
        if (mbstr != nullptr && count > 0)
        {
            mbstr[0] = '\0';
        }
        return 0;
    }
    
    // If destination is null but we're just querying size, that's valid
    if (mbstr == nullptr)
    {
        return 0;
    }
    
    // Perform the actual conversion
    return wcstombs(mbstr, wcstr, count);
}

// MultiByteToWideChar
#define CP_ACP 0
#define MultiByteToWideChar(cp, flags, mbstr, cb, wcstr, cch) mbstowcs(wcstr, mbstr, cch)
#define WideCharToMultiByte(cp, flags, wcstr, cch, mbstr, cb, defchar, used) safe_wcstombs(mbstr, wcstr, cb)

