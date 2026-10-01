#pragma once

#include <string_view>

// Menu and dialog strings. English is the default.
// Japanese is used only when Notepad++ itself is on japanese.xml.

enum class UiLang
{
	English,
	Japanese
};

struct UiText
{
	const wchar_t* pluginName;
	const wchar_t* saveCursor;
	const wchar_t* clearHistory;
	const wchar_t* clearHistoryQuestion;
};

UiLang uiLangFromNativeFileName(std::string_view fileName);
const UiText& uiText(UiLang lang);
