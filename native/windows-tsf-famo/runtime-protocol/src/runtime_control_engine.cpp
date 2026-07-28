#include "famo_runtime_service.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include <windows.h>
#include <winternl.h>

#include "famo_user_data_lock.h"
#include "runtime_style_config.h"
#include "win_handle.h"

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
  if (!path ||
      root.size() >
          static_cast<size_t>((std::numeric_limits<int>::max)()) ||
      !IsValidUtf8(root))
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

bool ReadSmallFilePinnedRoot(std::string_view root, std::wstring_view leaf,
                             std::string *text, bool *exists = nullptr);

bool ReadOptions(std::string_view root, std::map<std::string, bool> *options) {
  std::string text;
  if (!options ||
      !ReadSmallFilePinnedRoot(root, L"famo-options.yaml", &text) ||
      !IsValidUtf8(text))
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
  std::string text;
  if (!schema ||
      !ReadSmallFilePinnedRoot(root, L"famo-select-schema.txt", &text) ||
      !IsValidUtf8(text))
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
using win::UniqueHandle;

bool TestSwitch(const wchar_t *name) {
  wchar_t value[2]{};
  return GetEnvironmentVariableW(name, value,
                                 static_cast<DWORD>(std::size(value))) > 0;
}

std::wstring ExtendedPath(const std::filesystem::path &path) {
  const std::wstring value = path.wstring();
  if (value.starts_with(L"\\\\?\\"))
    return value;
  if (value.starts_with(L"\\\\"))
    return L"\\\\?\\UNC\\" + value.substr(2);
  return L"\\\\?\\" + value;
}

std::filesystem::path FinalPath(HANDLE handle) {
  std::wstring value(32768, L'\0');
  const DWORD length = GetFinalPathNameByHandleW(
      handle, value.data(), static_cast<DWORD>(value.size()),
      FILE_NAME_NORMALIZED);
  if (length == 0 || length >= value.size())
    return {};
  value.resize(length);
  if (value.starts_with(L"\\\\?\\UNC\\"))
    value = L"\\\\" + value.substr(8);
  else if (value.starts_with(L"\\\\?\\"))
    value.erase(0, 4);
  while (value.size() > 3 &&
         (value.back() == L'\\' || value.back() == L'/')) {
    value.pop_back();
  }
  return std::filesystem::path(std::move(value));
}

bool SamePath(const std::filesystem::path &left,
              const std::filesystem::path &right) {
  return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

bool PinnedDirectoryInformation(HANDLE handle,
                                const std::filesystem::path &expected,
                                std::filesystem::path *final_path) {
  if (!handle || handle == INVALID_HANDLE_VALUE || !final_path)
    return false;
  BY_HANDLE_FILE_INFORMATION information{};
  if (!GetFileInformationByHandle(handle, &information))
    return false;
  const uint64_t object_id =
      (static_cast<uint64_t>(information.nFileIndexHigh) << 32) |
      information.nFileIndexLow;
  *final_path = FinalPath(handle);
  return (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
         (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
         object_id != 0 && !final_path->empty() &&
         (expected.empty() || SamePath(*final_path, expected));
}

bool OpenPinnedDirectory(const std::filesystem::path &path, DWORD access,
                         UniqueHandle *opened,
                         std::filesystem::path *final_path) {
  if (!opened || !final_path)
    return false;
  UniqueHandle directory(CreateFileW(
      ExtendedPath(path).c_str(), access | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!directory ||
      !PinnedDirectoryInformation(directory.get(), {}, final_path)) {
    return false;
  }
  *opened = std::move(directory);
  return true;
}

bool SafeRelativeLeaf(std::wstring_view name) {
  return !name.empty() && name != L"." && name != L".." &&
         name.size() <= USHRT_MAX / sizeof(wchar_t) &&
         std::none_of(name.begin(), name.end(), [](wchar_t ch) {
           return ch == L'\\' || ch == L'/' || ch == L'\0';
         });
}

struct DirectoryEntry {
  std::wstring name;
  DWORD attributes = 0;
  uint64_t file_id = 0;
};

bool EnumerateDirectory(HANDLE directory,
                        std::vector<DirectoryEntry> *entries) {
  if (!directory || directory == INVALID_HANDLE_VALUE || !entries)
    return false;
  try {
  entries->clear();
  std::vector<unsigned char> storage(64 * 1024);
  bool restart = true;
  for (;;) {
    const FILE_INFO_BY_HANDLE_CLASS information_class =
        restart ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo;
    if (!GetFileInformationByHandleEx(
            directory, information_class, storage.data(),
            static_cast<DWORD>(storage.size()))) {
      return GetLastError() == ERROR_NO_MORE_FILES;
    }
    restart = false;
    size_t offset = 0;
    for (;;) {
      constexpr size_t kFixed = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
      if (offset > storage.size() - kFixed)
        return false;
      const auto *item = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO *>(
          storage.data() + offset);
      if ((item->FileNameLength % sizeof(wchar_t)) != 0 ||
          item->FileNameLength > storage.size() - offset - kFixed) {
        return false;
      }
      std::wstring name(
          item->FileName,
          item->FileNameLength / static_cast<DWORD>(sizeof(wchar_t)));
      if (name != L"." && name != L"..") {
        try {
          entries->push_back(
              {std::move(name), item->FileAttributes,
               static_cast<uint64_t>(item->FileId.QuadPart)});
        } catch (...) {
          return false;
        }
      }
      if (item->NextEntryOffset == 0)
        break;
      if (item->NextEntryOffset < kFixed ||
          item->NextEntryOffset > storage.size() - offset) {
        return false;
      }
      offset += item->NextEntryOffset;
    }
  }
  } catch (...) {
    entries->clear();
    return false;
  }
}

DWORD OpenRelativeObject(
    HANDLE parent, const std::filesystem::path &parent_final,
    std::wstring_view leaf, ACCESS_MASK access, ULONG share_access,
    ULONG disposition, ULONG create_options, ULONG attributes,
    bool directory, UniqueHandle *opened,
    std::filesystem::path *final_path = nullptr) {
  if (!parent || parent == INVALID_HANDLE_VALUE || !opened ||
      !SafeRelativeLeaf(leaf)) {
    return ERROR_INVALID_PARAMETER;
  }
  UNICODE_STRING name{};
  name.Buffer = const_cast<PWSTR>(leaf.data());
  name.Length = static_cast<USHORT>(leaf.size() * sizeof(wchar_t));
  name.MaximumLength = name.Length;
  OBJECT_ATTRIBUTES object_attributes{};
  InitializeObjectAttributes(&object_attributes, &name, OBJ_CASE_INSENSITIVE,
                             parent, nullptr);
  IO_STATUS_BLOCK status_block{};
  HANDLE raw = INVALID_HANDLE_VALUE;
  const NTSTATUS status = NtCreateFile(
      &raw, access | FILE_READ_ATTRIBUTES | SYNCHRONIZE, &object_attributes,
      &status_block, nullptr, attributes, share_access, disposition,
      create_options | FILE_OPEN_REPARSE_POINT |
          FILE_SYNCHRONOUS_IO_NONALERT,
      nullptr, 0);
  if (status < 0)
    return RtlNtStatusToDosError(status);

  UniqueHandle object(raw);
  BY_HANDLE_FILE_INFORMATION information{};
  const std::filesystem::path expected = parent_final / leaf;
  const std::filesystem::path actual = FinalPath(object.get());
  if (!GetFileInformationByHandle(object.get(), &information) ||
      ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) !=
          directory ||
      (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
      actual.empty() || !SamePath(actual, expected)) {
    return ERROR_BAD_PATHNAME;
  }
  if (final_path)
    *final_path = actual;
  *opened = std::move(object);
  return ERROR_SUCCESS;
}

DWORD OpenRelativeDirectory(
    HANDLE parent, const std::filesystem::path &parent_final,
    std::wstring_view leaf, ACCESS_MASK access, ULONG disposition,
    UniqueHandle *opened, std::filesystem::path *final_path = nullptr) {
  return OpenRelativeObject(
      parent, parent_final, leaf, access, FILE_SHARE_READ | FILE_SHARE_WRITE,
      disposition, FILE_DIRECTORY_FILE, FILE_ATTRIBUTE_DIRECTORY, true,
      opened, final_path);
}

DWORD OpenRelativeFile(HANDLE parent,
                       const std::filesystem::path &parent_final,
                       std::wstring_view leaf, ACCESS_MASK access,
                       ULONG share_access, ULONG disposition,
                       UniqueHandle *opened,
                       std::filesystem::path *final_path = nullptr) {
  return OpenRelativeObject(parent, parent_final, leaf, access, share_access,
                            disposition, FILE_NON_DIRECTORY_FILE,
                            FILE_ATTRIBUTE_NORMAL, false, opened, final_path);
}

struct FileIdentity {
  DWORD volume_serial = 0;
  uint64_t file_id = 0;
  auto operator<=>(const FileIdentity &) const = default;
};

bool IdentityOf(HANDLE handle, FileIdentity *identity) {
  if (!identity)
    return false;
  BY_HANDLE_FILE_INFORMATION information{};
  if (!GetFileInformationByHandle(handle, &information))
    return false;
  identity->volume_serial = information.dwVolumeSerialNumber;
  identity->file_id =
      (static_cast<uint64_t>(information.nFileIndexHigh) << 32) |
      information.nFileIndexLow;
  return identity->volume_serial != 0 && identity->file_id != 0;
}

struct PinnedUserDatabase {
  std::wstring name;
  FileIdentity identity;
  UniqueHandle handle;
  bool moved = false;
};

bool EnumeratePinnedUserDatabases(
    const std::filesystem::path &root_path, UniqueHandle *root,
    std::filesystem::path *root_final,
    std::vector<PinnedUserDatabase> *databases) {
  if (!root || !root_final || !databases ||
      TestSwitch(L"FAMO_TEST_USERDB_ENUMERATION_DENIED") ||
      !OpenPinnedDirectory(root_path,
                           FILE_TRAVERSE | FILE_LIST_DIRECTORY |
                               FILE_ADD_SUBDIRECTORY | FILE_ADD_FILE,
                           root, root_final)) {
    return false;
  }
  databases->clear();
  std::vector<DirectoryEntry> entries;
  if (!EnumerateDirectory(root->get(), &entries))
    return false;
  try {
    databases->reserve(entries.size());
  } catch (...) {
    return false;
  }
  for (const DirectoryEntry &entry : entries) {
    if (entry.name.size() <= kUserDatabaseSuffix.size() ||
        !entry.name.ends_with(kUserDatabaseSuffix)) {
      continue;
    }
    if ((entry.attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        !SafeRelativeLeaf(entry.name)) {
      databases->clear();
      return false;
    }
    UniqueHandle database;
    std::filesystem::path database_final;
    FileIdentity identity;
    if (OpenRelativeDirectory(
            root->get(), *root_final, entry.name, DELETE | FILE_TRAVERSE,
            FILE_OPEN, &database, &database_final) != ERROR_SUCCESS ||
        !IdentityOf(database.get(), &identity) ||
        identity.file_id != entry.file_id) {
      databases->clear();
      return false;
    }
    try {
      databases->push_back(PinnedUserDatabase{
          entry.name, identity, std::move(database), false});
    } catch (...) {
      databases->clear();
      return false;
    }
  }
  std::sort(databases->begin(), databases->end(),
            [](const PinnedUserDatabase &left,
               const PinnedUserDatabase &right) {
              return left.name < right.name;
            });
  return true;
}

std::wstring UserDictionaryBackupName() {
  wchar_t override_name[96]{};
  const DWORD override_length = GetEnvironmentVariableW(
      L"FAMO_TEST_USERDB_BACKUP_NAME", override_name,
      static_cast<DWORD>(std::size(override_name)));
  if (override_length > 0 && override_length < std::size(override_name)) {
    const std::wstring_view name(override_name, override_length);
    if (name.starts_with(L"userdb-reset-") &&
        std::all_of(name.begin(), name.end(), [](wchar_t ch) {
          return iswalnum(ch) || ch == L'-';
        })) {
      return std::wstring(name);
    }
  }
  SYSTEMTIME now{};
  GetLocalTime(&now);
  wchar_t timestamp[32]{};
  swprintf_s(timestamp, std::size(timestamp), L"%04u%02u%02u-%02u%02u%02u-%03u",
             now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
             now.wSecond, now.wMilliseconds);
  return std::wstring(L"userdb-reset-") + timestamp;
}

bool RenamePinnedDirectory(HANDLE source, HANDLE destination,
                           std::wstring_view name,
                           const std::filesystem::path &expected,
                           bool replace = false) {
  if (!source || !destination || name.empty() ||
      name.size() > (std::numeric_limits<DWORD>::max)() / sizeof(wchar_t) ||
      name.find_first_of(L"\\/") != std::wstring_view::npos) {
    return false;
  }
  const size_t bytes =
      offsetof(FILE_RENAME_INFO, FileName) +
      name.size() * sizeof(wchar_t);
  std::vector<unsigned char> storage;
  try {
    storage.assign(bytes, 0);
  } catch (...) {
    return false;
  }
  auto *rename = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
  rename->ReplaceIfExists = replace ? TRUE : FALSE;
  rename->RootDirectory = destination;
  rename->FileNameLength =
      static_cast<DWORD>(name.size() * sizeof(wchar_t));
  std::memcpy(rename->FileName, name.data(), rename->FileNameLength);
  using NtSetInformationFileFn = NTSTATUS(NTAPI *)(
      HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
  static const auto native_set = reinterpret_cast<NtSetInformationFileFn>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                     "NtSetInformationFile"));
  IO_STATUS_BLOCK status_block{};
  const NTSTATUS status =
      native_set
          ? native_set(source, &status_block, rename,
                       static_cast<ULONG>(storage.size()),
                       static_cast<FILE_INFORMATION_CLASS>(10))
          : static_cast<NTSTATUS>(0xc0000002L);
  if (status < 0)
    return false;
  const std::filesystem::path renamed = FinalPath(source);
  if (!SamePath(renamed, expected))
    return false;
  return true;
}

bool CreatePinnedBackup(
    HANDLE root, const std::filesystem::path &root_final,
    UniqueHandle *backup_root, std::filesystem::path *backup_root_final,
    UniqueHandle *backup,
    std::filesystem::path *backup_final) {
  if (!backup_root || !backup_root_final || !backup || !backup_final)
    return false;
  const DWORD backup_root_error = OpenRelativeDirectory(
      root, root_final, L".famo-backup",
      FILE_TRAVERSE | FILE_LIST_DIRECTORY | FILE_ADD_SUBDIRECTORY |
          FILE_ADD_FILE,
      FILE_OPEN_IF, backup_root, backup_root_final);
  if (backup_root_error != ERROR_SUCCESS)
    return false;
  return OpenRelativeDirectory(
             backup_root->get(), *backup_root_final,
             UserDictionaryBackupName(),
             FILE_TRAVERSE | FILE_LIST_DIRECTORY | FILE_ADD_SUBDIRECTORY |
                 FILE_ADD_FILE | DELETE,
             FILE_CREATE, backup, backup_final) == ERROR_SUCCESS;
}

bool ReadSmallFileRelative(HANDLE parent,
                           const std::filesystem::path &parent_final,
                           std::wstring_view leaf, std::string *text,
                           bool *exists = nullptr) {
  if (!text)
    return false;
  if (exists)
    *exists = false;
  UniqueHandle file;
  const DWORD opened = OpenRelativeFile(
      parent, parent_final, leaf, FILE_READ_DATA, FILE_SHARE_READ, FILE_OPEN,
      &file);
  if (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND) {
    text->clear();
    return true;
  }
  if (opened != ERROR_SUCCESS)
    return false;
  if (exists)
    *exists = true;
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0 ||
      static_cast<uint64_t>(size.QuadPart) > kMaxFrameSize) {
    return false;
  }
  std::string value;
  try {
    value.reserve(static_cast<size_t>(size.QuadPart));
  } catch (...) {
    return false;
  }
  char buffer[4096];
  for (;;) {
    DWORD transferred = 0;
    if (!ReadFile(file.get(), buffer, static_cast<DWORD>(sizeof(buffer)),
                  &transferred, nullptr)) {
      return false;
    }
    if (transferred == 0)
      break;
    if (value.size() > kMaxFrameSize - transferred)
      return false;
    try {
      value.append(buffer, transferred);
    } catch (...) {
      return false;
    }
  }
  text->swap(value);
  return true;
}

bool WriteAllAndFlush(HANDLE file, std::string_view text) {
  if (!file || file == INVALID_HANDLE_VALUE || text.size() > kMaxFrameSize)
    return false;
  size_t offset = 0;
  while (offset < text.size()) {
    const DWORD chunk = static_cast<DWORD>((std::min)(
        text.size() - offset,
        static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
    DWORD transferred = 0;
    if (!WriteFile(file, text.data() + offset, chunk, &transferred, nullptr) ||
        transferred == 0) {
      return false;
    }
    offset += transferred;
  }
  return FlushFileBuffers(file) != FALSE;
}

bool DeletePinnedFileRelative(HANDLE parent,
                              const std::filesystem::path &parent_final,
                              std::wstring_view leaf) {
  UniqueHandle file;
  const DWORD opened = OpenRelativeFile(
      parent, parent_final, leaf, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE,
      FILE_OPEN, &file);
  if (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND)
    return true;
  if (opened != ERROR_SUCCESS)
    return false;
  FILE_DISPOSITION_INFO disposition{TRUE};
  return SetFileInformationByHandle(file.get(), FileDispositionInfo,
                                    &disposition,
                                    sizeof(disposition)) != FALSE;
}

bool DeletePinnedObject(HANDLE object) {
  if (!object || object == INVALID_HANDLE_VALUE)
    return false;
  FILE_DISPOSITION_INFO disposition{TRUE};
  return SetFileInformationByHandle(object, FileDispositionInfo,
                                    &disposition,
                                    sizeof(disposition)) != FALSE;
}

bool WriteNewFlushedFile(HANDLE parent,
                         const std::filesystem::path &parent_final,
                         std::wstring_view leaf, std::string_view text) {
  if (text.size() > kMaxFrameSize)
    return false;
  UniqueHandle file;
  if (OpenRelativeFile(parent, parent_final, leaf,
                       FILE_WRITE_DATA | DELETE, 0, FILE_CREATE,
                       &file) != ERROR_SUCCESS) {
    return false;
  }
  if (WriteAllAndFlush(file.get(), text))
    return true;
  FILE_DISPOSITION_INFO disposition{TRUE};
  (void)SetFileInformationByHandle(file.get(), FileDispositionInfo,
                                   &disposition, sizeof(disposition));
  return false;
}

bool WriteSmallFileAtomicallyRelative(
    HANDLE root, const std::filesystem::path &root_final,
    std::wstring_view leaf, std::string_view text) {
  if (!SafeRelativeLeaf(leaf) || text.size() > kMaxFrameSize)
    return false;
  std::wstring temporary;
  try {
    temporary.assign(leaf);
    temporary += L".famo-write.tmp";
  } catch (...) {
    return false;
  }
  if (!DeletePinnedFileRelative(root, root_final, temporary))
    return false;
  UniqueHandle file;
  if (OpenRelativeFile(root, root_final, temporary,
                       FILE_WRITE_DATA | DELETE, 0, FILE_CREATE,
                       &file) != ERROR_SUCCESS) {
    return false;
  }
  bool written = WriteAllAndFlush(file.get(), text);
  if (written) {
    written = RenamePinnedDirectory(file.get(), root, leaf,
                                    root_final / leaf, true);
  }
  if (!written) {
    FILE_DISPOSITION_INFO disposition{TRUE};
    (void)SetFileInformationByHandle(file.get(), FileDispositionInfo,
                                     &disposition, sizeof(disposition));
  }
  return written;
}

bool OpenPinnedDataRoot(std::string_view root, UniqueHandle *opened,
                        std::filesystem::path *final_path) {
  std::filesystem::path root_path;
  return Utf8Path(root, L"", &root_path) &&
         OpenPinnedDirectory(root_path,
                             FILE_TRAVERSE | FILE_LIST_DIRECTORY |
                                 FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY,
                             opened, final_path);
}

bool ReadSmallFilePinnedRoot(std::string_view root, std::wstring_view leaf,
                             std::string *text, bool *exists) {
  UniqueHandle pinned_root;
  std::filesystem::path root_final;
  return OpenPinnedDataRoot(root, &pinned_root, &root_final) &&
         ReadSmallFileRelative(pinned_root.get(), root_final, leaf, text,
                               exists);
}

bool RestoreSmallFileRelative(HANDLE root,
                              const std::filesystem::path &root_final,
                              std::wstring_view leaf, bool existed,
                              std::string_view text) {
  if (existed)
    return WriteSmallFileAtomicallyRelative(root, root_final, leaf, text);
  return DeletePinnedFileRelative(root, root_final, leaf);
}

constexpr std::wstring_view kUserDbPendingManifest =
    L"famo-userdb.pending";
constexpr std::wstring_view kUserDbCommittedMarker =
    L"famo-userdb.committed";
constexpr std::wstring_view kUserDbRolledBackMarker =
    L"famo-userdb.rolledback";
constexpr std::wstring_view kUserDbProgress =
    L"famo-userdb.progress";
constexpr std::string_view kUserDbManifestMagic = "FAMOUDB1";

template <typename T> bool AppendManifestValue(std::string *output, T value) {
  static_assert(std::is_trivially_copyable_v<T>);
  if (!output || output->size() > kMaxFrameSize - sizeof(value))
    return false;
  try {
    output->append(reinterpret_cast<const char *>(&value), sizeof(value));
    return true;
  } catch (...) {
    return false;
  }
}

template <typename T>
bool ReadManifestValue(std::string_view manifest, size_t *offset, T *value) {
  static_assert(std::is_trivially_copyable_v<T>);
  if (!offset || !value || *offset > manifest.size() ||
      sizeof(T) > manifest.size() - *offset) {
    return false;
  }
  std::memcpy(value, manifest.data() + *offset, sizeof(T));
  *offset += sizeof(T);
  return true;
}

bool SerializeUserDatabaseManifest(
    const std::vector<PinnedUserDatabase> &databases,
    std::string *manifest) {
  if (!manifest || databases.size() > 4096)
    return false;
  std::string value(kUserDbManifestMagic);
  const uint32_t count = static_cast<uint32_t>(databases.size());
  if (!AppendManifestValue(&value, count))
    return false;
  for (const auto &database : databases) {
    if (!SafeRelativeLeaf(database.name) ||
        database.name.size() <= kUserDatabaseSuffix.size() ||
        !database.name.ends_with(kUserDatabaseSuffix) ||
        database.name.size() >
            (std::numeric_limits<uint32_t>::max)() / sizeof(wchar_t)) {
      return false;
    }
    const uint32_t name_bytes =
        static_cast<uint32_t>(database.name.size() * sizeof(wchar_t));
    if (!AppendManifestValue(&value, name_bytes) ||
        !AppendManifestValue(&value, database.identity.volume_serial) ||
        !AppendManifestValue(&value, database.identity.file_id) ||
        value.size() > kMaxFrameSize - name_bytes) {
      return false;
    }
    try {
      value.append(reinterpret_cast<const char *>(database.name.data()),
                   name_bytes);
    } catch (...) {
      return false;
    }
  }
  manifest->swap(value);
  return true;
}

struct UserDatabaseRecord {
  std::wstring name;
  FileIdentity identity;
};

bool ParseUserDatabaseManifest(
    std::string_view manifest, std::vector<UserDatabaseRecord> *records) {
  if (!records || manifest.size() < kUserDbManifestMagic.size() ||
      manifest.substr(0, kUserDbManifestMagic.size()) !=
          kUserDbManifestMagic) {
    return false;
  }
  size_t offset = kUserDbManifestMagic.size();
  uint32_t count = 0;
  if (!ReadManifestValue(manifest, &offset, &count) || count > 4096)
    return false;
  records->clear();
  try {
    records->reserve(count);
  } catch (...) {
    return false;
  }
  for (uint32_t index = 0; index < count; ++index) {
    uint32_t name_bytes = 0;
    UserDatabaseRecord record;
    if (!ReadManifestValue(manifest, &offset, &name_bytes) ||
        !ReadManifestValue(manifest, &offset,
                           &record.identity.volume_serial) ||
        !ReadManifestValue(manifest, &offset, &record.identity.file_id) ||
        name_bytes == 0 || (name_bytes % sizeof(wchar_t)) != 0 ||
        offset > manifest.size() ||
        name_bytes > manifest.size() - offset ||
        record.identity.volume_serial == 0 || record.identity.file_id == 0) {
      return false;
    }
    try {
      record.name.resize(name_bytes / sizeof(wchar_t));
      std::memcpy(record.name.data(), manifest.data() + offset, name_bytes);
    } catch (...) {
      return false;
    }
    offset += name_bytes;
    if (!SafeRelativeLeaf(record.name) ||
        record.name.size() <= kUserDatabaseSuffix.size() ||
        !record.name.ends_with(kUserDatabaseSuffix) ||
        std::any_of(records->begin(), records->end(),
                    [&](const UserDatabaseRecord &existing) {
                      return _wcsicmp(existing.name.c_str(),
                                     record.name.c_str()) == 0 ||
                             existing.identity == record.identity;
                    })) {
      return false;
    }
    records->push_back(std::move(record));
  }
  return offset == manifest.size() && records->size() == count;
}

bool ReadMarker(HANDLE transaction,
                const std::filesystem::path &transaction_final,
                std::wstring_view leaf, std::string_view expected,
                bool *exists) {
  if (!exists)
    return false;
  std::string text;
  if (!ReadSmallFileRelative(transaction, transaction_final, leaf, &text,
                             exists)) {
    return false;
  }
  return !*exists || text == expected;
}

bool EnsureMarker(HANDLE transaction,
                  const std::filesystem::path &transaction_final,
                  std::wstring_view leaf, std::string_view expected) {
  bool exists = false;
  if (!ReadMarker(transaction, transaction_final, leaf, expected, &exists))
    return false;
  return exists ||
         WriteNewFlushedFile(transaction, transaction_final, leaf, expected);
}

bool WriteUserDatabaseProgress(
    HANDLE transaction, const std::filesystem::path &transaction_final,
    std::string_view phase, size_t count) {
  try {
    std::string progress(phase);
    progress.push_back(':');
    progress += std::to_string(count);
    progress.push_back('\n');
    UniqueHandle file;
    return OpenRelativeFile(
               transaction, transaction_final, kUserDbProgress,
               FILE_WRITE_DATA, 0, FILE_OVERWRITE_IF, &file) ==
               ERROR_SUCCESS &&
           WriteAllAndFlush(file.get(), progress);
  } catch (...) {
    return false;
  }
}

enum class DatabasePlacement { Absent, Match, Mismatch };

DatabasePlacement OpenDatabasePlacement(
    HANDLE parent, const std::filesystem::path &parent_final,
    const UserDatabaseRecord &record, ACCESS_MASK access,
    UniqueHandle *opened) {
  UniqueHandle database;
  const DWORD result = OpenRelativeDirectory(
      parent, parent_final, record.name, access | FILE_TRAVERSE, FILE_OPEN,
      &database);
  if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND)
    return DatabasePlacement::Absent;
  FileIdentity identity;
  if (result != ERROR_SUCCESS || !IdentityOf(database.get(), &identity) ||
      identity != record.identity) {
    return DatabasePlacement::Mismatch;
  }
  if (opened)
    *opened = std::move(database);
  return DatabasePlacement::Match;
}

bool RollBackPendingUserDatabaseTransaction(
    HANDLE root, const std::filesystem::path &root_final, HANDLE transaction,
    const std::filesystem::path &transaction_final,
    const std::vector<UserDatabaseRecord> &records,
    bool inject_restore_failure = false) {
  bool injected = false;
  size_t restored_count = 0;
  for (const auto &record : records) {
    UniqueHandle root_database;
    UniqueHandle backup_database;
    const DatabasePlacement root_placement = OpenDatabasePlacement(
        root, root_final, record, 0, &root_database);
    const DatabasePlacement backup_placement = OpenDatabasePlacement(
        transaction, transaction_final, record, DELETE, &backup_database);
    if (root_placement == DatabasePlacement::Match &&
        backup_placement == DatabasePlacement::Absent) {
      continue;
    }
    if (root_placement != DatabasePlacement::Absent ||
        backup_placement != DatabasePlacement::Match) {
      return false;
    }
    if (inject_restore_failure && !injected) {
      injected = true;
      continue;
    }
    if (!RenamePinnedDirectory(backup_database.get(), root, record.name,
                               root_final / record.name)) {
      return false;
    }
    if (!WriteUserDatabaseProgress(transaction, transaction_final,
                                   "rollback", ++restored_count)) {
      return false;
    }
  }
  if (inject_restore_failure && injected)
    return false;
  return EnsureMarker(transaction, transaction_final,
                      kUserDbRolledBackMarker, "rolledback\n");
}

bool RecoverPendingUserDatabaseTransactionsAtRoot(
    HANDLE root, const std::filesystem::path &root_final) {
  UniqueHandle backup_root;
  std::filesystem::path backup_root_final;
  const DWORD backup_error = OpenRelativeDirectory(
      root, root_final, L".famo-backup",
      FILE_TRAVERSE | FILE_LIST_DIRECTORY | FILE_ADD_SUBDIRECTORY |
          FILE_ADD_FILE,
      FILE_OPEN, &backup_root, &backup_root_final);
  if (backup_error == ERROR_FILE_NOT_FOUND ||
      backup_error == ERROR_PATH_NOT_FOUND) {
    return true;
  }
  if (backup_error != ERROR_SUCCESS)
    return false;
  std::vector<DirectoryEntry> entries;
  if (!EnumerateDirectory(backup_root.get(), &entries))
    return false;
  std::sort(entries.begin(), entries.end(),
            [](const DirectoryEntry &left, const DirectoryEntry &right) {
              return left.name < right.name;
            });
  for (const auto &entry : entries) {
    if (!entry.name.starts_with(L"userdb-reset-"))
      continue;
    if ((entry.attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        !SafeRelativeLeaf(entry.name)) {
      return false;
    }
    UniqueHandle transaction;
    std::filesystem::path transaction_final;
    if (OpenRelativeDirectory(
            backup_root.get(), backup_root_final, entry.name,
            FILE_TRAVERSE | FILE_LIST_DIRECTORY | FILE_ADD_SUBDIRECTORY |
                FILE_ADD_FILE,
            FILE_OPEN, &transaction, &transaction_final) != ERROR_SUCCESS) {
      return false;
    }
    std::string manifest;
    bool pending_exists = false;
    if (!ReadSmallFileRelative(transaction.get(), transaction_final,
                               kUserDbPendingManifest, &manifest,
                               &pending_exists)) {
      return false;
    }
    // Pre-transaction releases used the same human-readable backup prefix
    // without a journal. They are completed historical backups, not debt.
    if (!pending_exists)
      continue;
    std::vector<UserDatabaseRecord> records;
    if (!ParseUserDatabaseManifest(manifest, &records))
      return false;
    bool committed = false;
    bool rolled_back = false;
    if (!ReadMarker(transaction.get(), transaction_final,
                    kUserDbCommittedMarker, "committed\n", &committed) ||
        !ReadMarker(transaction.get(), transaction_final,
                    kUserDbRolledBackMarker, "rolledback\n", &rolled_back) ||
        (committed && rolled_back)) {
      return false;
    }
    if (committed) {
      for (const auto &record : records) {
        if (OpenDatabasePlacement(transaction.get(), transaction_final,
                                  record, 0, nullptr) !=
            DatabasePlacement::Match) {
          return false;
        }
      }
      continue;
    }
    if (rolled_back)
      continue;
    if (!RollBackPendingUserDatabaseTransaction(
            root, root_final, transaction.get(), transaction_final,
            records)) {
      return false;
    }
  }
  return true;
}

bool CrashAtUserDatabasePhase(std::wstring_view phase) {
  wchar_t configured[64]{};
  const DWORD length = GetEnvironmentVariableW(
      L"FAMO_TEST_USERDB_CRASH_PHASE", configured,
      static_cast<DWORD>(std::size(configured)));
  if (length == 0 || length >= std::size(configured) ||
      std::wstring_view(configured, length) != phase) {
    return false;
  }
  TerminateProcess(GetCurrentProcess(), 197);
  return true;
}

bool PauseForSwapTest(const wchar_t *ready_environment,
                      const wchar_t *continue_environment) {
  wchar_t ready_name[160]{};
  wchar_t continue_name[160]{};
  const DWORD ready_length = GetEnvironmentVariableW(
      ready_environment, ready_name,
      static_cast<DWORD>(std::size(ready_name)));
  const DWORD continue_length = GetEnvironmentVariableW(
      continue_environment, continue_name,
      static_cast<DWORD>(std::size(continue_name)));
  if (ready_length == 0 && continue_length == 0)
    return true;
  if (ready_length == 0 || ready_length >= std::size(ready_name) ||
      continue_length == 0 ||
      continue_length >= std::size(continue_name)) {
    return false;
  }
  UniqueHandle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, ready_name));
  UniqueHandle proceed(OpenEventW(SYNCHRONIZE, FALSE, continue_name));
  return ready && proceed && SetEvent(ready.get()) &&
         WaitForSingleObject(proceed.get(), 5000) == WAIT_OBJECT_0;
}

bool PauseForUserDatabaseSwapTest() {
  return PauseForSwapTest(L"FAMO_TEST_USERDB_READY_EVENT",
                          L"FAMO_TEST_USERDB_CONTINUE_EVENT");
}

bool PauseForSchemaSwapTest() {
  return PauseForSwapTest(L"FAMO_TEST_SCHEMA_READY_EVENT",
                          L"FAMO_TEST_SCHEMA_CONTINUE_EVENT");
}

bool MoveUserDatabasesToBackup(
    std::vector<PinnedUserDatabase> *databases, HANDLE root,
    const std::filesystem::path &root_final, HANDLE backup,
    const std::filesystem::path &backup_final, bool *restored) {
  if (!databases || !restored)
    return false;
  *restored = true;
  bool moved_all = true;
  size_t moved_count = 0;
  const bool inject_partial =
      TestSwitch(L"FAMO_TEST_USERDB_PARTIAL_DELETE_FAILURE");
  for (auto &database : *databases) {
    if ((inject_partial && moved_count == 1) ||
        !RenamePinnedDirectory(database.handle.get(), backup, database.name,
                               backup_final / database.name)) {
      moved_all = false;
      break;
    }
    database.moved = true;
    ++moved_count;
    if (!WriteUserDatabaseProgress(backup, backup_final, "moving",
                                   moved_count)) {
      moved_all = false;
      break;
    }
    if (moved_count == 1)
      (void)CrashAtUserDatabasePhase(L"after-first-move");
  }
  if (moved_all)
    return true;

  std::vector<UserDatabaseRecord> records;
  try {
    records.reserve(databases->size());
    for (const auto &database : *databases)
      records.push_back({database.name, database.identity});
  } catch (...) {
    *restored = false;
    return false;
  }
  // The pinned source handles intentionally deny FILE_SHARE_DELETE so no
  // concurrent actor can rename a database during the transaction. Release
  // them before reopening the moved objects for rollback.
  for (auto &database : *databases)
    database.handle.reset();
  *restored = RollBackPendingUserDatabaseTransaction(
      root, root_final, backup, backup_final, records,
      TestSwitch(L"FAMO_TEST_USERDB_RESTORE_FAILURE"));
  return false;
}

} // namespace

bool RuntimeService::ApplyOptionsLocked(
    FamoEngineContext *context,
    const std::map<std::string, bool> &options) {
  if (!context)
    return false;
  for (const auto &[name, value] : options) {
    const FamoUtf8String option = EngineString(name);
    if (engine_.SetOption(context, &option, value ? 1 : 0) !=
        FAMO_ENGINE_OK)
      return false;
  }
  return true;
}

bool RuntimeService::SetOption(std::string_view name, bool value) {
  if (!SafeName(name))
    return false;
  std::map<std::string, bool> changes;
  try {
    changes.emplace(std::string(name), value);
  } catch (...) {
    return false;
  }
  return SetOptions(changes);
}

bool RuntimeService::SetOptions(
    const std::map<std::string, bool> &requested) {
  if (requested.empty() ||
      std::any_of(requested.begin(), requested.end(), [](const auto &entry) {
        return !SafeName(entry.first);
      })) {
    return false;
  }
  std::lock_guard lock(mutex_);
  if (!started_ || readiness_.load() != RuntimeReadiness::Ready ||
      !deliveries_.empty() ||
      std::any_of(sessions_.begin(), sessions_.end(), [](const auto &entry) {
        return entry.second.pending_recovery_action != 0 ||
               static_cast<bool>(entry.second.pending_recovery_result);
      }))
    return false;

  struct Change {
    Session *session = nullptr;
    std::map<std::string, int32_t> previous;
    Composition composition;
  };
  struct Applied {
    Session *session = nullptr;
    const std::string *name = nullptr;
    int32_t previous = 0;
  };
  std::map<std::string, bool> next_options;
  std::vector<Change> changes;
  std::vector<Applied> applied;
  try {
    next_options = options_;
    for (const auto &[name, value] : requested)
      next_options.insert_or_assign(name, value);
    changes.reserve(sessions_.size());
    if (!requested.empty() &&
        sessions_.size() >
            (std::numeric_limits<size_t>::max)() / requested.size()) {
      return false;
    }
    applied.reserve(sessions_.size() * requested.size());
  } catch (...) {
    return false;
  }

  for (auto &[key, session] : sessions_) {
    (void)key;
    if (!session.context)
      return false;
    Change change;
    change.session = &session;
    try {
      for (const auto &[name, value] : requested) {
        (void)value;
        int32_t previous = 0;
        const FamoUtf8String option = EngineString(name);
        if (engine_.GetOption(session.context, &option, &previous) !=
            FAMO_ENGINE_OK) {
          return false;
        }
        change.previous.emplace(name, previous);
      }
      changes.push_back(std::move(change));
    } catch (...) {
      return false;
    }
  }
  if (changes.empty()) {
    options_.swap(next_options);
    return true;
  }

  bool apply_failed = false;
  for (auto &change : changes) {
    for (const auto &[name, value] : requested) {
      const FamoUtf8String option = EngineString(name);
      if (engine_.SetOption(change.session->context, &option,
                            value ? 1 : 0) != FAMO_ENGINE_OK) {
        apply_failed = true;
        break;
      }
      applied.push_back(
          Applied{change.session, &name, change.previous.at(name)});
    }
    if (apply_failed)
      break;
  }

  const auto rollback = [&] {
    bool restored = true;
    for (auto item = applied.rbegin(); item != applied.rend(); ++item) {
      const FamoUtf8String option = EngineString(*item->name);
      if (engine_.SetOption(item->session->context, &option,
                            item->previous) != FAMO_ENGINE_OK) {
        restored = false;
      }
    }
    if (!restored)
      readiness_.store(RuntimeReadiness::Unavailable);
    return restored;
  };

  if (apply_failed) {
    (void)rollback();
    return false;
  }

  for (auto &change : changes) {
    if (!ReadStatusLocked(change.session->context, &change.composition)) {
      (void)rollback();
      return false;
    }
  }

  options_.swap(next_options);
  for (auto &change : changes) {
    change.session->composition = std::move(change.composition);
    Publish(*change.session, true);
  }
  return true;
}

bool RuntimeService::ReplaceContextsLocked(
    std::string_view schema, const std::map<std::string, bool> &options) {
  if (!deliveries_.empty() ||
      std::any_of(sessions_.begin(), sessions_.end(), [](const auto &entry) {
        return entry.second.pending_recovery_action != 0 ||
               static_cast<bool>(entry.second.pending_recovery_result);
      }))
    return false;
  struct Replacement {
    Session *session;
    FamoEngineContext *context;
    Composition composition;
  };
  std::vector<Replacement> replacements;
  try {
    replacements.reserve(sessions_.size());
    retired_contexts_.reserve(retired_contexts_.size() +
                              (std::max)(sessions_.size(), size_t{1}));
  } catch (...) {
    return false;
  }
  const auto discard_replacements = [&] {
    for (auto &built : replacements) {
      if (built.context &&
          engine_.DestroyContext(built.context) != FAMO_ENGINE_OK) {
        retired_contexts_.push_back(built.context);
      }
      built.context = nullptr;
    }
  };
  const FamoUtf8String engine_schema = EngineString(schema);
  if (sessions_.empty()) {
    FamoEngineContext *probe = nullptr;
    Composition composition;
    const bool valid =
        engine_.CreateContext(&engine_schema, &probe) ==
            FAMO_ENGINE_OK &&
        probe && ApplyOptionsLocked(probe, options) &&
        ReadStatusLocked(probe, &composition);
    const int32_t destroy_rc =
        probe ? engine_.DestroyContext(probe) : FAMO_ENGINE_OK;
    if (probe && destroy_rc != FAMO_ENGINE_OK)
      retired_contexts_.push_back(probe);
    return valid && destroy_rc == FAMO_ENGINE_OK;
  }
  try {
    for (auto &[key, session] : sessions_) {
      (void)key;
      FamoEngineContext *replacement = nullptr;
      Composition composition;
      if (engine_.CreateContext(&engine_schema, &replacement) !=
              FAMO_ENGINE_OK ||
          !replacement || !ApplyOptionsLocked(replacement, options) ||
          !ReadStatusLocked(replacement, &composition)) {
        if (replacement &&
            engine_.DestroyContext(replacement) != FAMO_ENGINE_OK) {
          retired_contexts_.push_back(replacement);
        }
        discard_replacements();
        return false;
      }
      try {
        replacements.push_back(
            Replacement{&session, replacement, std::move(composition)});
      } catch (...) {
        if (engine_.DestroyContext(replacement) != FAMO_ENGINE_OK)
          retired_contexts_.push_back(replacement);
        discard_replacements();
        return false;
      }
    }
  } catch (...) {
    discard_replacements();
    return false;
  }
  {
    std::lock_guard ui_lock(ui_sessions_mutex_);
    for (auto &replacement : replacements) {
      FamoEngineContext *retired = replacement.session->context;
      replacement.session->context = replacement.context;
      replacement.context = nullptr;
      replacement.session->composition = std::move(replacement.composition);
      replacement.session->composition_sequence =
          replacement.session->last_sequence;
      Publish(*replacement.session, true);
      if (retired &&
          engine_.DestroyContext(retired) != FAMO_ENGINE_OK) {
        retired_contexts_.push_back(retired);
      }
    }
  }
  return true;
}

ControlError RuntimeService::ReloadStyle() {
  UserDataTransactionLock transaction;
  if (!transaction.Acquire())
    return ControlError::Runtime;
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
  UserDataTransactionLock transaction;
  if (!transaction.Acquire())
    return ControlError::Runtime;
  std::map<std::string, bool> next;
  if (!ReadOptions(data_root_, &next))
    return ControlError::Config;
  std::lock_guard lock(mutex_);
  if (!deliveries_.empty())
    return ControlError::Runtime;
  // Build every replacement first. A failed set_option therefore leaves the
  // live contexts and the previous options map untouched.
  if (!ReplaceContextsLocked(selected_schema_, next))
    return ControlError::Engine;
  options_ = std::move(next);
  return ControlError::None;
}

ControlError RuntimeService::SelectSchema() {
  UserDataTransactionLock transaction;
  if (!transaction.Acquire())
    return ControlError::Runtime;
  std::string schema;
  if (!ReadSelectedSchema(data_root_, &schema))
    return ControlError::Config;
  std::lock_guard lock(mutex_);
  if (!deliveries_.empty())
    return ControlError::Runtime;
  if (schema == selected_schema_)
    return ControlError::None;
  if (!ReplaceContextsLocked(schema, options_))
    return ControlError::Engine;
  selected_schema_ = std::move(schema);
  engine_generation_.fetch_add(1);
  return ControlError::None;
}

ControlError RuntimeService::SelectSchemaAndPersist(
    std::string_view requested_schema) {
  try {
  if (!SafeName(requested_schema))
    return ControlError::Config;
  std::string schema;
  try {
    schema.assign(requested_schema);
  } catch (...) {
    return ControlError::Runtime;
  }
  UserDataTransactionLock transaction;
  if (!transaction.Acquire())
    return ControlError::Runtime;
  UniqueHandle root;
  std::filesystem::path root_final;
  std::string previous;
  bool existed = false;
  constexpr std::wstring_view kSelectedSchemaLeaf =
      L"famo-select-schema.txt";
  if (!OpenPinnedDataRoot(data_root_, &root, &root_final))
    return ControlError::Config;
  if (!PauseForSchemaSwapTest())
    return ControlError::Runtime;
  if (!ReadSmallFileRelative(root.get(), root_final, kSelectedSchemaLeaf,
                             &previous, &existed) ||
      !WriteSmallFileAtomicallyRelative(
          root.get(), root_final, kSelectedSchemaLeaf, schema)) {
    return ControlError::Config;
  }

  const auto rollback_file = [&] {
    const bool restored =
        !TestSwitch(L"FAMO_TEST_SCHEMA_ROLLBACK_FAILURE") &&
        RestoreSmallFileRelative(root.get(), root_final,
                                 kSelectedSchemaLeaf, existed, previous);
    if (!restored)
      readiness_.store(RuntimeReadiness::Unavailable);
    return restored;
  };

  std::lock_guard lock(mutex_);
  if (!started_ || readiness_.load() != RuntimeReadiness::Ready ||
      !deliveries_.empty()) {
    (void)rollback_file();
    return ControlError::Runtime;
  }
  if (schema == selected_schema_)
    return ControlError::None;
  if (!ReplaceContextsLocked(schema, options_)) {
    return rollback_file() ? ControlError::Engine : ControlError::Runtime;
  }
  selected_schema_ = std::move(schema);
  engine_generation_.fetch_add(1);
  return ControlError::None;
  } catch (...) {
    readiness_.store(RuntimeReadiness::Unavailable);
    return ControlError::Runtime;
  }
}

bool RuntimeService::RecoverPendingUserDictionaryTransactions(
    std::string_view data_root) {
  try {
    if (data_root.empty())
      return true;
    UserDataTransactionLock transaction;
    if (!transaction.Acquire())
      return false;
    std::filesystem::path root_path;
    UniqueHandle root;
    std::filesystem::path root_final;
    return Utf8Path(data_root, L"", &root_path) &&
           OpenPinnedDirectory(
               root_path,
               FILE_TRAVERSE | FILE_LIST_DIRECTORY |
                   FILE_ADD_SUBDIRECTORY | FILE_ADD_FILE,
               &root, &root_final) &&
           RecoverPendingUserDatabaseTransactionsAtRoot(root.get(),
                                                         root_final);
  } catch (...) {
    return false;
  }
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
  UserDataTransactionLock transaction;
  if (!transaction.Acquire()) {
    readiness_.store(RuntimeReadiness::Unavailable);
    return ControlError::Runtime;
  }
  if (!RecoverPendingUserDictionaryTransactions(data_root_)) {
    readiness_.store(RuntimeReadiness::Unavailable);
    return ControlError::UserDictionaryRollback;
  }
  if (!engine_.V2Runnable()) {
    const int32_t load_rc =
        engine_.LoadV2(engine_path_.c_str(), data_root_.c_str());
    if (load_rc != FAMO_ENGINE_OK || !engine_.V2Runnable()) {
      if (load_rc == FAMO_ENGINE_OK)
        engine_.Unload();
      readiness_.store(RuntimeReadiness::Unavailable);
      return ControlError::Engine;
    }
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
  try {
  UserDataTransactionLock transaction;
  if (!transaction.Acquire())
    return ControlError::Runtime;
  std::filesystem::path root_path;
  if (!Utf8Path(data_root_, L"", &root_path))
    return ControlError::Config;
  UniqueHandle root;
  std::filesystem::path root_final;
  std::vector<PinnedUserDatabase> databases;
  if (!EnumeratePinnedUserDatabases(root_path, &root, &root_final,
                                    &databases)) {
    return ControlError::UserDictionaryEnumeration;
  }
  if (databases.empty())
    return ControlError::None;
  std::string manifest;
  if (!SerializeUserDatabaseManifest(databases, &manifest))
    return ControlError::Runtime;
  std::vector<UserDatabaseRecord> records;
  try {
    records.reserve(databases.size());
    for (const auto &database : databases)
      records.push_back({database.name, database.identity});
  } catch (...) {
    return ControlError::Runtime;
  }

  const RuntimeReadiness before =
      readiness_.exchange(RuntimeReadiness::Maintenance);
  if (before != RuntimeReadiness::Ready) {
    readiness_.store(before);
    return ControlError::Runtime;
  }

  std::lock_guard lock(mutex_);
  if (!deliveries_.empty() ||
      std::any_of(sessions_.begin(), sessions_.end(), [](const auto &entry) {
        return entry.second.pending_recovery_action != 0 ||
               static_cast<bool>(entry.second.pending_recovery_result);
      })) {
    readiness_.store(RuntimeReadiness::Ready);
    return ControlError::Runtime;
  }
  if (!EnsureRetiredCapacityLocked(sessions_.size())) {
    readiness_.store(RuntimeReadiness::Ready);
    return ControlError::Runtime;
  }

  // Only create transaction artifacts after readiness and in-flight delivery
  // checks have accepted the operation. The manifest is flushed before the
  // first rename, so every crash state is recoverable from object identity.
  UniqueHandle backup_root;
  std::filesystem::path backup_root_final;
  UniqueHandle backup;
  std::filesystem::path backup_final;
  if (!CreatePinnedBackup(root.get(), root_final, &backup_root,
                          &backup_root_final, &backup, &backup_final)) {
    readiness_.store(RuntimeReadiness::Ready);
    return ControlError::Runtime;
  }
  if (!WriteNewFlushedFile(backup.get(), backup_final,
                           kUserDbPendingManifest, manifest)) {
    const bool removed = DeletePinnedObject(backup.get());
    readiness_.store(removed ? RuntimeReadiness::Ready
                             : RuntimeReadiness::Unavailable);
    return ControlError::Runtime;
  }
  if (!WriteUserDatabaseProgress(backup.get(), backup_final, "prepared", 0)) {
    const bool restored = RollBackPendingUserDatabaseTransaction(
        root.get(), root_final, backup.get(), backup_final, records);
    readiness_.store(restored ? RuntimeReadiness::Ready
                              : RuntimeReadiness::Unavailable);
    return restored ? ControlError::Runtime
                    : ControlError::UserDictionaryRollback;
  }
  (void)CrashAtUserDatabasePhase(L"after-manifest");
  if (!PauseForUserDatabaseSwapTest()) {
    const bool restored = RollBackPendingUserDatabaseTransaction(
        root.get(), root_final, backup.get(), backup_final, records);
    readiness_.store(restored ? RuntimeReadiness::Ready
                              : RuntimeReadiness::Unavailable);
    return restored ? ControlError::Runtime
                    : ControlError::UserDictionaryRollback;
  }

  {
    std::lock_guard ui_lock(ui_sessions_mutex_);
    for (auto &[key, session] : sessions_) {
      (void)key;
      Publish(session, false);
      (void)DestroyOrRetireContextLocked(session.context);
    }
    sessions_.clear();
    ui_sessions_.clear();
  }
  clients_.clear();
  acknowledged_deliveries_.clear();
  abandoned_epochs_.clear();
  RetryRetiredContextsLocked();
  if (!retired_contexts_.empty()) {
    (void)RollBackPendingUserDatabaseTransaction(
        root.get(), root_final, backup.get(), backup_final, records);
    readiness_.store(RuntimeReadiness::Unavailable);
    return ControlError::Engine;
  }
  engine_.Unload();

  bool restored = true;
  bool moved = MoveUserDatabasesToBackup(
      &databases, root.get(), root_final, backup.get(), backup_final,
      &restored);
  bool committed = false;
  if (moved) {
    (void)CrashAtUserDatabasePhase(L"before-commit");
    committed = EnsureMarker(backup.get(), backup_final,
                             kUserDbCommittedMarker, "committed\n");
    if (committed) {
      (void)CrashAtUserDatabasePhase(L"after-commit");
    } else {
      for (auto &database : databases)
        database.handle.reset();
      restored = RollBackPendingUserDatabaseTransaction(
          root.get(), root_final, backup.get(), backup_final, records);
      moved = false;
    }
  }
  if (!restored) {
    readiness_.store(RuntimeReadiness::Unavailable);
    return ControlError::UserDictionaryRollback;
  }

  const int32_t load_rc =
      engine_.LoadV2(engine_path_.c_str(), data_root_.c_str());
  if (load_rc != FAMO_ENGINE_OK || !engine_.V2Runnable()) {
    if (load_rc == FAMO_ENGINE_OK)
      engine_.Unload();
    readiness_.store(RuntimeReadiness::Unavailable);
    return ControlError::Engine;
  }
  const FamoUtf8String schema = EngineString(selected_schema_);
  if (engine_.DeploySchema(&schema) != FAMO_ENGINE_OK) {
    readiness_.store(RuntimeReadiness::Unavailable);
    return ControlError::Engine;
  }
  engine_generation_.fetch_add(1);
  readiness_.store(RuntimeReadiness::Ready);
  return moved && committed ? ControlError::None : ControlError::Runtime;
  } catch (...) {
    readiness_.store(RuntimeReadiness::Unavailable);
    return ControlError::Runtime;
  }
}

ControlError RuntimeService::ExecuteControl(Command command) {
  if (TestSwitch(L"FAMO_TEST_CONTROL_THROW"))
    throw std::runtime_error("injected control exception");
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
  if (!deliveries_.empty() ||
      std::any_of(sessions_.begin(), sessions_.end(), [](const auto &entry) {
        return entry.second.pending_recovery_action != 0 ||
               static_cast<bool>(entry.second.pending_recovery_result);
      })) {
    readiness_.store(RuntimeReadiness::Ready);
    return ControlError::Runtime;
  }
  if (!EnsureRetiredCapacityLocked(sessions_.size())) {
    readiness_.store(RuntimeReadiness::Ready);
    return ControlError::Runtime;
  }
  std::lock_guard ui_lock(ui_sessions_mutex_);
  // Deployment invalidates logical protocol sessions as well as engine
  // contexts. Existing clients receive StaleRequest and reconnect on their
  // next activation/focus control path; key callbacks remain fail-open.
  for (auto &[key, session] : sessions_) {
    (void)key;
    Publish(session, false);
    (void)DestroyOrRetireContextLocked(session.context);
  }
  sessions_.clear();
  ui_sessions_.clear();
  clients_.clear();
  acknowledged_deliveries_.clear();
  abandoned_epochs_.clear();
  RetryRetiredContextsLocked();
  if (!retired_contexts_.empty()) {
    readiness_.store(RuntimeReadiness::Unavailable);
    return ControlError::Engine;
  }
  const FamoUtf8String schema = EngineString(selected_schema_);
  const int32_t deploy_rc = engine_.DeploySchema(&schema);
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
