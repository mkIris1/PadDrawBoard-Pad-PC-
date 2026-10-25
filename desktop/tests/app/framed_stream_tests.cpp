#include "pdb/app/framed_stream.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <utility>
#include <vector>

namespace pdb::app::testing {

enum class VideoBackpressureAction : std::uint8_t {
  kNone,
  kNotifyIdr,
  kEndSession,
};

VideoBackpressureAction ObserveVideoBackpressure(
    bool* idr_notified, std::int64_t* first_blocked_ns,
    std::int64_t now_ns) noexcept;
void ClearVideoBackpressure(bool* idr_notified,
                            std::int64_t* first_blocked_ns) noexcept;

}  // namespace pdb::app::testing

namespace {

class MemoryStream final : public pdb::app::IByteStream {
 public:
  explicit MemoryStream(std::vector<std::uint8_t> input = {}, std::size_t chunk = 3)
      : input_(std::move(input)), chunk_(chunk) {}

  pdb::app::IoResult Read(std::span<std::uint8_t> destination) override {
    if (position_ == input_.size()) return {pdb::app::IoStatus::kClosed, 0, "eof"};
    const std::size_t count = std::min({chunk_, destination.size(), input_.size() - position_});
    std::copy_n(input_.data() + position_, count, destination.data());
    position_ += count;
    return {pdb::app::IoStatus::kOk, count, {}};
  }

  pdb::app::IoResult Write(std::span<const std::uint8_t> source) override {
    const std::size_t count = std::min(chunk_, source.size());
    output_.insert(output_.end(), source.begin(), source.begin() + static_cast<std::ptrdiff_t>(count));
    return {pdb::app::IoStatus::kOk, count, {}};
  }

  [[nodiscard]] const std::vector<std::uint8_t>& output() const { return output_; }

 private:
  std::vector<std::uint8_t> input_;
  std::vector<std::uint8_t> output_;
  std::size_t position_{};
  std::size_t chunk_{};
};

struct WriteStep {
  std::size_t max_bytes{};
  pdb::app::IoStatus status{pdb::app::IoStatus::kOk};
};

class ScriptedWriteStream final : public pdb::app::IByteStream {
 public:
  explicit ScriptedWriteStream(std::vector<WriteStep> steps) : steps_(std::move(steps)) {}

  pdb::app::IoResult Read(std::span<std::uint8_t>) override {
    return {pdb::app::IoStatus::kClosed, 0, "read not supported"};
  }

  pdb::app::IoResult Write(std::span<const std::uint8_t> source) override {
    if (steps_.empty()) {
      output_.insert(output_.end(), source.begin(), source.end());
      return {pdb::app::IoStatus::kOk, source.size(), {}};
    }
    const WriteStep step = steps_.front();
    steps_.erase(steps_.begin());
    const std::size_t count = std::min(step.max_bytes, source.size());
    output_.insert(output_.end(), source.begin(), source.begin() + static_cast<std::ptrdiff_t>(count));
    return {step.status, count, step.status == pdb::app::IoStatus::kError ? "fatal write" : ""};
  }

  [[nodiscard]] const std::vector<std::uint8_t>& output() const { return output_; }

 private:
  std::vector<WriteStep> steps_;
  std::vector<std::uint8_t> output_;
};

paddrawboard::protocol::Frame HelloFrame() {
  paddrawboard::protocol::ClientHello hello;
  hello.capabilities = paddrawboard::protocol::kCapabilityPressure |
                       paddrawboard::protocol::kCapabilityTouch;
  hello.displayWidth = 3200;
  hello.displayHeight = 2136;
  hello.maxTouchContacts = 10;
  hello.maxPenPressure = 8192;
  hello.videoCodecMask = paddrawboard::protocol::kCodecH264;
  hello.maxVideoWidth = 3200;
  hello.maxVideoHeight = 2136;
  hello.deviceName = "Pad 7";
  hello.osName = "HyperOS";
  return {{0, 42, paddrawboard::protocol::MessageType::ClientHello}, std::move(hello)};
}

paddrawboard::protocol::Frame VideoFrameForTest() {
  paddrawboard::protocol::VideoFrame video;
  video.captureTimestampNs = 123'456'789;
  video.presentationTimestampNs = 123'999'999;
  video.accessUnit.resize(4096);
  for (std::size_t index = 0; index < video.accessUnit.size(); ++index) {
    video.accessUnit[index] = static_cast<std::uint8_t>((index * 37U) & 0xffU);
  }
  return {{paddrawboard::protocol::kVideoFlagIdr, 77,
           paddrawboard::protocol::MessageType::VideoFrame}, std::move(video)};
}

std::vector<paddrawboard::protocol::Frame> AllFramesForSerializationTest() {
  using namespace paddrawboard::protocol;
  auto hello = HelloFrame();
  ServerConfig config{1234, 7, 3200, 2136, 60, 80, 0, 0, 3200, 2136,
                      kConfigFlagSuppressTouchWhilePenInRange};
  InputSample sample{100, 1, ToolType::Pen,
                     static_cast<std::uint8_t>(kInRange | kContact | kPrimary),
                     123, 456, 789, -120, 340, 55, kButton1};
  InputBatch input{987654321, {sample}};
  std::vector<Frame> frames;
  frames.push_back(std::move(hello));
  frames.push_back({{0, 2, MessageType::ServerConfig}, config});
  frames.push_back(VideoFrameForTest());
  frames.push_back({{0, 4, MessageType::InputBatch}, input});
  frames.push_back({{0, 5, MessageType::Control}, Control{ClockSyncRequest{11}}});
  frames.push_back({{0, 6, MessageType::Control}, Control{ClockSyncResponse{1, 2, 3}}});
  frames.push_back({{0, 7, MessageType::Control}, Control{ClockSyncComplete{1, 2, 3, 4}}});
  frames.push_back({{0, 8, MessageType::Control}, Control{RequestIdr{}}});
  frames.push_back({{0, 9, MessageType::Control}, Control{OrientationChanged{90}}});
  frames.push_back({{0, 10, MessageType::Control}, Control{CapabilityChanged{kCapabilityPressure}}});
  frames.push_back({{0, 11, MessageType::Control}, Control{Telemetry{1, 2, 3}}});
  frames.push_back({{0, 12, MessageType::Control}, Control{Disconnect{4, "done"}}});
  return frames;
}

void TestReusableEncodingPreservesWireBytesForEveryPayload() {
  std::vector<std::uint8_t> frame_output;
  std::vector<std::uint8_t> payload_scratch;
  for (const auto& frame : AllFramesForSerializationTest()) {
    const auto legacy = paddrawboard::protocol::encodeFrame(frame);
    paddrawboard::protocol::encodeFrame(frame, frame_output, payload_scratch);
    assert(frame_output == legacy);
  }
}

void TestReusableEncodingRetainsLargeVideoCapacities() {
  const auto frame = VideoFrameForTest();
  std::vector<std::uint8_t> frame_output;
  std::vector<std::uint8_t> payload_scratch;
  paddrawboard::protocol::encodeFrame(frame, frame_output, payload_scratch);
  const auto frame_capacity = frame_output.capacity();
  const auto payload_capacity = payload_scratch.capacity();
  for (int i = 0; i < 8; ++i) {
    paddrawboard::protocol::encodeFrame(frame, frame_output, payload_scratch);
    assert(frame_output.capacity() >= frame_capacity);
    assert(payload_scratch.capacity() >= payload_capacity);
  }
}

void TestExactReadAndWriteHandleShortOperations() {
  const auto source = paddrawboard::protocol::encodeFrame(HelloFrame());
  MemoryStream reader(source, 2);
  paddrawboard::protocol::Frame decoded;
  const auto read = pdb::app::FramedStream::ReadFrame(reader, &decoded);
  assert(read.status == pdb::app::IoStatus::kOk);
  assert(read.transferred == source.size());
  assert(decoded.header.sequence == 42);
  assert(std::holds_alternative<paddrawboard::protocol::ClientHello>(decoded.payload));

  MemoryStream writer({}, 4);
  const auto written = pdb::app::FramedStream::WriteFrame(writer, decoded);
  assert(written.status == pdb::app::IoStatus::kOk);
  assert(writer.output() == source);
}

void TestReaderRejectsOversizedLengthBeforeAllocation() {
  std::vector<std::uint8_t> header(paddrawboard::protocol::kFrameHeaderBytes, 0);
  header[0] = 0x50;
  header[1] = 0x44;
  header[2] = 0x42;
  header[3] = 0x31;
  header[4] = 1;
  header[6] = static_cast<std::uint8_t>(paddrawboard::protocol::MessageType::Control);
  header[16] = 0xff;
  header[17] = 0xff;
  header[18] = 0xff;
  header[19] = 0xff;
  MemoryStream reader(header, header.size());
  paddrawboard::protocol::Frame ignored;
  const auto result = pdb::app::FramedStream::ReadFrame(reader, &ignored);
  assert(result.status == pdb::app::IoStatus::kError);
  assert(result.transferred == header.size());
}

void TestReaderReportsTruncatedHeader() {
  MemoryStream reader({0x50, 0x44, 0x42}, 3);
  paddrawboard::protocol::Frame ignored;
  const auto result = pdb::app::FramedStream::ReadFrame(reader, &ignored);
  assert(result.status == pdb::app::IoStatus::kClosed);
  assert(result.transferred == 3);
}

void TestResumableWriterPreservesExactFrameAcrossWouldBlock() {
  const auto frame = VideoFrameForTest();
  const auto expected = paddrawboard::protocol::encodeFrame(frame);
  ScriptedWriteStream stream({
      {7, pdb::app::IoStatus::kWouldBlock},
      {0, pdb::app::IoStatus::kWouldBlock},
      {std::numeric_limits<std::size_t>::max(), pdb::app::IoStatus::kOk},
  });
  pdb::app::ResumableFrameWriter writer;
  assert(writer.Start(frame).status == pdb::app::IoStatus::kOk);

  const auto blocked = writer.Continue(stream);
  assert(blocked.status == pdb::app::IoStatus::kWouldBlock);
  assert(blocked.transferred == 7);
  assert(writer.pending());
  assert(writer.offset() == 7);

  const auto still_blocked = writer.Continue(stream);
  assert(still_blocked.status == pdb::app::IoStatus::kWouldBlock);
  assert(still_blocked.transferred == 0);
  assert(writer.pending());
  assert(writer.offset() == 7);

  const auto completed = writer.Continue(stream);
  assert(completed.status == pdb::app::IoStatus::kOk);
  assert(!writer.pending());
  assert(stream.output() == expected);
}

void TestResumableWriterReportsFatalPartialWriteForSessionTeardown() {
  const auto frame = VideoFrameForTest();
  const auto expected = paddrawboard::protocol::encodeFrame(frame);
  ScriptedWriteStream stream({{5, pdb::app::IoStatus::kError}});
  pdb::app::ResumableFrameWriter writer;
  assert(writer.Start(frame).status == pdb::app::IoStatus::kOk);

  const auto failed = writer.Continue(stream);
  assert(failed.status == pdb::app::IoStatus::kError);
  assert(failed.transferred == 5);
  assert(writer.pending());
  assert(writer.offset() == 5);
  assert(std::equal(stream.output().begin(), stream.output().end(), expected.begin()));

  // The owning session must close this stream; a new frame cannot be started
  // on a stream whose peer has observed only a prefix of this frame.
  writer.Clear();
  assert(!writer.pending());
}

void TestVideoBackpressureStateNotifiesOnceRecoversAndTimesOut() {
  bool notified = false;
  std::int64_t started_ns = 0;

  assert(pdb::app::testing::ObserveVideoBackpressure(
             &notified, &started_ns, 100) ==
         pdb::app::testing::VideoBackpressureAction::kNotifyIdr);
  assert(notified);
  assert(started_ns == 100);
  assert(pdb::app::testing::ObserveVideoBackpressure(
             &notified, &started_ns, 1'000'000'100) ==
         pdb::app::testing::VideoBackpressureAction::kNone);
  assert(pdb::app::testing::ObserveVideoBackpressure(
             &notified, &started_ns, 2'000'000'100) ==
         pdb::app::testing::VideoBackpressureAction::kEndSession);

  pdb::app::testing::ClearVideoBackpressure(&notified, &started_ns);
  assert(!notified);
  assert(started_ns == 0);
  assert(pdb::app::testing::ObserveVideoBackpressure(
             &notified, &started_ns, 4'000) ==
         pdb::app::testing::VideoBackpressureAction::kNotifyIdr);
  assert(started_ns == 4'000);
}

}  // namespace

int main() {
  TestReusableEncodingPreservesWireBytesForEveryPayload();
  TestReusableEncodingRetainsLargeVideoCapacities();
  TestExactReadAndWriteHandleShortOperations();
  TestReaderRejectsOversizedLengthBeforeAllocation();
  TestReaderReportsTruncatedHeader();
  TestResumableWriterPreservesExactFrameAcrossWouldBlock();
  TestResumableWriterReportsFatalPartialWriteForSessionTeardown();
  TestVideoBackpressureStateNotifiesOnceRecoversAndTimesOut();
  std::cout << "pdb_app_framing_tests: all tests passed\n";
  return 0;
}
