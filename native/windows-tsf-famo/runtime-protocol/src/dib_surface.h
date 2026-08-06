#pragma once

// The layered-window presentation surface: a 32-bit top-down DIB section plus
// its memory DC. Both FamoCandidateUiPaint and FamoStatusBarPaint require a DIB
// section (they write premultiplied alpha straight into its buffer), and both
// windows present through UpdateLayeredWindow, so the two share one
// implementation here rather than each keeping a copy.

#include <cstring>

#include <windows.h>

namespace famo::runtime {

class DibSurface {
public:
  ~DibSurface() { Reset(); }

  bool Ensure(int width, int height) {
    if (dc_ && width_ == width && height_ == height)
      return true;
    Reset();
    if (width <= 0 || height <= 0)
      return false;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    bitmap_ = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS,
                               reinterpret_cast<void **>(&pixels_), nullptr, 0);
    dc_ = bitmap_ ? CreateCompatibleDC(nullptr) : nullptr;
    if (!dc_) {
      Reset();
      return false;
    }
    old_ = SelectObject(dc_, bitmap_);
    width_ = width;
    height_ = height;
    return true;
  }

  void Clear() {
    if (pixels_)
      std::memset(pixels_, 0, static_cast<size_t>(width_) * height_ * 4u);
  }
  HDC dc() const { return dc_; }
  int width() const { return width_; }
  int height() const { return height_; }

private:
  void Reset() {
    if (dc_) {
      if (old_)
        SelectObject(dc_, old_);
      DeleteDC(dc_);
    }
    if (bitmap_)
      DeleteObject(bitmap_);
    dc_ = nullptr;
    bitmap_ = nullptr;
    old_ = nullptr;
    pixels_ = nullptr;
    width_ = 0;
    height_ = 0;
  }

  HDC dc_ = nullptr;
  HBITMAP bitmap_ = nullptr;
  HGDIOBJ old_ = nullptr;
  uint32_t *pixels_ = nullptr;
  int width_ = 0;
  int height_ = 0;
};

// Present a painted surface at a screen position and keep the window topmost
// without activating it. Moving and showing are part of the same call because
// UpdateLayeredWindow's destination point is what actually moves the window.
inline bool SubmitLayered(HWND window, const DibSurface &surface, int x,
                          int y) {
  HDC screen = GetDC(nullptr);
  POINT destination{x, y};
  POINT source{0, 0};
  SIZE size{surface.width(), surface.height()};
  BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  const BOOL updated =
      UpdateLayeredWindow(window, screen, &destination, &size, surface.dc(),
                          &source, 0, &blend, ULW_ALPHA);
  ReleaseDC(nullptr, screen);
  if (!updated)
    return false;
  const HWND insert_after = GetWindow(window, GW_OWNER) ? HWND_TOP
                                                        : HWND_TOPMOST;
  return SetWindowPos(window, insert_after, 0, 0, 0, 0,
                      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                          SWP_SHOWWINDOW) != FALSE;
}

} // namespace famo::runtime
