#pragma once

#include <msctf.h>

namespace famo::tsf {

// The composition's visual style, published over GUID_PROP_ATTRIBUTE. Hosts
// that rebuild the composition from that property (Chromium and XAML text
// stores) resolve the property's atom back to this object through
// ITfDisplayAttributeMgr; a composition whose atom cannot be resolved is
// dropped by those hosts instead of being handed to their renderer.
HRESULT CreateCompositionDisplayAttributeInfo(
    ITfDisplayAttributeInfo **info) noexcept;

HRESULT CreateDisplayAttributeInfoEnum(
    IEnumTfDisplayAttributeInfo **enumerator) noexcept;

} // namespace famo::tsf
