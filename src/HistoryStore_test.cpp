#include "HistoryStore.h"
#include "UiText.h"

#include <iostream>
#include <string>
#include <windows.h>

namespace
{
	int g_failures = 0;

	void check(bool cond, const char* expr, const char* file, int line)
	{
		if (cond)
			return;
		++g_failures;
		std::cout << "FAIL " << file << ':' << line << "  " << expr << '\n';
	}
}

#define CHECK(cond) check(static_cast<bool>(cond), #cond, __FILE__, __LINE__)

static void testPathRules()
{
	CHECK(!looksLikeFilesystemPath(L""));
	CHECK(!looksLikeFilesystemPath(L"new 1"));
	CHECK(!looksLikeFilesystemPath(L"new 12"));
	CHECK(looksLikeFilesystemPath(L"D:\\work\\foo.cpp"));
	CHECK(looksLikeFilesystemPath(L"D:/work/foo.cpp"));
	CHECK(looksLikeFilesystemPath(L"\\\\server\\share\\a.txt"));
	CHECK(looksLikeFilesystemPath(L"C:\\日本語\\§.txt"));

	CHECK(pathsEqual(L"D:\\Work\\Foo.cpp", L"d:\\work\\foo.cpp"));
	CHECK(!pathsEqual(L"D:\\Work\\Foo.cpp", L"D:\\Work\\Bar.cpp"));

	CHECK(clampPos(-3, 100) == 0);
	CHECK(clampPos(0, 100) == 0);
	CHECK(clampPos(50, 100) == 50);
	CHECK(clampPos(100, 100) == 100);
	CHECK(clampPos(101, 100) == 100);
	CHECK(clampPos(5, 0) == 0);

	CHECK(clampDocLine(-1, 10) == 0);
	CHECK(clampDocLine(0, 10) == 0);
	CHECK(clampDocLine(9, 10) == 9);
	CHECK(clampDocLine(10, 10) == 9);
	CHECK(clampDocLine(3, 0) == 0);
}

static bool parseFiles(std::string_view json, std::vector<FileRecord>& out, std::string& error)
{
	HistoryData data;
	const bool ok = parseHistoryJson(json, data, error) == HistoryParse::Ok;
	out = data.files;
	return ok;
}

static void testJsonRoundtrip()
{
	std::vector<FileRecord> in;
	FileRecord a;
	a.path = L"D:\\work\\foo.cpp";
	a.pos = 12480;
	a.firstVisibleLine = 410;
	a.updatedAt = "2026-09-18T01:00:00Z";
	in.push_back(a);

	FileRecord b;
	b.path = L"C:\\日本語\\§ file\"x.txt";
	b.pos = 7;
	b.firstVisibleLine = 2;
	b.updatedAt = "2026-09-18T02:00:00Z";
	in.push_back(b);

	HistoryData data;
	data.files = in;
	const std::string json = serializeHistoryJson(data);
	CHECK(json.find("\"version\": 1") != std::string::npos);
	CHECK(json.find("\\\\") != std::string::npos);
	CHECK(json.find("\\\"") != std::string::npos);

	std::vector<FileRecord> out;
	std::string error;
	CHECK(parseFiles(json, out, error));
	CHECK(error.empty());
	CHECK(out.size() == 2);

	const FileRecord* foo = nullptr;
	const FileRecord* jp = nullptr;
	for (const auto& f : out)
	{
		if (pathsEqual(f.path, a.path))
			foo = &f;
		if (pathsEqual(f.path, b.path))
			jp = &f;
	}
	CHECK(foo && foo->pos == 12480 && foo->firstVisibleLine == 410);
	CHECK(foo && foo->updatedAt == a.updatedAt);
	CHECK(jp && jp->pos == 7 && jp->path == b.path);
}

static void testJsonUnicodeEscapeAndBom()
{
	const std::string json =
		"\xEF\xBB\xBF{\"version\":1,\"files\":{\"C:\\\\a.txt\":{\"pos\":3,\"firstVisibleLine\":1,\"updatedAt\":\"\\u0032\\u0030\"}}}";
	std::vector<FileRecord> out;
	std::string error;
	CHECK(parseFiles(json, out, error));
	CHECK(out.size() == 1);
	CHECK(out[0].pos == 3);
	CHECK(out[0].updatedAt == "20");
}

static void testJsonDuplicatePathLastWins()
{
	const std::string json =
		"{\"version\":1,\"files\":{"
		"\"D:\\\\A.txt\":{\"pos\":1,\"firstVisibleLine\":0,\"updatedAt\":\"t1\"},"
		"\"d:\\\\a.txt\":{\"pos\":9,\"firstVisibleLine\":4,\"updatedAt\":\"t2\"}"
		"}}";
	std::vector<FileRecord> out;
	std::string error;
	CHECK(parseFiles(json, out, error));
	CHECK(out.size() == 1);
	CHECK(out[0].pos == 9);
	CHECK(out[0].firstVisibleLine == 4);
}

static void testJsonCorrupt()
{
	std::vector<FileRecord> out;
	std::string error;
	CHECK(!parseFiles("", out, error));
	CHECK(!parseFiles("{", out, error));
	CHECK(!parseFiles("{\"version\":2,\"files\":{}}", out, error));
	CHECK(!parseFiles("{\"files\":[]}", out, error));
	CHECK(!parseFiles("{\"version\":1}", out, error));
}

static void testPruneAndRename()
{
	std::vector<FileRecord> files;
	for (int i = 0; i < 5; ++i)
	{
		FileRecord r;
		r.path = L"D:\\f" + std::to_wstring(i) + L".txt";
		r.pos = i;
		r.updatedAt = std::string("2026-01-0") + std::to_string(i + 1) + "T00:00:00Z";
		files.push_back(r);
	}
	pruneOldest(files, 3);
	CHECK(files.size() == 3);
	CHECK(files[0].pos == 2);
	CHECK(files[2].pos == 4);

	CHECK(renameRecord(files, L"D:\\f3.txt", L"D:\\renamed.txt"));
	CHECK(renameRecord(files, L"D:\\f4.txt", L"D:\\renamed.txt"));
	CHECK(files.size() == 2);
	bool found = false;
	for (const auto& f : files)
	{
		if (pathsEqual(f.path, L"D:\\renamed.txt"))
		{
			found = true;
			CHECK(f.pos == 4);
		}
	}
	CHECK(found);
}

static void testSettings()
{
	PluginSettings s;
	s.enabled = false;
	const std::string json = serializeSettingsJson(s);
	PluginSettings out;
	std::string error;
	CHECK(parseSettingsJson(json, out, error));
	CHECK(out.enabled == false);
	CHECK(parseSettingsJson("{}", out, error));
	CHECK(out.enabled == true);
}

static void testStoreFileRoundtrip()
{
	wchar_t temp[MAX_PATH] = {};
	if (::GetTempPathW(MAX_PATH, temp) == 0)
	{
		CHECK(false);
		return;
	}
	std::wstring dir = temp;
	dir += L"NppRestoreCursorPositionTest_";
	dir += std::to_wstring(::GetCurrentProcessId());
	::CreateDirectoryW(dir.c_str(), nullptr);

	HistoryStore store;
	store.setDirectory(dir);
	store.upsert(L"D:\\work\\a.cpp", 11, 3);
	store.upsert(L"D:\\work\\b.cpp", 22, 5);
	CHECK(store.dirty());
	CHECK(store.saveHistory());
	CHECK(!store.dirty());

	HistoryStore loaded;
	loaded.setDirectory(dir);
	loaded.load();
	CHECK(loaded.size() == 2);
	const FileRecord* a = loaded.find(L"d:\\work\\a.cpp");
	CHECK(a && a->pos == 11 && a->firstVisibleLine == 3);

	loaded.clear();
	CHECK(loaded.saveHistory());

	std::string broken = "{not json";
	const std::wstring hist = dir + L"\\history.json";
	HANDLE h = ::CreateFileW(hist.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h != INVALID_HANDLE_VALUE)
	{
		DWORD w = 0;
		::WriteFile(h, broken.data(), static_cast<DWORD>(broken.size()), &w, nullptr);
		::CloseHandle(h);
	}
	HistoryStore recovered;
	recovered.setDirectory(dir);
	recovered.load();
	CHECK(recovered.size() == 0);
	const DWORD bak = ::GetFileAttributesW((dir + L"\\history.json.bak").c_str());
	CHECK(bak != INVALID_FILE_ATTRIBUTES);

	::DeleteFileW((dir + L"\\history.json").c_str());
	::DeleteFileW((dir + L"\\history.json.bak").c_str());
	::DeleteFileW((dir + L"\\settings.json").c_str());
	::RemoveDirectoryW(dir.c_str());
}

static void testPathKey()
{
	CHECK(pathKey(L"D:\\Work\\Foo.cpp") == pathKey(L"d:\\work\\FOO.CPP"));
	CHECK(pathKey(L"C:\\日本語\\ä.txt") == pathKey(L"C:\\日本語\\Ä.txt"));
	CHECK(pathKey(L"D:\\a.txt") != pathKey(L"D:\\b.txt"));
	CHECK(pathKey(L"").empty());
}

static void testJsonSurrogatesAndDepth()
{
	std::vector<FileRecord> out;
	std::string error;
	CHECK(parseFiles(
		"{\"version\":1,\"files\":{\"C:\\\\a.txt\":{\"pos\":1,\"updatedAt\":\"\\ud83d\\ude00\"}}}", out, error));
	CHECK(out.size() == 1 && out[0].updatedAt == "\xF0\x9F\x98\x80");
	CHECK(parseFiles(
		"{\"version\":1,\"files\":{\"C:\\\\a.txt\":{\"pos\":1,\"updatedAt\":\"\\ud83dx\"}}}", out, error));
	CHECK(out.size() == 1 && out[0].updatedAt == "\xEF\xBF\xBDx");

	const auto nested = [](int depth)
	{
		return "{\"version\":1,\"extra\":" + std::string(static_cast<size_t>(depth), '[')
			+ std::string(static_cast<size_t>(depth), ']') + ",\"files\":{}}";
	};
	CHECK(parseFiles(nested(10), out, error));
	CHECK(!parseFiles(nested(100000), out, error));
	CHECK(error == "nesting too deep");
}

static FileRecord record(const wchar_t* path, intptr_t pos, const char* updatedAt)
{
	FileRecord r;
	r.path = path;
	r.pos = pos;
	r.updatedAt = updatedAt;
	return r;
}

static const FileRecord* findIn(const std::vector<FileRecord>& files, const wchar_t* path)
{
	for (const auto& f : files)
	{
		if (pathsEqual(f.path, path))
			return &f;
	}
	return nullptr;
}

static std::wstring makeTempDir(const wchar_t* tag);
static void removeTempDir(const std::wstring& dir);

static void putChange(PendingChanges& pending, const FileRecord& rec)
{
	PendingChange c;
	c.record = rec;
	pending.byKey[pathKey(rec.path)] = c;
}

static void removeChange(PendingChanges& pending, const wchar_t* path)
{
	PendingChange c;
	c.kind = PendingChange::Kind::Remove;
	c.record.path = path;
	pending.byKey[pathKey(path)] = c;
}

static void moveChange(PendingChanges& pending, const wchar_t* from, const wchar_t* to)
{
	PendingChange c;
	c.kind = PendingChange::Kind::MoveFrom;
	c.record.path = to;
	c.record.updatedAt = "2026-01-01T00:00:05.000Z";
	c.fromKey = pathKey(from);
	pending.byKey[pathKey(to)] = c;
}

static void testApplyPending()
{
	std::vector<FileRecord> disk = {
		record(L"D:\\a.txt", 30, "2026-01-01T00:00:03.000Z"),
		record(L"D:\\b.txt", 10, "2026-01-01T00:00:01.000Z"),
		record(L"D:\\c.txt", 10, "2099-01-01T00:00:00.000Z"),
		record(L"D:\\old.txt", 7, "2026-01-01T00:00:02.000Z"),
	};
	PendingChanges pending;
	putChange(pending, record(L"d:\\A.txt", 20, "2026-01-01T00:00:02.000Z")); // older clock than disk
	putChange(pending, record(L"D:\\new.txt", 5, "2026-01-01T00:00:04.000Z"));
	removeChange(pending, L"D:\\c.txt");                                      // disk copy is dated later
	moveChange(pending, L"D:\\old.txt", L"D:\\moved.txt");
	removeChange(pending, L"D:\\old.txt");
	moveChange(pending, L"D:\\missing.txt", L"D:\\nothing.txt");
	applyPending(disk, pending);

	CHECK(disk.size() == 4);
	const FileRecord* a = findIn(disk, L"D:\\a.txt");
	CHECK(a && a->pos == 20);
	CHECK(a && a->updatedAt == "2099-01-01T00:00:00.000Z"); // never older than what is on disk
	CHECK(findIn(disk, L"D:\\b.txt") != nullptr);
	CHECK(findIn(disk, L"D:\\c.txt") == nullptr);
	CHECK(findIn(disk, L"D:\\old.txt") == nullptr);
	CHECK(findIn(disk, L"D:\\moved.txt") && findIn(disk, L"D:\\moved.txt")->pos == 7);
	CHECK(findIn(disk, L"D:\\nothing.txt") == nullptr);
	CHECK(findIn(disk, L"D:\\new.txt") != nullptr);

	std::vector<FileRecord> before = { record(L"D:\\x.txt", 1, "2099-01-01T00:00:00.000Z") };
	PendingChanges afterClear;
	afterClear.cleared = true;
	putChange(afterClear, record(L"D:\\y.txt", 2, "2026-01-01T00:00:01.000Z"));
	applyPending(before, afterClear);
	CHECK(before.size() == 1 && findIn(before, L"D:\\y.txt"));
}

static bool writeBytes(const std::wstring& path, const std::string& bytes)
{
	const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	DWORD written = 0;
	const BOOL ok = bytes.empty() || ::WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
	::CloseHandle(h);
	return ok && written == bytes.size();
}

static std::string readBytes(const std::wstring& path)
{
	const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return {};
	LARGE_INTEGER size{};
	::GetFileSizeEx(h, &size);
	std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
	DWORD read = 0;
	if (!bytes.empty())
		::ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
	::CloseHandle(h);
	bytes.resize(read);
	return bytes;
}

static bool exists(const std::wstring& path)
{
	return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static HistoryStore loadedStore(const std::wstring& dir)
{
	HistoryStore s;
	s.setDirectory(dir);
	s.load();
	return s;
}

static void testSameMillisecondOrder()
{
	const std::wstring dir = makeTempDir(L"samems");
	HistoryStore a = loadedStore(dir);
	a.upsert(L"D:\\x.txt", 1, 0);
	a.erase(L"D:\\x.txt");
	a.upsert(L"D:\\x.txt", 2, 0);
	a.upsert(L"D:\\y.txt", 3, 0);
	a.erase(L"D:\\y.txt");
	CHECK(a.saveHistory());
	HistoryStore reloaded = loadedStore(dir);
	CHECK(reloaded.find(L"D:\\x.txt") && reloaded.find(L"D:\\x.txt")->pos == 2);
	CHECK(reloaded.find(L"D:\\y.txt") == nullptr);

	a.clear();
	a.upsert(L"D:\\z.txt", 4, 0);
	CHECK(a.saveHistory());
	reloaded = loadedStore(dir);
	CHECK(reloaded.size() == 1 && reloaded.find(L"D:\\z.txt"));

	// Erased by one instance and recorded again by another, without waiting.
	HistoryStore b = loadedStore(dir);
	b.erase(L"D:\\z.txt");
	CHECK(b.saveHistory());
	a.upsert(L"D:\\z.txt", 5, 0);
	CHECK(a.saveHistory());
	reloaded = loadedStore(dir);
	CHECK(reloaded.find(L"D:\\z.txt") && reloaded.find(L"D:\\z.txt")->pos == 5);

	// And the other way round: a record made first, the erase saved after it.
	b.erase(L"D:\\z.txt");
	CHECK(b.saveHistory());
	reloaded = loadedStore(dir);
	CHECK(reloaded.find(L"D:\\z.txt") == nullptr);
	removeTempDir(dir);
}

static void testClockDoesNotDecide()
{
	const std::wstring dir = makeTempDir(L"clock");
	const std::wstring hist = dir + L"\\history.json";
	HistoryData future;
	future.files = {
		record(L"D:\\x.txt", 5, "2099-01-01T00:00:00.000Z"),
		record(L"D:\\y.txt", 6, "2099-01-01T00:00:00.000Z"),
	};
	CHECK(writeBytes(hist, serializeHistoryJson(future)));

	HistoryStore a = loadedStore(dir);
	a.upsert(L"D:\\x.txt", 7, 0);
	a.erase(L"D:\\y.txt");
	CHECK(a.saveHistory());
	HistoryStore reloaded = loadedStore(dir);
	CHECK(reloaded.find(L"D:\\x.txt") && reloaded.find(L"D:\\x.txt")->pos == 7);
	CHECK(reloaded.find(L"D:\\y.txt") == nullptr);

	// A full history dated ahead must not push out the record made now.
	HistoryData full;
	for (size_t i = 0; i < HistoryStore::kMaxFiles; ++i)
		full.files.push_back(record((L"D:\\full\\" + std::to_wstring(i) + L".txt").c_str(), 1, "2099-01-01T00:00:00.000Z"));
	CHECK(writeBytes(hist, serializeHistoryJson(full)));
	HistoryStore b = loadedStore(dir);
	b.upsert(L"D:\\now.txt", 3, 0);
	CHECK(b.saveHistory());
	reloaded = loadedStore(dir);
	CHECK(reloaded.size() == HistoryStore::kMaxFiles);
	CHECK(reloaded.find(L"D:\\now.txt") != nullptr);
	removeTempDir(dir);
}

static void testUnreadableHistoryIsKept()
{
	const std::wstring dir = makeTempDir(L"unreadable");
	const std::wstring hist = dir + L"\\history.json";
	const std::wstring bak = hist + L".bak";
	HistoryStore a = loadedStore(dir);
	a.upsert(L"D:\\mine.txt", 1, 0);

	const std::string corrupt = "{not json";
	CHECK(writeBytes(hist, corrupt));
	CHECK(!a.saveHistory());
	CHECK(a.dirty());
	CHECK(readBytes(hist) == corrupt);

	const std::string newer = "{\"version\":2,\"files\":[\"a new layout\"],\"extra\":{}}";
	CHECK(writeBytes(hist, newer));
	CHECK(!a.saveHistory());
	CHECK(a.dirty());
	CHECK(readBytes(hist) == newer);
	HistoryStore old = loadedStore(dir);
	CHECK(old.size() == 0);
	CHECK(!exists(bak)); // a newer file is not moved aside either
	old.upsert(L"D:\\old.txt", 1, 0);
	CHECK(!old.saveHistory());
	CHECK(readBytes(hist) == newer);

	const std::string huge(33u * 1024u * 1024u, ' ');
	CHECK(writeBytes(hist, huge));
	CHECK(!a.saveHistory());
	CHECK(a.dirty());
	CHECK(readBytes(hist).size() == huge.size());

	// At startup a corrupt file is moved aside, and saving works again.
	CHECK(writeBytes(hist, corrupt));
	HistoryStore fresh = loadedStore(dir);
	CHECK(fresh.size() == 0);
	CHECK(readBytes(bak) == corrupt);
	CHECK(!exists(hist));
	CHECK(a.saveHistory());
	CHECK(!a.dirty());
	HistoryStore reloaded = loadedStore(dir);
	CHECK(reloaded.find(L"D:\\mine.txt") != nullptr);
	removeTempDir(dir);
}

static void testEraseAndRenameWithoutLocalRecord()
{
	const std::wstring dir = makeTempDir(L"remote");
	HistoryStore a = loadedStore(dir);
	HistoryStore b = loadedStore(dir);
	b.upsert(L"D:\\x.txt", 1, 0);
	b.upsert(L"D:\\r.txt", 8, 0);
	b.upsert(L"D:\\p.txt", 9, 0);
	CHECK(b.saveHistory());

	CHECK(a.find(L"D:\\x.txt") == nullptr);
	a.erase(L"D:\\x.txt");
	CHECK(a.dirty());
	a.rename(L"D:\\r.txt", L"D:\\s.txt");
	a.rename(L"D:\\p.txt", L"D:\\q.txt");
	a.rename(L"D:\\q.txt", L"D:\\w.txt");
	CHECK(a.saveHistory());

	HistoryStore reloaded = loadedStore(dir);
	CHECK(reloaded.find(L"D:\\x.txt") == nullptr);
	CHECK(reloaded.find(L"D:\\r.txt") == nullptr);
	CHECK(reloaded.find(L"D:\\s.txt") && reloaded.find(L"D:\\s.txt")->pos == 8);
	CHECK(reloaded.find(L"D:\\p.txt") == nullptr && reloaded.find(L"D:\\q.txt") == nullptr);
	CHECK(reloaded.find(L"D:\\w.txt") && reloaded.find(L"D:\\w.txt")->pos == 9);
	CHECK(a.find(L"D:\\s.txt") != nullptr); // what the save found on disk is visible here too
	removeTempDir(dir);
}

static HistoryParse parseStatus(std::string_view json)
{
	HistoryData data;
	std::string error;
	return parseHistoryJson(json, data, error);
}

static void testFutureVersionIsNotCorrupt()
{
	const char* const newer[] = {
		"{\"version\":2,\"ratio\":1.5}",
		"{\"version\":2,\"big\":6.02e23,\"small\":-1E-7,\"files\":[]}",
		"{\"version\":2,\"x\":{\"a\":[1,{\"b\":[-0.5,true,null,\"s\"]}],\"c\":{}},\"files\":{\"k\":[[]]}}",
		"{\"ratio\":2.5e+3,\"version\":2,\"files\":{}}",
		"{\"version\":1.0,\"files\":{}}",
		"{\"version\":\"2\",\"files\":{}}",
		"{\"version\":2,\"files\":{}} and a layout this parser would not read",
	};
	for (const char* json : newer)
		CHECK(parseStatus(json) == HistoryParse::UnsupportedVersion);

	// Numbers in keys this version does not read are fine in version 1 too.
	CHECK(parseStatus("{\"version\":1,\"ratio\":1.5,\"n\":123456789012345678901234567890,\"files\":{}}") == HistoryParse::Ok);
	CHECK(parseStatus("{\"version\":1,\"bad\":1.,\"files\":{}}") == HistoryParse::Invalid);
	CHECK(parseStatus("{\"version\":1,\"bad\":1e,\"files\":{}}") == HistoryParse::Invalid);
	CHECK(parseStatus("{\"version\":1,\"bad\":-,\"files\":{}}") == HistoryParse::Invalid);

	const std::wstring dir = makeTempDir(L"future");
	const std::wstring hist = dir + L"\\history.json";
	for (const char* json : newer)
	{
		CHECK(writeBytes(hist, json));
		HistoryStore old = loadedStore(dir);
		CHECK(old.size() == 0);
		CHECK(!exists(hist + L".bak"));
		CHECK(readBytes(hist) == json);
		old.upsert(L"D:\\old.txt", 1, 0);
		CHECK(!old.saveHistory());
		CHECK(old.dirty());
		CHECK(readBytes(hist) == json);
	}
	removeTempDir(dir);
}

static void testRootMustBeComplete()
{
	CHECK(parseStatus("{}") == HistoryParse::Invalid);
	CHECK(parseStatus("{} \r\n") == HistoryParse::Invalid);
	CHECK(parseStatus("{}garbage") == HistoryParse::Invalid);
	CHECK(parseStatus("{\"files\":{}}garbage") == HistoryParse::Invalid);
	CHECK(parseStatus("{\"version\":1,\"files\":{}} x") == HistoryParse::Invalid);
	CHECK(parseStatus("{\"files\":{}} \r\n") == HistoryParse::Ok);

	const std::wstring dir = makeTempDir(L"root");
	const std::wstring hist = dir + L"\\history.json";
	for (const char* json : { "{}garbage", "{}" })
	{
		CHECK(writeBytes(hist, json));
		HistoryStore s = loadedStore(dir);
		CHECK(s.size() == 0);
		CHECK(readBytes(hist + L".bak") == json);
		CHECK(!exists(hist));
		s.upsert(L"D:\\a.txt", 1, 0);
		CHECK(s.saveHistory());
		HistoryStore reloaded = loadedStore(dir);
		CHECK(reloaded.find(L"D:\\a.txt") != nullptr);
	}
	removeTempDir(dir);
}

static void testCommandLineFlag()
{
	CHECK(commandLineHasFlag(L"\"C:\\npp\\notepad++.exe\" -multiInst -nosession \"D:\\a b.txt\"", L"-nosession"));
	CHECK(commandLineHasFlag(L"notepad++.exe\t-nosession", L"-nosession"));
	CHECK(!commandLineHasFlag(L"\"C:\\npp\\notepad++.exe\" -multiInst \"D:\\a b.txt\"", L"-nosession"));
	CHECK(!commandLineHasFlag(L"notepad++.exe -NOSESSION", L"-nosession"));
	CHECK(!commandLineHasFlag(L"notepad++.exe -nosessionX", L"-nosession"));
	CHECK(!commandLineHasFlag(L"notepad++.exe \"D:\\x -nosession y.txt\"", L"-nosession"));
	CHECK(!commandLineHasFlag(L"", L"-nosession"));
}

static void testStartupSessionWasLoaded()
{
	const std::vector<std::wstring> session = { L"D:\\a.txt", L"D:\\b.txt", L"D:\\gone.txt", L"new 1" };
	const auto exists = [](const std::wstring& path) { return !pathsEqual(path, L"D:\\gone.txt"); };
	const auto keys = [](std::initializer_list<const wchar_t*> paths)
	{
		std::unordered_set<std::wstring> out;
		for (const wchar_t* p : paths)
			out.insert(pathKey(p));
		return out;
	};
	CHECK(startupSessionWasLoaded(session, keys({ L"d:\\A.txt", L"D:\\b.txt", L"D:\\cmdline.txt" }), exists));
	CHECK(!startupSessionWasLoaded(session, keys({ L"D:\\a.txt" }), exists)); // b exists but was not opened
	CHECK(!startupSessionWasLoaded(session, keys({ L"D:\\cmdline.txt" }), exists));
	CHECK(!startupSessionWasLoaded(session, keys({}), exists));
	CHECK(!startupSessionWasLoaded({}, keys({ L"D:\\a.txt" }), exists));
	CHECK(!startupSessionWasLoaded({ L"D:\\gone.txt" }, keys({}), exists));
}

constexpr int kRaceWorkers = 4;
constexpr int kRaceRounds = 40;

static int raceWorker(const std::wstring& dir, const std::wstring& id, const std::wstring& startEvent)
{
	const HANDLE start = ::OpenEventW(SYNCHRONIZE, FALSE, startEvent.c_str());
	if (!start)
		return 3;
	::WaitForSingleObject(start, 10000);
	::CloseHandle(start);
	HistoryStore s = loadedStore(dir);
	for (int r = 0; r < kRaceRounds; ++r)
	{
		s.upsert(L"D:\\race\\w" + id + L"_" + std::to_wstring(r) + L".txt", r + 1, 0);
		bool saved = false;
		for (int tries = 0; tries < 200 && !saved; ++tries)
		{
			saved = s.saveHistory();
			if (!saved)
				::Sleep(1);
		}
		if (!saved)
			return 2;
	}
	return 0;
}

static void testConcurrentProcessesKeepEveryUpdate()
{
	const std::wstring dir = makeTempDir(L"race");
	wchar_t exe[MAX_PATH] = {};
	::GetModuleFileNameW(nullptr, exe, MAX_PATH);
	const std::wstring eventName = L"NppRestoreCursorPositionTest_race_" + std::to_wstring(::GetCurrentProcessId());
	const HANDLE start = ::CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
	CHECK(start != nullptr);

	std::vector<PROCESS_INFORMATION> children;
	for (int w = 0; w < kRaceWorkers; ++w)
	{
		std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --race-worker \"" + dir + L"\" "
			+ std::to_wstring(w) + L" " + eventName;
		STARTUPINFOW si{};
		si.cb = sizeof(si);
		PROCESS_INFORMATION pi{};
		if (::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
			children.push_back(pi);
	}
	CHECK(children.size() == static_cast<size_t>(kRaceWorkers));
	::Sleep(200);
	::SetEvent(start);
	for (const auto& pi : children)
	{
		CHECK(::WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0);
		DWORD code = 1;
		::GetExitCodeProcess(pi.hProcess, &code);
		CHECK(code == 0);
		::CloseHandle(pi.hThread);
		::CloseHandle(pi.hProcess);
	}
	if (start)
		::CloseHandle(start);

	HistoryStore reloaded = loadedStore(dir);
	CHECK(reloaded.size() == static_cast<size_t>(kRaceWorkers * kRaceRounds));
	if (reloaded.size() != static_cast<size_t>(kRaceWorkers * kRaceRounds))
		std::cout << "  race: " << reloaded.size() << " of " << kRaceWorkers * kRaceRounds << " records kept\n";
	removeTempDir(dir);
}

static void testSessionFileNames()
{
	const std::string xml =
		"\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\r\n"
		"<NotepadPlus>\r\n"
		"    <Session activeView=\"0\">\r\n"
		"        <mainView activeIndex=\"0\">\r\n"
		"            <File firstVisibleLine=\"0\" xOffset=\"0\" filename=\"C:\\a&amp;b\\x.txt\" backupFilePath=\"\" />\r\n"
		"            <File backupFilename=\"C:\\no.txt\" filename=\"D:\\&#x65E5;&#26412;\\&apos;y&apos;.txt\"/>\r\n"
		"        </mainView>\r\n"
		"        <subView activeIndex=\"0\">\r\n"
		"            <File\r\n filename=\"E:\\sub.txt\">\r\n"
		"            </File>\r\n"
		"        </subView>\r\n"
		"    </Session>\r\n"
		"    <FileBrowser><Filename not=\"a file\"/></FileBrowser>\r\n"
		"</NotepadPlus>\r\n";
	const std::vector<std::wstring> names = parseSessionFileNames(xml);
	CHECK(names.size() == 3);
	CHECK(names.size() > 0 && names[0] == L"C:\\a&b\\x.txt");
	CHECK(names.size() > 1 && names[1] == L"D:\\日本\\'y'.txt");
	CHECK(names.size() > 2 && names[2] == L"E:\\sub.txt");
	CHECK(parseSessionFileNames("").empty());
	CHECK(parseSessionFileNames("<File filename=\"unterminated").empty());
}
static void testEraseIsNotResurrected()
{
	const std::wstring dir = makeTempDir(L"tomb");
	HistoryStore a;
	a.setDirectory(dir);
	a.upsert(L"D:\\x.txt", 1, 0);
	a.upsert(L"D:\\y.txt", 2, 0);
	CHECK(a.saveHistory());

	HistoryStore b;
	b.setDirectory(dir);
	b.load();
	CHECK(b.find(L"D:\\x.txt") != nullptr);

	::Sleep(5);
	a.erase(L"D:\\x.txt");
	CHECK(a.saveHistory());

	// b still holds x in memory and saves for an unrelated change.
	b.upsert(L"D:\\z.txt", 3, 0);
	CHECK(b.saveHistory());
	CHECK(b.find(L"D:\\x.txt") == nullptr);

	HistoryStore c;
	c.setDirectory(dir);
	c.load();
	CHECK(c.find(L"D:\\x.txt") == nullptr);
	CHECK(c.find(L"D:\\y.txt") && c.find(L"D:\\z.txt"));

	// Opening the erased file again later records it anew.
	::Sleep(5);
	c.upsert(L"D:\\x.txt", 9, 0);
	CHECK(c.saveHistory());
	HistoryStore d;
	d.setDirectory(dir);
	d.load();
	CHECK(d.find(L"D:\\x.txt") && d.find(L"D:\\x.txt")->pos == 9);
	removeTempDir(dir);
}

static void testClearIsNotResurrected()
{
	const std::wstring dir = makeTempDir(L"clear");
	HistoryStore a;
	a.setDirectory(dir);
	a.upsert(L"D:\\p.txt", 1, 0);
	CHECK(a.saveHistory());
	HistoryStore b;
	b.setDirectory(dir);
	b.load();

	::Sleep(5);
	a.clear();
	CHECK(a.saveHistory());
	::Sleep(5);
	b.upsert(L"D:\\q.txt", 2, 0);
	CHECK(b.saveHistory());

	HistoryStore c;
	c.setDirectory(dir);
	c.load();
	CHECK(c.find(L"D:\\p.txt") == nullptr);
	CHECK(c.find(L"D:\\q.txt") != nullptr);
	removeTempDir(dir);
}

static std::wstring makeTempDir(const wchar_t* tag)
{
	wchar_t temp[MAX_PATH] = {};
	::GetTempPathW(MAX_PATH, temp);
	std::wstring dir = temp;
	dir += L"NppRestoreCursorPositionTest_";
	dir += tag;
	dir += L"_";
	dir += std::to_wstring(::GetCurrentProcessId());
	::CreateDirectoryW(dir.c_str(), nullptr);
	return dir;
}

static void removeTempDir(const std::wstring& dir)
{
	for (const wchar_t* name : { L"\\history.json", L"\\history.json.bak", L"\\history.json.lock", L"\\settings.json" })
		::DeleteFileW((dir + name).c_str());
	::RemoveDirectoryW(dir.c_str());
}

static void testTwoInstances()
{
	const std::wstring dir = makeTempDir(L"multi");
	HistoryStore a;
	a.setDirectory(dir);
	a.load();
	HistoryStore b;
	b.setDirectory(dir);
	b.load();

	a.upsert(L"D:\\from_a.txt", 1, 0);
	CHECK(a.saveHistory());
	b.upsert(L"D:\\from_b.txt", 2, 0);
	CHECK(b.saveHistory());

	HistoryStore c;
	c.setDirectory(dir);
	c.load();
	CHECK(c.size() == 2);
	CHECK(c.find(L"D:\\from_a.txt") && c.find(L"D:\\from_b.txt"));

	// Clearing in one instance drops what the other wrote before the clear.
	a.clear();
	CHECK(a.saveHistory());
	HistoryStore d;
	d.setDirectory(dir);
	d.load();
	CHECK(d.size() == 0);
	removeTempDir(dir);
}

static void testLockedHistoryIsNotOverwritten()
{
	const std::wstring dir = makeTempDir(L"locked");
	HistoryStore first;
	first.setDirectory(dir);
	first.upsert(L"D:\\keep.txt", 5, 0);
	CHECK(first.saveHistory());

	const std::wstring hist = dir + L"\\history.json";
	const HANDLE lock = ::CreateFileW(hist.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	CHECK(lock != INVALID_HANDLE_VALUE);

	HistoryStore second;
	second.setDirectory(dir);
	second.load();
	CHECK(second.size() == 0);
	second.upsert(L"D:\\other.txt", 1, 0);
	CHECK(!second.saveHistory());
	CHECK(second.dirty());

	::CloseHandle(lock);
	CHECK(second.saveHistory());
	HistoryStore reloaded;
	reloaded.setDirectory(dir);
	reloaded.load();
	CHECK(reloaded.find(L"D:\\keep.txt") && reloaded.find(L"D:\\other.txt"));
	removeTempDir(dir);
}

static void testStoreEraseAndRename()
{
	HistoryStore store;
	store.upsert(L"D:\\a.txt", 1, 0);
	store.upsert(L"D:\\b.txt", 2, 0);
	store.upsert(L"D:\\c.txt", 3, 0);
	store.erase(L"d:\\B.TXT");
	CHECK(store.size() == 2);
	CHECK(store.find(L"D:\\b.txt") == nullptr);
	CHECK(store.find(L"D:\\c.txt") && store.find(L"D:\\c.txt")->pos == 3);

	const std::string before = store.find(L"D:\\a.txt")->updatedAt;
	::Sleep(5);
	store.rename(L"D:\\a.txt", L"D:\\renamed.txt");
	CHECK(store.find(L"D:\\a.txt") == nullptr);
	const FileRecord* renamed = store.find(L"D:\\RENAMED.txt");
	CHECK(renamed && renamed->pos == 1 && renamed->updatedAt > before);
}

static void testUiLang()
{
	CHECK(uiLangFromNativeFileName("") == UiLang::English);
	CHECK(uiLangFromNativeFileName("english.xml") == UiLang::English);
	CHECK(uiLangFromNativeFileName("german.xml") == UiLang::English);
	CHECK(uiLangFromNativeFileName("taiwaneseMandarin.xml") == UiLang::English);
	CHECK(uiLangFromNativeFileName("japanese.xml") == UiLang::Japanese);
	CHECK(uiLangFromNativeFileName("Japanese.XML") == UiLang::Japanese);
	CHECK(uiLangFromNativeFileName("japanese") == UiLang::Japanese);
	CHECK(uiLangFromNativeFileName("C:\\npp\\localization\\japanese.xml") == UiLang::Japanese);

	CHECK(std::wstring(uiText(UiLang::English).pluginName) == L"NppRestoreCursorPosition");
	CHECK(std::wstring(uiText(UiLang::Japanese).pluginName) == L"カーソル位置復元");
	CHECK(std::wstring(uiText(UiLang::English).saveCursor) == L"Save cursor position");
	CHECK(std::wstring(uiText(UiLang::Japanese).saveCursor) == L"カーソル位置を保存");
}

int wmain(int argc, wchar_t* argv[])
{
	if (argc == 5 && std::wstring(argv[1]) == L"--race-worker")
		return raceWorker(argv[2], argv[3], argv[4]);

	testPathRules();
	testJsonRoundtrip();
	testJsonUnicodeEscapeAndBom();
	testJsonDuplicatePathLastWins();
	testJsonCorrupt();
	testPruneAndRename();
	testSettings();
	testStoreFileRoundtrip();
	testPathKey();
	testJsonSurrogatesAndDepth();
	testApplyPending();
	testTwoInstances();
	testEraseIsNotResurrected();
	testClearIsNotResurrected();
	testLockedHistoryIsNotOverwritten();
	testStoreEraseAndRename();
	testSameMillisecondOrder();
	testClockDoesNotDecide();
	testUnreadableHistoryIsKept();
	testEraseAndRenameWithoutLocalRecord();
	testConcurrentProcessesKeepEveryUpdate();
	testSessionFileNames();
	testFutureVersionIsNotCorrupt();
	testRootMustBeComplete();
	testCommandLineFlag();
	testStartupSessionWasLoaded();
	testUiLang();

	if (g_failures == 0)
	{
		std::cout << "OK\n";
		return 0;
	}
	std::cout << g_failures << " failed\n";
	return 1;
}
