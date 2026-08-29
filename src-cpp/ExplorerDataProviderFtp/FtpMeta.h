#pragma once
// Shared PIDL-external FTP metadata cache for the Microsoft-core namespace.
// Keyed by directory path; short TTL; cleared after successful mutations.
// Explorer process performs no network I/O (spawns the CLI bridge), but we
// avoid spawning a new process for every Properties dialog / Details cell.
#include <windows.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <string>
#include <vector>

typedef struct
{
    DWORD   dwMode;
    DWORD   dwSize;
    DWORD   dwMtime;
    DWORD   dwUid;      // 0xFFFFFFFF = unknown; 0 (root) is a VALID value
    DWORD   dwGid;
    BOOL    fIsFolder;
    BOOL    fIsSymlink;
    WCHAR   szOwner[40];
    WCHAR   szGroup[40];
    WCHAR   szName[MAX_PATH];
} FTPENTRY;

struct FtpCacheEntry
{
    WCHAR site[64];
    WCHAR path[600];
    ULONGLONG tick;
    std::vector<FTPENTRY> items;
};

inline std::vector<FtpCacheEntry> &FtpCacheEntries()
{
    static std::vector<FtpCacheEntry> v;
    return v;
}
inline SRWLOCK &FtpCacheLock()
{
    static SRWLOCK l = SRWLOCK_INIT;
    return l;
}

inline void FtpCacheClear()
{
    AcquireSRWLockExclusive(&FtpCacheLock());
    FtpCacheEntries().clear();
    ReleaseSRWLockExclusive(&FtpCacheLock());
}

// Returns count of cached entries for site+path (0 = miss/expired).
inline int FtpCacheLookup(PCWSTR site, PCWSTR path, FTPENTRY *out, int maxOut)
{
    if (maxOut <= 0) return 0;
    PCWSTR key = (path && path[0]) ? path : L"/";
    ULONGLONG now = GetTickCount64();
    int got = 0;
    AcquireSRWLockShared(&FtpCacheLock());
    auto &v = FtpCacheEntries();
    for (auto &e : v)
    {
        if (0 == StrCmp(e.path, key) && 0 == StrCmp(e.site, site))
        {
            if (now - e.tick < 30000)
            {
                int n = (int)e.items.size();
                if (n > maxOut) n = maxOut;
                for (int i = 0; i < n; i++) out[i] = e.items[i];
                got = n;
            }
            break;
        }
    }
    ReleaseSRWLockShared(&FtpCacheLock());
    return got;
}

inline void FtpCacheStore(PCWSTR site, PCWSTR path, const FTPENTRY *items, int count)
{
    if (!site || !site[0] || count < 0) return;
    PCWSTR key = (path && path[0]) ? path : L"/";
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&FtpCacheLock());
    auto &v = FtpCacheEntries();
    // Replace existing entry for this site+path.
    for (auto it = v.begin(); it != v.end(); )
    {
        if (0 == StrCmp(it->path, key) && 0 == StrCmp(it->site, site)) it = v.erase(it);
        else ++it;
    }
    FtpCacheEntry e;
    StringCchCopy(e.site, ARRAYSIZE(e.site), site);
    StringCchCopy(e.path, ARRAYSIZE(e.path), key);
    e.tick = now;
    for (int i = 0; i < count; i++) e.items.push_back(items[i]);
    v.push_back(std::move(e));
    // Prune expired entries and cap size.
    for (auto it = v.begin(); it != v.end(); )
    {
        if (now - it->tick >= 30000) it = v.erase(it);
        else ++it;
    }
    while (v.size() > 32) v.erase(v.begin());
    ReleaseSRWLockExclusive(&FtpCacheLock());
}

// Spawn the CLI bridge once per site+path per TTL, parse tab-separated items,
// cache the result. Returns item count (0 on failure).
inline int FtpListCached(PCWSTR site, PCWSTR path, FTPENTRY *out, int maxItems)
{
    int cached = FtpCacheLookup(site, path, out, maxItems);
    if (cached > 0) return cached;

    PCWSTR pszPath = (path && path[0]) ? path : L"/";
    WCHAR cmd[1200];
    StringCchPrintf(cmd, ARRAYSIZE(cmd),
        L"\"D:\\tools\\explorer-remote-fs\\dist\\cli\\ExplorerRemoteFs.Cli.exe\" pipe \"%s\" \"%s\"",
        site, pszPath);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return 0;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = {};
    BOOL spawned = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(wr);
    if (!spawned) { CloseHandle(rd); return 0; }
    std::string text; char buf[4096]; DWORD got = 0;
    while (ReadFile(rd, buf, sizeof(buf), &got, NULL) && got) text.append(buf, got);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 8000);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);

    int count = 0;
    size_t pos = 0;
    while (count < maxItems && pos < text.size())
    {
        size_t end = text.find('\n', pos); if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos); pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("ITEM\t", 0) != 0) continue;
        WCHAR wide[2048];
        if (!MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, wide, ARRAYSIZE(wide))) continue;
        WCHAR *fields[12] = {}; int nf = 0; WCHAR *ctx = NULL;
        WCHAR *tok = wcstok_s(wide, L"\t", &ctx);
        while (tok && nf < 12) { fields[nf++] = tok; tok = wcstok_s(NULL, L"\t", &ctx); }
        if (nf < 10) continue;
        FTPENTRY &item = out[count];
        DWORD mode = 0;
        if (fields[1]) { for (int i = 0; i < 9; i++) { if (fields[1][i + 1] != L'-') mode |= (0400 >> i); } }
        item.dwMode  = mode;
        item.dwMtime = (DWORD)_wtoi64(fields[2]);
        item.dwSize  = (DWORD)_wtoi64(fields[3]);
        StringCchCopy(item.szOwner, ARRAYSIZE(item.szOwner), fields[4]);
        StringCchCopy(item.szGroup, ARRAYSIZE(item.szGroup), fields[5]);
        item.fIsFolder  = _wtoi(fields[6]) != 0;
        item.fIsSymlink = _wtoi(fields[7]) != 0;
        item.dwUid = (nf > 10 && fields[10] && fields[10][0]) ? (DWORD)_wtoi64(fields[10]) : 0xFFFFFFFF;
        item.dwGid = (nf > 11 && fields[11] && fields[11][0]) ? (DWORD)_wtoi64(fields[11]) : 0xFFFFFFFF;
        StringCchCopy(item.szName, ARRAYSIZE(item.szName), fields[9]);
        count++;
    }
    // Only cache when the listing actually succeeded: a FAIL: output (bad
    // credentials, missing dir, CLI error) must not be cached as an "empty dir".
    if (text.rfind("FAIL:", 0) == std::string::npos)
    {
        FtpCacheStore(site, pszPath, out, count);
    }
    return count;
}
