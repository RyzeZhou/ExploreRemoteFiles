/**************************************************************************
    ProbeLog.h - debug logging for Explorer RemoteFS.
    Appends to C:\temp\remotefs-debug.log.

    Performance notes (2026-09-02):
    - The old implementation opened+closed the file on EVERY call. High-
      frequency probes ([ATTR], BindToObject) fired dozens of times per
      Explorer enumeration on the UI thread, so a refresh after a mutation
      became a file-IO storm that froze Explorer (44 MB logs).
    - Now: one FILE* stays open, writes are serialized with a critical
      section, and the log self-truncates past 4 MB.
    - Keep high-frequency probe sites out of the hot paths; reserve this
      log for command-level events ([FTP]/[BRIDGE]/[DBLCLK]/[BG]).
**************************************************************************/
#pragma once

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

inline void ProbeLog(const wchar_t *fmt, ...)
{
    static FILE *s_f = NULL;
    static CRITICAL_SECTION s_lock = {};
    static bool s_init = false;
    if (!s_init)
    {
        InitializeCriticalSection(&s_lock);
        s_init = true;
    }
    EnterCriticalSection(&s_lock);
    if (!s_f)
    {
        _wfopen_s(&s_f, L"C:\\temp\\remotefs-debug.log", L"a");
    }
    if (s_f)
    {
        // Rotate: if the log has grown past ~4 MB, truncate it and start over.
        if (_ftelli64(s_f) > 4LL * 1024 * 1024)
        {
            fclose(s_f);
            _wfopen_s(&s_f, L"C:\\temp\\remotefs-debug.log", L"w");
        }
        if (s_f)
        {
            va_list args;
            va_start(args, fmt);
            vfwprintf(s_f, fmt, args);
            va_end(args);
            fwprintf(s_f, L"\n");
            fflush(s_f);
        }
    }
    LeaveCriticalSection(&s_lock);
}
