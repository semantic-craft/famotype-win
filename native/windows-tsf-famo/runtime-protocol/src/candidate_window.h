#pragma once

#include <memory>
#include <thread>

#include "../../famo-candidate-ui/famo_candidate_ui.h"
#include "famo_runtime_service.h"

namespace famo::runtime {

struct PreviewSelection {
  uint32_t pages_forward = 0;
  uint32_t candidate_offset = 0;
};

bool PreviewSelectionAt(const FamoLayoutResult &layout, int x, int y,
                        uint32_t page_size,
                        PreviewSelection *selection) noexcept;

class CandidateWindow final : public RuntimeSnapshotSink {
public:
  struct Counters {
    uint64_t duplicate = 0;
    uint64_t anchor_only = 0;
    uint64_t selection_only = 0;
    uint64_t full = 0;
    uint64_t device_recovery = 0;
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
  void Stop();
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
