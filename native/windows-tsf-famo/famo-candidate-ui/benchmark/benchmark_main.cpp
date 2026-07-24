#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <limits>
#include <string>

#include "famo_benchmark.h"

namespace {

bool ParseUInt(const char* value, uint32_t* out) {
  if (!value || !*value || !out) return false;
  try {
    size_t used = 0;
    const unsigned long parsed = std::stoul(value, &used, 10);
    if (used != std::string(value).size() || parsed == 0 ||
        parsed > std::numeric_limits<uint32_t>::max())
      return false;
    *out = static_cast<uint32_t>(parsed);
    return true;
  } catch (...) {
    return false;
  }
}

int Usage(const char* message) {
  if (message) std::fprintf(stderr, "candidate_window_benchmark: %s\n", message);
  std::fprintf(stderr,
               "usage: candidate_window_benchmark --fixtures PATH --output DIR "
               "--iterations N --dpi N (--fixture ID|--all) "
               "(--skin ID|--all-skins) (--mode light|dark|--all-modes) "
               "[--replace-generated]\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  // Match the short-lived production candidate-worker bursts so scheduler
  // latency is not counted as renderer work in the warm paint budget.
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  famo::benchmark::Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--replace-generated") {
      options.replace_generated = true;
      continue;
    }
    if (arg == "--all") {
      options.all_fixtures = true;
      continue;
    }
    if (arg == "--all-skins") {
      options.all_skins = true;
      continue;
    }
    if (arg == "--all-modes") {
      options.all_modes = true;
      continue;
    }
    if (i + 1 >= argc) return Usage("missing option value");
    const char* value = argv[++i];
    if (arg == "--fixtures") options.fixtures_path = value;
    else if (arg == "--output") options.output_directory = value;
    else if (arg == "--iterations") {
      if (!ParseUInt(value, &options.iterations)) return Usage("invalid iterations");
    } else if (arg == "--dpi") {
      if (!ParseUInt(value, &options.dpi)) return Usage("invalid dpi");
    } else if (arg == "--fixture") options.fixture_id = value;
    else if (arg == "--skin") options.skin_id = value;
    else if (arg == "--mode") {
      const std::string mode = value;
      if (mode == "light") options.color_mode = famo::fixture::ColorMode::kLight;
      else if (mode == "dark") options.color_mode = famo::fixture::ColorMode::kDark;
      else return Usage("mode must be light or dark");
    } else {
      return Usage("unknown option");
    }
  }
  if (options.fixtures_path.empty() || options.output_directory.empty() ||
      options.iterations == 0 || options.dpi == 0 ||
      (!options.all_fixtures && options.fixture_id.empty()) ||
      (!options.all_skins && options.skin_id.empty())) {
    return Usage("all required options must be provided");
  }

  std::string error;
  const int result = famo::benchmark::Run(options, &error);
  if (result != 0) {
    std::fprintf(stderr, "candidate_window_benchmark: %s\n", error.c_str());
    return result;
  }
  std::printf("candidate_window_benchmark: %s\n", options.output_directory.c_str());
  return 0;
}
