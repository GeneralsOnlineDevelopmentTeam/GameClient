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

// FILE: DisplayString.cpp ////////////////////////////////////////////////////
//-----------------------------------------------------------------------------
//
//                       Westwood Studios Pacific.
//
//                       Confidential Information
//                Copyright (C) 2001 - All Rights Reserved
//
//-----------------------------------------------------------------------------
//
// Project:    RTS3
//
// File name:  DisplayString.cpp
//
// Created:    Colin Day, July 2001
//
// Desc:       Contstuct for holding double byte game string data and being
//						 able to draw that text to the screen.
//
//-----------------------------------------------------------------------------
///////////////////////////////////////////////////////////////////////////////

// SYSTEM INCLUDES ////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

// USER INCLUDES //////////////////////////////////////////////////////////////
#include "Common/Debug.h"
#include "Common/Language.h"
#include "GameClient/DisplayString.h"

// DEFINES ////////////////////////////////////////////////////////////////////

// PRIVATE TYPES //////////////////////////////////////////////////////////////

// PRIVATE DATA ///////////////////////////////////////////////////////////////

// PUBLIC DATA ////////////////////////////////////////////////////////////////

// PRIVATE PROTOTYPES /////////////////////////////////////////////////////////

// PRIVATE FUNCTIONS //////////////////////////////////////////////////////////

// Hebrew, Arabic and the other right-to-left scripts of the Basic Multilingual Plane
static Bool isRightToLeftChar( WideChar ch )
{
	return (ch >= 0x0590 && ch <= 0x08FF) || (ch >= 0xFB1D && ch <= 0xFDFF) || (ch >= 0xFE70 && ch <= 0xFEFC);
}

///////////////////////////////////////////////////////////////////////////////
// PUBLIC FUNCTIONS ///////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

// IsolateBidiText ============================================================
/** Text without right-to-left characters is returned as it is, so that it is
	* still drawn by the glyph cache of the font. */
//=============================================================================
UnicodeString IsolateBidiText( const UnicodeString &text )
{
	const WideChar *str = text.str();
	for( Int i = 0; str[i] != 0; ++i )
	{
		if( isRightToLeftChar( str[i] ) )
		{
			UnicodeString isolated;
			isolated.concat( (WideChar)0x2068 ); // first strong isolate
			isolated.concat( text );
			isolated.concat( (WideChar)0x2069 ); // pop directional isolate
			return isolated;
		}
	}
	return text;
}

// IsolateBidiChatLine ========================================================
/** A leading group ends at the first closing bracket that a space follows */
//=============================================================================
UnicodeString IsolateBidiChatLine( const UnicodeString &line )
{
	UnicodeString result;
	const WideChar *str = line.str();
	while( str[0] == L'[' )
	{
		const WideChar *close = wcschr( str, L']' );
		if( close == nullptr || close[1] != L' ' )
			break;

		UnicodeString name;
		name.set( str + 1, (Int)(close - str - 1) );
		result.concat( L'[' );
		result.concat( IsolateBidiText( name ) );
		result.concat( L"] " );
		str = close + 2;
	}

	result.concat( IsolateBidiText( UnicodeString( str ) ) );
	return result;
}

// DisplayString::DisplayString ===============================================
/** */
//=============================================================================
DisplayString::DisplayString()
{
	// m_textString = "";	// not necessary, done by default
	m_font = nullptr;

	m_next = nullptr;
	m_prev = nullptr;

}

// DisplayString::~DisplayString ==============================================
/** */
//=============================================================================
DisplayString::~DisplayString()
{

	// free any data
	reset();

}

// DisplayString::setText =====================================================
/** Copy the text to this instance */
//=============================================================================
void DisplayString::setText( UnicodeString text )
{
	if (text == m_textString)
		return;

	m_textString = text;

	// our text has now changed
	notifyTextChanged();

}

// DisplayString::reset =======================================================
/** Free and reset all the data for this string, effectively making this
	* instance like brand new */
//=============================================================================
void DisplayString::reset()
{

	m_textString.clear();

	// no font
	m_font = nullptr;

}

// DisplayString::removeLastChar ==============================================
/** Remove the last character from the string text */
//=============================================================================
void DisplayString::removeLastChar()
{
	m_textString.removeLastChar();

	// our text has now changed
	notifyTextChanged();

}

// DisplayString::truncateBy ==================================================
/** Remove the last charCount characters from the string text */
//=============================================================================
void DisplayString::truncateBy( const Int charCount )
{
	m_textString.truncateBy(charCount);

	// our text has now changed
	notifyTextChanged();

}

// DisplayString::truncateTo ==================================================
/** Remove the last characters from the string text so it's at the most
	* maxLength characters long */
//=============================================================================
void DisplayString::truncateTo( const Int maxLength )
{
	m_textString.truncateTo(maxLength);

	// our text has now changed
	notifyTextChanged();

}

// DisplayString::appendChar ==================================================
/** Append character to the end of the string */
//=============================================================================
void DisplayString::appendChar( WideChar c )
{
	m_textString.concat(c);

	// text has now changed
	notifyTextChanged();

}

