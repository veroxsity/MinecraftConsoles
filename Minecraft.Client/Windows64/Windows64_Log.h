#pragma once

#ifdef _WINDOWS64

// ============================================================================
// LceLog  —  lightweight file logger
//
// Writes a timestamped latest.log next to the exe on every run.
// Any previous run's log is renamed to previous.log.
//
// All writes use fputs/fflush directly (never printf) so the logger works in
// every build configuration, including _FINAL_BUILD where printf is disabled.
// Stdout is also redirected to the file so existing printf debug output in
// non-final builds lands in the log automatically.
//
// Usage:
//   LceLog::Init();                       // call once, early in WinMain
//   LceLog::Write("Connected to %s", ip); // any time
//   LceLog::Shutdown();                   // call once, at exit
//
//   Convenience macro with a fixed category tag:
//   LCELOG("RELAY", "host opened for session %s", sid.c_str());
// ============================================================================

#include <cstdarg>

namespace LceLog
{
    void Init();
    void Shutdown();
    void Write(const char* fmt, ...);
}

#define LCELOG(category, fmt, ...) \
    LceLog::Write("[" category "] " fmt, ##__VA_ARGS__)

#endif // _WINDOWS64
