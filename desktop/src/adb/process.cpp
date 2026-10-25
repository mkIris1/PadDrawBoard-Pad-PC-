#include "pdb/adb/process.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <string_view>
#include <thread>
#include <vector>

namespace pdb::adb {
namespace {

std::wstring Utf8ToWide(std::string_view input) {
  if (input.empty()) {
    return {};
  }
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         input.data(), static_cast<int>(input.size()),
                                         nullptr, 0);
  if (length <= 0) {
    const int fallback_length = MultiByteToWideChar(
        CP_ACP, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (fallback_length <= 0) {
      return {};
    }
    std::wstring result(static_cast<std::size_t>(fallback_length), L'\0');
    MultiByteToWideChar(CP_ACP, 0, input.data(), static_cast<int>(input.size()),
                        result.data(), fallback_length);
    return result;
  }
  std::wstring result(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                      static_cast<int>(input.size()), result.data(), length);
  return result;
}

std::string ReadPipe(HANDLE pipe, const std::atomic<bool>& process_finished) {
  std::string output;
  std::array<char, 4096> buffer{};
  for (;;) {
    DWORD available = 0;
    if (PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) == FALSE) {
      break;
    }

    if (available != 0) {
      const DWORD requested = std::min<DWORD>(available,
                                              static_cast<DWORD>(buffer.size()));
      DWORD bytes_read = 0;
      const BOOL read_succeeded = ReadFile(pipe, buffer.data(), requested,
                                           &bytes_read, nullptr);
      if (bytes_read != 0) {
        output.append(buffer.data(), bytes_read);
      }
      if (read_succeeded == FALSE) {
        break;
      }
      continue;
    }

    // A child process can inherit the pipe's write end, so EOF is not a
    // reliable indication that the process we launched has finished. Once
    // that process has been reaped, an empty pipe is sufficient: all bytes
    // written by it are already buffered and have been drained above.
    if (process_finished.load(std::memory_order_acquire)) {
      break;
    }
    Sleep(2);
  }
  return output;
}

}  // namespace

std::wstring QuoteWindowsArgument(std::wstring_view argument) {
  if (argument.empty()) {
    return L"\"\"";
  }

  bool needs_quotes = false;
  for (const wchar_t character : argument) {
    if (character == L' ' || character == L'\t' || character == L'\n' ||
        character == L'\v' || character == L'\"') {
      needs_quotes = true;
      break;
    }
  }
  if (!needs_quotes) {
    return std::wstring(argument);
  }

  std::wstring result;
  result.push_back(L'\"');
  std::size_t backslashes = 0;
  for (const wchar_t character : argument) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'\"') {
      result.append(backslashes * 2 + 1, L'\\');
      result.push_back(L'\"');
      backslashes = 0;
      continue;
    }
    result.append(backslashes, L'\\');
    backslashes = 0;
    result.push_back(character);
  }
  result.append(backslashes * 2, L'\\');
  result.push_back(L'\"');
  return result;
}

std::string QuoteWindowsArgument(std::string_view argument) {
  const std::wstring wide = QuoteWindowsArgument(Utf8ToWide(argument));
  if (wide.empty()) {
    return {};
  }
  const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                         wide.data(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
  if (length <= 0) {
    return std::string(argument);
  }
  std::string result(static_cast<std::size_t>(length), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                      static_cast<int>(wide.size()), result.data(), length,
                      nullptr, nullptr);
  return result;
}

std::wstring BuildWindowsCommandLine(const std::filesystem::path& executable,
                                     const std::vector<std::string>& arguments) {
  std::wstring command_line = QuoteWindowsArgument(executable.wstring());
  for (const std::string& argument : arguments) {
    command_line.push_back(L' ');
    command_line.append(QuoteWindowsArgument(Utf8ToWide(argument)));
  }
  return command_line;
}

ProcessResult Win32ProcessRunner::Run(const std::filesystem::path& executable,
                                      const std::vector<std::string>& arguments,
                                      const ProcessOptions& options,
                                      std::stop_token stop_token) {
  ProcessResult result;
  SECURITY_ATTRIBUTES security_attributes{};
  security_attributes.nLength = sizeof(security_attributes);
  security_attributes.bInheritHandle = TRUE;

  HANDLE stdout_read = nullptr;
  HANDLE stdout_write = nullptr;
  HANDLE stderr_read = nullptr;
  HANDLE stderr_write = nullptr;
  if (CreatePipe(&stdout_read, &stdout_write, &security_attributes, 0) == FALSE ||
      CreatePipe(&stderr_read, &stderr_write, &security_attributes, 0) == FALSE) {
    result.win32_error = GetLastError();
    if (stdout_read != nullptr) CloseHandle(stdout_read);
    if (stdout_write != nullptr) CloseHandle(stdout_write);
    if (stderr_read != nullptr) CloseHandle(stderr_read);
    if (stderr_write != nullptr) CloseHandle(stderr_write);
    return result;
  }
  if (SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0) == FALSE ||
      SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0) == FALSE) {
    result.win32_error = GetLastError();
    CloseHandle(stdout_read);
    CloseHandle(stdout_write);
    CloseHandle(stderr_read);
    CloseHandle(stderr_write);
    return result;
  }

  STARTUPINFOEXW startup_info{};
  startup_info.StartupInfo.cb = sizeof(startup_info);
  startup_info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup_info.StartupInfo.hStdInput = nullptr;
  startup_info.StartupInfo.hStdOutput = stdout_write;
  startup_info.StartupInfo.hStdError = stderr_write;

  SIZE_T attribute_list_size = 0;
  const BOOL queried_attribute_size = InitializeProcThreadAttributeList(
      nullptr, 1, 0, &attribute_list_size);
  const DWORD query_error = GetLastError();
  if (queried_attribute_size != FALSE ||
      query_error != ERROR_INSUFFICIENT_BUFFER || attribute_list_size == 0) {
    result.win32_error = query_error == ERROR_SUCCESS
                             ? ERROR_INVALID_PARAMETER
                             : query_error;
    CloseHandle(stdout_read);
    CloseHandle(stdout_write);
    CloseHandle(stderr_read);
    CloseHandle(stderr_write);
    return result;
  }

  std::vector<std::uint8_t> attribute_storage(attribute_list_size);
  auto* attribute_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
      attribute_storage.data());
  if (InitializeProcThreadAttributeList(attribute_list, 1, 0,
                                        &attribute_list_size) == FALSE) {
    result.win32_error = GetLastError();
    CloseHandle(stdout_read);
    CloseHandle(stdout_write);
    CloseHandle(stderr_read);
    CloseHandle(stderr_write);
    return result;
  }

  const std::array<HANDLE, 2> inherited_handles{stdout_write, stderr_write};
  if (UpdateProcThreadAttribute(
          attribute_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
          const_cast<HANDLE*>(inherited_handles.data()),
          sizeof(inherited_handles), nullptr, nullptr) == FALSE) {
    result.win32_error = GetLastError();
    DeleteProcThreadAttributeList(attribute_list);
    CloseHandle(stdout_read);
    CloseHandle(stdout_write);
    CloseHandle(stderr_read);
    CloseHandle(stderr_write);
    return result;
  }
  startup_info.lpAttributeList = attribute_list;

  PROCESS_INFORMATION process_info{};
  std::wstring command_line = BuildWindowsCommandLine(executable, arguments);
  DWORD creation_flags = CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT;
  if (options.hide_window) {
    creation_flags |= CREATE_NO_WINDOW;
  }
  const BOOL created = CreateProcessW(
      executable.wstring().c_str(), command_line.data(), nullptr, nullptr, TRUE,
      creation_flags, nullptr, nullptr,
      reinterpret_cast<LPSTARTUPINFOW>(&startup_info), &process_info);
  const DWORD create_error = created == FALSE ? GetLastError() : ERROR_SUCCESS;
  DeleteProcThreadAttributeList(attribute_list);
  CloseHandle(stdout_write);
  CloseHandle(stderr_write);
  if (created == FALSE) {
    result.win32_error = create_error;
    CloseHandle(stdout_read);
    CloseHandle(stderr_read);
    return result;
  }
  CloseHandle(process_info.hThread);
  result.started = true;

  std::string stdout_text;
  std::string stderr_text;
  std::atomic<bool> process_finished{false};
  std::thread stdout_reader(
      [&] { stdout_text = ReadPipe(stdout_read, process_finished); });
  std::thread stderr_reader(
      [&] { stderr_text = ReadPipe(stderr_read, process_finished); });

  const auto started_at = std::chrono::steady_clock::now();
  bool terminated = false;
  for (;;) {
    const DWORD wait_result = WaitForSingleObject(process_info.hProcess, 50);
    if (wait_result == WAIT_OBJECT_0) {
      break;
    }
    if (wait_result == WAIT_FAILED) {
      result.win32_error = GetLastError();
      TerminateProcess(process_info.hProcess, ERROR_GEN_FAILURE);
      terminated = true;
      break;
    }
    if (stop_token.stop_requested()) {
      result.cancelled = true;
      TerminateProcess(process_info.hProcess, ERROR_CANCELLED);
      terminated = true;
      break;
    }
    if (options.timeout.count() >= 0 &&
        std::chrono::steady_clock::now() - started_at >= options.timeout) {
      result.timed_out = true;
      TerminateProcess(process_info.hProcess, ERROR_TIMEOUT);
      terminated = true;
      break;
    }
  }
  if (terminated) {
    WaitForSingleObject(process_info.hProcess, INFINITE);
  }
  process_finished.store(true, std::memory_order_release);
  if (result.win32_error == 0) {
    DWORD process_exit_code = 0;
    if (GetExitCodeProcess(process_info.hProcess, &process_exit_code) == FALSE) {
      result.win32_error = GetLastError();
    } else {
      result.exit_code = process_exit_code;
    }
  }
  CloseHandle(process_info.hProcess);
  stdout_reader.join();
  stderr_reader.join();
  CloseHandle(stdout_read);
  CloseHandle(stderr_read);
  result.stdout_text = std::move(stdout_text);
  result.stderr_text = std::move(stderr_text);
  return result;
}

}  // namespace pdb::adb
