#pragma once

#include <string>

#include <msctf.h>

#include "com_ptr.h"
#include "famo_input_injection.h"
#include "famo_runtime_protocol.h"

namespace famo::tsf {

struct CompositionPlan {
  std::wstring commit;
  std::string preedit;
  uint32_t preedit_selection_start = 0;
  uint32_t preedit_selection_end = 0;
  uint32_t preedit_cursor = 0;
  uint32_t behavior_flags = 0;
};

class CompositionController {
public:
  HRESULT Apply(ITfContext *context, TfClientId client_id,
                const runtime::Composition &composition,
                ITfCompositionSink *sink);
  HRESULT Recover(ITfContext *context, TfClientId client_id,
                  std::string_view confirmed_preedit, ITfCompositionSink *sink);
  HRESULT End(ITfContext *context, TfClientId client_id);
  // Returns the range whose extent locates the caret, plus which edge of that
  // extent the caret sits on. The range is widened over one character where
  // possible because text stores cannot measure an empty one.
  HRESULT CloneLayoutCaret(TfEditCookie cookie, ITfContext *context,
                           ITfRange **range,
                           TfAnchor *caret_edge = nullptr) const;
  // Display-attribute atom published over the composition range. Hosts that
  // build their composition from GUID_PROP_ATTRIBUTE (every Chromium and XAML
  // text store) ignore a composition that carries no attribute property.
  void SetDisplayAttribute(TfGuidAtom atom) noexcept {
    display_attribute_atom_ = atom;
  }
  bool CompositionTerminated(ITfComposition *composition);
  void ObserveUnhandledKey(WPARAM key, bool down);
  void ResetBehaviorState();

private:
  friend class ApplyEditSession;
  friend class EndEditSession;

  HRESULT ApplyInSession(TfEditCookie cookie, ITfContext *context,
                         const CompositionPlan &plan, ITfCompositionSink *sink);
  HRESULT EndInSession(TfEditCookie cookie);
  HRESULT EndCurrent(TfEditCookie cookie);
  ArrowInjectionResult InjectArrow(WORD key, uint32_t count);

  ComPtr<ITfComposition> composition_;
  bool internal_end_ = false;
  wchar_t pending_close_ = 0;
  wchar_t previous_commit_ = 0;
  ULONGLONG injected_guard_until_ = 0;
  TfGuidAtom display_attribute_atom_ = TF_INVALID_GUIDATOM;
};

} // namespace famo::tsf
