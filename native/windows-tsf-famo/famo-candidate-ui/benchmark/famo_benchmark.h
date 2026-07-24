#pragma once

#include <cstdint>
#include <string>

#include "famo_fixture.h"

namespace famo::benchmark {

struct Options {
  std::string fixtures_path;
  std::string output_directory;
  uint32_t iterations = 0;
  uint32_t dpi = 96;
  std::string fixture_id;
  std::string skin_id;
  fixture::ColorMode color_mode = fixture::ColorMode::kLight;
  bool all_fixtures = false;
  bool all_skins = false;
  bool all_modes = false;
  bool replace_generated = false;
};

// Returns 0 on success, 2 for argument/fixture contract errors, 3 for
// renderer/Win32 failures, and 4 for output failures.
int Run(const Options& options, std::string* error);

}  // namespace famo::benchmark
