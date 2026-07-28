#pragma once

#include <memory>
#include <thread>

#include "../../famo-candidate-ui/famo_candidate_ui.h"
#include "famo_runtime_service.h"

namespace famo::runtime {

struct PreviewSelection {
  uint32_t absolute_index = 0;
};

inline constexpr uint32_t kCandidateScrollTransitionMs = 180;

struct ScrollTransitionPlan {
  FamoRect clip{};
  int32_t row_step = 0;
  int32_t direction = 0; // +1 next page, -1 previous page
};

bool PreviewSelectionAt(const FamoLayoutResult &layout, int x, int y,
                        uint32_t page_index, uint32_t page_size,
                        PreviewSelection *selection) noexcept;

// Sends a click only to the message-only window owned by the exact
// authenticated TSF process. source_window is carried in WM_COPYDATA.wParam so
// the receiver can independently bind the click to this Runtime process.
bool SendPreviewSelectionToOwner(
    HWND source_window, const PreviewSelectionRequest &request,
    const PipeClientIdentity &selection_owner) noexcept;

bool PlanScrollTransition(const FamoLayoutResult &previous,
                          const FamoLayoutResult &next,
                          uint32_t previous_page, uint32_t next_page,
                          bool animations_enabled,
                          ScrollTransitionPlan *plan) noexcept;

int32_t ScrollTransitionOffset(uint32_t elapsed_ms,
                               int32_t row_step) noexcept;

class CandidateWindow final : public RuntimeSnapshotSink {
public:
  struct Counters {
    uint64_t duplicate = 0;
    uint64_t anchor_only = 0;
    uint64_t selection_only = 0;
    uint64_t full = 0;
    uint64_t device_recovery = 0;
    uint64_t mode_indicator = 0;
  };

  enum class Fault {
    None,
    Create,
    Layout,
    Paint,
    PaintAfterVisible,
    DeviceLossOnce,
    Submit,
    Hang
  };

  explicit CandidateWindow(Fault fault = Fault::None) : fault_(fault) {}
  ~CandidateWindow() override;
  CandidateWindow(const CandidateWindow &) = delete;
  CandidateWindow &operator=(const CandidateWindow &) = delete;

  bool Start();
  bool Prewarm();
  void Stop() noexcept;
  void
  Publish(std::shared_ptr<const RuntimeSnapshot> snapshot) noexcept override;
  bool
  PrepareStyle(std::string_view text, bool exists,
               std::shared_ptr<const void> *presentation) noexcept override;
  void ActivateStyle(
      std::shared_ptr<const RuntimeStyleState> style) noexcept override;
  void PrepareForRuntimeReady() noexcept override;
  Counters counters() const noexcept;

private:
  struct State;
  static void ThreadMain(std::shared_ptr<State> state) noexcept;

  std::shared_ptr<State> state_;
  std::thread thread_;
  Fault fault_;
};

} // namespace famo::runtime
