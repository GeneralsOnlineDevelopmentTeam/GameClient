/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
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

#pragma once

#include "WWLib/always.h"
#include "WWLib/Vector.h"
#include "WWLib/win.h"
#include "usp10_adapter.h"

// Lays out text with Uniscribe for strings that the per character glyph cache of FontCharsClass
// cannot draw correctly: scripts that join letters or read right-to-left, such as Arabic and
// Persian, characters outside of the Basic Multilingual Plane, such as emoji, and characters that
// the font does not have. Uniscribe takes missing glyphs from fallback fonts of the system.
//
// The text is split into paragraphs at line feeds. Each paragraph reads in the direction of most of
// its strong characters. Text in directional isolates, such as a chat message after a player name,
// keeps its own direction. Paragraphs are wrapped at soft break opportunities, and each line is then
// shaped and reordered on its own.
class ShapedTextClass
{
public:
	ShapedTextClass();
	~ShapedTextClass();

	// Returns whether the text needs shaping to be displayed correctly with the font that is
	// selected into the device context.
	static bool Needs_Shaping( HDC dc, const WCHAR *text );

	// Lays out the text with the font that is selected into the device context. The device
	// context is used again by Draw, so it must stay valid until this object is destroyed.
	// Lines are wrapped before they reach the wrap width if it is positive.
	bool Layout( HDC dc, const WCHAR *text, int wrap_width, int line_height );

	// Draws the lines in white into the bitmap that is selected into the device context of Layout.
	// Lines of right-to-left paragraphs are aligned to the right unless the text is centered.
	bool Draw( bool centered ) const;

	int Get_Width() const							{ return Width; }
	int Get_Height() const							{ return Lines.Count() * LineHeight; }
	int Get_Line_Height() const					{ return LineHeight; }

	// Returns the distance from the left edge of the text to a caret after the last character.
	int Get_End_Caret_X( bool centered ) const;

	// Returns whether the first paragraph reads right-to-left.
	bool Is_Right_To_Left() const					{ return Lines.Count() > 0 && Lines[0].RightToLeft; }

private:
	struct LineStruct
	{
		SCRIPT_STRING_ANALYSIS Analysis;	// Null for an empty line.
		int Offset;		// Number of reopened embeddings in front of the text of the line.
		int Length;
		int Width;
		bool RightToLeft;

		bool operator== (const LineStruct &src) const	{ return false; }
		bool operator!= (const LineStruct &src) const	{ return true; }
	};

	void Free();
	WCHAR *Allocate_Text( int length );
	bool Add_Line( const WCHAR *paragraph, int start, int length, bool right_to_left );
	bool Add_Paragraph( const WCHAR *text, int length, int wrap_width );
	int Get_Line_X( const LineStruct &line, bool centered ) const;

	ShapedTextClass( const ShapedTextClass & );
	ShapedTextClass &operator=( const ShapedTextClass & );

	DynamicVectorClass<LineStruct> Lines;
	DynamicVectorClass<WCHAR *> Texts;
	HDC DC;
	int Width;
	int LineHeight;
};
