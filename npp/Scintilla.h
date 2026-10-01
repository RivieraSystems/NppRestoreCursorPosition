// Minimal Scintilla declarations used by NppRestoreCursorPosition.
// Message numbers match Notepad++'s bundled scintilla/include/Scintilla.h.

#pragma once

#include <windows.h>
#include <cstdint>

// Official Sci_Position.h (Notepad++ 8.3+): Sci_PositionCR is intptr_t.
using Sci_Position = ptrdiff_t;
using Sci_PositionU = size_t;
using Sci_PositionCR = intptr_t;

struct SCNotification
{
	NMHDR nmhdr;
	Sci_Position position;
	int ch;
	int modifiers;
	int modificationType;
	const char* text;
	Sci_Position length;
	Sci_Position linesAdded;
	int message;
	Sci_Position wParam;
	Sci_Position lParam;
	Sci_Position line;
	int foldLevelNow;
	int foldLevelPrev;
	int margin;
	int listType;
	int x;
	int y;
	int token;
	Sci_Position annotationLinesAdded;
	int updated;
	int listCompletionMethod;
	int characterSource;
};

#define SCI_GETLENGTH 2006
#define SCI_GETCURRENTPOS 2008
#define SCI_GETFIRSTVISIBLELINE 2152
#define SCI_GETLINECOUNT 2154
#define SCI_GETMARGINLEFT 2156
#define SCI_GETMARGINRIGHT 2158
#define SCI_POINTXFROMPOSITION 2164
#define SCI_LINEFROMPOSITION 2166
#define SCI_SCROLLCARET 2169
#define SCI_VISIBLEFROMDOCLINE 2220
#define SCI_DOCLINEFROMVISIBLE 2221
#define SCI_GETMARGINWIDTHN 2243
#define SCI_GETMARGINS 2253
#define SCI_GETWRAPMODE 2269
#define SCI_LINESONSCREEN 2370
#define SCI_SETXOFFSET 2397
#define SCI_GETXOFFSET 2398
#define SCI_CHOOSECARETX 2399
#define SCI_POSITIONBEFORE 2417
#define SCI_POSITIONAFTER 2418
#define SCI_SETEMPTYSELECTION 2556
#define SCI_SETFIRSTVISIBLELINE 2613

#define SC_WRAP_NONE 0

#define SC_UPDATE_SELECTION 0x2
#define SC_UPDATE_V_SCROLL 0x4

#define SCN_UPDATEUI 2007
#define SCN_PAINTED 2013
