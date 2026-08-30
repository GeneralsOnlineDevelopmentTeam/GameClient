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

#include <wchar.h>
#include <string.h>

// WCHAR
typedef wchar_t WCHAR;
typedef const WCHAR* LPCWSTR;
typedef WCHAR* LPWSTR;

#define _wcsicmp wcscasecmp
#define wcsicmp wcscasecmp

// MultiByteToWideChar
#define CP_ACP 0
#define CP_UTF8 65001
#define MultiByteToWideChar(cp, flags, mbstr, cb, wcstr, cch) mbstowcs(wcstr, mbstr, cch)

// WideCharToMultiByte implementation with proper NULL checking and bounds handling
static inline int WideCharToMultiByte(
    unsigned int cp,
    unsigned long flags,
    const wchar_t* wcstr,
    int cch,
    char* mbstr,
    int cb,
    const char* defchar,
    int* used)
{
    // Validate input pointer
    if (wcstr == NULL)
    {
        return 0;
    }

    // Calculate the source string length if cch is -1 (null-terminated string)
    size_t src_len;
    if (cch == -1)
    {
        src_len = wcslen(wcstr);
    }
    else if (cch == 0)
    {
        return 0;
    }
    else
    {
        src_len = (size_t)cch;
    }

    // If mbstr is NULL, we're querying the required buffer size
    if (mbstr == NULL)
    {
        // Calculate the required buffer size
        size_t required = wcstombs(NULL, wcstr, 0);
        if (required == (size_t)-1)
        {
            return 0;
        }
        return (int)(required + 1); // +1 for null terminator
    }

    // Validate destination buffer size
    if (cb <= 0)
    {
        return 0;
    }

    // Perform the conversion
    // Note: We need to ensure we don't overflow the destination buffer
    // wcstombs uses the buffer size as the max number of bytes to write
    size_t result = wcstombs(mbstr, wcstr, (size_t)cb);
    
    if (result == (size_t)-1)
    {
        // Conversion error
        return 0;
    }

    // Ensure null termination if there's space
    if (result < (size_t)cb)
    {
        mbstr[result] = '\0';
    }
    else if (cb > 0)
    {
        // Buffer was too small, ensure null termination
        mbstr[cb - 1] = '\0';
        result = cb - 1;
    }

    return (int)result;
}

