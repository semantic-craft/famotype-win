#include "famo_runtime_service.h"

#include <algorithm>
#include <cctype>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <set>
#include <utility>
#include <vector>

#include <windows.h>

#include "runtime_style_config.h"

namespace famo::runtime {
namespace {

FamoUtf8String EngineString(std::string_view value) {
  return {static_cast<uint32_t>(sizeof(FamoUtf8String)), value.data(),
          static_cast<uint32_t>(value.size())};
}

std::string_view Trim(std::string_view value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front())))
    value.remove_prefix(1);
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back())))
    value.remove_suffix(1);
  return value;
}

bool SafeName(std::string_view value) {
  return !value.empty() && value.size() <= 128 &&
         std::all_of(value.begin(), value.end(), [](char ch) {
           const auto c = static_cast<unsigned char>(ch);
           return std::isalnum(c) || ch == '_' || ch == '-' || ch == '.';
         });
}

bool Utf8Path(std::string_view root, std::wstring_view name,
              std::filesystem::path *path) {
  if (!path || !IsValidUtf8(root))
    return false;
  const int needed =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, root.data(),
                          static_cast<int>(root.size()), nullptr, 0);
  if (needed <= 0)
    return false;
  std::wstring wide(static_cast<size_t>(needed), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, root.data(),
                          static_cast<int>(root.size()), wide.data(),
                          needed) != needed)
    return false;
  *path = std::filesystem::path(wide) / name;
  return true;
}

bool ReadSmallFile(const std::filesystem::path &path, std::string *text) {
  if (!text)
    return false;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    text->clear();
    return !ec;
  }
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size > kMaxFrameSize)
    return false;
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return false;
  text->assign(std::istreambuf_iterator<char>(input),
               std::istreambuf_iterator<char>());
  return input.good() || input.eof();
}

bool ReadOptions(std::string_view root, std::map<std::string, bool> *options) {
  std::filesystem::path path;
  std::string text;
  if (!options || !Utf8Path(root, L"famo-options.yaml", &path) ||
      !ReadSmallFile(path, &text) || !IsValidUtf8(text))
    return false;
  options->clear();
  static const std::set<std::string_view> kAllowed = {
      "ascii_mode",         "ascii_punct", "full_shape",
      "traditionalization", "zh_trad",     "emoji"};
  bool saw_options = false;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t end = text.find('\n', start);
    std::string_view line(text.data() + start,
                          (end == std::string::npos ? text.size() : end) -
                              start);
    if (!line.empty() && line.back() == '\r')
      line.remove_suffix(1);
    const std::string_view trimmed = Trim(line);
    if (!trimmed.empty() && trimmed.front() != '#') {
      if (line == "options:" && !saw_options) {
        saw_options = true;
      } else {
        if (!saw_options || line.size() < 3 || line.substr(0, 2) != "  " ||
            std::isspace(static_cast<unsigned char>(line[2])))
          return false;
        const std::string_view child = line.substr(2);
        const size_t colon = child.find(':');
        const std::string_view name = colon == std::string_view::npos
                                          ? std::string_view{}
                                          : child.substr(0, colon);
        const std::string_view value = colon == std::string_view::npos
                                           ? std::string_view{}
                                           : Trim(child.substr(colon + 1));
        if (!SafeName(name) || !kAllowed.contains(name) ||
            (value != "true" && value != "false") ||
            options->contains(std::string(name)))
          return false;
        (*options)[std::string(name)] = value == "true";
      }
    }
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  return text.empty() || saw_options;
}

bool ReadSelectedSchema(std::string_view root, std::string *schema) {
  std::filesystem::path path;
  std::string text;
  if (!schema || !Utf8Path(root, L"famo-select-schema.txt", &path) ||
      !ReadSmallFile(path, &text) || !IsValidUtf8(text))
    return false;
  const std::string_view value = Trim(text);
  // SafeName rejects embedded newlines while Trim permits one trailing line
  // end.
  if (!value.empty() && !SafeName(value))
    return false;
  *schema = std::string(value);
  return true;
}

constexpr std::wstring_view kUserDatabaseSuffix = L".userdb";

bool TestSwitch(const wchar_t *name) {
  wchar_t value[2]{};
  return GetEnvironmentVariableW(name, value,
                                 static_cast<DWORD>(std::size(value))) > 0;
}

bool UserDatabases(const std::filesystem::path &root,
                   std::vector<std::filesystem::path> *result) {
  if (!result || TestSwitch(L"FAMO_TEST_USERDB_ENUMERATION_DENIED"))
    return false;
  result->clear();
  std::error_code ec;
  for (std::filesystem::directory_iterator it(root, ec), end;
       !ec && it != end; it.increment(ec)) {
    const std::wstring name = it->path().filename().wstring();
    const bool directory = it->is_directory(ec);
    if (ec)
      break;
    if (directory && name.size() > kUserDatabaseSuffix.size() &&
        name.ends_with(kUserDatabaseSuffix))
      result->push_back(it->path());
  }
  if (ec) {
    result->clear();
    return false;
  }
  std::sort(result->begin(), result->end());
  return true;
}

std::filesystem::path UserDictionaryBackup(const std::filesystem::path &root) {
  SYSTEMTIME now{};
  GetLocalTime(&now);
  wchar_t timestamp[32]{};
  swprintf_s(timestamp, std::size(timestamp), L"%04u%02u%02u-%02u%02u%02u-%03u",
             now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
             now.wSecond, now.wMilliseconds);
  return root / L".famo-backup" /
         (std::wstring(L"userdb-reset-") + timestamp);
}

bool CopyDirectory(const std::filesystem::path &source,
                   const std::filesystem::path &destination) {
  std::error_code ec;
  std::filesystem::copy(source, destination,
                        std::filesystem::copy_options::recursive, ec);
  return !ec;
}

bool RestoreUserDatabases(const std::vector<std::filesystem::path> &databases,
                          const std::filesystem::path &backup) {
  bool restored = true;
  bool inject_failure = TestSwitch(L"FAMO_TEST_USERDB_RESTORE_FAILURE");
  for (const auto &database : databases) {
    std::error_code ec;
    std::filesystem::remove_all(database, ec);
    if (ec || inject_failure ||
        !CopyDirectory(backup / database.filename(), database))
      restored = false;
    inject_failure = false;
  }
  return restored;
}

} // namespace

bool RuntimeService::ApplyOptionsLocked(
    FamoEngineContext *context,
    const std::map<std::string, bool> &options) const {
  if (!context)
    return false;
  for (const auto &[name, value] : options) {
    const FamoUtf8String option = EngineString(name);
    if (engine_.api().set_option(context, &option, value ? 1 : 0) !=
        FAMO_ENGINE_OK)
      return false;
  }
  return true;
}

bool RuntimeService::SetOption(std::string_view name, bool value) {
  const std::map<std::string, bool> option{{std::string(name), value}};
  std::lock_guard lock(mutex_);
  // Start() refuses an engine without the v1.1 table, so a started service
  // always has get_status available here.
  if (!started_)
    return false;
  bool applied = false;
  for (auto &[key, session] : sessions_) {
    (void)key;
    if (!session.context || !ApplyOptionsLocked(session.context, option))
      continue;
    // set_option reports only success, so pull the new status back through a
    // view that consumes nothing -- the same refresh the thin session ops use.
    Composition composition;
    if (ReadStatusLocked(session.context, &composition))
      session.composition = std::move(composition);
    Publish(session, true);
    applied = true;
  }
  if (applied)
    options_[std::string(name)] = value;
  return applied;
}

bool RuntimeService::ReplaceContextsLocked(
    std::string_view schema, const std::map<std::string, bool> &options) {
  struct Replacement {
    Session *session;
    FamoEngineContext *context;
    Composition composition;
  };
  std::vector<Replacement> replacements;
  replacements.reserve(sessions_.size());
  const FamoUtf8String engine_schema = EngineString(schema);
  for (auto &[key, session] : sessions_) {
    (void)key;
    FamoEngineContext *replacement = nullptr;
    Composition composition;
    if (engine_.api().create_context(&engine_schema, &replacement) !=
            FAMO_ENGINE_OK ||
        !replacement || !ApplyOptionsLocked(replacement, options) ||
        !ReadStatusLocked(replacement, &composition)) {
      if (replacement)
        engine_.api().destroy_context(replacement);
      for (const auto &built : replacements)
        engine_.api().destroy_context(built.context);
      return false;
    }
    replacements.push_back(
        Replacement{&session, replacement, std::move(composition)});
  }
  {
    std::lock_guard ui_lock(ui_sessions_mutex_);
    for (auto &replacement : replacements) {
      if (replacement.session->context)
        engine_.api().destroy_context(replacement.session->context);
      replacement.session->context = replacement.context;
      replacement.session->composition = std::move(replacement.composition);
      replacement.session->composition_sequence =
          replacement.session->last_sequence;
      Publish(*replacement.session, true);
    }
  }
  return true;
}

ControlError RuntimeService::ReloadStyle() {
  RuntimeSnapshotSink *sink = nullptr;
  std::string root;
  {
    std::lock_guard lock(mutex_);
    sink = snapshot_sink_.load();
    root = data_root_;
  }
  RuntimeStyleOverlay overlay;
  if (!sink || !ReadRuntimeStyleOverlay(root, &overlay))
    return ControlError::Config;
  std::shared_ptr<const void> presentation;
  if (!sink->PrepareStyle(overlay.text, overlay.exists, &presentation))
    return ControlError::Config;
  std::shared_ptr<const RuntimeStyleState> next;
  try {
    next = std::make_shared<const RuntimeStyleState>(RuntimeStyleState{
        overlay.host_behavior_flags, std::move(presentation)});
  } catch (...) {
    return ControlError::Runtime;
  }
  {
    std::lock_guard lock(mutex_);
    style_state_ = next;
  }
  sink->ActivateStyle(std::move(next));
  return ControlError::None;
}

ControlError RuntimeService::ReloadOptions() {
  std::map<std::string, bool> next;
  if (!ReadOptions(data_root_, &next))
    return ControlError::Config;
  std::lock_guard lock(mutex_);
  // Build every replacement first. A failed set_option therefore leaves the
  // live contexts and the previous options map untouched.
  if (!ReplaceContextsLocked(selected_schema_, next))
    return ControlError::Engine;
  options_ = std::move(next);
  return ControlError::None;
}

ControlError RuntimeService::SelectSchema() {
  std::string schema;
  if (!ReadSelectedSchema(data_root_, &schema))
    return ControlError::Config;
  std::lock_guard lock(mutex_);
  if (schema == selected_schema_)
    return ControlError::None;
  if (!ReplaceContextsLocked(schema, options_))
    return ControlError::Engine;
  selected_schema_ = std::move(schema);
  engine_generation_.fetch_add(1);
  return ControlError::None;
}

ControlError
RuntimeService::InitializeControlState(uint32_t empty_root_behavior_flags) {
  readiness_.store(RuntimeReadiness::Starting);
  if (data_root_.empty()) {
    {
      std::lock_guard lock(mutex_);
      try {
        style_state_ = std::make_shared<const RuntimeStyleState>(
            RuntimeStyleState{empty_root_behavior_flags, nullptr});
      } catch (...) {
        readiness_.store(RuntimeReadiness::Unavailable);
        return ControlError::Runtime;
      }
    }
    if (RuntimeSnapshotSink *sink = snapshot_sink_.load())
      sink->PrepareForRuntimeReady();
    readiness_.store(RuntimeReadiness::Ready);
    return ControlError::None;
  }
  const ControlError style = ReloadStyle();
  const ControlError options = ReloadOptions();
  const ControlError schema = SelectSchema();
  const ControlError result = style != ControlError::None     ? style
                              : options != ControlError::None ? options
                                                              : schema;
  if (result == ControlError::None) {
    if (RuntimeSnapshotSink *sink = snapshot_sink_.load())
      sink->PrepareForRuntimeReady();
  }
  readiness_.store(result == ControlError::None
                       ? RuntimeReadiness::Ready
                       : RuntimeReadiness::Unavailable);
  return result;
}

ControlError RuntimeService::ResetUserDictionary() {
  std::filesystem::path root;
  if (!Utf8Path(data_root_, L"", &root))
    return ControlError::Config;
  std::vector<std::filesystem::path> databases;
  if (!UserDatabases(root, &databases))
    return ControlError::UserDictionaryEnumeration;
  if (databases.empty())
    return ControlError::None;

  const RuntimeReadiness before =
      readiness_.exchange(RuntimeReadiness::Maintenance);
  if (before != RuntimeReadiness::Ready) {
    readiness_.store(before);
    return ControlError::Runtime;
  }

  std::lock_guard lock(mutex_);
  std::lock_guard ui_lock(ui_sessions_mutex_);
  for (auto &[key, session] : sessions_) {
    (void)key;
    Publish(session, false);
    if (session.context)
      engine_.api().destroy_context(session.context);
  }
  sessions_.clear();
  ui_sessions_.clear();
  clients_.clear();
  engine_.Unload();

  const auto backup = UserDictionaryBackup(root);
  std::error_code ec;
  std::filesystem::create_directories(backup, ec);
  bool backed_up = !ec;
  for (const auto &database : databases) {
    if (!backed_up || !CopyDirectory(database, backup / database.filename())) {
      backed_up = false;
      break;
    }
  }

  bool removed = backed_up;
  bool restored = true;
  if (backed_up) {
    int removed_count = 0;
    const bool inject_partial_delete =
        TestSwitch(L"FAMO_TEST_USERDB_PARTIAL_DELETE_FAILURE");
    for (const auto &database : databases) {
      if (inject_partial_delete && removed_count == 1) {
        removed = false;
        break;
      }
      std::filesystem::remove_all(database, ec);
      if (ec) {
        removed = false;
        break;
      }
      ++removed_count;
    }
    if (!removed)
      restored = RestoreUserDatabases(databases, backup);
  }

  const int32_t load_rc = engine_.Load(engine_path_.c_str(), data_root_.c_str());
  if (load_rc != FAMO_ENGINE_OK || !engine_.AbiRunnable()) {
    if (load_rc == FAMO_ENGINE_OK)
      engine_.Unload();
    readiness_.store(RuntimeReadiness::Unavailable);
    return ControlError::Engine;
  }
  if (!restored) {
    readiness_.store(RuntimeReadiness::Ready);
    return ControlError::UserDictionaryRollback;
  }
  if (!backed_up || !removed) {
    readiness_.store(RuntimeReadiness::Ready);
    return ControlError::Runtime;
  }

  const FamoUtf8String schema = EngineString(selected_schema_);
  FamoUtf8String deploy_error{};
  deploy_error.size = static_cast<uint32_t>(sizeof(deploy_error));
  if (engine_.api().deploy_schema(&schema, &deploy_error) != FAMO_ENGINE_OK) {
    readiness_.store(RuntimeReadiness::Unavailable);
    return ControlError::Engine;
  }
  engine_generation_.fetch_add(1);
  readiness_.store(RuntimeReadiness::Ready);
  return ControlError::None;
}

ControlError RuntimeService::ExecuteControl(Command command) {
  if (!IsControlOperation(command))
    return ControlError::InvalidOperation;
  RuntimeReadiness current = readiness_.load();
  if (current == RuntimeReadiness::Unavailable &&
      (command == Command::ControlReloadStyle ||
       command == Command::ControlReloadOptions ||
       command == Command::ControlSelectSchema))
    return InitializeControlState();
  if (current == RuntimeReadiness::Unavailable &&
      command == Command::ControlDeploy) {
    const ControlError recovered = InitializeControlState();
    if (recovered != ControlError::None)
      return recovered;
    current = readiness_.load();
  }
  if (command != Command::ControlDeploy && current != RuntimeReadiness::Ready)
    return ControlError::Runtime;

  if (command == Command::ControlReloadStyle)
    return ReloadStyle();
  if (command == Command::ControlReloadOptions)
    return ReloadOptions();
  if (command == Command::ControlSelectSchema)
    return SelectSchema();
  if (command == Command::ControlResetUserDictionary)
    return ResetUserDictionary();
  if (command == Command::ControlShutdown) {
    readiness_.store(RuntimeReadiness::Stopping);
    return ControlError::None;
  }

  const RuntimeReadiness before =
      readiness_.exchange(RuntimeReadiness::Maintenance);
  if (before != RuntimeReadiness::Ready) {
    readiness_.store(before);
    return ControlError::Runtime;
  }
  std::lock_guard lock(mutex_);
  std::lock_guard ui_lock(ui_sessions_mutex_);
  // Deployment invalidates logical protocol sessions as well as engine
  // contexts. Existing clients receive StaleRequest and reconnect on their
  // next activation/focus control path; key callbacks remain fail-open.
  for (auto &[key, session] : sessions_) {
    (void)key;
    Publish(session, false);
    if (session.context)
      engine_.api().destroy_context(session.context);
  }
  sessions_.clear();
  ui_sessions_.clear();
  clients_.clear();
  const FamoUtf8String schema = EngineString(selected_schema_);
  FamoUtf8String deploy_error{};
  deploy_error.size = static_cast<uint32_t>(sizeof(deploy_error));
  const int32_t deploy_rc = engine_.api().deploy_schema(&schema, &deploy_error);
  if (deploy_rc == FAMO_ENGINE_OK) {
    engine_generation_.fetch_add(1);
    readiness_.store(RuntimeReadiness::Ready);
  } else {
    readiness_.store(RuntimeReadiness::Unavailable);
  }
  return deploy_rc == FAMO_ENGINE_OK ? ControlError::None
                                     : ControlError::Engine;
}

} // namespace famo::runtime
