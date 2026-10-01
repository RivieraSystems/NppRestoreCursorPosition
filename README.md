# NppRestoreCursorPosition

[日本語](#日本語) | [English](#english)

`Notepad++` plugin that remembers the caret position of a closed file and restores it the next time that file is opened.  
閉じたファイルのカーソル位置を覚え、次に開いたときに戻す `Notepad++` プラグインです。

---

## 日本語

### 概要

`Notepad++` 本体は、タブを開いたまま終了したファイルの位置をセッションに残します。  
タブを閉じてから開き直したファイルは、標準では先頭に戻ります。

このプラグインはその穴だけを埋めます。  
ディスク上のファイルを閉じた位置を、次に開いて最初に前面へ出したときに 1 回だけ戻します。  
履歴の戻る／進むや、編集箇所の一覧は持ちません。

### 機能

- 閉じた実ファイルのキャレット位置と先頭の文書行を覚えます。
- 同じファイルを開き直すと、その位置へ戻します。
- メニューは `Notepad++` の UI 言語に合わせます。日本語のときは プラグイン → カーソル位置復元、それ以外は Plugins → NppRestoreCursorPosition です。
  - カーソル位置を保存 / Save cursor position（有効／無効）
  - 履歴をクリア / Clear history
- 履歴の上限は 2000 ファイルです。古いものから捨てます。
- 未保存のタブ（`new 1` など）は対象外です。

### 動作環境

- `Notepad++` 64-bit（Windows）

### インストール

1. `build.bat` でビルドした `bin\x64\Release\NppRestoreCursorPosition.dll` を用意します。
2. 次のフォルダを作成し、DLL を配置します。  
   `<Notepad++のインストールフォルダ>\plugins\NppRestoreCursorPosition\NppRestoreCursorPosition.dll`  
   例: `C:\Program Files\Notepad++\plugins\NppRestoreCursorPosition\NppRestoreCursorPosition.dll`
3. `Notepad++` を再起動します。

> フォルダ名と DLL 名は一致させてください。`Notepad++` は `plugins\<DLL名>\<DLL名>.dll` の形でのみプラグインを読み込みます。

履歴は次の場所に保存します。  
`<Notepad++の設定フォルダ>\plugins\config\NppRestoreCursorPosition\history.json`

同じフォルダの `history.json.lock` は、複数の `Notepad++` が同時に保存するときの順番待ちに使います。  
壊れた `history.json` は、起動時に `history.json.bak` へ移してから作り直します。  
新しい版のプラグインが書いた `history.json` は書き換えません。

### 使い方

普段どおりファイルを閉じて、あとから同じファイルを開きます。  
メニュー操作は不要です。

セッション記憶をオンにしたまま終了したタブは、本体のセッションに任せます。  
起動時に本体がセッションから開いたファイルは、先頭の位置で保存されていても戻しません。  
あとから読み込んだセッションや、`-n` で行を指定して開いたファイルも、本体が位置を決めていれば上書きしません。

### 既知の制限

- 選択範囲は復元しません。キャレットと先頭の文書行だけです。
- 折り返し表示では、同じ行の何番目の折り返し行を表示していたかまでは復元しません。
- 開いたときに本体が位置を決めたタブ（セッション、`-n` の行指定など）には手を出しません。戻すのは、開いた時点で文書の先頭のままだったファイルだけです。
- あとから読み込んだセッションや `-n` で文書の先頭を指定されたファイルは、普通に開いたファイルと区別できません。履歴があれば、その位置へ戻します。
- 起動時にセッションを読み込んだかどうかは、`-nosession` の指定と、セッションに残っていてディスクにもあるファイルが起動時にすべて開かれたかで判断します。セッションを読み込まない起動でも、それらのファイルをすべて開いたときは、セッションから開いたものとして扱い、戻しません。
- セッションを読み込んだ起動でも、セッションにあるファイルが 1 つでも開けなかったときは、セッションを読み込まなかったものとして扱います。このとき、セッションで文書の先頭に保存されていたタブは、履歴があればその位置へ戻ります。
- 閉じたあとに外部で書き換えたファイルは、保存したバイト位置にある文字の先頭へ戻します。元の行からずれることがあります。
- 一度も前面に出さずに閉じたタブは記録しません。

### ビルド方法2つ

ビルドには Visual Studio の「`C++` によるデスクトップ開発」が必要です。  
Visual Studio インストール時に「`C++` によるデスクトップ開発」を選択してください。

- batファイル
1. `build.bat` を実行します。

- Visual Studio
1. `NppRestoreCursorPosition.sln` を開きます。
2. ツールバーで Release と x64 を選びます。
3. メニューから ビルド → ソリューションのビルド を実行します。

いずれも出力は `bin\x64\Release\NppRestoreCursorPosition.dll` です。

### 公開ファイル

GitHub に載せるファイルは [PUBLIC_FILES](PUBLIC_FILES) が正本です。  
`build.bat` は追跡中のファイルをこの一覧と突き合わせ、余分があれば失敗します。

### ライセンス

[MIT License](LICENSE) です。Copyright (c) 2026 RivieraSystems  
著作権者は `LICENSE` と `NppRestoreCursorPosition.rc` の `RCP_COMPANY` / `RCP_COPYRIGHT` の 3 箇所に書いています。  
DLL のバージョンは `NppRestoreCursorPosition.rc` 冒頭の `RCP_VER_*` で定義しています。リリースのたびに更新してください。

---

## English

### Overview

`Notepad++` itself stores caret positions for files that stay open in the session.  
A file that you close and later reopen starts at the top of the document.

This plugin fills only that gap.  
It remembers the last position of a file on disk and restores it once, the first time that file is brought to the front after you open it.  
It is not a back/forward location history.

### Features

- Remembers the caret and the top document line of a closed file
- Restores that position when the same file is opened again
- The menu follows the `Notepad++` UI language. Japanese uses プラグイン → カーソル位置復元; every other language uses Plugins → NppRestoreCursorPosition
  - Save cursor position / カーソル位置を保存 (on/off)
  - Clear history / 履歴をクリア
- History is capped at 2000 files; the oldest entries are dropped
- Untitled tabs (`new 1` and the like) are ignored

### Requirements

- `Notepad++` 64-bit (Windows)

### Installation

1. Build with `build.bat` and take `bin\x64\Release\NppRestoreCursorPosition.dll`.
2. Place it here:  
   `<Notepad++ install dir>\plugins\NppRestoreCursorPosition\NppRestoreCursorPosition.dll`  
   Example: `C:\Program Files\Notepad++\plugins\NppRestoreCursorPosition\NppRestoreCursorPosition.dll`
3. Restart `Notepad++`

> The folder name must match the DLL name. `Notepad++` only loads plugins from `plugins\<DllName>\<DllName>.dll`.

History is stored here:  
`<Notepad++ config dir>\plugins\config\NppRestoreCursorPosition\history.json`

`history.json.lock` in the same folder makes saves from several `Notepad++` instances take turns.  
A corrupt `history.json` is moved to `history.json.bak` at startup and a new one is started.  
A `history.json` written by a newer version of the plugin is never overwritten.

### Usage

Close files as usual, then open the same file later.  
No menu action is required for restore.

Tabs that stay open across a quit are left to the `Notepad++` session.  
Files that `Notepad++` opens from its session at startup are not restored, even when the session saved them at the top.  
A session loaded later and a file opened at a given line with `-n` are not overwritten either, as long as `Notepad++` has moved them.

### Known limitations

- The selection is not restored; only the caret and the top document line
- With word wrap on, which wrapped row within a document line was at the top of the view is not restored
- Tabs that `Notepad++` positions as they open (a session, a `-n` line number, and so on) are left alone. Only a file that is still at the top of the document when it opens is restored
- A file that a later-loaded session or `-n` puts at the top of the document cannot be told apart from a plain open. If it has history, it is restored there
- Whether the session was loaded at startup is judged from `-nosession` and from whether every file left in the session that still exists was opened at startup. If the session was not loaded but all of those files were opened anyway, they are treated as opened from the session and are not restored
- If the session was loaded but any file in it failed to open, the session is treated as not loaded. A tab that the session saved at the top of the document is then restored from history, if it has any
- If a file was changed outside `Notepad++` after it was closed, the caret goes to the start of the character at the saved byte offset, which may be on a different line than before
- A tab that is closed without ever being brought to the front is not recorded

### Two ways to build

Building requires the Desktop development with `C++` workload in Visual Studio.  
When you install Visual Studio, please select Desktop development with `C++`.

- bat file
1. Run `build.bat`.

- Visual Studio
1. Open `NppRestoreCursorPosition.sln`.
2. On the toolbar, select Release and x64.
3. From the menu, run Build → Build Solution.

Either way, the output is `bin\x64\Release\NppRestoreCursorPosition.dll`.

### Published files

The files that belong on GitHub are listed in [PUBLIC_FILES](PUBLIC_FILES).  
`build.bat` compares the Git-tracked files with that list and fails if anything extra is present.

### License

[MIT License](LICENSE). Copyright (c) 2026 RivieraSystems  
The copyright holder appears in three places: `LICENSE`, and `RCP_COMPANY` / `RCP_COPYRIGHT` in `NppRestoreCursorPosition.rc`.  
The DLL version is defined by `RCP_VER_*` at the top of `NppRestoreCursorPosition.rc`; bump it for each release.
