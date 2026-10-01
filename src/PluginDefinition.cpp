#include "PluginDefinition.h"
#include "HistoryStore.h"
#include "UiText.h"

#include <string>
#include <unordered_map>
#include <unordered_set>

FuncItem funcItem[nbFunc];
NppData nppData;

namespace
{
	using BufferId = UINT_PTR;

	HistoryStore g_store;
	UiLang g_uiLang = UiLang::English;
	wchar_t g_pluginName[menuItemSize] = L"NppRestoreCursorPosition";
	bool g_ready = false;
	std::unordered_set<BufferId> g_pending;
	std::unordered_set<BufferId> g_openedBeforeReady;
	std::unordered_set<BufferId> g_activated;
	std::vector<std::wstring> g_sessionFiles;           // as listed in session.xml at startup
	std::unordered_set<std::wstring> g_openedBeforeReadyKeys; // pathKey, kept even if closed again
	std::unordered_set<std::wstring> g_sessionKeys;     // pathKey of files opened from the session
	bool g_sessionRead = false;

	struct KnownBuffer
	{
		std::wstring path;
		bool rememberable = false;
	};
	std::unordered_map<BufferId, KnownBuffer> g_buffers;
	std::unordered_map<BufferId, std::wstring> g_renameOld;

	constexpr UINT kSaveHistoryMessage = WM_APP + 1;
	constexpr UINT kApplyWrappedScrollMessage = WM_APP + 2;
	constexpr wchar_t kSaveWindowClass[] = L"NppRestoreCursorPosition.Save";

	HINSTANCE g_instance = nullptr;
	HWND g_saveWindow = nullptr;
	bool g_savePosted = false;

	// With wrap on, Notepad++ resets the first visible line to its own value on
	// SCN_PAINTED (after plugins are notified) until it sees that value once.
	// The scroll is therefore applied from a message posted on the first paint.
	struct WrappedScroll
	{
		HWND sci = nullptr;
		BufferId id = 0;
		Sci_Position pos = 0;
		Sci_Position docLine = 0;
		Sci_Position registeredDocLine = 0; // top line when the restore set the caret
		Sci_Position appliedDocLine = -1;   // top line our last scroll left, SCI_SCROLLCARET included
		ULONGLONG deadline = 0;
		int attempts = 0;
		bool posted = false;
	};
	constexpr int kMaxWrappedScrollAttempts = 10;
	constexpr ULONGLONG kWrappedScrollTimeoutMs = 2000;
	WrappedScroll g_wrappedScroll;

	void applyWrappedScroll();

	void flushHistoryNow()
	{
		if (g_store.dirty())
			g_store.saveHistory();
	}

	LRESULT CALLBACK saveWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		if (message == kSaveHistoryMessage)
		{
			g_savePosted = false;
			flushHistoryNow();
			return 0;
		}
		if (message == kApplyWrappedScrollMessage)
		{
			g_wrappedScroll.posted = false;
			applyWrappedScroll();
			return 0;
		}
		return ::DefWindowProcW(hwnd, message, wParam, lParam);
	}

	bool ensureSaveWindow()
	{
		if (g_saveWindow)
			return true;
		if (!g_instance)
			return false;
		WNDCLASSW windowClass{};
		windowClass.lpfnWndProc = saveWindowProc;
		windowClass.hInstance = g_instance;
		windowClass.lpszClassName = kSaveWindowClass;
		if (!::RegisterClassW(&windowClass) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
			return false;
		g_saveWindow = ::CreateWindowExW(0, kSaveWindowClass, L"", 0,
			0, 0, 0, 0, HWND_MESSAGE, nullptr, g_instance, nullptr);
		return g_saveWindow != nullptr;
	}

	void destroySaveWindow()
	{
		if (g_saveWindow)
		{
			::DestroyWindow(g_saveWindow);
			g_saveWindow = nullptr;
		}
		g_savePosted = false;
		if (g_instance)
			::UnregisterClassW(kSaveWindowClass, g_instance);
	}

	void scheduleHistorySave()
	{
		if (!g_store.dirty())
			return;
		if (!ensureSaveWindow())
		{
			flushHistoryNow();
			return;
		}
		if (g_savePosted)
			return;
		if (::PostMessageW(g_saveWindow, kSaveHistoryMessage, 0, 0))
			g_savePosted = true;
		else
			flushHistoryNow();
	}

	Sci_Position sciGet(HWND scintilla, UINT message, Sci_Position wParam = 0, Sci_Position lParam = 0)
	{
		return static_cast<Sci_Position>(::SendMessage(scintilla, message,
			static_cast<WPARAM>(wParam), static_cast<LPARAM>(lParam)));
	}

	void sciSend(HWND scintilla, UINT message, Sci_Position wParam = 0, Sci_Position lParam = 0)
	{
		::SendMessage(scintilla, message, static_cast<WPARAM>(wParam), static_cast<LPARAM>(lParam));
	}

	HWND scintillaForView(int view)
	{
		if (view == SUB_VIEW)
			return nppData._scintillaSecondHandle;
		return nppData._scintillaMainHandle;
	}

	int viewOfScintilla(HWND sci)
	{
		if (sci == nppData._scintillaMainHandle)
			return MAIN_VIEW;
		if (sci == nppData._scintillaSecondHandle)
			return SUB_VIEW;
		return -1;
	}

	HWND currentScintilla()
	{
		int which = -1;
		::SendMessage(nppData._nppHandle, NPPM_GETCURRENTSCINTILLA, 0, reinterpret_cast<LPARAM>(&which));
		if (which == SUB_VIEW)
			return nppData._scintillaSecondHandle;
		if (which == MAIN_VIEW)
			return nppData._scintillaMainHandle;
		return nullptr;
	}

	std::wstring pathFromBuffer(BufferId id)
	{
		if (!id)
			return {};
		const LRESULT chars = ::SendMessage(nppData._nppHandle, NPPM_GETFULLPATHFROMBUFFERID,
			static_cast<WPARAM>(id), 0);
		if (chars <= 0)
			return {};
		std::wstring path(static_cast<size_t>(chars) + 1, L'\0');
		const LRESULT copied = ::SendMessage(nppData._nppHandle, NPPM_GETFULLPATHFROMBUFFERID,
			static_cast<WPARAM>(id), reinterpret_cast<LPARAM>(path.data()));
		if (copied <= 0)
			return {};
		path.resize(static_cast<size_t>(copied));
		return path;
	}

	bool isRememberablePath(const std::wstring& path)
	{
		if (!looksLikeFilesystemPath(path))
			return false;
		const DWORD attr = ::GetFileAttributesW(path.c_str());
		if (attr == INVALID_FILE_ATTRIBUTES)
			return true;
		return (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
	}

	// GetFileAttributesW can stall on a slow network path, so it runs only when
	// the path of a buffer changes, not on every caret move.
	const KnownBuffer* rememberBuffer(BufferId id)
	{
		std::wstring path = pathFromBuffer(id);
		if (path.empty())
		{
			const auto it = g_buffers.find(id);
			return it != g_buffers.end() ? &it->second : nullptr;
		}
		KnownBuffer& known = g_buffers[id];
		if (known.path != path)
		{
			known.rememberable = isRememberablePath(path);
			known.path = std::move(path);
		}
		return &known;
	}

	const KnownBuffer* knownBuffer(BufferId id)
	{
		const auto it = g_buffers.find(id);
		if (it != g_buffers.end())
			return &it->second;
		return rememberBuffer(id);
	}

	std::wstring knownPath(BufferId id)
	{
		const KnownBuffer* known = knownBuffer(id);
		return known ? known->path : std::wstring();
	}

	bool isRememberableBuffer(BufferId id)
	{
		const KnownBuffer* known = knownBuffer(id);
		return known && known->rememberable;
	}

	std::wstring pluginsConfigDir()
	{
		const LRESULT chars = ::SendMessage(nppData._nppHandle, NPPM_GETPLUGINSCONFIGDIR, 0, 0);
		if (chars <= 0)
			return {};
		std::wstring dir(static_cast<size_t>(chars) + 1, L'\0');
		if (!::SendMessage(nppData._nppHandle, NPPM_GETPLUGINSCONFIGDIR,
			static_cast<WPARAM>(dir.size()), reinterpret_cast<LPARAM>(dir.data())))
		{
			return {};
		}
		dir.resize(::wcslen(dir.c_str()));
		return dir;
	}

	// Empty on a Notepad++ older than this message.
	std::wstring nppSettingsDir()
	{
		const LRESULT chars = ::SendMessage(nppData._nppHandle, NPPM_GETNPPSETTINGSDIRPATH, 0, 0);
		if (chars <= 0)
			return {};
		std::wstring dir(static_cast<size_t>(chars) + 1, L'\0');
		if (::SendMessage(nppData._nppHandle, NPPM_GETNPPSETTINGSDIRPATH,
			static_cast<WPARAM>(dir.size()), reinterpret_cast<LPARAM>(dir.data())) <= 0)
		{
			return {};
		}
		dir.resize(::wcslen(dir.c_str()));
		return dir;
	}

	std::wstring withoutTrailingSlash(std::wstring dir)
	{
		while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/'))
			dir.pop_back();
		return dir;
	}

	std::wstring parentDir(const std::wstring& dir)
	{
		const std::wstring trimmed = withoutTrailingSlash(dir);
		const auto slash = trimmed.find_last_of(L"\\/");
		return slash == std::wstring::npos ? std::wstring() : trimmed.substr(0, slash);
	}

	// Tabs from the startup session are positioned by Notepad++, at the top too
	// when that is where they were, so they are not restored. session.xml is in
	// the settings dir above plugins\Config, or in -settingsDir; never on the
	// cloud path, which the settings-dir message would return instead. Reading
	// both only means fewer restores when unsure.
	void readStartupSession()
	{
		if (g_sessionRead)
			return;
		g_sessionRead = true;
		std::vector<std::wstring> dirs;
		const std::wstring userDir = parentDir(parentDir(pluginsConfigDir()));
		if (!userDir.empty())
			dirs.push_back(userDir);
		const std::wstring settingsDir = withoutTrailingSlash(nppSettingsDir());
		if (!settingsDir.empty() && !pathsEqual(settingsDir, userDir))
			dirs.push_back(settingsDir);
		for (const auto& dir : dirs)
		{
			for (auto& name : readSessionFileNames(dir + L"\\session.xml"))
				g_sessionFiles.push_back(std::move(name));
		}
	}

	// session.xml stays on disk when Notepad++ does not load it (-nosession, the
	// session setting off, a second instance, and so on). Its files count only
	// if this start really opened them from it.
	void resolveStartupSession()
	{
		g_sessionKeys.clear();
		if (commandLineHasFlag(::GetCommandLineW(), L"-nosession"))
			return;
		const auto exists = [](const std::wstring& path)
		{
			return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
		};
		if (!startupSessionWasLoaded(g_sessionFiles, g_openedBeforeReadyKeys, exists))
			return;
		for (const auto& name : g_sessionFiles)
			g_sessionKeys.insert(pathKey(name));
	}

	void syncEnabledCheck()
	{
		if (!nppData._nppHandle || funcItem[0]._cmdID == 0)
			return;
		::SendMessage(nppData._nppHandle, NPPM_SETMENUITEMCHECK,
			static_cast<WPARAM>(funcItem[0]._cmdID),
			g_store.settings().enabled ? TRUE : FALSE);
	}

	std::string nativeLangFileName()
	{
		if (!nppData._nppHandle)
			return {};
		const LRESULT chars = ::SendMessage(nppData._nppHandle, NPPM_GETNATIVELANGFILENAME, 0, 0);
		if (chars <= 0)
			return {};
		std::string name(static_cast<size_t>(chars) + 1, '\0');
		const LRESULT copied = ::SendMessage(nppData._nppHandle, NPPM_GETNATIVELANGFILENAME,
			static_cast<WPARAM>(name.size()), reinterpret_cast<LPARAM>(name.data()));
		if (copied <= 0)
			return {};
		name.resize(static_cast<size_t>(copied));
		return name;
	}

	bool menuDirectlyContainsCmd(HMENU menu, UINT cmd)
	{
		if (!menu)
			return false;
		const int count = ::GetMenuItemCount(menu);
		for (int i = 0; i < count; ++i)
		{
			if (::GetMenuItemID(menu, i) == cmd)
				return true;
		}
		return false;
	}

	bool modifyMenuItemById(HMENU menu, UINT cmd, const wchar_t* text)
	{
		if (!menu || !text)
			return false;
		const int count = ::GetMenuItemCount(menu);
		for (int i = 0; i < count; ++i)
		{
			if (::GetMenuItemID(menu, i) == cmd)
			{
				MENUITEMINFOW info{};
				info.cbSize = sizeof(info);
				info.fMask = MIIM_STRING;
				info.dwTypeData = const_cast<wchar_t*>(text);
				return ::SetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &info) != FALSE;
			}
			if (HMENU sub = ::GetSubMenu(menu, i))
			{
				if (modifyMenuItemById(sub, cmd, text))
					return true;
			}
		}
		return false;
	}

	bool renameDirectChildPopup(HMENU menu, UINT childCmd, const wchar_t* title)
	{
		if (!menu || !title)
			return false;
		const int count = ::GetMenuItemCount(menu);
		for (int i = 0; i < count; ++i)
		{
			HMENU sub = ::GetSubMenu(menu, i);
			if (!sub || !menuDirectlyContainsCmd(sub, childCmd))
				continue;
			MENUITEMINFOW info{};
			info.cbSize = sizeof(info);
			info.fMask = MIIM_STRING;
			info.dwTypeData = const_cast<wchar_t*>(title);
			return ::SetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &info) != FALSE;
		}
		return false;
	}

	void refreshPluginMenus()
	{
		if (!nppData._nppHandle || funcItem[0]._cmdID == 0)
			return;
		const UiText& text = uiText(g_uiLang);
		const HMENU root = ::GetMenu(nppData._nppHandle);
		if (root)
		{
			modifyMenuItemById(root, static_cast<UINT>(funcItem[0]._cmdID), text.saveCursor);
			modifyMenuItemById(root, static_cast<UINT>(funcItem[1]._cmdID), text.clearHistory);
		}
		// Only rename the plugin popup under Plugins. Searching from the menubar
		// would match Plugins itself, because our commands live under it.
		const HMENU plugins = reinterpret_cast<HMENU>(::SendMessage(nppData._nppHandle,
			NPPM_GETMENUHANDLE, NPPPLUGINMENU, 0));
		if (plugins)
			renameDirectChildPopup(plugins, static_cast<UINT>(funcItem[0]._cmdID), text.pluginName);
		::DrawMenuBar(nppData._nppHandle);
		syncEnabledCheck();
	}

	void applyUiLanguage()
	{
		g_uiLang = uiLangFromNativeFileName(nativeLangFileName());
		const UiText& text = uiText(g_uiLang);
		wcsncpy_s(g_pluginName, menuItemSize, text.pluginName, _TRUNCATE);
		if (funcItem[0]._pFunc)
			wcsncpy_s(funcItem[0]._itemName, menuItemSize, text.saveCursor, _TRUNCATE);
		if (funcItem[1]._pFunc)
			wcsncpy_s(funcItem[1]._itemName, menuItemSize, text.clearHistory, _TRUNCATE);
		refreshPluginMenus();
	}

	void captureFromScintilla(BufferId id, HWND sci)
	{
		const KnownBuffer* known = knownBuffer(id);
		if (!sci || !known || !known->rememberable)
			return;
		const Sci_Position pos = sciGet(sci, SCI_GETCURRENTPOS);
		const Sci_Position vis = sciGet(sci, SCI_GETFIRSTVISIBLELINE);
		const Sci_Position docLine = sciGet(sci, SCI_DOCLINEFROMVISIBLE, vis);
		g_store.upsert(known->path, pos, docLine);
	}

	BufferId bufferInView(int view)
	{
		const LRESULT index = ::SendMessage(nppData._nppHandle, NPPM_GETCURRENTDOCINDEX, 0,
			static_cast<LPARAM>(view));
		if (index < 0)
			return 0;
		return static_cast<BufferId>(::SendMessage(nppData._nppHandle, NPPM_GETBUFFERIDFROMPOS,
			static_cast<WPARAM>(index), static_cast<LPARAM>(view)));
	}

	// A clone can be displayed in both views; prefer the one with focus.
	HWND displayedScintillaFor(BufferId id)
	{
		HWND current = currentScintilla();
		const int currentView = viewOfScintilla(current);
		if (currentView >= 0 && bufferInView(currentView) == id)
			return current;
		for (const int view : { MAIN_VIEW, SUB_VIEW })
		{
			if (bufferInView(view) == id)
				return scintillaForView(view);
		}
		return nullptr;
	}

	// A saved byte offset can fall inside a multi-byte character or a CR LF pair
	// once the file has changed on disk. Typing there would split the character.
	Sci_Position snapToCharBoundary(HWND sci, Sci_Position pos, Sci_Position length)
	{
		if (pos <= 0 || pos >= length)
			return pos;
		return sciGet(sci, SCI_POSITIONBEFORE, sciGet(sci, SCI_POSITIONAFTER, pos));
	}

	Sci_Position firstVisibleDocLine(HWND sci)
	{
		return sciGet(sci, SCI_DOCLINEFROMVISIBLE, sciGet(sci, SCI_GETFIRSTVISIBLELINE));
	}

	// SCI_SCROLLCARET would also move vertically under some caret policies,
	// undoing the restored first line, so the horizontal offset is set directly.
	void revealCaretHorizontally(HWND sci, Sci_Position pos)
	{
		if (sciGet(sci, SCI_GETWRAPMODE) != SC_WRAP_NONE)
			return;
		RECT client{};
		if (!::GetClientRect(sci, &client))
			return;
		Sci_Position textLeft = sciGet(sci, SCI_GETMARGINLEFT);
		const Sci_Position margins = sciGet(sci, SCI_GETMARGINS);
		for (Sci_Position m = 0; m < margins; ++m)
			textLeft += sciGet(sci, SCI_GETMARGINWIDTHN, m);
		const Sci_Position textRight = client.right - sciGet(sci, SCI_GETMARGINRIGHT);
		const Sci_Position width = textRight - textLeft;
		const Sci_Position x = sciGet(sci, SCI_POINTXFROMPOSITION, 0, pos);
		if (width <= 0 || (x >= textLeft && x < textRight))
			return;
		Sci_Position offset = sciGet(sci, SCI_GETXOFFSET) + (x - textLeft) - width / 2;
		if (offset < 0)
			offset = 0;
		sciSend(sci, SCI_SETXOFFSET, offset);
	}

	void scrollToDocLine(HWND sci, Sci_Position docLine, Sci_Position pos)
	{
		sciSend(sci, SCI_SETFIRSTVISIBLELINE, sciGet(sci, SCI_VISIBLEFROMDOCLINE, docLine));

		const Sci_Position first = sciGet(sci, SCI_GETFIRSTVISIBLELINE);
		const Sci_Position onScreen = sciGet(sci, SCI_LINESONSCREEN);
		const Sci_Position caretVis = sciGet(sci, SCI_VISIBLEFROMDOCLINE,
			sciGet(sci, SCI_LINEFROMPOSITION, pos));
		if (onScreen > 0 && (caretVis < first || caretVis >= first + onScreen))
			sciSend(sci, SCI_SCROLLCARET);
		else
			revealCaretHorizontally(sci, pos);
		sciSend(sci, SCI_CHOOSECARETX);
	}

	void restoreRecord(BufferId id, HWND sci, const FileRecord& rec)
	{
		if (!sci)
			return;
		const Sci_Position length = sciGet(sci, SCI_GETLENGTH);
		const Sci_Position pos = snapToCharBoundary(sci, clampPos(rec.pos, length), length);
		const Sci_Position docLine = clampDocLine(rec.firstVisibleLine, sciGet(sci, SCI_GETLINECOUNT));

		sciSend(sci, SCI_SETEMPTYSELECTION, pos);
		sciSend(sci, SCI_CHOOSECARETX);
		if (sciGet(sci, SCI_GETWRAPMODE) != SC_WRAP_NONE && ensureSaveWindow())
		{
			g_wrappedScroll = WrappedScroll{};
			g_wrappedScroll.sci = sci;
			g_wrappedScroll.id = id;
			g_wrappedScroll.pos = pos;
			g_wrappedScroll.docLine = docLine;
			g_wrappedScroll.registeredDocLine = firstVisibleDocLine(sci);
			g_wrappedScroll.deadline = ::GetTickCount64() + kWrappedScrollTimeoutMs;
			return;
		}
		scrollToDocLine(sci, docLine, pos);
	}

	bool wrappedScrollPendingFor(BufferId id)
	{
		return g_wrappedScroll.sci && g_wrappedScroll.id == id;
	}

	// While the scroll waits, the caret stays where the restore put it, and the
	// top line is only ever the document start (where Notepad++ keeps putting
	// it), the target, or where our own scroll left it. Anything else is the user.
	bool leftToRestore(const WrappedScroll& w)
	{
		if (sciGet(w.sci, SCI_GETCURRENTPOS) != w.pos)
			return false;
		const Sci_Position top = firstVisibleDocLine(w.sci);
		return top == 0 || top == w.docLine || top == w.registeredDocLine || top == w.appliedDocLine;
	}

	bool releaseWrappedScrollIfMoved(BufferId id)
	{
		if (!wrappedScrollPendingFor(id))
			return true;
		if (leftToRestore(g_wrappedScroll))
			return false;
		g_wrappedScroll = WrappedScroll{};
		return true;
	}

	void onPainted(HWND sci)
	{
		WrappedScroll& w = g_wrappedScroll;
		if (!w.sci || w.sci != sci || w.posted)
			return;
		if (::PostMessageW(g_saveWindow, kApplyWrappedScrollMessage, 0, 0))
			w.posted = true;
		else
			w = WrappedScroll{};
	}

	void applyWrappedScroll()
	{
		WrappedScroll& w = g_wrappedScroll;
		if (!w.sci)
			return;
		const int view = viewOfScintilla(w.sci);
		const bool stillOurs = view >= 0
			&& bufferInView(view) == w.id
			&& leftToRestore(w)
			&& ::GetTickCount64() < w.deadline;
		if (!stillOurs
			|| (w.attempts > 0 && firstVisibleDocLine(w.sci) == w.docLine)
			|| w.attempts >= kMaxWrappedScrollAttempts)
		{
			w = WrappedScroll{};
			return;
		}
		++w.attempts;
		const Sci_Position firstBefore = sciGet(w.sci, SCI_GETFIRSTVISIBLELINE);
		scrollToDocLine(w.sci, w.docLine, w.pos);
		w.appliedDocLine = firstVisibleDocLine(w.sci);
		// No scroll means no further SCN_PAINTED to confirm it on.
		if (sciGet(w.sci, SCI_GETFIRSTVISIBLELINE) == firstBefore)
			w = WrappedScroll{};
	}

	bool isAtDocumentOrigin(HWND sci)
	{
		if (!sci)
			return false;
		return sciGet(sci, SCI_GETCURRENTPOS) == 0
			&& sciGet(sci, SCI_GETFIRSTVISIBLELINE) == 0;
	}

	// Notepad++ has already positioned tabs from a session or -n; only a plain
	// open is still at the origin.
	void tryRestore(BufferId id, HWND sci)
	{
		if (!sci || !g_store.settings().enabled)
			return;
		const FileRecord* rec = g_store.find(knownPath(id));
		if (!rec)
			return;
		if (!isAtDocumentOrigin(sci))
			return;
		restoreRecord(id, sci, *rec);
	}

	void restoreVisiblePending()
	{
		for (const int view : { MAIN_VIEW, SUB_VIEW })
		{
			const BufferId id = bufferInView(view);
			if (!id)
				continue;
			if (g_pending.count(id))
			{
				tryRestore(id, scintillaForView(view));
				g_pending.erase(id);
			}
			g_activated.insert(id);
		}
	}

	void onReady()
	{
		std::wstring dir = pluginsConfigDir();
		if (!dir.empty())
		{
			if (dir.back() == L'\\' || dir.back() == L'/')
				dir.pop_back();
			dir += L"\\NppRestoreCursorPosition";
			g_store.setDirectory(std::move(dir));
			g_store.load();
		}
		g_ready = true;
		applyUiLanguage();
		if (g_store.settings().enabled)
		{
			readStartupSession();
			resolveStartupSession();
			for (const BufferId id : g_openedBeforeReady)
			{
				if (!isRememberableBuffer(id) || !g_store.find(knownPath(id)))
					continue;
				if (g_sessionKeys.count(pathKey(knownPath(id))))
					continue;
				g_pending.insert(id);
			}
		}
		restoreVisiblePending();
		g_openedBeforeReady.clear();
		g_openedBeforeReadyKeys.clear();
		g_sessionFiles.clear();
		g_sessionKeys.clear();
	}

	void onFileOpened(BufferId id)
	{
		rememberBuffer(id);
		if (!g_ready)
		{
			// Before Notepad++ writes session.xml again (snapshot mode does so periodically).
			readStartupSession();
			g_openedBeforeReady.insert(id);
			const std::wstring key = pathKey(knownPath(id));
			if (!key.empty())
				g_openedBeforeReadyKeys.insert(key);
			return;
		}
		if (!g_store.settings().enabled)
			return;
		if (!isRememberableBuffer(id) || !g_store.find(knownPath(id)))
			return;
		g_pending.insert(id);
	}

	void onBufferActivated(BufferId id)
	{
		rememberBuffer(id);
		if (!g_ready)
			return;

		const auto pending = g_pending.find(id);
		if (pending != g_pending.end())
		{
			g_pending.erase(pending);
			tryRestore(id, currentScintilla());
		}
		g_activated.insert(id);
	}

	void onUpdateUi(HWND sci, int updated)
	{
		// Content-only and horizontal-scroll updates cannot change what is recorded.
		if ((updated & (SC_UPDATE_SELECTION | SC_UPDATE_V_SCROLL)) == 0)
			return;
		if (!g_ready || !g_store.settings().enabled)
			return;
		const int view = viewOfScintilla(sci);
		if (view < 0)
			return;
		const BufferId id = bufferInView(view);
		if (!id || g_pending.count(id) || !g_activated.count(id))
			return;
		if (!releaseWrappedScrollIfMoved(id))
			return;
		captureFromScintilla(id, sci);
	}

	void onFileSaved(BufferId id)
	{
		rememberBuffer(id);
	}

	bool isOpenInAnyView(BufferId id)
	{
		return ::SendMessage(nppData._nppHandle, NPPM_GETPOSFROMBUFFERID,
			static_cast<WPARAM>(id), MAIN_VIEW) != -1;
	}

	void onFileBeforeClose(BufferId id)
	{
		const bool moved = releaseWrappedScrollIfMoved(id);
		if (wrappedScrollPendingFor(id))
			g_wrappedScroll = WrappedScroll{};
		if (!g_ready || !g_store.settings().enabled)
			return;
		if (g_activated.count(id) && moved)
		{
			if (HWND sci = displayedScintillaFor(id))
				captureFromScintilla(id, sci);
		}
		scheduleHistorySave();
	}

	// Notepad++ sends this for each view a buffer leaves, including when only
	// the clone in one view is closed. Keep tracking while the other view has it.
	void onFileClosed(BufferId id)
	{
		if (isOpenInAnyView(id))
			return;
		g_pending.erase(id);
		g_openedBeforeReady.erase(id);
		g_activated.erase(id);
		if (g_wrappedScroll.id == id)
			g_wrappedScroll = WrappedScroll{};
		g_buffers.erase(id);
		g_renameOld.erase(id);
	}

	void onBeforeShutdown()
	{
		if (!g_ready || !g_store.settings().enabled)
			return;
		for (const int view : { MAIN_VIEW, SUB_VIEW })
		{
			const BufferId id = bufferInView(view);
			if (!id || !g_activated.count(id))
				continue;
			if (!releaseWrappedScrollIfMoved(id))
				continue;
			rememberBuffer(id);
			captureFromScintilla(id, scintillaForView(view));
		}
		flushHistoryNow();
	}

	void onFileBeforeRename(BufferId id)
	{
		rememberBuffer(id);
		g_renameOld[id] = knownPath(id);
	}

	void onFileRenameCancel(BufferId id)
	{
		g_renameOld.erase(id);
	}

	void onFileRenamed(BufferId id)
	{
		const auto it = g_renameOld.find(id);
		if (it == g_renameOld.end())
			return;
		const std::wstring oldPath = it->second;
		g_renameOld.erase(it);
		if (!g_ready || !g_store.settings().enabled)
			return;
		g_store.rename(oldPath, pathFromBuffer(id));
		rememberBuffer(id);
		scheduleHistorySave();
	}

	void onFileDeleted(BufferId id)
	{
		const std::wstring path = knownPath(id);
		g_buffers.erase(id);
		g_pending.erase(id);
		g_openedBeforeReady.erase(id);
		g_activated.erase(id);
		if (g_wrappedScroll.id == id)
			g_wrappedScroll = WrappedScroll{};
		if (!g_ready || !g_store.settings().enabled)
			return;
		g_store.erase(path);
		scheduleHistorySave();
	}
}

void pluginInit(HANDLE hModule)
{
	g_instance = static_cast<HINSTANCE>(hModule);
}

void pluginCleanUp()
{
	flushHistoryNow();
	destroySaveWindow();
	g_ready = false;
	g_pending.clear();
	g_openedBeforeReady.clear();
	g_activated.clear();
	g_sessionFiles.clear();
	g_openedBeforeReadyKeys.clear();
	g_sessionKeys.clear();
	g_sessionRead = false;
	g_wrappedScroll = WrappedScroll{};
	g_buffers.clear();
	g_renameOld.clear();
}

const wchar_t* pluginDisplayName()
{
	return g_pluginName;
}

void commandMenuInit()
{
	setCommand(0, uiText(UiLang::English).saveCursor, toggleEnabled, nullptr, true);
	setCommand(1, uiText(UiLang::English).clearHistory, clearHistory);
	applyUiLanguage();
}

void commandMenuCleanUp()
{
}

bool setCommand(size_t index, const wchar_t* cmdName, PFUNCPLUGINCMD pFunc, ShortcutKey* sk, bool check0nInit)
{
	if (index >= nbFunc || !cmdName || !pFunc)
		return false;
	wcsncpy_s(funcItem[index]._itemName, menuItemSize, cmdName, _TRUNCATE);
	funcItem[index]._pFunc = pFunc;
	funcItem[index]._init2Check = check0nInit;
	funcItem[index]._pShKey = sk;
	return true;
}

void beNotifiedImpl(SCNotification* notifyCode)
{
	switch (notifyCode->nmhdr.code)
	{
	case SCN_UPDATEUI:
		onUpdateUi(reinterpret_cast<HWND>(notifyCode->nmhdr.hwndFrom), notifyCode->updated);
		break;
	case SCN_PAINTED:
		onPainted(reinterpret_cast<HWND>(notifyCode->nmhdr.hwndFrom));
		break;
	case NPPN_READY:
		onReady();
		break;
	case NPPN_FILEOPENED:
		onFileOpened(static_cast<BufferId>(notifyCode->nmhdr.idFrom));
		break;
	case NPPN_BUFFERACTIVATED:
		onBufferActivated(static_cast<BufferId>(notifyCode->nmhdr.idFrom));
		break;
	case NPPN_FILEBEFORECLOSE:
		onFileBeforeClose(static_cast<BufferId>(notifyCode->nmhdr.idFrom));
		break;
	case NPPN_FILECLOSED:
		onFileClosed(static_cast<BufferId>(notifyCode->nmhdr.idFrom));
		break;
	case NPPN_FILESAVED:
		onFileSaved(static_cast<BufferId>(notifyCode->nmhdr.idFrom));
		break;
	case NPPN_BEFORESHUTDOWN:
		onBeforeShutdown();
		break;
	case NPPN_FILEBEFORERENAME:
		onFileBeforeRename(static_cast<BufferId>(notifyCode->nmhdr.idFrom));
		break;
	case NPPN_FILERENAMECANCEL:
		onFileRenameCancel(static_cast<BufferId>(notifyCode->nmhdr.idFrom));
		break;
	case NPPN_FILERENAMED:
		onFileRenamed(static_cast<BufferId>(notifyCode->nmhdr.idFrom));
		break;
	case NPPN_FILEDELETED:
		onFileDeleted(static_cast<BufferId>(notifyCode->nmhdr.idFrom));
		break;
	case NPPN_NATIVELANGCHANGED:
		applyUiLanguage();
		break;
	case NPPN_SHUTDOWN:
		commandMenuCleanUp();
		pluginCleanUp();
		break;
	default:
		break;
	}
}

// Menu commands are called by Notepad++ directly, outside beNotified's guard.
void toggleEnabled()
{
	try
	{
		g_store.setEnabled(!g_store.settings().enabled);
		syncEnabledCheck();
	}
	catch (...)
	{
	}
}

void clearHistory()
{
	try
	{
		const UiText& text = uiText(g_uiLang);
		const int answer = ::MessageBoxW(nppData._nppHandle,
			text.clearHistoryQuestion,
			text.pluginName,
			MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
		if (answer != IDYES)
			return;
		g_store.clear();
		g_pending.clear();
		flushHistoryNow();
	}
	catch (...)
	{
	}
}
