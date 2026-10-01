@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0"

call :build
set "BUILD_RESULT=%ERRORLEVEL%"

echo.
if "%BUILD_RESULT%"=="0" (
  echo Build completed successfully.
) else (
  echo Build failed (exit code: %BUILD_RESULT%).
)
echo Press any key to close.
pause >nul
exit /b %BUILD_RESULT%

:build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo Visual Studio was not found. Install Visual Studio with the C++ desktop workload.
  exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
  echo Visual Studio with a C++ toolchain was not found.
  exit /b 1
)

call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1

if exist ".git" (
  echo --- PUBLIC_FILES ---
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\check_public_files.ps1 || exit /b 1
) else (
  echo --- PUBLIC_FILES ---
  echo skip: not a git repository yet
)

rem VS2026=v145, VS2022=v143. Use the newest installed toolset.
set "PLATFORM_TOOLSET="
if exist "%VSINSTALL%\VC\Auxiliary\Build\Microsoft.VCToolsVersion.v145.default.txt" set "PLATFORM_TOOLSET=v145"
if not defined PLATFORM_TOOLSET if exist "%VSINSTALL%\VC\Auxiliary\Build\Microsoft.VCToolsVersion.v143.default.txt" set "PLATFORM_TOOLSET=v143"
if not defined PLATFORM_TOOLSET set "PLATFORM_TOOLSET=v145"
echo Using PlatformToolset=%PLATFORM_TOOLSET%

set "TESTDIR=obj\x64\Release\Tests"
if not exist "%TESTDIR%" mkdir "%TESTDIR%" || exit /b 1
cl /nologo /c /std:c++17 /EHsc /utf-8 /W4 /O2 /I src ^
  /Fo:"%TESTDIR%\HistoryStore.obj" src\HistoryStore.cpp || exit /b 1
cl /nologo /c /std:c++17 /EHsc /utf-8 /W4 /O2 /I src ^
  /Fo:"%TESTDIR%\UiText.obj" src\UiText.cpp || exit /b 1
cl /nologo /c /std:c++17 /EHsc /utf-8 /W4 /O2 /I src ^
  /Fo:"%TESTDIR%\HistoryStore_test.obj" src\HistoryStore_test.cpp || exit /b 1
cl /nologo /Fe:"%TESTDIR%\NppRestoreCursorPositionTest.exe" ^
  "%TESTDIR%\HistoryStore.obj" "%TESTDIR%\UiText.obj" "%TESTDIR%\HistoryStore_test.obj" || exit /b 1
"%TESTDIR%\NppRestoreCursorPositionTest.exe" || exit /b 1

msbuild NppRestoreCursorPosition.sln /t:Rebuild /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=%PLATFORM_TOOLSET% /m || exit /b 1

set "OUT=bin\x64\Release\NppRestoreCursorPosition.dll"
if not exist "%OUT%" (
  echo Output DLL was not found: %OUT%
  exit /b 1
)

if exist "bin\x64\Release\NppRestoreCursorPosition.pdb" del /q "bin\x64\Release\NppRestoreCursorPosition.pdb"

echo.
echo Build succeeded: %OUT%
echo.
echo --- pre-release check (path strings) ---
findstr /I /L /M /C:"\Users" /C:"/Users/" "%OUT%" >nul 2>&1
if errorlevel 1 (
  echo OK: no Users path string found.
) else (
  echo WARNING: a Users-like path string may be embedded in the DLL.
)
echo.
echo --- pre-release check (version copyright) ---
powershell -NoProfile -ExecutionPolicy Bypass -File tools\check_version_info.ps1 -Rc NppRestoreCursorPosition.rc -Dll "%OUT%" -License LICENSE
if errorlevel 1 exit /b 1

echo.
echo --- pre-release check (arch and exports) ---
set "DUMPOUT=%TEMP%\NppRestoreCursorPosition_dumpbin.txt"
dumpbin /nologo /headers /exports "%OUT%" > "%DUMPOUT%" 2>&1
if errorlevel 1 (
  echo NG: dumpbin failed.
  exit /b 1
)

set "CHECK_NG=0"
findstr /L /C:"machine (x64)" "%DUMPOUT%" >nul || (echo NG: not x64.& set "CHECK_NG=1")
for %%F in (setInfo getName getFuncsArray beNotified messageProc isUnicode) do (
  findstr /L /C:" %%F" "%DUMPOUT%" >nul || (echo NG: missing export: %%F& set "CHECK_NG=1")
)
del /q "%DUMPOUT%" >nul 2>&1
if "%CHECK_NG%"=="1" exit /b 1
echo OK: x64, and the required 6 exports are present.

echo.
echo --- SHA-256 ---
certutil -hashfile "%OUT%" SHA256 | findstr /V /C:"CertUtil" /C:"SHA256"

echo.
echo Place NppRestoreCursorPosition.dll in Notepad++\plugins\NppRestoreCursorPosition\
exit /b 0
