/**************************************************************************
    RemoteFsShell - remote data source (bridge to C# Provider CLI).

    No network IO inside explorer.  Listing is obtained by spawning the
    C# bridge (ExplorerRemoteFs.Cli.exe "pipe" command, FluentFTP backend)
    and parsing its tab-separated stdout:
      ITEM\tmode\tmtimeUnix\tsize\towner\tgroup\tisFolder\tisSymlink\tremotePath\tname
**************************************************************************/

#include <windows.h>
#include <stdio.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <time.h>
#include <map>
#include <string>
#include "FtpSource.h"

// ---------------------------------------------------------------------------
// Path to the C# bridge.  Phase 2 will replace the per-call process spawn
// with a persistent named-pipe service (RemoteFS.Service).
// ---------------------------------------------------------------------------
static const wchar_t *c_szBridgeExe = L"D:\\tools\\explorer-remote-fs\\dist\\cli\\ExplorerRemoteFs.Cli.exe";
static const wchar_t *c_szConnName  = L"local-ftp";

// ---------------------------------------------------------------------------
// Simple result cache: the shell enumerates the same directory repeatedly
// (tree view + details pane + columns).  Caching avoids a process spawn per
// call.  TTL 3 s is enough to stay fresh while killing the repetition.
// ---------------------------------------------------------------------------
struct CacheEntry
{
    __int64      tCreate;    // GetTickCount64() at fill time
    ITEMDATA     items[64];
    int          count;
};

static std::map<std::wstring, CacheEntry> g_cache;
// SRWLOCK 用静态初始化（SRWLOCK_INIT），不需要显式初始化调用——
// 避免 explorer 多线程首次枚举时并发 InitializeCriticalSection 的竞态
// （重复初始化同一 CS 会导致堆损坏 0xc0000374）。
static SRWLOCK g_cacheLock = SRWLOCK_INIT;

// ---------------------------------------------------------------------------
// Last-enumerated directory path (recovery for the context menu handler).
// Explorer hands IShellExtInit a CIDA whose parent PIDL stops at the
// connection segment (missing the current directory, e.g. www), so the
// handler cannot rebuild "/www" from it.  The enumerator always knows the
// real path it listed, so we record it here and let the handler read it
// back with a short TTL to avoid cross-window confusion.
// ---------------------------------------------------------------------------
static SRWLOCK g_pathLock = SRWLOCK_INIT;
static WCHAR g_lastDeepPath[512] = {};
static PIDLIST_ABSOLUTE g_lastDeepPidl = NULL;
static __int64 g_lastDeepTick = 0;
static int g_lastDeepLevel = 0;

void RememberEnumPath(int nLevel, const wchar_t *pszPath, PCIDLIST_ABSOLUTE pidlFull)
{
    // Only deep directories (level >= 2) need the cache: for the connection
    // root (level 1) an empty rebuilt path already means "/", which is
    // correct.  The tree view enumerates level 1 repeatedly and would
    // otherwise clobber the deep-directory entry with "/".
    if (nLevel < 2)
    {
        return;
    }
    AcquireSRWLockExclusive(&g_pathLock);
    StringCchCopy(g_lastDeepPath, ARRAYSIZE(g_lastDeepPath), pszPath ? pszPath : L"");
    CoTaskMemFree(g_lastDeepPidl);
    g_lastDeepPidl = pidlFull ? ILCloneFull(pidlFull) : NULL;
    g_lastDeepTick = GetTickCount64();
    g_lastDeepLevel = nLevel;
    ReleaseSRWLockExclusive(&g_pathLock);
    DebugLog(L"[FS] RememberEnumPath level=%d path='%s' pidl=%p", nLevel, g_lastDeepPath, g_lastDeepPidl);
}

const wchar_t *GetLastEnumPath()
{
    AcquireSRWLockExclusive(&g_pathLock);
    const wchar_t *psz = L"";
    // TTL 5 s: long enough for a right-click right after navigating,
    // short enough to avoid acting on a stale folder from another window.
    if (g_lastDeepTick && (GetTickCount64() - g_lastDeepTick) < 5000)
    {
        psz = g_lastDeepPath;
    }
    ReleaseSRWLockExclusive(&g_pathLock);
    return psz;
}

PCIDLIST_ABSOLUTE GetLastEnumPidl()
{
    AcquireSRWLockExclusive(&g_pathLock);
    PCIDLIST_ABSOLUTE p = NULL;
    if (g_lastDeepPidl && g_lastDeepTick && (GetTickCount64() - g_lastDeepTick) < 5000)
    {
        p = ILCloneFull(g_lastDeepPidl);
    }
    ReleaseSRWLockExclusive(&g_pathLock);
    return p;
}

// Parse "-rwxr-xr-x" (10 chars) into unix permission bits.
static DWORD ModeFromString(const wchar_t *pszMode)
{
    DWORD mode = 0;
    if (!pszMode)
    {
        return 0;
    }
    for (int i = 0; i < 9; i++)
    {
        if (pszMode[i + 1] != L'-')
        {
            mode |= (0400 >> i);
        }
    }
    return mode;
}

// ---------------------------------------------------------------------------
// Spawn the bridge and read its stdout into a buffer.
// Returns FALSE on any failure (process spawn / timeout / no output).
// ---------------------------------------------------------------------------
static BOOL RunCli(const wchar_t *cmdline, std::string &out)
{
    WCHAR szCmdBuf[1600];
    StringCchCopy(szCmdBuf, ARRAYSIZE(szCmdBuf), cmdline ? cmdline : L"");

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE hOutRead = NULL, hOutWrite = NULL;
    if (!CreatePipe(&hOutRead, &hOutWrite, &sa, 0))
    {
        return FALSE;
    }
    // Make sure the write handle is inherited by the child only.
    SetHandleInformation(hOutRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hOutWrite;
    si.hStdError  = hOutWrite;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi = { 0 };

    BOOL ok = CreateProcessW(NULL, szCmdBuf, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(hOutWrite);
    if (!ok)
    {
        CloseHandle(hOutRead);
        return FALSE;
    }

    // Read all stdout (the bridge output is small).
    char buf[4096];
    DWORD read = 0;
    for (;;)
    {
        if (!ReadFile(hOutRead, buf, sizeof(buf), &read, NULL) || read == 0)
        {
            break;
        }
        out.append(buf, read);
        if (out.size() > 256 * 1024)   // safety cap
        {
            break;
        }
    }
    CloseHandle(hOutRead);

    // Wait for the process to exit (bounded).
    DWORD wait = WaitForSingleObject(pi.hProcess, 8000);
    if (wait == WAIT_TIMEOUT)
    {
        TerminateProcess(pi.hProcess, 1);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return TRUE;   // process spawned; caller inspects the output
}

// Parse one "ITEM\tmode\tmtime\tsize\towner\tgroup\tisFolder\tisSymlink\tpath\tname"
// line into an ITEMDATA.
static BOOL ParsePipeLine(const char *line, ITEMDATA *item)
{
    // Convert to wide, split by tab.
    WCHAR wline[2048];
    if (MultiByteToWideChar(CP_UTF8, 0, line, -1, wline, ARRAYSIZE(wline)) <= 0)
    {
        return FALSE;
    }

    wchar_t *fields[10] = { 0 };
    int n = 0;
    wchar_t *ctx = NULL;
    wchar_t *tok = wcstok_s(wline, L"\t", &ctx);
    while (tok && n < 10)
    {
        fields[n++] = tok;
        tok = wcstok_s(NULL, L"\t", &ctx);
    }
    if (n < 10 || 0 != StrCmpW(fields[0], L"ITEM"))
    {
        return FALSE;
    }

    item->dwMode    = ModeFromString(fields[1]);       // "-rwxr-xr-x"
    item->dwMtime   = (DWORD)_wtoi64(fields[2]);        // unix epoch seconds
    item->dwSize    = (DWORD)_wtoi64(fields[3]);
    StringCchCopy(item->szOwner, ARRAYSIZE(item->szOwner), fields[4]);
    StringCchCopy(item->szGroup, ARRAYSIZE(item->szGroup), fields[5]);
    item->fIsFolder  = (_wtoi(fields[6]) != 0);
    item->fIsSymlink = (_wtoi(fields[7]) != 0);
    StringCchCopy(item->szName, ARRAYSIZE(item->szName), fields[9]);
    item->nLevel = 0; // caller sets
    return TRUE;
}

// ---------------------------------------------------------------------------
// Public entry point (same signature as before).
// ---------------------------------------------------------------------------
// Command wrapper for listing (pipe op).
static BOOL RunBridge(const wchar_t *path, std::string &out)
{
    WCHAR szCmd[1200];
    HRESULT hr;
    if (path && path[0])
    {
        hr = StringCchPrintf(szCmd, ARRAYSIZE(szCmd),
                             L"\"%s\" pipe \"%s\" \"%s\"",
                             c_szBridgeExe, c_szConnName, path);
    }
    else
    {
        hr = StringCchPrintf(szCmd, ARRAYSIZE(szCmd),
                             L"\"%s\" pipe \"%s\" \"/\"",
                             c_szBridgeExe, c_szConnName);
    }
    if (FAILED(hr))
    {
        return FALSE;
    }
    return RunCli(szCmd, out);
}

// ---- file operations ----
static int FsOpCommon(const wchar_t *op, const wchar_t *arg1, const wchar_t *arg2)
{
    WCHAR szCmd[1600];
    HRESULT hr;
    if (arg2 && arg2[0])
    {
        hr = StringCchPrintf(szCmd, ARRAYSIZE(szCmd),
                             L"\"%s\" %s \"%s\" \"%s\" \"%s\"",
                             c_szBridgeExe, op, c_szConnName, arg1, arg2);
    }
    else
    {
        hr = StringCchPrintf(szCmd, ARRAYSIZE(szCmd),
                             L"\"%s\" %s \"%s\" \"%s\"",
                             c_szBridgeExe, op, c_szConnName, arg1);
    }
    if (FAILED(hr))
    {
        return -1;
    }
    DebugLog(L"[FS] FsOpCommon cmd='%s'", szCmd);
    std::string out;
    if (!RunCli(szCmd, out))
    {
        DebugLog(L"[FS] FsOpCommon RunCli FAILED (spawn)");
        return -1;
    }
    DebugLog(L"[FS] FsOpCommon output='%.300s'", out.c_str());
    // The bridge writes "FAIL:" to stderr on errors.
    return out.find("FAIL") != std::string::npos ? -1 : 0;
}

int FsOpDelete(const wchar_t *path)
{
    return FsOpCommon(L"delete", path, NULL);
}

int FsOpRename(const wchar_t *from, const wchar_t *to)
{
    return FsOpCommon(L"rename", from, to);
}

int FsOpMkdir(const wchar_t *path)
{
    return FsOpCommon(L"mkdir", path, NULL);
}

int FtpListDirectory(const wchar_t *path, ITEMDATA *out, int maxItems)
{
    if (!out || maxItems <= 0)
    {
        return -1;
    }

    std::wstring key = path ? path : L"";
    __int64 now = GetTickCount64();

    // Serve from cache if fresh.
    AcquireSRWLockExclusive(&g_cacheLock);
    auto it = g_cache.find(key);
    if (it != g_cache.end() && (now - it->second.tCreate) < 3000)
    {
        int n = min(it->second.count, maxItems);
        for (int i = 0; i < n; i++)
        {
            out[i] = it->second.items[i];
        }
        ReleaseSRWLockExclusive(&g_cacheLock);
        return n;
    }
    ReleaseSRWLockExclusive(&g_cacheLock);

    // Miss: spawn the bridge.
    std::string raw;
    if (!RunBridge(key.c_str(), raw))
    {
        return -1;
    }
    // The bridge writes "FAIL: ..." to stderr (merged into stdout) on connect
    // errors; an empty output means an empty directory.
    if (raw.find("FAIL") != std::string::npos)
    {
        return -1;
    }

    int count = 0;
    const char *p = raw.c_str();
    while (*p && count < maxItems)
    {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len > 0)
        {
            std::string line(p, len);
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            if (line.compare(0, 4, "ITEM") == 0)
            {
                ITEMDATA item = {};
                if (ParsePipeLine(line.c_str(), &item))
                {
                    out[count++] = item;
                }
            }
        }
        if (!eol)
        {
            break;
        }
        p = eol + 1;
    }

    // Store in cache.
    AcquireSRWLockExclusive(&g_cacheLock);
    CacheEntry &ce = g_cache[key];
    ce.tCreate = now;
    ce.count = min(count, 64);
    for (int i = 0; i < ce.count; i++)
    {
        ce.items[i] = out[i];
    }
    ReleaseSRWLockExclusive(&g_cacheLock);

    return count;
}
