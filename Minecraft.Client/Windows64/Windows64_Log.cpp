#include "stdafx.h"

#ifdef _WINDOWS64

#include "Windows64_Log.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>

namespace LceLog
{

static FILE* s_logFile = nullptr;

// ---------------------------------------------------------------------------
// Init — open latest.log (rotating the previous one).
// ---------------------------------------------------------------------------
void Init()
{
    // Rotate: latest → previous
    rename("latest.log", "previous.log");

    if (fopen_s(&s_logFile, "latest.log", "w") != 0 || !s_logFile)
    {
        s_logFile = nullptr;
        OutputDebugStringA("[LceLog] Failed to open latest.log\n");
        return;
    }

    // Write header so it's easy to tell sessions apart.
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char header[128];
    snprintf(header, sizeof(header),
        "=== LCELive session started %04d-%02d-%02d %02d:%02d:%02d ===\n",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);
    fputs(header, s_logFile);
    fflush(s_logFile);
}

// ---------------------------------------------------------------------------
// Shutdown — flush and close.
// ---------------------------------------------------------------------------
void Shutdown()
{
    if (!s_logFile)
        return;

    fputs("=== session ended ===\n", s_logFile);
    fflush(s_logFile);
    fclose(s_logFile);
    s_logFile = nullptr;
}

// ---------------------------------------------------------------------------
// Write — timestamped log entry.  Safe to call before Init (silently skipped)
// and from any thread (FILE* writes are internally serialised on MSVC CRT).
// ---------------------------------------------------------------------------
void Write(const char* fmt, ...)
{
    if (!s_logFile && !IsDebuggerPresent())
        return;

    // Build timestamp
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char timeBuf[24];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d.%03d",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    // Format the caller's message
    char msgBuf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msgBuf, sizeof(msgBuf) - 1, fmt, args);
    va_end(args);
    msgBuf[sizeof(msgBuf) - 1] = '\0';

    // Full line: "[HH:MM:SS.mmm] message\n"
    char line[2048 + 32];
    snprintf(line, sizeof(line), "[%s] %s\n", timeBuf, msgBuf);

    if (s_logFile)
    {
        fputs(line, s_logFile);
        fflush(s_logFile);      // flush every write — we want to see crashes
    }

    // Also send to VS Output window when a debugger is attached.
    OutputDebugStringA(line);
}

} // namespace LceLog

#endif // _WINDOWS64
