"""Self-contained protocol-v1 golden-vector and rejection tests.

Run from the repository root with: python protocol/tests/test_conformance.py
The decoder below intentionally has no dependency on either production runtime;
it makes fixture drift and wire-layout changes visible in CI without requiring
an Android SDK or a Windows compiler.
"""

from __future__ import annotations

import json
import struct
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIXTURES = json.loads((ROOT / "golden" / "packets.json").read_text(encoding="utf-8"))["fixtures"]
NTP_FIXTURE = json.loads((ROOT / "golden" / "packets.json").read_text(encoding="utf-8"))["ntp_fixture"]
MAGIC = 0x31424450
VERSION = 1
MAX_PAYLOAD = 8 * 1024 * 1024
KNOWN_CAPABILITIES = 0xFF


class ProtocolError(ValueError):
    pass


def fail_if(condition: bool, message: str) -> None:
    if condition:
        raise ProtocolError(message)


def unpack(fmt: str, data: bytes, offset: int) -> tuple[tuple[int, ...], int]:
    size = struct.calcsize(fmt)
    fail_if(offset + size > len(data), "truncated payload")
    return struct.unpack_from(fmt, data, offset), offset + size


def valid_rotation(value: int) -> bool:
    return value in (0, 90, 180, 270)


def parse_control(payload: bytes) -> dict:
    fail_if(len(payload) < 4, "truncated Control")
    (opcode, body_size), offset = unpack("<HH", payload, 0)
    fail_if(body_size != len(payload) - offset, "Control body size mismatch")
    body = payload[offset:]
    required = {1: 8, 2: 24, 3: 0, 4: 4, 5: 4, 6: 12, 8: 32}
    if opcode in required:
        fail_if(len(body) != required[opcode], "invalid fixed Control body")
    elif opcode == 7:
        fail_if(len(body) < 4, "truncated Disconnect")
        reason, text_size = struct.unpack_from("<HH", body)
        del reason
        fail_if(text_size > 128 or text_size != len(body) - 4, "invalid Disconnect text length")
        body[4:].decode("utf-8", errors="strict")
    else:
        raise ProtocolError("unknown Control opcode")
    if opcode == 4:
        rotation, reserved = struct.unpack("<HH", body)
        fail_if(not valid_rotation(rotation) or reserved != 0, "invalid OrientationChanged")
    if opcode == 5:
        (caps,) = struct.unpack("<I", body)
        fail_if(caps & ~KNOWN_CAPABILITIES, "unknown capability")
    if opcode == 8:
        struct.unpack("<QQQQ", body)
    return {"opcode": opcode, "body": body}


def ntp_metrics(t1: int, t2: int, t3: int, t4: int) -> tuple[int, int]:
    """Return NTP clock offset and round-trip delay in nanoseconds."""
    return ((t2 - t1 + t3 - t4) // 2, (t4 - t1) - (t3 - t2))


def parse_payload(message_type: int, flags: int, payload: bytes) -> dict:
    if message_type == 1:
        fail_if(len(payload) < 28, "truncated ClientHello")
        values = struct.unpack_from("<IHHHHHHIIHH", payload)
        caps, width, height, rotation, touches, pressure, codecs, max_width, max_height, device_size, os_size = values
        fail_if(caps & ~KNOWN_CAPABILITIES, "unknown capability")
        fail_if(width == 0 or height == 0 or not valid_rotation(rotation), "invalid ClientHello display")
        fail_if(touches > 10 or pressure == 0 or not codecs & 1 or max_width == 0 or max_height == 0, "invalid ClientHello limit")
        fail_if(device_size > 64 or os_size > 32 or 28 + device_size + os_size != len(payload), "invalid ClientHello string bounds")
        payload[28:28 + device_size].decode("utf-8", errors="strict")
        payload[28 + device_size:].decode("utf-8", errors="strict")
        return {"capabilities": caps, "device_name": payload[28:28 + device_size].decode()}
    if message_type == 2:
        fail_if(len(payload) != 40, "invalid ServerConfig length")
        session, display, width, height, fps, bitrate, left, top, content_width, content_height, config = struct.unpack("<QIHHHHIIIII", payload)
        fail_if(session == 0 or width == 0 or height == 0 or not 1 <= fps <= 60 or not 20 <= bitrate <= 120, "invalid ServerConfig")
        fail_if(content_width == 0 or content_height == 0 or config & ~1, "invalid ServerConfig mapping")
        return {"session_id": session, "display_id": display, "content_left": left, "content_top": top}
    if message_type == 3:
        fail_if(flags & ~1 or len(payload) < 20, "invalid VideoFrame")
        capture, presentation, access_unit_size = struct.unpack_from("<QQI", payload)
        fail_if(access_unit_size == 0 or access_unit_size != len(payload) - 20, "invalid access unit length")
        return {"capture_ns": capture, "presentation_ns": presentation, "idr": bool(flags & 1), "access_unit": payload[20:]}
    if message_type == 4:
        fail_if(flags != 0 or len(payload) < 12, "invalid InputBatch")
        timestamp, count, reserved = struct.unpack_from("<QHH", payload)
        fail_if(not 1 <= count <= 64 or reserved != 0 or len(payload) != 12 + count * 22, "invalid InputBatch layout")
        samples, previous = [], 0
        for index in range(count):
            delta, pointer, tool, contact, x, y, pressure, tilt_x, tilt_y, distance, buttons = struct.unpack_from("<IHBBHHHhhHH", payload, 12 + index * 22)
            fail_if(pointer == 0 or tool not in (1, 2, 3) or contact & ~0x0F or buttons & ~7, "invalid input sample")
            fail_if(not -9000 <= tilt_x <= 9000 or not -9000 <= tilt_y <= 9000 or (index and delta < previous), "invalid input timing or tilt")
            previous = delta
            samples.append({"delta_us": delta, "pointer_id": pointer, "tool": tool, "x": x, "y": y, "pressure": pressure, "distance": distance})
        return {"timestamp_ns": timestamp, "samples": samples}
    if message_type == 5:
        fail_if(flags != 0, "Control flags must be zero")
        return parse_control(payload)
    raise ProtocolError("unknown message type")


def parse_frame(frame: bytes) -> dict:
    fail_if(len(frame) < 20, "truncated header")
    magic, version, message_type, flags, sequence, payload_size = struct.unpack_from("<IHHIII", frame)
    fail_if(magic != MAGIC, "wrong magic")
    fail_if(version != VERSION, "wrong version")
    fail_if(message_type not in range(1, 6), "unknown message type")
    fail_if(payload_size > MAX_PAYLOAD or payload_size != len(frame) - 20, "invalid payload size")
    return {"type": message_type, "flags": flags, "sequence": sequence, "payload": parse_payload(message_type, flags, frame[20:])}


class ConformanceTests(unittest.TestCase):
    def test_golden_frames_parse_and_cover_all_message_types(self) -> None:
        parsed = {fixture["name"]: parse_frame(bytes.fromhex(fixture["frame_hex"])) for fixture in FIXTURES}
        self.assertEqual({frame["type"] for frame in parsed.values()}, {1, 2, 3, 4, 5})
        self.assertEqual(parsed["client_hello"]["payload"]["device_name"], "Xiaomi Pad 7")
        self.assertEqual(parsed["server_config"]["payload"]["session_id"], 0x0102030405060708)
        self.assertTrue(parsed["video_idr"]["payload"]["idr"])
        self.assertEqual(parsed["input_batch"]["payload"]["samples"][1]["delta_us"], 4000)
        self.assertEqual({parsed[name]["payload"]["opcode"] for name in parsed if name.startswith("control_")}, set(range(1, 9)))

    def test_clock_sync_complete_matches_ntp_fixture(self) -> None:
        complete = next(f for f in FIXTURES if f["name"] == "control_clock_sync_complete")
        parsed = parse_frame(bytes.fromhex(complete["frame_hex"]))
        self.assertEqual(parsed["payload"]["opcode"], 8)
        t1, t2, t3, t4 = struct.unpack("<QQQQ", parsed["payload"]["body"])
        self.assertEqual((t1, t2, t3, t4), tuple(NTP_FIXTURE["timestamps_ns"]))
        self.assertEqual(ntp_metrics(t1, t2, t3, t4), (NTP_FIXTURE["offset_ns"], NTP_FIXTURE["delay_ns"]))

    def test_rejects_malformed_clock_sync_complete_body(self) -> None:
        source = bytearray(bytes.fromhex(next(f for f in FIXTURES if f["name"] == "control_clock_sync_complete")["frame_hex"]))
        struct.pack_into("<H", source, 22, 24)
        with self.assertRaises(ProtocolError): parse_frame(source)

    def test_rejects_header_and_length_corruption(self) -> None:
        source = bytearray(bytes.fromhex(FIXTURES[0]["frame_hex"]))
        for offset, value in ((0, 0), (4, 2), (6, 9)):
            broken = bytearray(source); broken[offset] = value
            with self.assertRaises(ProtocolError): parse_frame(broken)
        broken = bytearray(source); struct.pack_into("<I", broken, 16, len(broken))
        with self.assertRaises(ProtocolError): parse_frame(broken)

    def test_rejects_internal_count_string_and_flag_corruption(self) -> None:
        input_frame = bytearray(bytes.fromhex(next(f["frame_hex"] for f in FIXTURES if f["name"] == "input_batch")))
        struct.pack_into("<H", input_frame, 28, 65)  # payload offset 8: sample count
        with self.assertRaises(ProtocolError): parse_frame(input_frame)
        hello_frame = bytearray(bytes.fromhex(FIXTURES[0]["frame_hex"]))
        struct.pack_into("<H", hello_frame, 44, 65)  # payload offset 24: device name bytes
        with self.assertRaises(ProtocolError): parse_frame(hello_frame)
        video_frame = bytearray(bytes.fromhex(next(f["frame_hex"] for f in FIXTURES if f["name"] == "video_idr")))
        struct.pack_into("<I", video_frame, 8, 2)
        with self.assertRaises(ProtocolError): parse_frame(video_frame)

    def test_runtime_sources_publish_frozen_header_constants(self) -> None:
        cpp = (ROOT / "cpp" / "paddraw_protocol.hpp").read_text(encoding="utf-8")
        kotlin = (ROOT / "kotlin" / "io" / "paddrawboard" / "protocol" / "PdbProtocol.kt").read_text(encoding="utf-8")
        self.assertIn("kFrameHeaderBytes = 20", cpp)
        self.assertIn("const val HEADER_BYTES = 20", kotlin)
        self.assertIn("0x31424450", cpp)
        self.assertIn("0x31424450L", kotlin)


if __name__ == "__main__":
    unittest.main(verbosity=2)
