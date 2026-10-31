#pragma once

// The exit codes launch_probe reports with, shared with launch_test so the
// checks name them instead of repeating numbers. They stay clear of 1 and 2
// (the launcher's own errors), 127 (exec failed) and 128 and up (signals).

namespace tatrace::probe {

enum ProbeExit : int {
  Passed = 0,
  BadUsage = 3,
  FdStillOpen = 10,
  ShimNotFirst = 11,
  NoPoolMapping = 12,
  BadMagic = 13,
  NotAttachedPid = 14,
  StaleHeartbeat = 15,
  PoolMapped = 16,
  ExecFailed = 17,
  ThreadFailed = 18,
  MainNotActive = 19,
  WrongClosedCount = 20,
  BadThreadRecords = 21,
  DuplicateRing = 22,
  WrongDisabled = 23,
  WrongLateEvents = 24,
  ForkClaimed = 25,
  ForkFailed = 26,
  StrayRing = 27,
};

} // namespace tatrace::probe
