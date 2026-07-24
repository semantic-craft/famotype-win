#include "famo_benchmark_internal.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>

#ifndef FAMO_BENCHMARK_GIT_COMMIT
#define FAMO_BENCHMARK_GIT_COMMIT "unknown"
#endif
#ifndef FAMO_BENCHMARK_GIT_DIRTY
#define FAMO_BENCHMARK_GIT_DIRTY 1
#endif

namespace famo::benchmark::internal {
namespace {

struct StageStats {
  size_t count = 0;
  double p50 = 0;
  double p95 = 0;
  double p99 = 0;
  double max = 0;
};

StageStats Summarize(std::vector<double> samples) {
  StageStats result;
  if (samples.empty()) return result;
  std::sort(samples.begin(), samples.end());
  const auto percentile = [&](double p) {
    const size_t rank = static_cast<size_t>(std::ceil(p * samples.size()));
    return samples[(rank < 1 ? 1 : rank) - 1];
  };
  result.count = samples.size();
  result.p50 = percentile(0.50);
  result.p95 = percentile(0.95);
  result.p99 = percentile(0.99);
  result.max = samples.back();
  return result;
}

std::string JsonEscape(const std::string& value) {
  std::ostringstream out;
  for (unsigned char c : value) {
    switch (c) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (c < 0x20) {
          out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
              << static_cast<int>(c) << std::dec;
        } else {
          out << static_cast<char>(c);
        }
    }
  }
  return out.str();
}

std::string StatsJson(const StageStats& stats) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(3)
      << "{\"count\":" << stats.count << ",\"p50Us\":" << stats.p50
      << ",\"p95Us\":" << stats.p95 << ",\"p99Us\":" << stats.p99
      << ",\"maxUs\":" << stats.max << "}";
  return out.str();
}

std::string StagesJson(const StageSamples& samples) {
  std::ostringstream out;
  out << "{\"snapshot_prepare\":"
      << StatsJson(Summarize(samples.snapshot_prepare))
      << ",\"layout\":" << StatsJson(Summarize(samples.layout))
      << ",\"paint\":" << StatsJson(Summarize(samples.paint))
      << ",\"window_submit\":" << StatsJson(Summarize(samples.window_submit))
      << ",\"window_move\":" << StatsJson(Summarize(samples.window_move))
      << ",\"total\":" << StatsJson(Summarize(samples.total)) << "}";
  return out.str();
}

std::string TimestampUtc() {
  SYSTEMTIME time{};
  GetSystemTime(&time);
  char value[32]{};
  std::snprintf(value, sizeof(value), "%04u-%02u-%02uT%02u:%02u:%02uZ",
                time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
                time.wSecond);
  return value;
}

std::string MachineName() {
  char value[MAX_COMPUTERNAME_LENGTH + 1]{};
  DWORD size = static_cast<DWORD>(std::size(value));
  return GetComputerNameA(value, &size) ? std::string(value, size) : "unknown";
}

const char* BuildConfiguration() {
#ifdef NDEBUG
  return "Release";
#else
  return "Debug";
#endif
}

const char* Architecture() {
#ifdef _M_X64
  return "x64";
#elif defined(_M_ARM64)
  return "arm64";
#else
  return "unknown";
#endif
}

bool WriteText(const std::filesystem::path& path, const std::string& value) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(value.data(), static_cast<std::streamsize>(value.size()));
  return output.good();
}

int64_t Delta(uint64_t after, uint64_t before) {
  return static_cast<int64_t>(after) - static_cast<int64_t>(before);
}

std::string MatrixJson(const MatrixResult& item) {
  std::ostringstream out;
  out << "{\"fixtureId\":\"" << JsonEscape(item.fixture_id) << "\""
      << ",\"skinId\":\"" << JsonEscape(item.skin_id) << "\""
      << ",\"colorMode\":\"" << item.mode << "\""
      << ",\"dpi\":" << item.dpi << ",\"layout\":\"" << item.layout << "\""
      << ",\"interaction\":\"" << item.interaction << "\""
      << ",\"iterations\":" << item.iterations
      << ",\"cold\":" << StagesJson(item.cold)
      << ",\"warm\":" << StagesJson(item.warm)
      << ",\"resources\":{\"gdiObjects\":{\"before\":"
      << item.before.gdi_objects << ",\"after\":" << item.after.gdi_objects
      << ",\"delta\":" << Delta(item.after.gdi_objects, item.before.gdi_objects)
      << "},\"workingSetBytes\":{\"before\":" << item.before.working_set_bytes
      << ",\"after\":" << item.after.working_set_bytes
      << ",\"peakAfter\":" << item.after.peak_working_set_bytes
      << ",\"delta\":"
      << Delta(item.after.working_set_bytes, item.before.working_set_bytes)
      << ",\"peakDelta\":"
      << Delta(item.after.peak_working_set_bytes, item.before.working_set_bytes)
      << "},\"creates\":{\"hostSurface\":" << item.host_surface_creates
      << ",\"textSurface\":" << item.render_creates.text_surface
      << ",\"d2dTarget\":" << item.render_creates.d2d_target
      << ",\"brush\":" << item.render_creates.brush
      << ",\"textLayout\":" << item.render_creates.text_layout << "}}"
      << ",\"visiblePixelCount\":" << item.visible_pixel_count
      << ",\"capturePath\":\"" << JsonEscape(item.capture_path) << "\""
      << ",\"status\":\"" << item.status << "\"";
  if (!item.error.empty()) out << ",\"error\":\"" << JsonEscape(item.error) << "\"";
  out << "}";
  return out.str();
}

std::string BenchmarkJson(const std::vector<MatrixResult>& matrix,
                          uint32_t manifest_version) {
  std::ostringstream out;
  out << "{\"schemaVersion\":1,\"generatedAtUtc\":\"" << TimestampUtc()
      << "\",\"gitCommit\":\"" << FAMO_BENCHMARK_GIT_COMMIT << "\""
      << ",\"gitDirty\":" << (FAMO_BENCHMARK_GIT_DIRTY ? "true" : "false")
      << ",\"buildConfiguration\":\"" << BuildConfiguration() << "\""
      << ",\"architecture\":\"" << Architecture() << "\""
      << ",\"renderer\":\"windows-current-native\""
      << ",\"host\":\"synthetic-layered-hwnd\""
      << ",\"machine\":\"" << JsonEscape(MachineName()) << "\""
      << ",\"fixtureManifestVersion\":" << manifest_version << ",\"matrix\":[";
  for (size_t i = 0; i < matrix.size(); ++i) {
    if (i) out << ',';
    out << MatrixJson(matrix[i]);
  }
  out << "]}";
  return out.str();
}

std::string CaptureJson(const MatrixResult& item) {
  std::ostringstream out;
  out << "{\"fixtureId\":\"" << JsonEscape(item.fixture_id) << "\""
      << ",\"skinId\":\"" << JsonEscape(item.skin_id) << "\""
      << ",\"colorMode\":\"" << item.mode << "\",\"dpi\":" << item.dpi
      << ",\"layout\":\"" << item.layout << "\""
      << ",\"renderer\":\"windows-current-native\""
      << ",\"host\":\"synthetic-layered-hwnd\""
      << ",\"path\":\"" << JsonEscape(item.capture_path) << "\""
      << ",\"capabilities\":{\"sourceProvenance\":\"unsupported-current\""
      << ",\"independentHover\":\"unsupported-current\""
      << ",\"pageDots\":\"unsupported-current\""
      << ",\"material\":\"unsupported-current\""
      << ",\"expandedForm\":\"mapped-to-vertical-current\"}}";
  return out.str();
}

std::string CapturesJson(const std::vector<MatrixResult>& matrix) {
  std::ostringstream out;
  out << "{\"schemaVersion\":1,\"captures\":[";
  bool first = true;
  for (const auto& item : matrix) {
    if (item.status != "ok") continue;
    if (!first) out << ',';
    first = false;
    out << CaptureJson(item);
  }
  out << "]}";
  return out.str();
}

}  // namespace

bool OutputDirectoryReady(const Options& options, std::string* error) {
  std::error_code ec;
  const std::filesystem::path path(options.output_directory);
  if (std::filesystem::exists(path, ec)) {
    if (!std::filesystem::is_directory(path, ec)) {
      *error = "output path is not a directory";
      return false;
    }
    if (!options.replace_generated &&
        std::filesystem::directory_iterator(path, ec) !=
            std::filesystem::directory_iterator()) {
      *error = "output directory is not empty (use --replace-generated)";
      return false;
    }
  } else if (!std::filesystem::create_directories(path, ec) || ec) {
    *error = "cannot create output directory";
    return false;
  }
  return true;
}

bool WriteArtifacts(const std::filesystem::path& output,
                    const std::vector<MatrixResult>& matrix,
                    uint32_t manifest_version) {
  return WriteText(output / "benchmark-v1.json",
                   BenchmarkJson(matrix, manifest_version)) &&
         WriteText(output / "captures-v1.json", CapturesJson(matrix));
}

}  // namespace famo::benchmark::internal
