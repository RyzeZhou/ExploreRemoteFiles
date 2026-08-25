/**************************************************************************
    ProbeLog.h - debug logging for the Microsoft sample (comparison probe).
    Appends to C:\temp\remotefs-debug.log, same format as RemoteFsShell.
**************************************************************************/
#pragma once

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

inline void ProbeLog(const wchar_t *fmt, ...)
{
    FILE *f = NULL;
    if (_wfopen_s(&f, L"C:\\temp\\remotefs-debug.log", L"a") == 0 && f)
    {
        va_list args;
        va_start(args, fmt);
        vfwprintf(f, fmt, args);
        va_end(args);
        fwprintf(f, L"\n");
        fclose(f);
    }
}
