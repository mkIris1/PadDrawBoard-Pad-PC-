# PadDrawBoard protocol v1

`protocol/` is the wire-format source of truth.  The C++ implementation is for
the Windows host and the Kotlin implementation is the Android client's only
protocol dependency.  Neither side may duplicate its constants in an
application module.

## Framing

Every TCP channel carries a sequence of frames.  Integers are unsigned unless
marked `i`, use little-endian byte order, and there is no alignment padding.
The frozen 20-byte header is:

| Offset | Field | Meaning |
| ---: | --- | --- |
| 0 | `u32 magic` | `0x31424450` (`PDB1` on the wire) |
| 4 | `u16 version` | `1` |
| 6 | `u16 message_type` | 1 ClientHello, 2 ServerConfig, 3 VideoFrame, 4 InputBatch, 5 Control |
| 8 | `u32 flags` | message-specific flags |
| 12 | `u32 sequence` | strictly increasing per channel; wrap is permitted |
| 16 | `u32 payload_length` | payload bytes following this header |

Receivers reject a wrong magic, version, type, unknown flags, incomplete
frame, payload larger than 8 MiB, or a payload whose declared internal lengths
do not exactly consume it.  Strings are UTF-8 byte sequences; malformed UTF-8
is rejected by the platform integration before values are presented to users.

## Shared values

`Capability` is a bitset: pressure `0x00000001`, hover `0x00000002`, tilt
`0x00000004`, distance `0x00000008`, touch `0x00000010`, and the first,
second, and third pen buttons `0x00000020`, `0x00000040`, and `0x00000080`.
Unknown capability bits are rejected in v1.  H.264 is codec bit `0x0001`.

Normalized X/Y and pressure are `u16` values from 0 through 65535.  Tilt is
centi-degrees (`i16`, -9000 through 9000).  Distance is device-axis units
scaled by 256 (`u16`); `65535` means unavailable.  Pointer IDs are non-zero
`u16` values.  Tools are pen=1, touch=2, eraser=3.  Contact flags are
in-range=`0x01`, contact=`0x02`, cancelled=`0x04`, primary=`0x08`; button bits
are button-1=`0x0001`, button-2=`0x0002`, button-3=`0x0004`.

## Payload layouts

### ClientHello (type 1, control channel)

Fixed prefix is 28 bytes, followed by the two UTF-8 strings.

| Offset | Field |
| ---: | --- |
| 0 | `u32 capabilities` |
| 4 | `u16 display_width_px` |
| 6 | `u16 display_height_px` |
| 8 | `u16 rotation_degrees` (0, 90, 180, 270) |
| 10 | `u16 max_touch_contacts` (0..10) |
| 12 | `u16 max_pen_pressure` (1..65535) |
| 14 | `u16 video_codec_mask` (must include H.264) |
| 16 | `u32 max_video_width_px` |
| 20 | `u32 max_video_height_px` |
| 24 | `u16 device_name_bytes` (0..64) |
| 26 | `u16 os_name_bytes` (0..32) |
| 28 | `device_name`, then `os_name` |

### ServerConfig (type 2, control channel)

Exactly 40 bytes.  The content rectangle is in selected-display physical
pixels and is the only region accepting normalized input.

| Offset | Field |
| ---: | --- |
| 0 | `u64 session_id` (non-zero) |
| 8 | `u32 display_id` |
| 12 | `u16 video_width_px` |
| 14 | `u16 video_height_px` |
| 16 | `u16 video_fps` (1..60) |
| 18 | `u16 video_bitrate_mbps` (20..120) |
| 20 | `u32 content_left_px` |
| 24 | `u32 content_top_px` |
| 28 | `u32 content_width_px` (non-zero) |
| 32 | `u32 content_height_px` (non-zero) |
| 36 | `u32 config_flags` (`0x1` suppress-touch-while-pen-in-range) |

### VideoFrame (type 3, video channel)

The payload is 20 bytes plus one complete H.264 access unit.  Header flag
`0x00000001` means the access unit contains an IDR; no other video flags are
valid.

| Offset | Field |
| ---: | --- |
| 0 | `u64 capture_timestamp_ns` (host monotonic clock) |
| 8 | `u64 presentation_timestamp_ns` (host monotonic clock) |
| 16 | `u32 access_unit_bytes` (1..8 MiB - 20) |
| 20 | H.264 access unit bytes |

### InputBatch (type 4, input channel)

The prefix is 12 bytes and samples are exactly 22 bytes each.  A sample's
monotonic timestamp is `batch_timestamp_ns + timestamp_delta_us * 1000`;
deltas must be non-decreasing.  A batch contains 1..64 samples.

| Offset | Field |
| ---: | --- |
| 0 | `u64 batch_timestamp_ns` (Android monotonic clock) |
| 8 | `u16 sample_count` |
| 10 | `u16 reserved` (zero) |
| 12 + 0 | `u32 timestamp_delta_us` |
| 12 + 4 | `u16 pointer_id` |
| 12 + 6 | `u8 tool_type` |
| 12 + 7 | `u8 contact_flags` |
| 12 + 8 | `u16 normalized_x` |
| 12 + 10 | `u16 normalized_y` |
| 12 + 12 | `u16 pressure` |
| 12 + 14 | `i16 tilt_x_cdeg` |
| 12 + 16 | `i16 tilt_y_cdeg` |
| 12 + 18 | `u16 distance_q8` |
| 12 + 20 | `u16 buttons` |

### Control (type 5, control channel)

Each control payload begins with `u16 opcode`, `u16 body_bytes`, followed by
exactly `body_bytes`.  Header flags must be zero.

| Opcode | Body layout |
| ---: | --- |
| 1 ClockSyncRequest | `u64 client_send_timestamp_ns` |
| 2 ClockSyncResponse | `u64 client_send_timestamp_ns`, `u64 host_receive_timestamp_ns`, `u64 host_send_timestamp_ns` |
| 3 RequestIdr | empty |
| 4 OrientationChanged | `u16 rotation_degrees`, `u16 reserved=0` |
| 5 CapabilityChanged | `u32 capabilities` |
| 6 Telemetry | `u32 rtt_us`, `u32 video_latency_us`, `u32 dropped_video_frames` |
| 7 Disconnect | `u16 reason`, `u16 message_bytes` (0..128), UTF-8 message |
| 8 ClockSyncComplete | `u64 device_send_ns` (t1), `u64 host_receive_ns` (t2), `u64 host_send_ns` (t3), `u64 device_receive_ns` (t4) |

`ClockSyncComplete` is the pre-release v1 completion message for the four-
timestamp NTP exchange.  Its body is exactly 32 bytes; all four timestamps are
unsigned nanoseconds from the respective monotonic clocks.  The standard NTP
clock offset is `((t2-t1)+(t3-t4))/2`, and round-trip delay is
`(t4-t1)-(t3-t2)`.

## Compatibility and limits

v1 peers only accept version 1 and the listed message/control types.  Opcode 8
is an additive pre-release v1 extension: no v1 artifacts have shipped, so
implementations must be updated together before release.  After v1 ships,
future incompatible layouts or additional message/control types require a new
protocol version because unknown v1 values are rejected.  Never use native
structs or Kotlin object serialization on the wire.

Payload cap is 8 MiB.  `ClientHello` is at most 124 bytes, `ServerConfig` is
40 bytes, `InputBatch` is at most 1420 bytes, and a Control payload is at most
136 bytes.  `VideoFrame` is the sole payload allowed above these control/input
limits.  Golden frames are in `golden/packets.json`; run
`python protocol/tests/test_conformance.py` from the repository root.

The Kotlin/JVM encoder regression is
`tests/kotlin/PdbProtocolJvmRegression.kt`. Run it with
`gradle -p protocol/tests/jvm test`. It locks in full-width little-endian writes
for display dimensions, input axes, signed tilt, timestamps, frame magic, and
sequence numbers.
