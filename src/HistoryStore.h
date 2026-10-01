#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// File-position history. No Notepad++ types; tests can link this alone.

struct FileRecord
{
	std::wstring path;
	intptr_t pos = 0;
	intptr_t firstVisibleLine = 0; // document line, not wrap display line
	std::string updatedAt;
};

struct HistoryData
{
	std::vector<FileRecord> files;
};

enum class HistoryParse
{
	Ok,
	Invalid,
	UnsupportedVersion // written by a newer plugin; must not be overwritten
};

// What this instance changed since its last save. A save replays only these
// onto whatever history.json holds at that moment, under a lock shared by all
// instances, so the order of saves decides and no clock is compared.
struct PendingChange
{
	enum class Kind { Put, Remove, MoveFrom };
	Kind kind = Kind::Put;
	FileRecord record;   // Put: the record. MoveFrom: path and updatedAt of the new name.
	std::wstring fromKey; // MoveFrom: pathKey of the record on disk to take over
};

struct PendingChanges
{
	bool cleared = false; // applied before the changes below
	std::unordered_map<std::wstring, PendingChange> byKey;
	bool empty() const { return !cleared && byKey.empty(); }
};

struct PluginSettings
{
	bool enabled = true;
};

bool looksLikeFilesystemPath(std::wstring_view path);
std::wstring pathKey(std::wstring_view path);
bool pathsEqual(const std::wstring& a, const std::wstring& b);
intptr_t clampPos(intptr_t pos, intptr_t length);
intptr_t clampDocLine(intptr_t line, intptr_t lineCount);

std::string wideToUtf8(std::wstring_view wide);
std::wstring utf8ToWide(std::string_view utf8);

std::string serializeHistoryJson(const HistoryData& data);
HistoryParse parseHistoryJson(std::string_view json, HistoryData& out, std::string& error);

// File names of <File filename="..."> entries in a Notepad++ session.xml.
std::vector<std::wstring> parseSessionFileNames(std::string_view xml);
std::vector<std::wstring> readSessionFileNames(const std::wstring& sessionXmlPath);

// Whitespace-separated tokens, quotes grouping, compared exactly as Notepad++ does.
bool commandLineHasFlag(std::wstring_view commandLine, std::wstring_view flag);

// Notepad++ opens every session file that still exists before NPPN_READY.
// If one that exists was not opened, the session was not loaded this time.
bool startupSessionWasLoaded(const std::vector<std::wstring>& sessionFiles,
	const std::unordered_set<std::wstring>& openedBeforeReady, // by pathKey
	const std::function<bool(const std::wstring&)>& existsOnDisk);

std::string serializeSettingsJson(const PluginSettings& settings);
bool parseSettingsJson(std::string_view json, PluginSettings& out, std::string& error);

void pruneOldest(std::vector<FileRecord>& files, size_t maxFiles);
bool renameRecord(std::vector<FileRecord>& files, const std::wstring& oldPath, const std::wstring& newPath);

// Replays pending changes onto the records read from disk. Records put here
// are dated no earlier than the newest one on disk, so a clock set back does
// not make them the first to be pruned.
void applyPending(std::vector<FileRecord>& files, const PendingChanges& pending);

class HistoryStore
{
public:
	static constexpr size_t kMaxFiles = 2000;

	void setDirectory(std::wstring configDir);
	const std::wstring& directory() const { return configDir_; }

	bool load();
	bool saveHistory();
	bool saveSettings();

	const PluginSettings& settings() const { return settings_; }
	void setEnabled(bool enabled);

	const FileRecord* find(const std::wstring& path) const;
	void upsert(const std::wstring& path, intptr_t pos, intptr_t firstVisibleLine);
	void erase(const std::wstring& path);
	void rename(const std::wstring& oldPath, const std::wstring& newPath);
	void clear();

	size_t size() const { return data_.files.size(); }
	bool dirty() const { return dirty_; }

private:
	std::wstring historyPath() const;
	std::wstring settingsPath() const;
	void rebuildIndex();
	FileRecord* findMutable(const std::wstring& path);

	std::wstring lockPath() const;

	std::wstring configDir_;
	HistoryData data_; // disk as of the last load or save, plus pending_
	PendingChanges pending_;
	std::unordered_map<std::wstring, size_t> index_;
	PluginSettings settings_;
	bool dirty_ = false;
};
