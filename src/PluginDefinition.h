#pragma once

#include "PluginInterface.h"

const int nbFunc = 2;

const wchar_t* pluginDisplayName();

void pluginInit(HANDLE hModule);
void pluginCleanUp();
void commandMenuInit();
void commandMenuCleanUp();
bool setCommand(size_t index, const wchar_t* cmdName, PFUNCPLUGINCMD pFunc, ShortcutKey* sk = nullptr, bool check0nInit = false);

void beNotifiedImpl(SCNotification* notifyCode);

void toggleEnabled();
void clearHistory();
