#pragma once

#include <windows.h>

namespace famo::tsf {

HRESULT RegisterDevelopmentProfile();
HRESULT RegisterMachineProfile();
HRESULT UnregisterDevelopmentProfile();
HRESULT UnregisterMachineProfile();

} // namespace famo::tsf
