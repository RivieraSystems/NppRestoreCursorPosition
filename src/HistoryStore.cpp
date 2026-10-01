#include "HistoryStore.h"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <unordered_set>
#include <windows.h>

namespace
{
	constexpr char kIso8601[] = "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ";
	constexpr int kMaxJsonDepth = 64;

	std::string nowUtcIso8601()
	{
		SYSTEMTIME st{};
		::GetSystemTime(&st);
		char buf[32] = {};
		std::snprintf(buf, sizeof(buf), kIso8601,
			static_cast<unsigned>(st.wYear),
			static_cast<unsigned>(st.wMonth),
			static_cast<unsigned>(st.wDay),
			static_cast<unsigned>(st.wHour),
			static_cast<unsigned>(st.wMinute),
			static_cast<unsigned>(st.wSecond),
			static_cast<unsigned>(st.wMilliseconds));
		return buf;
	}

	void skipWs(std::string_view s, size_t& i)
	{
		while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
			++i;
	}

	bool hexDigit(char c, int& out)
	{
		if (c >= '0' && c <= '9') { out = c - '0'; return true; }
		if (c >= 'a' && c <= 'f') { out = c - 'a' + 10; return true; }
		if (c >= 'A' && c <= 'F') { out = c - 'A' + 10; return true; }
		return false;
	}

	void appendUtf8(std::string& out, char32_t cp)
	{
		if (cp <= 0x7F)
		{
			out.push_back(static_cast<char>(cp));
		}
		else if (cp <= 0x7FF)
		{
			out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		}
		else if (cp <= 0xFFFF)
		{
			out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		}
		else
		{
			out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		}
	}

	bool parseHex4(std::string_view s, size_t i, char32_t& out)
	{
		if (i + 4 > s.size())
			return false;
		int d0 = 0, d1 = 0, d2 = 0, d3 = 0;
		if (!hexDigit(s[i], d0) || !hexDigit(s[i + 1], d1)
			|| !hexDigit(s[i + 2], d2) || !hexDigit(s[i + 3], d3))
		{
			return false;
		}
		out = static_cast<char32_t>((d0 << 12) | (d1 << 8) | (d2 << 4) | d3);
		return true;
	}

	bool parseJsonString(std::string_view s, size_t& i, std::string& out, std::string& error)
	{
		if (i >= s.size() || s[i] != '"')
		{
			error = "expected string";
			return false;
		}
		++i;
		out.clear();
		while (i < s.size())
		{
			const unsigned char c = static_cast<unsigned char>(s[i]);
			++i;
			if (c == '"')
				return true;
			if (c == '\\')
			{
				if (i >= s.size())
				{
					error = "unterminated escape";
					return false;
				}
				const char e = s[i++];
				switch (e)
				{
				case '"': case '\\': case '/': out.push_back(e); break;
				case 'b': out.push_back('\b'); break;
				case 'f': out.push_back('\f'); break;
				case 'n': out.push_back('\n'); break;
				case 'r': out.push_back('\r'); break;
				case 't': out.push_back('\t'); break;
				case 'u':
				{
					char32_t cp = 0;
					if (!parseHex4(s, i, cp))
					{
						error = "bad unicode escape";
						return false;
					}
					i += 4;
					if (cp >= 0xD800 && cp <= 0xDBFF)
					{
						char32_t low = 0;
						if (i + 2 <= s.size() && s[i] == '\\' && s[i + 1] == 'u'
							&& parseHex4(s, i + 2, low) && low >= 0xDC00 && low <= 0xDFFF)
						{
							i += 6;
							cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
						}
						else
						{
							cp = 0xFFFD;
						}
					}
					else if (cp >= 0xDC00 && cp <= 0xDFFF)
					{
						cp = 0xFFFD;
					}
					appendUtf8(out, cp);
					break;
				}
				default:
					error = "unknown escape";
					return false;
				}
				continue;
			}
			if (c < 0x20)
			{
				error = "raw control in string";
				return false;
			}
			out.push_back(static_cast<char>(c));
		}
		error = "unterminated string";
		return false;
	}

	bool parseJsonNumber(std::string_view s, size_t& i, long long& out, std::string& error)
	{
		if (i >= s.size())
		{
			error = "expected number";
			return false;
		}
		const size_t begin = i;
		if (s[i] == '-')
			++i;
		if (i >= s.size() || s[i] < '0' || s[i] > '9')
		{
			error = "expected number";
			return false;
		}
		while (i < s.size() && s[i] >= '0' && s[i] <= '9')
			++i;
		// Reject fractions / exponents: positions are integers.
		if (i < s.size() && (s[i] == '.' || s[i] == 'e' || s[i] == 'E'))
		{
			error = "positions must be integers";
			return false;
		}
		try
		{
			out = std::stoll(std::string(s.substr(begin, i - begin)));
		}
		catch (...)
		{
			error = "number out of range";
			return false;
		}
		return true;
	}

	bool parseJsonLiteral(std::string_view s, size_t& i, std::string_view lit)
	{
		if (s.size() - i < lit.size())
			return false;
		if (s.substr(i, lit.size()) != lit)
			return false;
		i += lit.size();
		return true;
	}

	bool skipDigits(std::string_view s, size_t& i)
	{
		const size_t begin = i;
		while (i < s.size() && s[i] >= '0' && s[i] <= '9')
			++i;
		return i > begin;
	}

	// Any JSON number, of any size: values this plugin does not read are only skipped.
	bool skipJsonNumber(std::string_view s, size_t& i, std::string& error)
	{
		if (i < s.size() && s[i] == '-')
			++i;
		bool ok = skipDigits(s, i);
		if (ok && i < s.size() && s[i] == '.')
		{
			++i;
			ok = skipDigits(s, i);
		}
		if (ok && i < s.size() && (s[i] == 'e' || s[i] == 'E'))
		{
			++i;
			if (i < s.size() && (s[i] == '+' || s[i] == '-'))
				++i;
			ok = skipDigits(s, i);
		}
		if (!ok)
			error = "bad number";
		return ok;
	}

	bool skipJsonValue(std::string_view s, size_t& i, std::string& error, int depth = 0);

	bool skipJsonObject(std::string_view s, size_t& i, std::string& error, int depth)
	{
		if (i >= s.size() || s[i] != '{')
		{
			error = "expected object";
			return false;
		}
		++i;
		skipWs(s, i);
		if (i < s.size() && s[i] == '}')
		{
			++i;
			return true;
		}
		for (;;)
		{
			skipWs(s, i);
			std::string key;
			if (!parseJsonString(s, i, key, error))
				return false;
			skipWs(s, i);
			if (i >= s.size() || s[i] != ':')
			{
				error = "expected colon";
				return false;
			}
			++i;
			if (!skipJsonValue(s, i, error, depth + 1))
				return false;
			skipWs(s, i);
			if (i < s.size() && s[i] == ',')
			{
				++i;
				continue;
			}
			if (i < s.size() && s[i] == '}')
			{
				++i;
				return true;
			}
			error = "expected comma or end of object";
			return false;
		}
	}

	bool skipJsonArray(std::string_view s, size_t& i, std::string& error, int depth)
	{
		if (i >= s.size() || s[i] != '[')
		{
			error = "expected array";
			return false;
		}
		++i;
		skipWs(s, i);
		if (i < s.size() && s[i] == ']')
		{
			++i;
			return true;
		}
		for (;;)
		{
			if (!skipJsonValue(s, i, error, depth + 1))
				return false;
			skipWs(s, i);
			if (i < s.size() && s[i] == ',')
			{
				++i;
				continue;
			}
			if (i < s.size() && s[i] == ']')
			{
				++i;
				return true;
			}
			error = "expected comma or end of array";
			return false;
		}
	}

	bool skipJsonValue(std::string_view s, size_t& i, std::string& error, int depth)
	{
		skipWs(s, i);
		if (i >= s.size())
		{
			error = "unexpected end";
			return false;
		}
		const char c = s[i];
		if (c == '"')
		{
			std::string tmp;
			return parseJsonString(s, i, tmp, error);
		}
		if ((c == '{' || c == '[') && depth >= kMaxJsonDepth)
		{
			error = "nesting too deep";
			return false;
		}
		if (c == '{')
			return skipJsonObject(s, i, error, depth);
		if (c == '[')
			return skipJsonArray(s, i, error, depth);
		if (c == '-' || (c >= '0' && c <= '9'))
			return skipJsonNumber(s, i, error);
		if (parseJsonLiteral(s, i, "true") || parseJsonLiteral(s, i, "false") || parseJsonLiteral(s, i, "null"))
			return true;
		error = "unexpected value";
		return false;
	}

	void appendEscaped(std::string& out, std::string_view utf8)
	{
		out.push_back('"');
		for (unsigned char c : utf8)
		{
			switch (c)
			{
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\b': out += "\\b"; break;
			case '\f': out += "\\f"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (c < 0x20)
				{
					char buf[8];
					std::snprintf(buf, sizeof(buf), "\\u%04x", c);
					out += buf;
				}
				else
				{
					out.push_back(static_cast<char>(c));
				}
				break;
			}
		}
		out.push_back('"');
	}

	std::string_view stripBom(std::string_view json)
	{
		if (json.size() >= 3
			&& static_cast<unsigned char>(json[0]) == 0xEF
			&& static_cast<unsigned char>(json[1]) == 0xBB
			&& static_cast<unsigned char>(json[2]) == 0xBF)
		{
			return json.substr(3);
		}
		return json;
	}

	enum class ReadResult
	{
		Ok,
		NotFound,
		TooLarge,
		Failed // locked or unreadable; the file may still hold good data
	};

	ReadResult readWholeFile(const std::wstring& path, std::string& bytes)
	{
		bytes.clear();
		const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
		{
			const DWORD err = ::GetLastError();
			return (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
				? ReadResult::NotFound : ReadResult::Failed;
		}
		LARGE_INTEGER size{};
		if (!::GetFileSizeEx(h, &size) || size.QuadPart < 0)
		{
			::CloseHandle(h);
			return ReadResult::Failed;
		}
		if (size.QuadPart > 32 * 1024 * 1024)
		{
			::CloseHandle(h);
			return ReadResult::TooLarge;
		}
		bytes.assign(static_cast<size_t>(size.QuadPart), '\0');
		DWORD read = 0;
		const BOOL ok = bytes.empty()
			|| ::ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
		::CloseHandle(h);
		if (!ok || read != bytes.size())
		{
			bytes.clear();
			return ReadResult::Failed;
		}
		return ReadResult::Ok;
	}

	bool writeFileAtomic(const std::wstring& dest, const std::string& bytes)
	{
		const std::wstring tmp = dest + L"." + std::to_wstring(::GetCurrentProcessId()) + L".tmp";
		const HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return false;
		DWORD written = 0;
		const BOOL ok = bytes.empty()
			|| ::WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
		::FlushFileBuffers(h);
		::CloseHandle(h);
		if (!ok || written != bytes.size())
		{
			::DeleteFileW(tmp.c_str());
			return false;
		}
		if (!::MoveFileExW(tmp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			::DeleteFileW(tmp.c_str());
			return false;
		}
		return true;
	}

	bool ensureDirectory(const std::wstring& path)
	{
		if (path.empty())
			return false;
		if (path.size() == 2 && path[1] == L':')
			return true;
		if (path.size() == 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/'))
			return true;

		const DWORD attr = ::GetFileAttributesW(path.c_str());
		if (attr != INVALID_FILE_ATTRIBUTES)
			return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;

		const auto slash = path.find_last_of(L"\\/");
		if (slash != std::wstring::npos)
		{
			if (!ensureDirectory(path.substr(0, slash)))
				return false;
		}
		return ::CreateDirectoryW(path.c_str(), nullptr) || ::GetLastError() == ERROR_ALREADY_EXISTS;
	}

	bool parseFileRecordObject(std::string_view s, size_t& i, FileRecord& rec, std::string& error)
	{
		skipWs(s, i);
		if (i >= s.size() || s[i] != '{')
		{
			error = "expected file object";
			return false;
		}
		++i;
		skipWs(s, i);
		if (i < s.size() && s[i] == '}')
		{
			++i;
			return true;
		}
		for (;;)
		{
			skipWs(s, i);
			std::string key;
			if (!parseJsonString(s, i, key, error))
				return false;
			skipWs(s, i);
			if (i >= s.size() || s[i] != ':')
			{
				error = "expected colon";
				return false;
			}
			++i;
			skipWs(s, i);
			if (key == "pos" || key == "firstVisibleLine")
			{
				long long n = 0;
				if (!parseJsonNumber(s, i, n, error))
					return false;
				if (key == "pos")
					rec.pos = static_cast<intptr_t>(n);
				else
					rec.firstVisibleLine = static_cast<intptr_t>(n);
			}
			else if (key == "updatedAt")
			{
				if (!parseJsonString(s, i, rec.updatedAt, error))
					return false;
			}
			else if (!skipJsonValue(s, i, error))
			{
				return false;
			}
			skipWs(s, i);
			if (i < s.size() && s[i] == ',')
			{
				++i;
				continue;
			}
			if (i < s.size() && s[i] == '}')
			{
				++i;
				return true;
			}
			error = "expected comma or end of file object";
			return false;
		}
	}

	bool parseFilesObject(std::string_view s, size_t& i, std::vector<FileRecord>& out, std::string& error)
	{
		skipWs(s, i);
		if (i >= s.size() || s[i] != '{')
		{
			error = "expected files object";
			return false;
		}
		++i;
		skipWs(s, i);
		if (i < s.size() && s[i] == '}')
		{
			++i;
			return true;
		}
		std::unordered_map<std::wstring, size_t> seen;
		for (;;)
		{
			skipWs(s, i);
			std::string keyUtf8;
			if (!parseJsonString(s, i, keyUtf8, error))
				return false;
			skipWs(s, i);
			if (i >= s.size() || s[i] != ':')
			{
				error = "expected colon";
				return false;
			}
			++i;
			FileRecord rec;
			rec.path = utf8ToWide(keyUtf8);
			if (!parseFileRecordObject(s, i, rec, error))
				return false;
			if (!rec.path.empty())
			{
				const auto [it, inserted] = seen.emplace(pathKey(rec.path), out.size());
				if (inserted)
					out.push_back(std::move(rec));
				else
					out[it->second] = std::move(rec);
			}
			skipWs(s, i);
			if (i < s.size() && s[i] == ',')
			{
				++i;
				continue;
			}
			if (i < s.size() && s[i] == '}')
			{
				++i;
				return true;
			}
			error = "expected comma or end of files object";
			return false;
		}
	}

	// Held across read, replay and replace of history.json. A file opened with no
	// sharing works across processes and sessions and is released if the owner dies.
	class HistoryLock
	{
	public:
		HistoryLock(const std::wstring& path, ULONGLONG timeoutMs)
		{
			const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
			for (;;)
			{
				handle_ = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
					OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (handle_ != INVALID_HANDLE_VALUE)
					return;
				const DWORD err = ::GetLastError();
				if (err != ERROR_SHARING_VIOLATION && err != ERROR_LOCK_VIOLATION && err != ERROR_ACCESS_DENIED)
					return;
				if (::GetTickCount64() >= deadline)
					return;
				::Sleep(10);
			}
		}
		~HistoryLock()
		{
			if (handle_ != INVALID_HANDLE_VALUE)
				::CloseHandle(handle_);
		}
		HistoryLock(const HistoryLock&) = delete;
		HistoryLock& operator=(const HistoryLock&) = delete;
		bool held() const { return handle_ != INVALID_HANDLE_VALUE; }

	private:
		HANDLE handle_ = INVALID_HANDLE_VALUE;
	};

	constexpr ULONGLONG kLockTimeoutMs = 1500;

	void decodeXmlEntities(std::string_view raw, std::string& out)
	{
		out.clear();
		for (size_t i = 0; i < raw.size();)
		{
			if (raw[i] != '&')
			{
				out.push_back(raw[i++]);
				continue;
			}
			const size_t semi = raw.find(';', i);
			if (semi == std::string_view::npos || semi - i > 10)
			{
				out.push_back(raw[i++]);
				continue;
			}
			const std::string_view name = raw.substr(i + 1, semi - i - 1);
			char32_t cp = 0;
			bool known = true;
			if (name == "amp") cp = '&';
			else if (name == "lt") cp = '<';
			else if (name == "gt") cp = '>';
			else if (name == "quot") cp = '"';
			else if (name == "apos") cp = '\'';
			else if (name.size() > 1 && name[0] == '#')
			{
				const bool hex = name[1] == 'x' || name[1] == 'X';
				unsigned long value = 0;
				for (size_t k = hex ? 2 : 1; k < name.size() && known; ++k)
				{
					int digit = 0;
					if (hex ? hexDigit(name[k], digit) : (name[k] >= '0' && name[k] <= '9'))
					{
						if (!hex)
							digit = name[k] - '0';
						value = value * (hex ? 16 : 10) + static_cast<unsigned long>(digit);
						if (value > 0x10FFFF)
							known = false;
					}
					else
					{
						known = false;
					}
				}
				if (name.size() == (hex ? 2u : 1u) || (value >= 0xD800 && value <= 0xDFFF))
					known = false;
				cp = static_cast<char32_t>(value);
			}
			else
			{
				known = false;
			}
			if (!known)
			{
				out.push_back(raw[i++]);
				continue;
			}
			appendUtf8(out, cp);
			i = semi + 1;
		}
	}
}

bool looksLikeFilesystemPath(std::wstring_view path)
{
	if (path.empty())
		return false;
	return path.find(L'\\') != std::wstring_view::npos
		|| path.find(L'/') != std::wstring_view::npos;
}

std::wstring pathKey(std::wstring_view path)
{
	if (path.empty())
		return {};
	std::wstring key(path);
	const int n = ::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE,
		path.data(), static_cast<int>(path.size()), key.data(), static_cast<int>(key.size()),
		nullptr, nullptr, 0);
	if (n != static_cast<int>(key.size()))
		return std::wstring(path);
	return key;
}

bool pathsEqual(const std::wstring& a, const std::wstring& b)
{
	return pathKey(a) == pathKey(b);
}

intptr_t clampPos(intptr_t pos, intptr_t length)
{
	if (length <= 0)
		return 0;
	if (pos < 0)
		return 0;
	if (pos > length)
		return length;
	return pos;
}

intptr_t clampDocLine(intptr_t line, intptr_t lineCount)
{
	if (lineCount <= 0)
		return 0;
	if (line < 0)
		return 0;
	if (line >= lineCount)
		return lineCount - 1;
	return line;
}

std::string wideToUtf8(std::wstring_view wide)
{
	if (wide.empty())
		return {};
	const int n = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
		nullptr, 0, nullptr, nullptr);
	if (n <= 0)
		return {};
	std::string out(static_cast<size_t>(n), '\0');
	::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
		out.data(), n, nullptr, nullptr);
	return out;
}

std::wstring utf8ToWide(std::string_view utf8)
{
	if (utf8.empty())
		return {};
	const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
	if (n <= 0)
		return {};
	std::wstring out(static_cast<size_t>(n), L'\0');
	::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
	return out;
}

std::string serializeHistoryJson(const HistoryData& data)
{
	std::string out;
	out += "{\n  \"version\": 1,\n  \"files\": {";
	bool first = true;
	for (const auto& f : data.files)
	{
		if (f.path.empty())
			continue;
		if (!first)
			out += ',';
		first = false;
		out += "\n    ";
		appendEscaped(out, wideToUtf8(f.path));
		out += ": {\n      \"pos\": ";
		out += std::to_string(static_cast<long long>(f.pos));
		out += ",\n      \"firstVisibleLine\": ";
		out += std::to_string(static_cast<long long>(f.firstVisibleLine));
		out += ",\n      \"updatedAt\": ";
		appendEscaped(out, f.updatedAt);
		out += "\n    }";
	}
	if (!first)
		out += "\n  ";
	out += '}';

	out += "\n}\n";
	return out;
}

namespace
{
	// Only the version is read here, so that a newer layout that this parser
	// would reject is still recognised as newer rather than as corrupt. The
	// scan stops at the first version that is not the integer 1, whatever follows.
	bool readRootVersion(std::string_view json, bool& unsupported, std::string& error)
	{
		unsupported = false;
		size_t i = 0;
		skipWs(json, i);
		if (i >= json.size() || json[i] != '{')
		{
			error = "expected root object";
			return false;
		}
		++i;
		skipWs(json, i);
		if (i < json.size() && json[i] == '}')
			return true;
		for (;;)
		{
			skipWs(json, i);
			std::string key;
			if (!parseJsonString(json, i, key, error))
				return false;
			skipWs(json, i);
			if (i >= json.size() || json[i] != ':')
			{
				error = "expected colon";
				return false;
			}
			++i;
			skipWs(json, i);
			const size_t valueBegin = i;
			if (!skipJsonValue(json, i, error))
				return false;
			if (key == "version" && json.substr(valueBegin, i - valueBegin) != "1")
			{
				unsupported = true;
				return true;
			}
			skipWs(json, i);
			if (i < json.size() && json[i] == ',')
			{
				++i;
				continue;
			}
			if (i < json.size() && json[i] == '}')
				return true;
			error = "expected comma or end of root";
			return false;
		}
	}
}

HistoryParse parseHistoryJson(std::string_view json, HistoryData& out, std::string& error)
{
	out = HistoryData{};
	error.clear();
	json = stripBom(json);

	bool unsupported = false;
	if (!readRootVersion(json, unsupported, error))
		return HistoryParse::Invalid;
	if (unsupported)
	{
		error = "unsupported version";
		return HistoryParse::UnsupportedVersion;
	}

	size_t i = 0;
	skipWs(json, i);
	++i; // '{', checked above
	skipWs(json, i);
	bool sawFiles = false;
	// An empty root goes through the same checks as any other.
	const auto endOfRoot = [&]()
	{
		++i;
		skipWs(json, i);
		if (i != json.size())
		{
			error = "trailing data";
			return HistoryParse::Invalid;
		}
		if (!sawFiles)
		{
			error = "missing files object";
			return HistoryParse::Invalid;
		}
		return HistoryParse::Ok;
	};
	if (i < json.size() && json[i] == '}')
		return endOfRoot();
	for (;;)
	{
		skipWs(json, i);
		std::string key;
		if (!parseJsonString(json, i, key, error))
			return HistoryParse::Invalid;
		skipWs(json, i);
		if (i >= json.size() || json[i] != ':')
		{
			error = "expected colon";
			return HistoryParse::Invalid;
		}
		++i;
		skipWs(json, i);
		if (key == "files")
		{
			if (!parseFilesObject(json, i, out.files, error))
				return HistoryParse::Invalid;
			sawFiles = true;
		}
		else if (!skipJsonValue(json, i, error))
		{
			return HistoryParse::Invalid;
		}
		skipWs(json, i);
		if (i < json.size() && json[i] == ',')
		{
			++i;
			continue;
		}
		if (i < json.size() && json[i] == '}')
			return endOfRoot();
		error = "expected comma or end of root";
		return HistoryParse::Invalid;
	}
}

std::vector<std::wstring> parseSessionFileNames(std::string_view xml)
{
	std::vector<std::wstring> names;
	constexpr std::string_view kTag = "<File";
	constexpr std::string_view kAttr = "filename=\"";
	std::string decoded;
	for (size_t at = xml.find(kTag); at != std::string_view::npos; at = xml.find(kTag, at + kTag.size()))
	{
		const size_t afterTag = at + kTag.size();
		if (afterTag >= xml.size() || (xml[afterTag] != ' ' && xml[afterTag] != '\t'
			&& xml[afterTag] != '\r' && xml[afterTag] != '\n'))
		{
			continue;
		}
		const size_t end = xml.find('>', afterTag);
		if (end == std::string_view::npos)
			break;
		const std::string_view tag = xml.substr(afterTag, end - afterTag);
		// Paths cannot contain '"', '<' or '>', so a plain scan of the tag is enough.
		size_t attr = std::string_view::npos;
		for (size_t p = tag.find(kAttr); p != std::string_view::npos; p = tag.find(kAttr, p + 1))
		{
			if (p > 0 && (tag[p - 1] == ' ' || tag[p - 1] == '\t' || tag[p - 1] == '\r' || tag[p - 1] == '\n'))
			{
				attr = p;
				break;
			}
		}
		if (attr == std::string_view::npos)
			continue;
		const size_t valueBegin = attr + kAttr.size();
		const size_t valueEnd = tag.find('"', valueBegin);
		if (valueEnd == std::string_view::npos)
			continue;
		decodeXmlEntities(tag.substr(valueBegin, valueEnd - valueBegin), decoded);
		std::wstring name = utf8ToWide(decoded);
		if (!name.empty())
			names.push_back(std::move(name));
	}
	return names;
}

std::vector<std::wstring> readSessionFileNames(const std::wstring& sessionXmlPath)
{
	std::string bytes;
	if (readWholeFile(sessionXmlPath, bytes) != ReadResult::Ok)
		return {};
	return parseSessionFileNames(stripBom(bytes));
}

bool commandLineHasFlag(std::wstring_view commandLine, std::wstring_view flag)
{
	std::wstring token;
	bool quoted = false;
	bool inToken = false;
	for (size_t i = 0; i <= commandLine.size(); ++i)
	{
		const wchar_t c = i < commandLine.size() ? commandLine[i] : L' ';
		if (c == L'"')
		{
			quoted = !quoted;
			inToken = true;
			continue;
		}
		if (!quoted && (c == L' ' || c == L'\t'))
		{
			if (inToken && token == flag)
				return true;
			token.clear();
			inToken = false;
			continue;
		}
		token.push_back(c);
		inToken = true;
	}
	return false;
}

bool startupSessionWasLoaded(const std::vector<std::wstring>& sessionFiles,
	const std::unordered_set<std::wstring>& openedBeforeReady,
	const std::function<bool(const std::wstring&)>& existsOnDisk)
{
	bool anyOpened = false;
	for (const auto& name : sessionFiles)
	{
		if (openedBeforeReady.count(pathKey(name)))
		{
			anyOpened = true;
			continue;
		}
		if (looksLikeFilesystemPath(name) && existsOnDisk(name))
			return false;
	}
	return anyOpened;
}

std::string serializeSettingsJson(const PluginSettings& settings)
{
	std::string out = "{\n  \"enabled\": ";
	out += settings.enabled ? "true" : "false";
	out += "\n}\n";
	return out;
}

bool parseSettingsJson(std::string_view json, PluginSettings& out, std::string& error)
{
	out = PluginSettings{};
	error.clear();
	json = stripBom(json);
	size_t i = 0;
	skipWs(json, i);
	if (i >= json.size() || json[i] != '{')
	{
		error = "expected object";
		return false;
	}
	++i;
	skipWs(json, i);
	if (i < json.size() && json[i] == '}')
	{
		++i;
		return true;
	}
	for (;;)
	{
		skipWs(json, i);
		std::string key;
		if (!parseJsonString(json, i, key, error))
			return false;
		skipWs(json, i);
		if (i >= json.size() || json[i] != ':')
		{
			error = "expected colon";
			return false;
		}
		++i;
		skipWs(json, i);
		if (key == "enabled")
		{
			if (parseJsonLiteral(json, i, "true"))
				out.enabled = true;
			else if (parseJsonLiteral(json, i, "false"))
				out.enabled = false;
			else
			{
				error = "enabled must be boolean";
				return false;
			}
		}
		else if (!skipJsonValue(json, i, error))
		{
			return false;
		}
		skipWs(json, i);
		if (i < json.size() && json[i] == ',')
		{
			++i;
			continue;
		}
		if (i < json.size() && json[i] == '}')
		{
			++i;
			return true;
		}
		error = "expected comma or end of settings";
		return false;
	}
}

void pruneOldest(std::vector<FileRecord>& files, size_t maxFiles)
{
	if (files.size() <= maxFiles)
		return;
	std::stable_sort(files.begin(), files.end(),
		[](const FileRecord& a, const FileRecord& b)
		{
			return a.updatedAt < b.updatedAt;
		});
	files.erase(files.begin(), files.begin() + static_cast<std::ptrdiff_t>(files.size() - maxFiles));
}

bool renameRecord(std::vector<FileRecord>& files, const std::wstring& oldPath, const std::wstring& newPath)
{
	if (oldPath.empty() || newPath.empty() || pathsEqual(oldPath, newPath))
		return false;
	FileRecord* src = nullptr;
	FileRecord* dest = nullptr;
	for (auto& f : files)
	{
		if (pathsEqual(f.path, oldPath))
			src = &f;
		else if (pathsEqual(f.path, newPath))
			dest = &f;
	}
	if (!src)
		return false;
	if (dest)
	{
		*dest = *src;
		dest->path = newPath;
		files.erase(files.begin() + (src - files.data()));
	}
	else
	{
		src->path = newPath;
	}
	return true;
}

void applyPending(std::vector<FileRecord>& files, const PendingChanges& pending)
{
	if (pending.cleared)
		files.clear();

	std::unordered_map<std::wstring, size_t> index;
	std::string newest;
	for (size_t i = 0; i < files.size(); ++i)
	{
		index.emplace(pathKey(files[i].path), i);
		if (files[i].updatedAt > newest)
			newest = files[i].updatedAt;
	}

	// Moves read the records as they were on disk, before any is replaced.
	std::unordered_set<std::wstring> replaced;
	std::vector<FileRecord> added;
	for (const auto& [key, change] : pending.byKey)
	{
		if (change.kind == PendingChange::Kind::MoveFrom)
		{
			const auto src = index.find(change.fromKey);
			if (src == index.end())
				continue;
			FileRecord moved = files[src->second];
			moved.path = change.record.path;
			moved.updatedAt = change.record.updatedAt;
			added.push_back(std::move(moved));
		}
		else if (change.kind == PendingChange::Kind::Put)
		{
			added.push_back(change.record);
		}
		replaced.insert(key);
	}

	std::vector<FileRecord> result;
	result.reserve(files.size() + added.size());
	for (auto& f : files)
	{
		if (!replaced.count(pathKey(f.path)))
			result.push_back(std::move(f));
	}
	for (auto& r : added)
	{
		if (r.updatedAt < newest)
			r.updatedAt = newest;
		result.push_back(std::move(r));
	}
	files = std::move(result);
}

void HistoryStore::setDirectory(std::wstring configDir)
{
	configDir_ = std::move(configDir);
}

bool HistoryStore::load()
{
	data_ = HistoryData{};
	pending_ = PendingChanges{};
	settings_ = PluginSettings{};
	dirty_ = false;
	index_.clear();
	if (configDir_.empty())
		return true;

	std::string bytes;
	{
		const HistoryLock lock(lockPath(), kLockTimeoutMs);
		const ReadResult history = readWholeFile(historyPath(), bytes);
		HistoryParse parsed = HistoryParse::Invalid;
		if (history == ReadResult::Ok)
		{
			std::string error;
			parsed = parseHistoryJson(bytes, data_, error);
		}
		if (parsed != HistoryParse::Ok)
			data_ = HistoryData{};
		// A corrupt or oversized file is moved aside once, at startup; saving
		// would otherwise refuse to replace it for good. A newer version stays.
		const bool unreadable = history == ReadResult::TooLarge
			|| (history == ReadResult::Ok && parsed == HistoryParse::Invalid);
		if (unreadable && lock.held())
		{
			const std::wstring bak = historyPath() + L".bak";
			::MoveFileExW(historyPath().c_str(), bak.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
		}
	}
	rebuildIndex();

	if (readWholeFile(settingsPath(), bytes) == ReadResult::Ok)
	{
		std::string error;
		PluginSettings parsed;
		if (parseSettingsJson(bytes, parsed, error))
			settings_ = parsed;
	}
	return true;
}

// Read, replay and replace run under one lock, so a save by another instance
// is either wholly before or wholly after this one. Anything that cannot be
// read as version 1 is left as it is, and the changes stay pending.
bool HistoryStore::saveHistory()
{
	if (configDir_.empty())
		return false;
	if (!ensureDirectory(configDir_))
		return false;

	const HistoryLock lock(lockPath(), kLockTimeoutMs);
	if (!lock.held())
		return false;

	std::string bytes;
	HistoryData onDisk;
	switch (readWholeFile(historyPath(), bytes))
	{
	case ReadResult::NotFound:
		break;
	case ReadResult::Ok:
	{
		std::string error;
		if (parseHistoryJson(bytes, onDisk, error) != HistoryParse::Ok)
			return false;
		break;
	}
	default:
		return false;
	}

	applyPending(onDisk.files, pending_);
	pruneOldest(onDisk.files, kMaxFiles);
	if (!writeFileAtomic(historyPath(), serializeHistoryJson(onDisk)))
		return false;
	data_ = std::move(onDisk);
	pending_ = PendingChanges{};
	rebuildIndex();
	dirty_ = false;
	return true;
}

bool HistoryStore::saveSettings()
{
	if (configDir_.empty())
		return false;
	if (!ensureDirectory(configDir_))
		return false;
	return writeFileAtomic(settingsPath(), serializeSettingsJson(settings_));
}

void HistoryStore::setEnabled(bool enabled)
{
	if (settings_.enabled == enabled)
		return;
	settings_.enabled = enabled;
	saveSettings();
}

const FileRecord* HistoryStore::find(const std::wstring& path) const
{
	const auto it = index_.find(pathKey(path));
	return it == index_.end() ? nullptr : &data_.files[it->second];
}

void HistoryStore::upsert(const std::wstring& path, intptr_t pos, intptr_t firstVisibleLine)
{
	if (!looksLikeFilesystemPath(path))
		return;
	FileRecord* rec = findMutable(path);
	if (rec)
	{
		if (rec->pos == pos && rec->firstVisibleLine == firstVisibleLine)
			return;
	}
	else
	{
		index_.emplace(pathKey(path), data_.files.size());
		data_.files.emplace_back();
		rec = &data_.files.back();
	}
	rec->path = path;
	rec->pos = pos;
	rec->firstVisibleLine = firstVisibleLine;
	rec->updatedAt = nowUtcIso8601();

	PendingChange change;
	change.record = *rec;
	pending_.byKey[pathKey(path)] = std::move(change);
	dirty_ = true;
	if (data_.files.size() > kMaxFiles)
	{
		pruneOldest(data_.files, kMaxFiles);
		rebuildIndex();
	}
}

// Recorded even without a local record: another instance may have one on disk.
void HistoryStore::erase(const std::wstring& path)
{
	const std::wstring key = pathKey(path);
	if (key.empty())
		return;
	const auto it = index_.find(key);
	if (it != index_.end())
	{
		data_.files.erase(data_.files.begin() + static_cast<std::ptrdiff_t>(it->second));
		rebuildIndex();
	}
	PendingChange change;
	change.kind = PendingChange::Kind::Remove;
	change.record.path = path;
	pending_.byKey[key] = std::move(change);
	dirty_ = true;
}

void HistoryStore::rename(const std::wstring& oldPath, const std::wstring& newPath)
{
	if (!looksLikeFilesystemPath(newPath) || oldPath.empty() || pathsEqual(oldPath, newPath))
		return;
	const std::wstring oldKey = pathKey(oldPath);
	const std::wstring newKey = pathKey(newPath);
	const std::string now = nowUtcIso8601();

	std::optional<PendingChange> carried;
	if (renameRecord(data_.files, oldPath, newPath))
	{
		rebuildIndex();
		FileRecord* renamed = findMutable(newPath);
		renamed->updatedAt = now;
		carried.emplace();
		carried->record = *renamed;
	}
	else
	{
		// The record, if any, is on disk only, written by another instance.
		const auto prior = pending_.byKey.find(oldKey);
		if (prior == pending_.byKey.end())
		{
			carried.emplace();
			carried->kind = PendingChange::Kind::MoveFrom;
			carried->fromKey = oldKey;
		}
		else if (prior->second.kind != PendingChange::Kind::Remove)
		{
			carried = prior->second;
		}
		if (carried)
		{
			carried->record.path = newPath;
			carried->record.updatedAt = now;
		}
	}
	if (carried)
		pending_.byKey[newKey] = std::move(*carried);

	PendingChange removed;
	removed.kind = PendingChange::Kind::Remove;
	removed.record.path = oldPath;
	pending_.byKey[oldKey] = std::move(removed);
	dirty_ = true;
}

void HistoryStore::clear()
{
	data_.files.clear();
	index_.clear();
	pending_ = PendingChanges{};
	pending_.cleared = true;
	dirty_ = true;
}

std::wstring HistoryStore::lockPath() const
{
	return configDir_ + L"\\history.json.lock";
}

std::wstring HistoryStore::historyPath() const
{
	return configDir_ + L"\\history.json";
}

std::wstring HistoryStore::settingsPath() const
{
	return configDir_ + L"\\settings.json";
}

void HistoryStore::rebuildIndex()
{
	index_.clear();
	for (size_t i = 0; i < data_.files.size(); ++i)
		index_[pathKey(data_.files[i].path)] = i;
}

FileRecord* HistoryStore::findMutable(const std::wstring& path)
{
	const auto it = index_.find(pathKey(path));
	return it == index_.end() ? nullptr : &data_.files[it->second];
}
