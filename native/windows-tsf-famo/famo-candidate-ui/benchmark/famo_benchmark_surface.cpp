#include "famo_benchmark_surface.h"

#include <objidl.h>
#include <psapi.h>
#define GDIPVER 0x0110
#include <gdiplus.h>

#include <cstring>
#include <cwchar>
#include <vector>

namespace famo::benchmark::internal {

DibSurface::~DibSurface() { Reset(); }

bool DibSurface::Ensure(int width, int height, uint64_t* create_count) {
  if (dc_ && width_ == width && height_ == height) return true;
  Reset();
  if (width <= 0 || height <= 0) return false;
  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = width;
  bmi.bmiHeader.biHeight = -height;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  HDC screen = GetDC(nullptr);
  bitmap_ = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS,
                             reinterpret_cast<void**>(&pixels_), nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (!bitmap_ || !pixels_) {
    Reset();
    return false;
  }
  dc_ = CreateCompatibleDC(nullptr);
  if (!dc_) {
    Reset();
    return false;
  }
  old_ = SelectObject(dc_, bitmap_);
  width_ = width;
  height_ = height;
  if (create_count) ++*create_count;
  return true;
}

void DibSurface::Clear() {
  if (pixels_)
    std::memset(pixels_, 0, static_cast<size_t>(width_) * height_ * 4u);
}

void DibSurface::Reset() {
  if (dc_) {
    if (old_) SelectObject(dc_, old_);
    DeleteDC(dc_);
  }
  if (bitmap_) DeleteObject(bitmap_);
  dc_ = nullptr;
  bitmap_ = nullptr;
  old_ = nullptr;
  pixels_ = nullptr;
  width_ = 0;
  height_ = 0;
}

uint64_t DibSurface::VisiblePixelCount() const {
  if (!pixels_ || width_ <= 0 || height_ <= 0) return 0;
  const size_t count = static_cast<size_t>(width_) * height_;
  uint64_t visible = 0;
  for (size_t i = 0; i < count; ++i) {
    if ((pixels_[i] >> 24) != 0) ++visible;
  }
  return visible;
}

LayeredHost::~LayeredHost() { Reset(); }

bool LayeredHost::Ensure() {
  if (window_) return true;
  const wchar_t* class_name = L"FamoCandidateBenchmarkLayeredHost";
  WNDCLASSW wc{};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = class_name;
  if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    return false;
  window_ = CreateWindowExW(
      WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, class_name, L"",
      WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  return window_ != nullptr;
}

bool LayeredHost::Submit(const DibSurface& surface, int x, int y) {
  if (!window_ || !surface.dc()) return false;
  HDC screen = GetDC(nullptr);
  POINT destination{x, y};
  POINT source{0, 0};
  SIZE size{surface.width(), surface.height()};
  BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  const BOOL ok = UpdateLayeredWindow(window_, screen, &destination, &size,
                                      surface.dc(), &source, 0, &blend, ULW_ALPHA);
  ReleaseDC(nullptr, screen);
  return ok != FALSE;
}

bool LayeredHost::Move(int x, int y) {
  return window_ && SetWindowPos(window_, HWND_TOPMOST, x, y, 0, 0,
                                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
}

void LayeredHost::Reset() {
  if (window_) DestroyWindow(window_);
  window_ = nullptr;
}

GdiplusSession::GdiplusSession() {
  Gdiplus::GdiplusStartupInput input;
  ok_ = Gdiplus::GdiplusStartup(&token_, &input, nullptr) == Gdiplus::Ok;
}

GdiplusSession::~GdiplusSession() {
  if (ok_) Gdiplus::GdiplusShutdown(token_);
}

ResourceSnapshot ReadResources() {
  ResourceSnapshot result;
  result.gdi_objects = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
  PROCESS_MEMORY_COUNTERS counters{};
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
    result.working_set_bytes = static_cast<uint64_t>(counters.WorkingSetSize);
    result.peak_working_set_bytes =
        static_cast<uint64_t>(counters.PeakWorkingSetSize);
  }
  return result;
}

namespace {

bool GetPngEncoder(CLSID* out) {
  UINT count = 0;
  UINT bytes = 0;
  if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok || bytes == 0)
    return false;
  std::vector<BYTE> storage(bytes);
  auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
  if (Gdiplus::GetImageEncoders(count, bytes, codecs) != Gdiplus::Ok) return false;
  for (UINT i = 0; i < count; ++i) {
    if (std::wcscmp(codecs[i].MimeType, L"image/png") == 0) {
      *out = codecs[i].Clsid;
      return true;
    }
  }
  return false;
}

}  // namespace

bool SavePng(const DibSurface& surface, const std::filesystem::path& path) {
  CLSID encoder{};
  if (!surface.pixels() || !GetPngEncoder(&encoder)) return false;
  Gdiplus::Bitmap bitmap(surface.width(), surface.height(), surface.width() * 4,
                         PixelFormat32bppPARGB,
                         reinterpret_cast<BYTE*>(surface.pixels()));
  return bitmap.Save(path.c_str(), &encoder, nullptr) == Gdiplus::Ok;
}

}  // namespace famo::benchmark::internal
