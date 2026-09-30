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

#include "shapedtext.h"
#include "Usp10Loader.h"


////////////////////////////////////////////////////////////////////////////////////
//	Local functions
////////////////////////////////////////////////////////////////////////////////////

static inline bool Is_Surrogate (WCHAR ch)
{
	return ch >= 0xD800 && ch <= 0xDFFF;
}


// GetGlyphIndicesW is resolved at runtime, because the headers of VC6 do not declare it.
typedef DWORD (WINAPI *Get_Glyph_Indices_Function)(HDC dc, const WCHAR *text, int length, WORD *glyphs, DWORD flags);
enum { MARK_NONEXISTING_GLYPHS = 0x0001 };

static Get_Glyph_Indices_Function Get_Glyph_Indices ()
{
	static const Get_Glyph_Indices_Function function =
		(Get_Glyph_Indices_Function)::GetProcAddress (::GetModuleHandleA ("gdi32.dll"), "GetGlyphIndicesW");
	return function;
}


// Returns the bidirectional type of a character outside of the Basic Multilingual Plane. Emoji and
// other symbols are neutral, historic scripts of the right-to-left blocks are right-to-left.
static int Get_Supplementary_Direction (unsigned codepoint)
{
	if ((codepoint >= 0x10800 && codepoint <= 0x10FFF) || (codepoint >= 0x1E800 && codepoint <= 0x1EFFF)) {
		return C2_RIGHTTOLEFT;
	}
	if (codepoint >= 0x1F000 && codepoint <= 0x1FBFF) {
		return C2_OTHERNEUTRAL;
	}
	return C2_LEFTTORIGHT;
}


static inline bool Is_Isolate_Initiator (WCHAR ch)
{
	return ch >= 0x2066 && ch <= 0x2068;
}


static inline bool Is_Pop_Directional_Isolate (WCHAR ch)
{
	return ch == 0x2069;
}


// Returns the length of the text up to the isolate terminator that matches an isolate that has
// just been opened, or up to the end of the text.
static int Get_Isolate_Length (const WCHAR *text, int length)
{
	int depth = 0;
	for (int index = 0; index < length; ++index) {
		if (Is_Isolate_Initiator(text[index])) {
			++depth;
		} else if (Is_Pop_Directional_Isolate(text[index])) {
			if (depth == 0) {
				return index;
			}
			--depth;
		}
	}
	return length;
}


// Finds the reading direction of text from the direction of most of its strong characters, and
// from its first strong character when there are as many of each. With the first strong character
// alone, as in rules P2 and P3 of the Unicode Bidirectional Algorithm, a mostly Arabic chat message
// that starts with an English word reads in the wrong order. Characters inside of directional
// isolates count only if there are no strong characters outside of them, so that a line such as
// "[name] message" with an isolated Arabic name and message reads right-to-left as a whole. Text
// without strong characters reads left-to-right.
static bool Is_Right_To_Left_Text (const WCHAR *text, int length)
{
	// Counts of left-to-right and right-to-left characters, outside and inside of isolates
	int counts[2][2] = { { 0, 0 }, { 0, 0 } };
	int first_types[2] = { C2_NOTAPPLICABLE, C2_NOTAPPLICABLE };
	int isolate_depth = 0;
	for (int index = 0; index < length; ++index) {
		const WCHAR ch = text[index];
		if (Is_Isolate_Initiator(ch)) {
			++isolate_depth;
			continue;
		}
		if (Is_Pop_Directional_Isolate(ch)) {
			if (isolate_depth > 0) {
				--isolate_depth;
			}
			continue;
		}

		int type = C2_NOTAPPLICABLE;
		if (ch >= 0xD800 && ch <= 0xDBFF && index + 1 < length && text[index + 1] >= 0xDC00 && text[index + 1] <= 0xDFFF) {
			type = Get_Supplementary_Direction(0x10000 + ((ch - 0xD800) << 10) + (text[index + 1] - 0xDC00));
			++index;
		} else if (!Is_Surrogate(ch)) {
			WORD char_type = C2_NOTAPPLICABLE;
			if (::GetStringTypeW(CT_CTYPE2, &ch, 1, &char_type)) {
				type = char_type;
			}
		}

		if (type != C2_LEFTTORIGHT && type != C2_RIGHTTOLEFT) {
			continue;
		}
		const int inside = isolate_depth > 0 ? 1 : 0;
		++counts[inside][type == C2_RIGHTTOLEFT ? 1 : 0];
		if (first_types[inside] == C2_NOTAPPLICABLE) {
			first_types[inside] = type;
		}
	}

	const int scope = (counts[0][0] + counts[0][1] > 0) ? 0 : 1;
	if (counts[scope][0] != counts[scope][1]) {
		return counts[scope][1] > counts[scope][0];
	}
	return first_types[scope] == C2_RIGHTTOLEFT;
}


// Uniscribe does not know the directional isolates of Unicode 6.3 and would draw them as boxes.
// They are replaced by the directional embeddings of the same direction, which order the text in
// the same way for what isolates are used for here, such as a message after a player name.
static void Replace_Directional_Isolates (WCHAR *text, int length)
{
	for (int index = 0; index < length; ++index) {
		switch (text[index]) {
			case 0x2066:	// Left-to-right isolate
				text[index] = 0x202A;	// Left-to-right embedding
				break;
			case 0x2067:	// Right-to-left isolate
				text[index] = 0x202B;	// Right-to-left embedding
				break;
			case 0x2068:	// First strong isolate
			{
				const int isolate_length = Get_Isolate_Length(text + index + 1, length - index - 1);
				text[index] = Is_Right_To_Left_Text(text + index + 1, isolate_length) ? 0x202B : 0x202A;
				break;
			}
			case 0x2069:	// Pop directional isolate
				text[index] = 0x202C;	// Pop directional formatting
				break;
		}
	}
}


// Gets the directional embeddings and overrides that are still open at the end of the text.
static int Get_Open_Embeddings (const WCHAR *text, int length, WCHAR *embeddings, int max_count)
{
	int count = 0;
	int overflow = 0;
	for (int index = 0; index < length; ++index) {
		const WCHAR ch = text[index];
		if (ch >= 0x202A && ch <= 0x202E && ch != 0x202C) {
			if (count < max_count) {
				embeddings[count++] = ch;
			} else {
				++overflow;
			}
		} else if (ch == 0x202C) {
			if (overflow > 0) {
				--overflow;
			} else if (count > 0) {
				--count;
			}
		}
	}
	return count;
}


static SCRIPT_STRING_ANALYSIS Analyse_Text (HDC dc, const WCHAR *text, int length, bool right_to_left, DWORD flags)
{
	SCRIPT_CONTROL control;
	SCRIPT_STATE state;
	::memset(&control, 0, sizeof(control));
	::memset(&state, 0, sizeof(state));
	state.uBidiLevel = right_to_left ? 1 : 0;

	flags |= SSA_GLYPHS | SSA_FALLBACK | SSA_LINK;
	if (right_to_left) {
		flags |= SSA_RTL;
	}

	// The glyph buffer size is the one recommended by the documentation of ScriptStringAnalyse.
	SCRIPT_STRING_ANALYSIS analysis = nullptr;
	if (::ScriptStringAnalyse(dc, text, length, length * 3 / 2 + 16, -1, flags, 0, &control, &state,
		nullptr, nullptr, nullptr, &analysis) != S_OK)
	{
		return nullptr;
	}
	return analysis;
}


static int Get_Length_Without_Trailing_White_Space (const SCRIPT_LOGATTR *attributes, int start, int end)
{
	while (end > start && attributes[end - 1].fWhiteSpace) {
		--end;
	}
	return end - start;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	ShapedTextClass
//
////////////////////////////////////////////////////////////////////////////////////
ShapedTextClass::ShapedTextClass () :
	DC (nullptr),
	Width (0),
	LineHeight (0)
{
}


ShapedTextClass::~ShapedTextClass ()
{
	Free ();
}


void
ShapedTextClass::Free ()
{
	for (int index = 0; index < Lines.Count (); ++index) {
		if (Lines[index].Analysis != nullptr) {
			::ScriptStringFree (&Lines[index].Analysis);
		}
	}
	Lines.Delete_All ();

	for (int text_index = 0; text_index < Texts.Count (); ++text_index) {
		delete [] Texts[text_index];
	}
	Texts.Delete_All ();

	DC = nullptr;
	Width = 0;
	LineHeight = 0;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Allocate_Text
//
//	The analyses may refer to the text that they were made from, so it is kept
//	until they are freed.
//
////////////////////////////////////////////////////////////////////////////////////
WCHAR *
ShapedTextClass::Allocate_Text (int length)
{
	WCHAR *text = W3DNEWARRAY WCHAR[length + 1];
	text[length] = 0;
	Texts.Add (text);
	return text;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Needs_Shaping
//
////////////////////////////////////////////////////////////////////////////////////
bool
ShapedTextClass::Needs_Shaping (HDC dc, const WCHAR *text)
{
	if (text == nullptr || !Usp10Loader::isLoaded ()) {
		return false;
	}

	int length = 0;
	bool has_unicode = false;
	for (; text[length] != 0; ++length) {
		if (Is_Surrogate (text[length])) {
			return true;
		}
		if (text[length] >= 0x0100) {
			has_unicode = true;
		}
	}

	// Latin-1 is always drawn by the glyph cache.
	if (!has_unicode) {
		return false;
	}

	if (::ScriptIsComplex (text, length, SIC_COMPLEX) == S_OK) {
		return true;
	}

	// Characters without a glyph in the font would be drawn as boxes.
	const Get_Glyph_Indices_Function get_glyph_indices = Get_Glyph_Indices ();
	if (get_glyph_indices == nullptr) {
		return false;
	}

	WORD *glyphs = W3DNEWARRAY WORD[length];
	bool has_missing_glyph = false;
	if (get_glyph_indices (dc, text, length, glyphs, MARK_NONEXISTING_GLYPHS) != GDI_ERROR) {
		for (int index = 0; index < length; ++index) {
			if (text[index] >= 0x0100 && glyphs[index] == 0xFFFF) {
				has_missing_glyph = true;
				break;
			}
		}
	}
	delete [] glyphs;
	return has_missing_glyph;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Layout
//
////////////////////////////////////////////////////////////////////////////////////
bool
ShapedTextClass::Layout (HDC dc, const WCHAR *text, int wrap_width, int line_height)
{
	Free ();

	if (dc == nullptr || text == nullptr || line_height <= 0) {
		return false;
	}

	DC = dc;
	LineHeight = line_height;

	const WCHAR *paragraph = text;
	for (;;) {
		const WCHAR *paragraph_end = paragraph;
		while (*paragraph_end != 0 && *paragraph_end != L'\n') {
			++paragraph_end;
		}

		if (!Add_Paragraph (paragraph, (int)(paragraph_end - paragraph), wrap_width)) {
			Free ();
			return false;
		}

		if (*paragraph_end == 0) {
			break;
		}
		paragraph = paragraph_end + 1;
	}

	for (int index = 0; index < Lines.Count (); ++index) {
		Width = max (Width, Lines[index].Width);
	}

	if (Width <= 0) {
		Free ();
		return false;
	}
	return true;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Add_Line
//
////////////////////////////////////////////////////////////////////////////////////
bool
ShapedTextClass::Add_Line (const WCHAR *paragraph, int start, int length, bool right_to_left)
{
	LineStruct line;
	line.Analysis = nullptr;
	line.Offset = 0;
	line.Length = length;
	line.Width = 0;
	line.RightToLeft = right_to_left;

	if (length > 0) {
		//
		//	Embeddings that are open where a wrapped line starts are opened again on it
		//
		enum { MAX_OPEN_EMBEDDINGS = 16 };
		WCHAR open_embeddings[MAX_OPEN_EMBEDDINGS];
		line.Offset = Get_Open_Embeddings (paragraph, start, open_embeddings, MAX_OPEN_EMBEDDINGS);
		if (line.Offset == 0) {
			line.Analysis = Analyse_Text (DC, paragraph + start, length, right_to_left, 0);
		} else {
			WCHAR *text = Allocate_Text (line.Offset + length);
			::memcpy (text, open_embeddings, line.Offset * sizeof (WCHAR));
			::memcpy (text + line.Offset, paragraph + start, length * sizeof (WCHAR));
			line.Analysis = Analyse_Text (DC, text, line.Offset + length, right_to_left, 0);
		}

		const SIZE *size = line.Analysis != nullptr ? ::ScriptString_pSize (line.Analysis) : nullptr;
		if (size == nullptr) {
			if (line.Analysis != nullptr) {
				::ScriptStringFree (&line.Analysis);
			}
			return false;
		}
		line.Width = size->cx;
	}

	Lines.Add (line);
	return true;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Add_Paragraph
//
//	Breaks the paragraph at soft break opportunities so that no line reaches the wrap
//	width. A word that is too wide for a line of its own is broken between clusters.
//	White space at the end of a wrapped line is left out, so that it does not shift
//	the line away from the edge where its paragraph starts.
//
////////////////////////////////////////////////////////////////////////////////////
bool
ShapedTextClass::Add_Paragraph (const WCHAR *paragraph_text, int length, int wrap_width)
{
	const bool right_to_left = Is_Right_To_Left_Text (paragraph_text, length);

	WCHAR *text = Allocate_Text (length);
	::memcpy (text, paragraph_text, length * sizeof (WCHAR));
	Replace_Directional_Isolates (text, length);

	if (wrap_width <= 0 || length == 0) {
		return Add_Line (text, 0, length, right_to_left);
	}

	SCRIPT_STRING_ANALYSIS paragraph = Analyse_Text (DC, text, length, right_to_left, SSA_BREAK);
	const SIZE *size = paragraph != nullptr ? ::ScriptString_pSize (paragraph) : nullptr;
	const SCRIPT_LOGATTR *paragraph_attributes = paragraph != nullptr ? ::ScriptString_pLogAttr (paragraph) : nullptr;
	if (size == nullptr || paragraph_attributes == nullptr) {
		if (paragraph != nullptr) {
			::ScriptStringFree (&paragraph);
		}
		return false;
	}

	if (size->cx < wrap_width) {
		::ScriptStringFree (&paragraph);
		return Add_Line (text, 0, length, right_to_left);
	}

	// Copy what is needed, so that the analysis of the paragraph can be released before the
	// lines are shaped.
	int *widths = W3DNEWARRAY int[length];
	SCRIPT_LOGATTR *attributes = W3DNEWARRAY SCRIPT_LOGATTR[length];
	::memcpy (attributes, paragraph_attributes, length * sizeof (SCRIPT_LOGATTR));
	bool success = ::ScriptStringGetLogicalWidths (paragraph, widths) == S_OK;
	::ScriptStringFree (&paragraph);

	int line_start = 0;
	int line_width = 0;
	int break_position = 0;
	for (int index = 0; success && index < length; ++index) {
		if (index > line_start && attributes[index].fSoftBreak) {
			break_position = index;
		}

		while (success && index > line_start && !attributes[index].fWhiteSpace &&
			line_width + widths[index] >= wrap_width)
		{
			int line_end = break_position > line_start ? break_position : index;
			while (line_end > line_start + 1 && !attributes[line_end].fCharStop) {
				--line_end;
			}

			success = Add_Line (text, line_start,
				Get_Length_Without_Trailing_White_Space (attributes, line_start, line_end), right_to_left);

			line_start = line_end;
			line_width = 0;
			for (int width_index = line_start; width_index < index; ++width_index) {
				line_width += widths[width_index];
			}
		}

		line_width += widths[index];
	}

	if (success) {
		success = Add_Line (text, line_start,
			Get_Length_Without_Trailing_White_Space (attributes, line_start, length), right_to_left);
	}

	delete [] attributes;
	delete [] widths;
	return success;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Get_Line_X
//
////////////////////////////////////////////////////////////////////////////////////
int
ShapedTextClass::Get_Line_X (const LineStruct &line, bool centered) const
{
	const int free_width = Width - line.Width;
	if (centered) {
		return free_width / 2;
	}
	return line.RightToLeft ? free_width : 0;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Draw
//
////////////////////////////////////////////////////////////////////////////////////
bool
ShapedTextClass::Draw (bool centered) const
{
	if (DC == nullptr) {
		return false;
	}

	const COLORREF old_text_color = ::SetTextColor (DC, RGB (255, 255, 255));
	const int old_background_mode = ::SetBkMode (DC, TRANSPARENT);

	bool success = true;
	for (int index = 0; success && index < Lines.Count (); ++index) {
		const LineStruct &line = Lines[index];
		if (line.Analysis != nullptr) {
			success = ::ScriptStringOut (line.Analysis, Get_Line_X (line, centered), index * LineHeight,
				0, nullptr, 0, 0, FALSE) == S_OK;
		}
	}

	::SetBkMode (DC, old_background_mode);
	::SetTextColor (DC, old_text_color);

	// Complete the drawing before the caller reads the bitmap.
	return ::GdiFlush () != FALSE && success;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Get_End_Caret_X
//
////////////////////////////////////////////////////////////////////////////////////
int
ShapedTextClass::Get_End_Caret_X (bool centered) const
{
	if (Lines.Count () == 0) {
		return 0;
	}

	const LineStruct &line = Lines[Lines.Count () - 1];
	const int line_x = Get_Line_X (line, centered);
	int caret_x = line.RightToLeft ? line.Width : 0;
	if (line.Analysis != nullptr) {
		::ScriptStringCPtoX (line.Analysis, line.Offset + line.Length - 1, TRUE, &caret_x);
	}
	return line_x + caret_x;
}
