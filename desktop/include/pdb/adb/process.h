#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace pdb::adb {

struct ProcessOptions {
  std::chrono::milliseconds timeout{10'000};
  bool hide_window = true;
};

struct ProcessResult {
  bool started = false;
  bool timed_out = false;
  bool cancelled = false;
  std::uint32_t win32_error = 0;
  std::uint32_t exit_code = 0;
  std::string stdout_text;
  std::string stderr_text;

  [[nodiscard]] bool succeeded() const noexcept {
    return started && !timed_out && !cancelled && win32_error == 0 &&
           exit_code == 0;
  }
};

class IProcessRunner {
 public:
  virtual ~IProcessRunner() = default;
  virtual ProcessResult Run(const std::filesystem::path& executable,
                            const std::vector<std::string>& arguments,
                            const ProcessOptions& options,
                            std::stop_token stop_token) = 0;
};

[[nodiscard]] std::string QuoteWindowsArgument(std::string_view argument);
[[nodiscard]] std::wstring QuoteWindowsArgument(std::wstring_view argument);
[[nodiscard]] std::wstring BuildWindowsCommandLine(
    const std::filesystem::path& executable,
    const std::vector<std::string>& arguments);

class Win32ProcessRunner final : public IProcessRunner {
 public:
  ProcessResult Run(const std::filesystem::path& executable,
                    const std::vector<std::string>& arguments,
                    const ProcessOptions& options,
                    std::stop_token stop_token) override;
};

}  // namespace pdb::adb
