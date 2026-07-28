#include <windows.h>
#include <objbase.h>

#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

std::wstring NewNonce() {
  GUID guid{};
  if (FAILED(CoCreateGuid(&guid)))
    return {};
  wchar_t text[40]{};
  if (StringFromGUID2(guid, text, ARRAYSIZE(text)) <= 0)
    return {};
  std::wstring result;
  for (const wchar_t value : std::wstring_view(text)) {
    if ((value >= L'0' && value <= L'9') ||
        (value >= L'a' && value <= L'f') ||
        (value >= L'A' && value <= L'F'))
      result.push_back(value);
  }
  return result;
}

struct Child {
  HANDLE process = nullptr;
  HANDLE thread = nullptr;

  ~Child() {
    if (thread)
      CloseHandle(thread);
    if (process)
      CloseHandle(process);
  }
};

bool Start(const std::wstring &tool, const std::wstring &arguments,
           Child *child) {
  std::wstring command = L"\"" + tool + L"\" " + arguments;
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(tool.c_str(), command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
    return false;
  child->process = process.hProcess;
  child->thread = process.hThread;
  return true;
}

DWORD Wait(Child *child) {
  if (WaitForSingleObject(child->process, 20000) != WAIT_OBJECT_0)
    return STILL_ACTIVE;
  DWORD exit_code = STILL_ACTIVE;
  return GetExitCodeProcess(child->process, &exit_code) ? exit_code
                                                        : STILL_ACTIVE;
}

DWORD Run(const std::wstring &tool, const std::wstring &arguments) {
  Child child;
  return Start(tool, arguments, &child) ? Wait(&child) : STILL_ACTIVE;
}

bool ContainsLine(const std::string &record, std::string_view prefix) {
  const size_t at = record.find(prefix);
  return at != std::string::npos &&
         (at == 0 || record[at - 1] == '\n') &&
         record.find("\r\n", at) != std::string::npos;
}

bool ExerciseExchange(const std::wstring &tool, const std::wstring &pipe_id,
                      const std::wstring &challenge,
                      const std::wstring &record, bool expect_server_success) {
  Child server;
  if (!Start(tool, L"capture-original-user " + pipe_id + L" " + challenge +
                       L" \"" + record + L"\"",
             &server))
    return false;
  const DWORD client =
      Run(tool, L"prove-current-token " + pipe_id + L" " + challenge);
  const DWORD server_exit = Wait(&server);
  return client == 0 &&
         ((server_exit == 0) == expect_server_success);
}

HANDLE OpenProofPipe(const std::wstring &pipe_id) {
  const std::wstring pipe =
      L"\\\\.\\pipe\\FamoInstallerIdentity-" + pipe_id;
  const ULONGLONG deadline = GetTickCount64() + 5000;
  HANDLE handle = INVALID_HANDLE_VALUE;
  do {
    if (WaitNamedPipeW(pipe.c_str(), 100)) {
      handle = CreateFileW(pipe.c_str(), GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (handle != INVALID_HANDLE_VALUE)
        break;
    }
    Sleep(10);
  } while (GetTickCount64() < deadline);
  return handle;
}

bool WriteRawProof(const std::wstring &pipe_id, const void *data,
                   DWORD bytes) {
  HANDLE handle = OpenProofPipe(pipe_id);
  if (handle == INVALID_HANDLE_VALUE)
    return false;
  DWORD written = 0;
  const bool success =
      WriteFile(handle, data, bytes, &written, nullptr) && written == bytes;
  CloseHandle(handle);
  return success;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  if (argc != 2)
    return 2;
  const std::wstring tool = argv[1];
  wchar_t temp[MAX_PATH]{};
  if (GetTempPathW(ARRAYSIZE(temp), temp) == 0)
    return 3;
  const std::wstring directory =
      std::wstring(temp) + L"FamoIdentity-" + NewNonce();
  if (!CreateDirectoryW(directory.c_str(), nullptr))
    return 4;
  const std::wstring record = directory + L"\\identity.txt";

  const std::wstring pipe_id = NewNonce();
  const std::wstring challenge = NewNonce();
  if (pipe_id.size() != 32 || challenge.size() != 32 ||
      !ExerciseExchange(tool, pipe_id, challenge, record, true))
    return 5;

  std::ifstream input(record, std::ios::binary);
  const std::string content((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
  DWORD session = 0;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &session) || session == 0 ||
      content.find('\0') != std::string::npos ||
      !ContainsLine(content, "format=1") ||
      !ContainsLine(content, "pipe_id=") ||
      !ContainsLine(content, "challenge=") ||
      !ContainsLine(content, "sid=S-1-") ||
      !ContainsLine(content, "session=" + std::to_string(session)) ||
      !ContainsLine(content, "account=") ||
      !(ContainsLine(content, "resume_capable=0") ||
        ContainsLine(content, "resume_capable=1")))
    return 6;

  if (!ExerciseExchange(tool, NewNonce(), NewNonce(), record, false))
    return 7;
  const std::wstring rejected_record = directory + L"\\rejected.txt";
  const std::wstring rejected_pipe = NewNonce();
  const std::wstring expected_challenge = NewNonce();
  Child rejected_server;
  if (!Start(tool, L"capture-original-user " + rejected_pipe + L" " +
                       expected_challenge + L" \"" + rejected_record + L"\"",
             &rejected_server) ||
      Run(tool, L"prove-current-token " + rejected_pipe + L" " +
                    NewNonce()) != 0 ||
      Wait(&rejected_server) == 0 ||
      GetFileAttributesW(rejected_record.c_str()) != INVALID_FILE_ATTRIBUTES)
    return 8;
  const std::wstring odd_record = directory + L"\\odd.txt";
  const std::wstring odd_pipe = NewNonce();
  Child odd_server;
  if (!Start(tool, L"capture-original-user " + odd_pipe + L" " +
                       NewNonce() + L" \"" + odd_record + L"\"",
             &odd_server))
    return 9;
  const char odd = 'x';
  if (!WriteRawProof(odd_pipe, &odd, 1) || Wait(&odd_server) == 0 ||
      GetFileAttributesW(odd_record.c_str()) != INVALID_FILE_ATTRIBUTES)
    return 10;
  const std::wstring silent_record = directory + L"\\silent.txt";
  const std::wstring silent_pipe_id = NewNonce();
  Child silent_server;
  if (!Start(tool, L"capture-original-user " + silent_pipe_id + L" " +
                       NewNonce() + L" \"" + silent_record + L"\"",
             &silent_server))
    return 11;
  HANDLE silent_pipe = OpenProofPipe(silent_pipe_id);
  if (silent_pipe == INVALID_HANDLE_VALUE)
    return 12;
  const DWORD silent_exit = Wait(&silent_server);
  CloseHandle(silent_pipe);
  if (silent_exit == 0 || silent_exit == STILL_ACTIVE ||
      GetFileAttributesW(silent_record.c_str()) != INVALID_FILE_ATTRIBUTES)
    return 13;
  if (Run(tool, L"prove-current-token bad bad") == 0)
    return 14;
  const std::wstring timeout_record = directory + L"\\timeout.txt";
  if (Run(tool, L"capture-original-user " + NewNonce() + L" " + NewNonce() +
                    L" \"" + timeout_record + L"\"") == 0 ||
      GetFileAttributesW(timeout_record.c_str()) != INVALID_FILE_ATTRIBUTES)
    return 15;
  if (Run(tool, L"prove-current-token " + NewNonce() + L" " + NewNonce()) ==
      0)
    return 16;

  DeleteFileW(record.c_str());
  RemoveDirectoryW(directory.c_str());
  std::wcout << L"identity_broker_selfcheck=ok\n";
  return 0;
}
