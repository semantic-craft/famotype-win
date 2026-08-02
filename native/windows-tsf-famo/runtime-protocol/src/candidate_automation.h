#pragma once

#include <cstdint>
#include <memory>

#include <windows.h>

#include "../../famo-candidate-ui/famo_candidate_ui.h"
#include "famo_runtime_protocol.h"

namespace famo::runtime {

inline constexpr UINT kCandidateAutomationSelectMessage = WM_APP + 0x47;

// Owns the server-side UI Automation fragment for the custom layered
// candidate window. Rendering remains the source of truth; callers publish
// only after a frame is on screen and hide only after the HWND is hidden.
class CandidateAutomation final {
public:
  static std::unique_ptr<CandidateAutomation> Create(HWND window) noexcept;
  ~CandidateAutomation();
  CandidateAutomation(const CandidateAutomation &) = delete;
  CandidateAutomation &operator=(const CandidateAutomation &) = delete;

  LRESULT HandleGetObject(WPARAM wparam, LPARAM lparam) noexcept;
  void WindowDestroyed() noexcept;
  void Present(const Composition &composition, const FamoLayoutResult &layout,
               int shadow_margin) noexcept;
  void Move(const FamoLayoutResult &layout, int shadow_margin) noexcept;
  void Hide() noexcept;
  bool IsCurrentItem(uint32_t generation, uint32_t index) const noexcept;

private:
  struct Impl;
  explicit CandidateAutomation(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

} // namespace famo::runtime
