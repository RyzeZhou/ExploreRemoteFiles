#pragma once
// Shared PIDL-external FTP metadata cache for the Microsoft-core namespace.
// Keyed by directory path; short TTL; cleared after successful mutations.
// Explorer process performs no network I/O (spawns the CLI bridge), but we
// avoid spawning a new process for every Properties dialog / Details cell.
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <wchar.h>
#include <wctype.h>
#include <string>
#include <vector>
#include "ProbeLog.h"

// Display language for strings created directly by the Explorer extension.
// zh-CN and en-US are built into the binary. Other language codes are read
// from %APPDATA%\ExplorerRemoteFs\explorer-translations.yaml.
struct ExplorerTranslation { std::wstring key; std::wstring value; };
inline void ExplorerTrim(std::wstring &text)
{
    size_t first = text.find_first_not_of(L" \t\r");
    size_t last = text.find_last_not_of(L" \t\r");
    text = first == std::wstring::npos ? L"" : text.substr(first, last - first + 1);
}
inline void ExplorerUnescapeYaml(std::wstring &text)
{
    std::wstring result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == L'\\' && i + 1 < text.size())
        {
            WCHAR escaped = text[++i];
            if (escaped == L'n') { result += L'\n'; continue; }
            if (escaped == L'r') { result += L'\r'; continue; }
            if (escaped == L't') { result += L'\t'; continue; }
            result += escaped;
            continue;
        }
        result += text[i];
    }
    text.swap(result);
}inline void ExplorerGetLanguage(PWSTR value, UINT cch)
{
    if (!value || cch == 0) return;
    StringCchCopyW(value, cch, L"zh-CN");
    DWORD cb = cch * sizeof(WCHAR);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\ExplorerRemoteFs", L"ExplorerLanguage",
                 RRF_RT_REG_SZ, NULL, value, &cb);
}
inline BOOL ExplorerReadTranslations(PCWSTR language, std::vector<ExplorerTranslation> &out)
{
    out.clear();
    WCHAR appData[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appData, ARRAYSIZE(appData));
    if (!n || n >= ARRAYSIZE(appData)) return FALSE;
    WCHAR path[MAX_PATH] = {};
    if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\ExplorerRemoteFs\\explorer-translations.yaml", appData))) return FALSE;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    DWORD size = GetFileSize(file, NULL);
    if (size == INVALID_FILE_SIZE || size > 512 * 1024) { CloseHandle(file); return FALSE; }
    std::string bytes(size, '\0'); DWORD read = 0;
    BOOL ok = size == 0 || ReadFile(file, &bytes[0], size, &read, NULL);
    CloseHandle(file);
    if (!ok || read != size) return FALSE;
    int cch = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), NULL, 0);
    if (cch <= 0) return FALSE;
    std::wstring yaml((size_t)cch, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), &yaml[0], cch);
    std::wstring active;
    size_t pos = 0;
    while (pos <= yaml.size())
    {
        size_t eol = yaml.find(L'\n', pos); if (eol == std::wstring::npos) eol = yaml.size();
        std::wstring line = yaml.substr(pos, eol - pos); pos = eol + 1;
        if (line.empty() || line[0] == L'#') continue;
        BOOL indented = line[0] == L' ' || line[0] == L'\t';
        ExplorerTrim(line); if (line.empty() || line[0] == L'#') continue;
        size_t colon = line.find(L':'); if (colon == std::wstring::npos) continue;
        if (!indented)
        {
            active = line.substr(0, colon); ExplorerTrim(active);
            continue;
        }
        if (0 != lstrcmpiW(active.c_str(), language)) continue;
        std::wstring key = line.substr(0, colon), value = line.substr(colon + 1);
        ExplorerTrim(key); ExplorerTrim(value);
        if (value.size() >= 2 && ((value.front() == L'"' && value.back() == L'"') || (value.front() == L'\'' && value.back() == L'\'')))
            value = value.substr(1, value.size() - 2);
        ExplorerUnescapeYaml(value);
        if (!key.empty() && !value.empty()) out.push_back({ key, value });
    }
    return !out.empty();
}
inline PCWSTR ExplorerText(PCWSTR key, PCWSTR zh, PCWSTR en)
{
    WCHAR language[32] = {}; ExplorerGetLanguage(language, ARRAYSIZE(language));
    if (0 == lstrcmpiW(language, L"zh-CN")) return zh;
    if (0 == lstrcmpiW(language, L"en-US")) return en;
    static std::wstring loadedLanguage;
    static std::vector<ExplorerTranslation> translations;
    if (0 != lstrcmpiW(loadedLanguage.c_str(), language))
    {
        loadedLanguage = language;
        ExplorerReadTranslations(language, translations);
    }
    for (auto const &item : translations)
        if (0 == lstrcmpW(item.key.c_str(), key)) return item.value.c_str();
    return en;
}
inline PCWSTR ExplorerUiText(PCWSTR zh, PCWSTR en) { return ExplorerText(L"", zh, en); }
// CLI bridge path: HKCU\Software\ExplorerRemoteFs\CliPath (set by install.ps1).
// If the registry value is missing or stale, resolve the CLI next to this DLL
// so the installed package remains relocatable. The old development path is
// the development checkout is derived from the DLL location as a compatibility fallback.
inline const WCHAR *GetCliPath()
{
    static WCHAR s_path[MAX_PATH] = {};
    if (!s_path[0])
    {
        DWORD cb = sizeof(s_path);
        LONG r = RegGetValueW(HKEY_CURRENT_USER, L"Software\\ExplorerRemoteFs", L"CliPath",
                              RRF_RT_REG_SZ, NULL, s_path, &cb);
        if (r == ERROR_SUCCESS && s_path[0] &&
            GetFileAttributesW(s_path) != INVALID_FILE_ATTRIBUTES)
            return s_path;

        s_path[0] = L'\0';

        HMODULE module = NULL;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&GetCliPath), &module))
        {
            WCHAR modulePath[MAX_PATH] = {};
            DWORD length = GetModuleFileNameW(module, modulePath, ARRAYSIZE(modulePath));
            if (length > 0 && length < ARRAYSIZE(modulePath))
            {
                WCHAR *slash = wcsrchr(modulePath, L'\\');
                if (slash)
                {
                    *slash = L'\0';
                    StringCchPrintfW(s_path, ARRAYSIZE(s_path),
                                     L"%s\\cli\\ExplorerRemoteFs.Cli.exe", modulePath);
                    if (GetFileAttributesW(s_path) != INVALID_FILE_ATTRIBUTES)
                        return s_path;
                    s_path[0] = L'\0';

                    StringCchPrintfW(s_path, ARRAYSIZE(s_path),
                                     L"%s\\..\\..\\dist\\cli\\ExplorerRemoteFs.Cli.exe", modulePath);
                    if (GetFileAttributesW(s_path) != INVALID_FILE_ATTRIBUTES)
                        return s_path;
                    s_path[0] = L'\0';
                }
            }
        }
    }
    return s_path;
}

// GUI client path: written by install.ps1. It activates the resident tray
// manager from Explorer without relying on a development checkout path.
inline const WCHAR *GetClientPath()
{
    static WCHAR s_path[MAX_PATH] = {};
    if (!s_path[0])
    {
        DWORD cb = sizeof(s_path);
        LONG r = RegGetValueW(HKEY_CURRENT_USER, L"Software\\ExplorerRemoteFs", L"ClientPath",
                              RRF_RT_REG_SZ, NULL, s_path, &cb);
        if (r == ERROR_SUCCESS && s_path[0] && GetFileAttributesW(s_path) != INVALID_FILE_ATTRIBUTES)
            return s_path;
        s_path[0] = L'\0';
        HMODULE module = NULL;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&GetClientPath), &module))
        {
            WCHAR modulePath[MAX_PATH] = {};
            DWORD length = GetModuleFileNameW(module, modulePath, ARRAYSIZE(modulePath));
            if (length > 0 && length < ARRAYSIZE(modulePath))
            {
                WCHAR *slash = wcsrchr(modulePath, L'\\');
                if (slash)
                {
                    *slash = L'\0';
                    StringCchPrintfW(s_path, ARRAYSIZE(s_path), L"%s\\client\\RemoteFsClient.exe", modulePath);
                    if (GetFileAttributesW(s_path) != INVALID_FILE_ATTRIBUTES) return s_path;
                    s_path[0] = L'\0';
                }
            }
        }
    }
    return s_path;
}
// The resident tray process owns the long-lived provider connection pool.
// Use it for listings when available; callers fall back to the one-shot CLI
// when the control center is intentionally not running.
inline BOOL FtpBridgeWriteLine(HANDLE pipe, PCWSTR value)
{
    PCWSTR source = value ? value : L"";
    int cch = lstrlenW(source); // Exclude the terminator: the wire format is UTF-8 text plus '\n'.
    std::string line;
    if (cch > 0)
    {
        int cb = WideCharToMultiByte(CP_UTF8, 0, source, cch, NULL, 0, NULL, NULL);
        if (cb <= 0) return FALSE;
        line.resize((size_t)cb);
        if (!WideCharToMultiByte(CP_UTF8, 0, source, cch, &line[0], cb, NULL, NULL)) return FALSE;
    }
    line += '\n';
    DWORD written = 0;
    return WriteFile(pipe, line.data(), (DWORD)line.size(), &written, NULL) && written == line.size();
}

inline BOOL FtpBridgeList(PCWSTR site, PCWSTR path, std::string &text)
{
    text.clear();
    const WCHAR pipeName[] = L"\\\\.\\pipe\\ExplorerRemoteFs.Bridge.v1";
    const ULONGLONG started = GetTickCount64();
    HANDLE pipe = INVALID_HANDLE_VALUE;
    DWORD lastError = ERROR_SUCCESS;

    // A first folder open causes Explorer to ask more than once (binding/view
    // preparation). Do not turn a briefly busy resident bridge into a separate
    // one-shot CLI session; wait for a queued pipe instance instead.
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        pipe = CreateFileW(pipeName, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (pipe != INVALID_HANDLE_VALUE) break;
        lastError = GetLastError();
        if (lastError != ERROR_PIPE_BUSY || !WaitNamedPipeW(pipeName, 5000)) break;
    }
    if (pipe == INVALID_HANDLE_VALUE)
    {
        ProbeLog(L"[BRIDGE] unavailable site='%s' path='%s' err=%lu elapsed=%llu", site, path ? path : L"/", lastError, GetTickCount64() - started);
        return FALSE;
    }

    BOOL sent = FtpBridgeWriteLine(pipe, L"LIST") && FtpBridgeWriteLine(pipe, site) &&
                FtpBridgeWriteLine(pipe, (path && path[0]) ? path : L"/");
    if (sent)
    {
        char buf[4096]; DWORD got = 0;
        while (ReadFile(pipe, buf, sizeof(buf), &got, NULL) && got) text.append(buf, got);
    }
    CloseHandle(pipe);
    BOOL complete = text.find("BRIDGE-END\r\n") != std::string::npos ||
                    text.find("BRIDGE-END\n") != std::string::npos || text.rfind("FAIL:", 0) == 0;
    ProbeLog(L"[BRIDGE] site='%s' path='%s' sent=%d complete=%d bytes=%u elapsed=%llu", site, path ? path : L"/", sent, complete, (UINT)text.size(), GetTickCount64() - started);
    return sent && complete;
}

// Tell the resident bridge service to drop its listing cache for one site
// ("*" = all). Best effort: if the service is not running this is a no-op and
// the caller simply falls back to spawning the CLI, which has no cache of its
// own. Called from FtpCacheClear so every post-mutation refresh sees fresh data.
//
// IMPORTANT: the bridge handler always reads THREE request lines (operation,
// site, path) before dispatching, so a third (empty) line must be sent — a
// two-line request deadlocks (server waits for line 3, client waits for "OK").
inline void FtpBridgeClearCache(PCWSTR site)
{
    const WCHAR pipeName[] = L"\\\\.\\pipe\\ExplorerRemoteFs.Bridge.v1";
    HANDLE pipe = CreateFileW(pipeName, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (pipe == INVALID_HANDLE_VALUE) return;
    if (FtpBridgeWriteLine(pipe, L"CACHE-CLEAR") &&
        FtpBridgeWriteLine(pipe, (site && site[0]) ? site : L"*") &&
        FtpBridgeWriteLine(pipe, L""))                       // required 3rd line
    {
        char buf[64]; DWORD got = 0;
        ReadFile(pipe, buf, sizeof(buf), &got, NULL);   // consume "OK"
    }
    CloseHandle(pipe);
}
typedef struct
{
    DWORD   dwMode;
    ULONGLONG dwSize;
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

struct FtpDiskCacheHeader { DWORD magic; DWORD version; DWORD count; };
inline BOOL FtpMetadataCacheDirectory(PWSTR out, UINT cch)
{
    if (!out || !cch) return FALSE; out[0] = 0; DWORD cb = cch * sizeof(WCHAR);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\ExplorerRemoteFs", L"MetadataCachePath", RRF_RT_REG_SZ, NULL, out, &cb) != ERROR_SUCCESS || !out[0]) {
        WCHAR local[MAX_PATH] = {}; if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, local))) return FALSE;
        if (FAILED(StringCchPrintfW(out, cch, L"%s\\ExplorerRemoteFs\\MetadataCache", local))) return FALSE;
    }
    if (!PathIsDirectoryW(out)) SHCreateDirectoryExW(NULL, out, NULL);
    return PathIsDirectoryW(out);
}
inline ULONGLONG FtpMetadataCacheHash(PCWSTR site, PCWSTR path)
{
    ULONGLONG h = 1469598103934665603ULL; const WCHAR *parts[] = { site ? site : L"", L"|", (path && path[0]) ? path : L"/" };
    for (int i = 0; i < 3; ++i) for (const WCHAR *p = parts[i]; *p; ++p) { h ^= (ULONGLONG)towlower(*p); h *= 1099511628211ULL; }
    return h;
}
inline BOOL FtpMetadataCacheFile(PCWSTR site, PCWSTR path, PWSTR out, UINT cch)
{
    WCHAR dir[MAX_PATH] = {}; if (!FtpMetadataCacheDirectory(dir, ARRAYSIZE(dir))) return FALSE;
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\ExplorerRemoteFs-meta-%016llX.bin", dir, FtpMetadataCacheHash(site, path)));
}
inline BOOL FtpDiskCacheLoad(PCWSTR site, PCWSTR path, std::vector<FTPENTRY> &items)
{
    items.clear(); WCHAR file[MAX_PATH] = {}; if (!FtpMetadataCacheFile(site, path, file, ARRAYSIZE(file))) return FALSE;
    WIN32_FILE_ATTRIBUTE_DATA a = {}; if (!GetFileAttributesExW(file, GetFileExInfoStandard, &a)) return FALSE;
    FILETIME ft = {}; GetSystemTimeAsFileTime(&ft); ULARGE_INTEGER now = {}, written = {}; now.LowPart = ft.dwLowDateTime; now.HighPart = ft.dwHighDateTime; written.LowPart = a.ftLastWriteTime.dwLowDateTime; written.HighPart = a.ftLastWriteTime.dwHighDateTime;
    if (now.QuadPart < written.QuadPart || now.QuadPart - written.QuadPart > 100000000ULL) { DeleteFileW(file); return FALSE; }
    HANDLE f = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL); if (f == INVALID_HANDLE_VALUE) return FALSE;
    FtpDiskCacheHeader head = {}; DWORD got = 0; BOOL ok = ReadFile(f, &head, sizeof(head), &got, NULL) && got == sizeof(head) && head.magic == 0x45524653 && head.version == 1 && head.count <= 100000;
    if (ok && head.count) { items.resize(head.count); DWORD bytes = head.count * (DWORD)sizeof(FTPENTRY); ok = ReadFile(f, items.data(), bytes, &got, NULL) && got == bytes; }
    CloseHandle(f); if (!ok) items.clear(); return ok;
}
inline void FtpDiskCacheStore(PCWSTR site, PCWSTR path, const FTPENTRY *items, int count)
{
    if (count < 0 || count > 100000 || (count && !items)) return; WCHAR file[MAX_PATH] = {}, temp[MAX_PATH] = {}; if (!FtpMetadataCacheFile(site, path, file, ARRAYSIZE(file))) return;
    if (FAILED(StringCchPrintfW(temp, ARRAYSIZE(temp), L"%s.%lu.%lu.tmp", file, GetCurrentProcessId(), GetCurrentThreadId()))) return;
    HANDLE f = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL); if (f == INVALID_HANDLE_VALUE) return;
    FtpDiskCacheHeader head = { 0x45524653, 1, (DWORD)count }; DWORD wrote = 0; BOOL ok = WriteFile(f, &head, sizeof(head), &wrote, NULL) && wrote == sizeof(head);
    if (ok && count) { DWORD bytes = (DWORD)count * (DWORD)sizeof(FTPENTRY); ok = WriteFile(f, items, bytes, &wrote, NULL) && wrote == bytes; }
    CloseHandle(f); if (ok) MoveFileExW(temp, file, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH); DeleteFileW(temp);
}
inline void FtpDiskCacheClear()
{
    WCHAR dir[MAX_PATH] = {}, pattern[MAX_PATH] = {}; if (!FtpMetadataCacheDirectory(dir, ARRAYSIZE(dir))) return;
    if (FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\ExplorerRemoteFs-meta-*.bin", dir))) return;
    WIN32_FIND_DATAW data = {}; HANDLE find = FindFirstFileW(pattern, &data); if (find == INVALID_HANDLE_VALUE) return;
    do { WCHAR file[MAX_PATH] = {}; if (SUCCEEDED(StringCchPrintfW(file, ARRAYSIZE(file), L"%s\\%s", dir, data.cFileName))) DeleteFileW(file); } while (FindNextFileW(find, &data)); FindClose(find);
}
inline void FtpCacheClear()
{
    AcquireSRWLockExclusive(&FtpCacheLock()); FtpCacheEntries().clear(); ReleaseSRWLockExclusive(&FtpCacheLock());
    FtpDiskCacheClear();
    // The resident bridge service keeps its own listing cache; drop it too or
    // the next enumeration after a mutation would still be served stale data.
    FtpBridgeClearCache(L"*");
}

// Returns count of cached entries for site+path (0 = miss/expired).
inline int FtpCacheLookup(PCWSTR site, PCWSTR path, FTPENTRY *out, int maxOut)
{
    if (maxOut <= 0) return 0; PCWSTR key = (path && path[0]) ? path : L"/"; ULONGLONG now = GetTickCount64(); int got = 0;
    AcquireSRWLockShared(&FtpCacheLock());
    for (auto &e : FtpCacheEntries()) if (0 == StrCmp(e.path, key) && 0 == StrCmp(e.site, site)) { if (now - e.tick < 3000) { got = (int)e.items.size(); if (got > maxOut) got = maxOut; for (int i = 0; i < got; ++i) out[i] = e.items[i]; } break; }
    ReleaseSRWLockShared(&FtpCacheLock()); if (got) return got;
    std::vector<FTPENTRY> disk; if (!FtpDiskCacheLoad(site, key, disk)) return 0; got = (int)disk.size(); if (got > maxOut) got = maxOut; for (int i = 0; i < got; ++i) out[i] = disk[i]; return got;
}

inline void FtpCacheStore(PCWSTR site, PCWSTR path, const FTPENTRY *items, int count)
{
    if (!site || !site[0] || count < 0) return; PCWSTR key = (path && path[0]) ? path : L"/"; ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&FtpCacheLock()); auto &v = FtpCacheEntries();
    for (auto it = v.begin(); it != v.end();) { if (0 == StrCmp(it->path, key) && 0 == StrCmp(it->site, site)) it = v.erase(it); else ++it; }
    FtpCacheEntry e = {}; StringCchCopy(e.site, ARRAYSIZE(e.site), site); StringCchCopy(e.path, ARRAYSIZE(e.path), key); e.tick = now; for (int i = 0; i < count; ++i) e.items.push_back(items[i]); v.push_back(std::move(e));
    for (auto it = v.begin(); it != v.end();) { if (now - it->tick >= 3000) it = v.erase(it); else ++it; } while (v.size() > 32) v.erase(v.begin()); ReleaseSRWLockExclusive(&FtpCacheLock());
    FtpDiskCacheStore(site, key, items, count);
}

// Spawn the CLI bridge once per site+path per TTL, parse tab-separated items,
// cache the result. Returns item count (0 on failure).
inline int FtpListCached(PCWSTR site, PCWSTR path, FTPENTRY *out, int maxItems)
{
    int cached = FtpCacheLookup(site, path, out, maxItems);
    if (cached > 0) return cached;

    PCWSTR pszPath = (path && path[0]) ? path : L"/";
    std::string text;
    if (!FtpBridgeList(site, pszPath, text))
    {
        WCHAR cmd[1200];
        StringCchPrintf(cmd, ARRAYSIZE(cmd),
            L"\"%s\" pipe \"%s\" \"%s\"",
            GetCliPath(), site, pszPath);
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
        char buf[4096]; DWORD got = 0;
        while (ReadFile(rd, buf, sizeof(buf), &got, NULL) && got) text.append(buf, got);
        CloseHandle(rd);
        WaitForSingleObject(pi.hProcess, 8000);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    std::vector<FTPENTRY> listed;
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t end = text.find('\n', pos); if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos); pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("ITEM\t", 0) != 0) continue;
        WCHAR wide[2048];
        if (!MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, wide, ARRAYSIZE(wide))) continue;
        // Manual split that KEEPS empty fields. wcstok_s would skip consecutive
        // tabs (e.g. SFTP rows have empty owner/group => "\t\t"), shifting every
        // field index and corrupting the name/uid/gid columns.
        WCHAR *fields[12] = {}; int nf = 0;
        WCHAR *p = wide;
        while (nf < 12)
        {
            WCHAR *tab = wcschr(p, L'\t');
            if (tab) *tab = 0;
            fields[nf++] = p;
            if (!tab) break;
            p = tab + 1;
        }
        if (nf < 10) continue;
        FTPENTRY &item = listed.emplace_back();
        DWORD mode = 0;
        if (fields[1]) { for (int i = 0; i < 9; i++) { if (fields[1][i + 1] != L'-') mode |= (0400 >> i); } }
        item.dwMode  = mode;
        item.dwMtime = (DWORD)_wtoi64(fields[2]);
        item.dwSize  = _wcstoui64(fields[3], NULL, 10);
        StringCchCopy(item.szOwner, ARRAYSIZE(item.szOwner), fields[4]);
        StringCchCopy(item.szGroup, ARRAYSIZE(item.szGroup), fields[5]);
        item.fIsFolder  = _wtoi(fields[6]) != 0;
        item.fIsSymlink = _wtoi(fields[7]) != 0;
        item.dwUid = (nf > 10 && fields[10] && fields[10][0]) ? (DWORD)_wtoi64(fields[10]) : 0xFFFFFFFF;
        item.dwGid = (nf > 11 && fields[11] && fields[11][0]) ? (DWORD)_wtoi64(fields[11]) : 0xFFFFFFFF;
        StringCchCopy(item.szName, ARRAYSIZE(item.szName), fields[9]);
    }
    // Cache the complete remote listing. The caller may ask for only a slice,
    // but a later Explorer enumeration must not inherit that artificial cap.
    if (text.rfind("FAIL:", 0) == std::string::npos)
    {
        FtpCacheStore(site, pszPath, listed.data(), (int)listed.size());
        int n = (int)listed.size();
        if (n > maxItems) n = maxItems;
        for (int i = 0; i < n; i++) out[i] = listed[i];
        return n;
    }
    return 0;
}

// Returns the complete cached listing. On a miss, populate the cache once via
// FtpListCached, which now parses the full CLI output before copying a slice.
inline BOOL FtpListCachedAll(PCWSTR site, PCWSTR path, std::vector<FTPENTRY> &out)
{
    out.clear();
    PCWSTR key = (path && path[0]) ? path : L"/";
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockShared(&FtpCacheLock());
    for (auto const &entry : FtpCacheEntries())
    {
        if (0 == StrCmp(entry.path, key) && 0 == StrCmp(entry.site, site) && now - entry.tick < 3000)
        {
            out = entry.items;
            ReleaseSRWLockShared(&FtpCacheLock());
            return TRUE;
        }
    }
    ReleaseSRWLockShared(&FtpCacheLock());

    if (FtpDiskCacheLoad(site, key, out)) return TRUE;

    FTPENTRY first = {};
    FtpListCached(site, key, &first, 1);

    AcquireSRWLockShared(&FtpCacheLock());
    for (auto const &entry : FtpCacheEntries())
    {
        if (0 == StrCmp(entry.path, key) && 0 == StrCmp(entry.site, site) && now - entry.tick < 3000)
        {
            out = entry.items;
            ReleaseSRWLockShared(&FtpCacheLock());
            return TRUE;
        }
    }
    ReleaseSRWLockShared(&FtpCacheLock());
    return FALSE;
}
