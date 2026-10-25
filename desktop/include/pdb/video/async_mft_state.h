#pragma once

#include <cstdint>
#include <optional>

namespace pdb::video {

// Pure state for an event-driven asynchronous IMFTransform. It deliberately
// has no Media Foundation or D3D dependency so event ordering can be tested
// on any build host.
enum class AsyncMftEvent {
  kNeedInput,
  kHaveOutput,
  kDrainComplete,
  kEndOfStream,
};

class AsyncMftStateMachine final {
 public:
  void StartStream() noexcept {
    input_requested_ = false;
    output_available_ = false;
    draining_ = false;
    drain_complete_ = false;
    end_of_stream_ = false;
    faulted_ = false;
    restart_requested_ = false;
    pending_sequence_.reset();
  }

  void OnEvent(AsyncMftEvent event) noexcept {
    switch (event) {
      case AsyncMftEvent::kNeedInput:
        if (!draining_ && !faulted_) input_requested_ = true;
        break;
      case AsyncMftEvent::kHaveOutput:
        if (!faulted_) output_available_ = true;
        break;
      case AsyncMftEvent::kDrainComplete:
        drain_complete_ = true;
        break;
      case AsyncMftEvent::kEndOfStream:
        end_of_stream_ = true;
        break;
    }
  }

  // One input may be in flight at a time. Some encoders request another input
  // before exposing the previous output; accepting it would add latency.
  [[nodiscard]] bool CanAcceptInput() const noexcept {
    return input_requested_ && !pending_sequence_.has_value() && !draining_ && !faulted_;
  }
  [[nodiscard]] bool HasOutput() const noexcept { return output_available_; }
  [[nodiscard]] bool HasPendingInput() const noexcept { return pending_sequence_.has_value(); }
  [[nodiscard]] std::optional<std::uint64_t> pending_sequence() const noexcept {
    return pending_sequence_;
  }
  [[nodiscard]] bool draining() const noexcept { return draining_; }
  [[nodiscard]] bool drain_complete() const noexcept { return drain_complete_; }
  [[nodiscard]] bool end_of_stream() const noexcept { return end_of_stream_; }
  [[nodiscard]] bool faulted() const noexcept { return faulted_; }

  [[nodiscard]] bool OnInputAccepted(std::uint64_t sequence) noexcept {
    if (!CanAcceptInput()) return false;
    input_requested_ = false;
    pending_sequence_ = sequence;
    return true;
  }

  // A HaveOutput event can be stale. The caller must first check this method's
  // result before associating a produced access unit with an input sequence.
  [[nodiscard]] std::optional<std::uint64_t> OnOutputProduced() noexcept {
    output_available_ = false;
    const auto sequence = pending_sequence_;
    pending_sequence_.reset();
    return sequence;
  }

  void OnOutputUnavailable() noexcept { output_available_ = false; }

  void BeginDrain() noexcept {
    input_requested_ = false;
    draining_ = true;
  }

  void MarkFatal() noexcept {
    input_requested_ = false;
    output_available_ = false;
    faulted_ = true;
    restart_requested_ = true;
  }

  [[nodiscard]] bool TakeRestartRequest() noexcept {
    const bool requested = restart_requested_;
    restart_requested_ = false;
    return requested;
  }

 private:
  bool input_requested_{};
  bool output_available_{};
  bool draining_{};
  bool drain_complete_{};
  bool end_of_stream_{};
  bool faulted_{};
  bool restart_requested_{};
  std::optional<std::uint64_t> pending_sequence_;
};

}  // namespace pdb::video
