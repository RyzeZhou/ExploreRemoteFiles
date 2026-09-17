/**************************************************************************
    ProbeLog.h - debug logging for Explorer RemoteFS.

    日志位置：%LOCALAPPDATA%\ExplorerRemoteFs\logs\remotefs-debug.log
    （2026-09-16 改：早先硬写 C:\temp\remotefs-debug.log，那是开发机的路径，
     装到别人机器上不能这么干 —— 见 MILESTONES 的安装包清单。
     取不到 LOCALAPPDATA 时退回 %TEMP%；再失败就静默关闭日志：
     它只是诊断，绝不能影响资源管理器里的任何功能。）

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
#include <shlobj.h>
#include <strsafe.h>
#include <stdio.h>
#include <stdarg.h>
#include <share.h>

// 计算日志全路径（只算一次）。返回空串表示拿不到可用路径。
inline const wchar_t *ProbeLogPath()
{
    static wchar_t s_path[MAX_PATH] = {};
    static bool s_done = false;
    if (s_done) return s_path;
    s_done = true;

    wchar_t dir[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, dir)))
    {
        StringCchCatW(dir, MAX_PATH, L"\\ExplorerRemoteFs\\logs");
        CreateDirectoryW(dir, NULL);   // 已存在时返回 FALSE，正常情况，忽略
        StringCchCatW(dir, MAX_PATH, L"\\remotefs-debug.log");
        StringCchCopyW(s_path, MAX_PATH, dir);
        return s_path;
    }

    DWORD n = GetTempPathW(MAX_PATH, dir);
    if (n > 0 && n < MAX_PATH - 20)
    {
        StringCchCatW(dir, MAX_PATH, L"remotefs-debug.log");
        StringCchCopyW(s_path, MAX_PATH, dir);
    }
    return s_path;
}

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
        // _SH_DENYNO: keep the log readable by other processes (diagnostics).
        const wchar_t *path = ProbeLogPath();
        if (path[0]) s_f = _wfsopen(path, L"a", _SH_DENYNO);
    }
    if (s_f)
    {
        // Rotate: if the log has grown past ~4 MB, truncate it and start over.
        if (_ftelli64(s_f) > 4LL * 1024 * 1024)
        {
            fclose(s_f);
            const wchar_t *path = ProbeLogPath();
            s_f = path[0] ? _wfsopen(path, L"w", _SH_DENYNO) : NULL;
        }
        if (s_f)
        {
            // ms timestamp prefix so navigation "busy windows" can be measured.
            fwprintf(s_f, L"[%llu] ", GetTickCount64());
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
