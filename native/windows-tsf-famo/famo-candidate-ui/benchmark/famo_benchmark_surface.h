#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>

#include "famo_benchmark_internal.h"

namespace famo::benchmark::internal {

class DibSurface {
 public:
  ~DibSurface();
  bool Ensure(int width, int height, uint64_t* create_count);
  void Clear();
  void Reset();
  HDC dc() const { return dc_; }
  uint32_t* pixels() const { return pixels_; }
  int width() const { return width_; }
  int height() const { return height_; }
  uint64_t VisiblePixelCount() const;

 private:
  HDC dc_ = nullptr;
  HBITMAP bitmap_ = nullptr;
  HGDIOBJ old_ = nullptr;
  uint32_t* pixels_ = nullptr;
  int width_ = 0;
  int height_ = 0;
};

class LayeredHost {
 public:
  ~LayeredHost();
  bool Ensure();
  bool Submit(const DibSurface& surface, int x, int y);
  bool Move(int x, int y);
  void Reset();

 private:
  HWND window_ = nullptr;
};

class GdiplusSession {
 public:
  GdiplusSession();
  ~GdiplusSession();
  bool ok() const { return ok_; }

 private:
  ULONG_PTR token_ = 0;
  bool ok_ = false;
};

ResourceSnapshot ReadResources();
bool SavePng(const DibSurface& surface, const std::filesystem::path& path);

}  // namespace famo::benchmark::internal
