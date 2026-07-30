#pragma once

#include <string>
#include <string_view>

namespace famo::runtime {

bool InstallTargetAllowed(std::wstring_view state,
                          std::wstring_view install_directory,
                          std::wstring_view module_directory,
                          bool allow_activating = false);
bool ActiveRuntimeProjectionAllowed(
    std::wstring_view state, std::wstring_view install_directory,
    std::wstring_view server_executable,
    bool allow_activating = false);
bool ResolveProductionRuntime(std::wstring *server_executable,
                              bool allow_activating = false);
bool ProductionInstallAllowed(std::wstring_view module_directory,
                              bool allow_activating = false);

} // namespace famo::runtime
