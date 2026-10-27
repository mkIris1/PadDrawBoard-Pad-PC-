#pragma once

#include <optional>
#include <stop_token>
#include <string>

namespace pdb::adb {

// Asks the already-running adb server for its device list over the host
// protocol instead of starting a new adb.exe process.
//
// The device probe runs continuously while the host waits for a tablet, and a
// process spawn per poll was measurably the largest idle cost of the tray app.
// The server socket answers the same query in well under a millisecond, so the
// poll can stay fast (short detection latency for a freshly plugged tablet)
// while costing essentially no CPU.
//
// Returns std::nullopt when the server is not reachable - for example on a cold
// start before anything has launched it - so the caller falls back to spawning
// adb, which has the side effect of starting the server.
[[nodiscard]] std::optional<std::string> QueryAdbHostDevices(
    std::stop_token stop_token = {});

}  // namespace pdb::adb
