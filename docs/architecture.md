# PadDrawBoard v1 architecture contract
This document freezes the integration boundaries for the first implementation.
Subsystem implementations may evolve internally, but changes to this contract
must be reviewed centrally.

## Supported baseline

- Windows 11 23H2 or newer, x64.
- Xiaomi Pad 7 on HyperOS 2, one authorized device at a time.
- USB transport through Android Debug Bridge only.
- Windows Ink through synthetic pointer injection; no kernel driver or WinTab.
- H.264/AVC, 8-bit SDR, 60 frames per second, no B-frames.

## Process and module boundaries

- `desktop/`: C++20 Windows tray/server, capture, encoding, ADB orchestration,
  input injection, configuration, and diagnostics.
- `android/`: Kotlin Android application, MediaCodec rendering, input capture,
  orientation handling, and capability probe.
- `protocol/`: the sole source of truth for wire constants, layouts, golden
  packets, and protocol conformance tests.
- `tools/`: development-only diagnostics and latency measurement utilities.

## Transport

The desktop binds only to loopback. The Android application reaches these
listeners through `adb reverse`:

| Channel | Device port | Host port | Purpose |
| --- | ---: | ---: | --- |
| Control | 48100 | 48100 | handshake, clock sync, configuration, telemetry |
| Video | 48101 | 48101 | H.264 access units |
| Input | 48102 | 48102 | pen, touch, and button batches |

All channels use the protocol v1 frame header. A session is valid only after a
compatible `ClientHello`/`ServerConfig` exchange on the control channel.

## Frozen protocol header

All integers are little-endian. The 20-byte header is:

| Offset | Type | Field |
| ---: | --- | --- |
| 0 | u32 | magic `0x31424450` (`PDB1`) |
| 4 | u16 | protocol version, currently `1` |
| 6 | u16 | message type |
| 8 | u32 | flags |
| 12 | u32 | sequence number |
| 16 | u32 | payload length |

Message types are `ClientHello=1`, `ServerConfig=2`, `VideoFrame=3`,
`InputBatch=4`, and `Control=5`. Payload layouts and golden vectors live only
under `protocol/`.

## Latency policy

- Capture and encode queues hold at most one pending frame.
- Encoded reference frames are never selectively discarded. If transport
  backpressure would create a backlog, restart the stream at a fresh IDR.
- Input has a dedicated socket and is never queued behind video.
- Native-fit resolution is preferred, then longest-edge ladders 2560 and 1920.
- Release threshold on the reference hardware is median glass-to-glass latency
  at most 50 ms; the optimization target is 35 ms.

## Capability policy

Pressure, hover, tilt, distance, touch, and each pen button are independently
advertised. Unsupported hardware capabilities are reported as unavailable and
are never synthesized. All three Focus Pen buttons must be observable through
ordinary Android events or the no-root ADB probe before a public v1 release.
