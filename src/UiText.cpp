#include "UiText.h"

namespace
{
	char asciiLower(char c)
	{
		if (c >= 'A' && c <= 'Z')
			return static_cast<char>(c - 'A' + 'a');
		return c;
	}

	std::string_view baseName(std::string_view path)
	{
		const auto slash = path.find_last_of("\\/");
		if (slash == std::string_view::npos)
			return path;
		return path.substr(slash + 1);
	}

	bool equalsIgnoreAsciiCase(std::string_view a, std::string_view b)
	{
		if (a.size() != b.size())
			return false;
		for (size_t i = 0; i < a.size(); ++i)
		{
			if (asciiLower(a[i]) != asciiLower(b[i]))
				return false;
		}
		return true;
	}

	constexpr UiText kEnglish{
		L"NppRestoreCursorPosition",
		L"Save cursor position",
		L"Clear history",
		L"Clear all saved cursor positions?"
	};

	constexpr UiText kJapanese{
		L"カーソル位置復元",
		L"カーソル位置を保存",
		L"履歴をクリア",
		L"保存されたカーソル位置をすべて削除しますか。"
	};
}

UiLang uiLangFromNativeFileName(std::string_view fileName)
{
	const std::string_view name = baseName(fileName);
	if (equalsIgnoreAsciiCase(name, "japanese.xml")
		|| equalsIgnoreAsciiCase(name, "japanese"))
	{
		return UiLang::Japanese;
	}
	return UiLang::English;
}

const UiText& uiText(UiLang lang)
{
	if (lang == UiLang::Japanese)
		return kJapanese;
	return kEnglish;
}
