#pragma once

#include <string>

#include <windows.h>

namespace famo::tsf {

void SetModuleHandle(HMODULE module);
HMODULE ModuleHandle();
std::wstring ModulePath();
std::wstring ModuleDirectory();
void AddModuleObject();
void RemoveModuleObject();
void AddServerLock();
void RemoveServerLock();
bool ModuleCanUnload();

} // namespace famo::tsf
