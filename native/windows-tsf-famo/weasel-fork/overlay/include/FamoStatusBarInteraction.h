#pragma once

#include <vector>

struct FamoStatusBarPoint {
  int x = 0;
  int y = 0;
};

struct FamoStatusBarRect {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  bool Contains(FamoStatusBarPoint p) const {
    return p.x >= left && p.x < right && p.y >= top && p.y < bottom;
  }
};

struct FamoStatusBarClick {
  bool has_value = false;
  int index = -1;
};

// Pure interaction state for the floating status bar. Win32 owns capture,
// dragging and drawing; this model owns button hover/press/cancel semantics.
class FamoStatusBarInteractionModel {
 public:
  void SetButtonRects(std::vector<FamoStatusBarRect> rects) {
    rects_ = std::move(rects);
    if (hover_index_ >= static_cast<int>(rects_.size()))
      hover_index_ = -1;
    if (press_index_ >= static_cast<int>(rects_.size()))
      ClearPress();
  }

  int HoverIndex() const { return hover_index_; }
  int PressIndex() const { return press_index_; }
  bool PressOutside() const { return press_outside_; }

  bool MouseMove(FamoStatusBarPoint p) {
    const int next_hover = HitTest(p);
    bool changed = false;
    if (press_index_ >= 0 && next_hover != press_index_ && !press_outside_) {
      press_outside_ = true;
      changed = true;
    }
    if (next_hover != hover_index_) {
      hover_index_ = next_hover;
      changed = true;
    }
    return changed;
  }

  bool MouseLeave() {
    bool changed = false;
    if (hover_index_ != -1) {
      hover_index_ = -1;
      changed = true;
    }
    if (press_index_ >= 0 && !press_outside_) {
      press_outside_ = true;
      changed = true;
    }
    return changed;
  }

  void LeftDown(FamoStatusBarPoint p) {
    hover_index_ = HitTest(p);
    press_index_ = hover_index_;
    press_outside_ = false;
  }

  FamoStatusBarClick LeftUp(FamoStatusBarPoint p, bool was_drag) {
    const int release_index = HitTest(p);
    const int pressed_index = press_index_;
    const bool cancelled = press_outside_;
    ClearPress();
    hover_index_ = release_index;
    if (was_drag || pressed_index < 0 || cancelled ||
        release_index != pressed_index) {
      return {};
    }
    return {true, pressed_index};
  }

  bool CaptureChanged() {
    const bool changed =
        hover_index_ != -1 || press_index_ != -1 || press_outside_;
    hover_index_ = -1;
    ClearPress();
    return changed;
  }

 private:
  int HitTest(FamoStatusBarPoint p) const {
    for (size_t i = 0; i < rects_.size(); ++i) {
      if (rects_[i].Contains(p))
        return static_cast<int>(i);
    }
    return -1;
  }

  void ClearPress() {
    press_index_ = -1;
    press_outside_ = false;
  }

  std::vector<FamoStatusBarRect> rects_;
  int hover_index_ = -1;
  int press_index_ = -1;
  bool press_outside_ = false;
};
