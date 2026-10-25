// Manual GPU regression, intentionally excluded from headless CTest runs.
// Usage: pdb_video_memory_probe [seconds >= 60]. Requests an IDR every 5s.
// No screen pixels are saved.
#include "pdb/video/video_pipeline.h"
#include "pdb/video/monitor_enumerator.h"
#include "pdb/video/frame_pacer.h"

#include <psapi.h>

#include <algorithm>
#include <charconv>
#include <iostream>
#include <string_view>
#include <thread>

namespace {
class Metrics final : public pdb::video::VideoTelemetrySink {
 public:
  void OnCapture(const pdb::video::CaptureTelemetry&) override {}
  void OnStreamResetRequested() override { ++resets; }
  void OnEncoderSelected(const std::wstring& name, bool) override {
    std::wcerr << L"Encoder: " << name << L'\n';
  }
  void OnEncode(const pdb::video::EncodeTelemetry& sample) override {
    // Fixed-size histogram, so the measurement itself cannot grow with time.
    const auto ms = std::clamp<std::int64_t>(sample.capture_to_encode_duration.count() / 1000, 0, 100);
    ++latency[static_cast<std::size_t>(ms)];
    ++frames;
    if (sample.idr) ++idrs;
  }
  unsigned P95() const {
    std::uint64_t count{};
    for (unsigned i = 0; i <= 100; ++i) {
      count += latency[i];
      if (count * 100 >= frames * 95) return i;
    }
    return 100;
  }
  std::uint64_t latency[101]{};
  std::uint64_t frames{};
  std::uint64_t resets{};
  std::uint64_t idrs{};
};
}

int main(int argc, char** argv) {
  int seconds = 120;
  if (argc > 2) return 1;
  if (argc == 2) {
    const std::string_view value(argv[1]);
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || seconds < 60) return 1;
  }
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(com)) return 2;
  int result = 0;
  {
    Metrics metrics;
    pdb::video::VideoPipeline pipeline;
    std::vector<pdb::video::MonitorInfo> monitors;
    HRESULT hr = pdb::video::MonitorEnumerator::Enumerate(&monitors);
    if (SUCCEEDED(hr) && !monitors.empty()) hr = pipeline.Start(monitors.front(), 3200, &metrics);
    else hr = E_FAIL;
    const auto start = std::chrono::steady_clock::now();
    auto report = start;
    auto next_frame = start;
    pdb::video::FramePacer pacer;
    SIZE_T warm_memory{}, final_memory{};
    pdb::video::EncodedAccessUnit output;
    std::cout << "seconds,frames,private_bytes,width,height,capture_encode_p95_ms,resets\n";
    while (SUCCEEDED(hr) && std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds)) {
      if (!pipeline.has_pending_encode()) pacer.WaitUntil(next_frame);
      hr = pipeline.EncodeLatest(&output);
      if (hr == S_FALSE && !pipeline.has_pending_encode()) {
        hr = pipeline.CaptureOnce(16);
        if (hr == S_OK) hr = pipeline.EncodeLatest(&output);
      }
      if (hr == S_OK && !output.bytes.empty()) {
        next_frame = output.acquired_at + std::chrono::nanoseconds(1'000'000'000 / 60);
      } else if (hr == S_FALSE && pipeline.has_pending_encode()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      const auto now = std::chrono::steady_clock::now();
      if (now - report >= std::chrono::seconds(5)) {
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        if (!GetProcessMemoryInfo(GetCurrentProcess(),
              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) { hr = E_FAIL; break; }
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
        if (!warm_memory && elapsed >= 20) warm_memory = memory.PrivateUsage;
        final_memory = memory.PrivateUsage;
        const auto size = pipeline.encode_size();
        std::cout << elapsed << ',' << metrics.frames << ',' << final_memory << ','
                  << size.width << ',' << size.height << ',' << metrics.P95() << ',' << metrics.resets << std::endl;
        report = now;
        (void)pipeline.NotifyTransportWouldBlock();
      }
    }
    if (FAILED(hr)) {
      std::cerr << "Pipeline failed at " << pipeline.last_failure_stage() << ": " << std::hex << hr << '\n';
      result = 3;
    } else if (!warm_memory || metrics.idrs < metrics.resets ||
               metrics.frames < static_cast<std::uint64_t>(seconds) * 30 ||
               final_memory > warm_memory + 16u * 1024u * 1024u) {
      std::cerr << "FAIL: no sustained video or memory grew >16 MiB after warmup\n";
      result = 4;
    }
    pipeline.Stop();
  }
  CoUninitialize();
  return result;
}
