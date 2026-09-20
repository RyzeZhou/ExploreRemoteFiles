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
#include <time.h>
#include <vector>
#include <algorithm>
#include "ProbeLog.h"

// ---------------------------------------------------------------------------
// Shell 资源协议实验档位（2026-09-20）：HKCU\Software\ExplorerRemoteFs\ShellResourceMode
// IShellItemResources 是**未公开协议**（SDK 无文档/样例）。Shell 的复制引擎经
// ITransferSource::OpenItem 拿到资源对象后，可能：只要元数据、或者要枚举资源、
// 或者干脆不要资源而直接要 IStream。不同版本/不同来源行为不同，所以做成档位：
//   0（默认）= 返回资源对象，EnumResources 枚举 1 个"数据资源"（自定义 GUID）
//   1        = 同上，但资源 GUID 用 GUID_NULL（"默认数据流"的常见约定）
//   2        = OpenItem(IShellItemResources) 直接返回 S_FALSE + NULL（表示没有资源）
//   3        = OpenItem(IShellItemResources) 直接返回 E_NOTIMPL
// 改完重启 explorer 生效，不需要重装。
// ---------------------------------------------------------------------------
inline DWORD ErfShellResourceMode()
{
    static volatile LONG s_loaded = 0;
    static DWORD s_value = 0;
    if (InterlockedCompareExchange(&s_loaded, 1, 0) == 0)
    {
        HKEY k = NULL;
        if (ERROR_SUCCESS == RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\ExplorerRemoteFs", 0, KEY_READ, &k))
        {
            DWORD v = 0, cb = sizeof(v), type = 0;
            if (ERROR_SUCCESS == RegQueryValueExW(k, L"ShellResourceMode", NULL, &type, (LPBYTE)&v, &cb) &&
                type == REG_DWORD)
                s_value = v;
            RegCloseKey(k);
        }
    }
    return s_value;
}

// ---------------------------------------------------------------------------
// 复制/粘贴的数据通路：
//   1（默认）= 提供 CFSTR_FILEDESCRIPTORW / CFSTR_FILECONTENTS。**实测结论（2026-09-20）**：
//              Explorer 复制虚拟文件夹时只能走这条通路 —— ITransferSource 在 Win11 上
//              只被用于删除/移动/元数据；复制引擎拿到 IShellItemResources 后就收工，
//              从不要 IStream（四种资源档位都试过）。关掉 FD 的结果就是"只有占位、没有数据"。
//              原来的"占 UI 线程"问题改由 CRemoteStream **边下边读**解决。
//   0 = 不提供虚拟文件格式（实验档，仅供对照）。
// 读 HKCU\Software\ExplorerRemoteFs\UseVirtualFileFormats（DWORD，缺省=1）。
// ---------------------------------------------------------------------------
inline BOOL ErfUseVirtualFileFormats()
{
    static volatile LONG s_loaded = 0;
    static BOOL s_value = TRUE;   // 默认走 FD/FILECONTENTS（唯一能真正拿到数据的通路）
    if (InterlockedCompareExchange(&s_loaded, 1, 0) == 0)
    {
        HKEY k = NULL;
        if (ERROR_SUCCESS == RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\ExplorerRemoteFs", 0, KEY_READ, &k))
        {
            DWORD v = 0, cb = sizeof(v), type = 0;
            if (ERROR_SUCCESS == RegQueryValueExW(k, L"UseVirtualFileFormats", NULL, &type, (LPBYTE)&v, &cb) &&
                type == REG_DWORD)
                s_value = (v != 0);
            RegCloseKey(k);
        }
    }
    return s_value;
}
// Module lifetime: worker threads below must pin the DLL (DllCanUnloadNow
// only counts COM objects; an unpinned background thread that outlives the
// last object crashed hosts AFTER unload: svchost _unloaded 0xc0000005,
// 2026-09-20). Same declarations as Utils.h; repeated here to avoid a cycle.
void DllAddRef();
void DllRelease();

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
        // Bounded read: a synchronous ReadFile here has NO timeout, so a stalled
        // server would hang the enumerating thread forever (Explorer freeze seen
        // after mutations). Poll with PeekNamedPipe and give up after 8s; the
        // caller then falls back to the CLI path, which has its own timeout.
        char buf[4096]; DWORD got = 0;
        const ULONGLONG deadline = GetTickCount64() + 8000;
        while (GetTickCount64() < deadline)
        {
            DWORD avail = 0;
            if (!PeekNamedPipe(pipe, NULL, 0, NULL, &avail, NULL)) break;   // server closed
            if (avail > 0)
            {
                if (!ReadFile(pipe, buf, sizeof(buf), &got, NULL) || got == 0) break;
                text.append(buf, got);
                if (text.find("BRIDGE-END") != std::string::npos ||
                    text.rfind("FAIL:", 0) == 0) break;
            }
            else Sleep(25);
        }
        ProbeLog(L"[BRIDGE] read done sent=%d bytes=%u (deadline)", sent, (UINT)text.size());
    }
    CloseHandle(pipe);
    BOOL complete = text.find("BRIDGE-END\r\n") != std::string::npos ||
                    text.find("BRIDGE-END\n") != std::string::npos || text.rfind("FAIL:", 0) == 0;
    ProbeLog(L"[BRIDGE] site='%s' path='%s' sent=%d complete=%d bytes=%u elapsed=%llu", site, path ? path : L"/", sent, complete, (UINT)text.size(), GetTickCount64() - started);
    return sent && complete;
}

// Synchronous mutation request to the resident service.  This is used from
// ITransferSource, where Explorer's IFileOperation must not receive success
// until the remote deletion is actually complete.  The service owns the
// provider/session and its task queue; this DLL only waits for its final OK or
// FAIL line while Explorer displays the native progress UI.
inline BOOL FtpBridgeDelete(PCWSTR site, PCWSTR path, BOOL recursive, std::string &response)
{
    response.clear();
    const WCHAR pipeName[] = L"\\\\.\\pipe\\ExplorerRemoteFs.Bridge.v1";
    HANDLE pipe = INVALID_HANDLE_VALUE;
    DWORD lastError = ERROR_SUCCESS;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        pipe = CreateFileW(pipeName, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (pipe != INVALID_HANDLE_VALUE) break;
        lastError = GetLastError();
        if (lastError != ERROR_PIPE_BUSY || !WaitNamedPipeW(pipeName, 5000)) break;
    }
    if (pipe == INVALID_HANDLE_VALUE)
    {
        ProbeLog(L"[XFER] resident delete bridge unavailable site='%s' path='%s' err=%lu", site ? site : L"", path ? path : L"", lastError);
        return FALSE;
    }

    BOOL sent = FtpBridgeWriteLine(pipe, L"DELETE") &&
                FtpBridgeWriteLine(pipe, site ? site : L"") &&
                FtpBridgeWriteLine(pipe, path ? path : L"/") &&
                FtpBridgeWriteLine(pipe, recursive ? L"1" : L"0");
    const ULONGLONG deadline = GetTickCount64() + 15ull * 60ull * 1000ull;
    if (sent)
    {
        while (GetTickCount64() < deadline)
        {
            DWORD avail = 0;
            if (!PeekNamedPipe(pipe, NULL, 0, NULL, &avail, NULL)) break;
            if (avail > 0)
            {
                char buf[1024]; DWORD got = 0;
                if (!ReadFile(pipe, buf, sizeof(buf), &got, NULL) || got == 0) break;
                response.append(buf, got);
                if (response.find('\n') != std::string::npos) break;
            }
            else Sleep(25);
        }
    }
    CloseHandle(pipe);
    BOOL ok = sent && response.rfind("OK", 0) == 0;
    ProbeLog(L"[XFER] resident delete bridge site='%s' path='%s' recursive=%d sent=%d ok=%d reply='%hs'",
             site ? site : L"", path ? path : L"", recursive, sent, ok, response.c_str());
    return ok;
}

// ── 桥接管道的小工具（发起/查询共用）──────────────────────────────────────
inline HANDLE FtpBridgeOpenPipe()
{
    const WCHAR pipeName[] = L"\\\\.\\pipe\\ExplorerRemoteFs.Bridge.v1";
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        HANDLE pipe = CreateFileW(pipeName, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, NULL);
        if (pipe != INVALID_HANDLE_VALUE) return pipe;
        if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(pipeName, 5000)) break;
    }
    return INVALID_HANDLE_VALUE;
}

inline BOOL FtpBridgeReadReply(HANDLE pipe, std::string &line, ULONGLONG timeoutMs)
{
    line.clear();
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline)
    {
        DWORD avail = 0;
        if (!PeekNamedPipe(pipe, NULL, 0, NULL, &avail, NULL)) return FALSE;   // 服务关了连接
        if (avail > 0)
        {
            char buf[512]; DWORD got = 0;
            if (!ReadFile(pipe, buf, sizeof(buf), &got, NULL) || got == 0) return FALSE;
            line.append(buf, got);
            if (line.find('\n') != std::string::npos) return TRUE;
        }
        else
        {
            Sleep(20);
        }
    }
    return FALSE;
}

// 发起一个后台下载：发请求 → 收 "STARTED <jobId>" → **立即返回**。
//
// 为什么不能像以前那样"发完就等回执直到文件下完"：命名管道的实例只有几个，
// 一个请求占着它几分钟，就等于把 LIST 一起堵死 —— 实测日志里已经出现过
// `[BRIDGE] unavailable site='WSL-SFTP' path='/home/zhou/AI_work' err=231 elapsed=5000`
//（err=231 就是 ERROR_PIPE_BUSY），用户那边看到的就是"复制时更卡了"。
inline BOOL FtpBridgeFetchStart(PCWSTR op, PCWSTR site, PCWSTR remote, PCWSTR local,
                                PCWSTR batchId, std::string &jobId)
{
    jobId.clear();
    HANDLE pipe = FtpBridgeOpenPipe();
    if (pipe == INVALID_HANDLE_VALUE)
    {
        ProbeLog(L"[XFER] fetch bridge unavailable op='%s' site='%s' err=%lu", op, site ? site : L"", GetLastError());
        return FALSE;
    }

    BOOL sent = FtpBridgeWriteLine(pipe, op) &&
                FtpBridgeWriteLine(pipe, site ? site : L"") &&
                FtpBridgeWriteLine(pipe, remote ? remote : L"") &&
                FtpBridgeWriteLine(pipe, local ? local : L"") &&
                FtpBridgeWriteLine(pipe, (batchId && batchId[0]) ? batchId : L"-");
    std::string line;
    BOOL got = sent && FtpBridgeReadReply(pipe, line, 15000);
    CloseHandle(pipe);

    if (!got || line.rfind("STARTED ", 0) != 0)
    {
        ProbeLog(L"[XFER] fetch start FAILED op='%s' reply='%hs'", op, line.c_str());
        return FALSE;
    }
    jobId = line.substr(8);
    while (!jobId.empty() && (jobId.back() == '\r' || jobId.back() == '\n')) jobId.pop_back();
    return !jobId.empty();
}

// 查询后台下载状态：RUNNING <done> <total> / DONE / FAILED <msg> / CANCELLED / UNKNOWN
inline BOOL FtpBridgeFetchStatus(PCWSTR jobId, std::string &line)
{
    line.clear();
    HANDLE pipe = FtpBridgeOpenPipe();
    if (pipe == INVALID_HANDLE_VALUE) return FALSE;
    BOOL sent = FtpBridgeWriteLine(pipe, L"FETCHSTATUS") && FtpBridgeWriteLine(pipe, jobId);
    BOOL got = sent && FtpBridgeReadReply(pipe, line, 15000);
    CloseHandle(pipe);
    return got;
}

// 通知常驻服务：这一批（batchId）别再跑了。
// 为什么需要它：Explorer 的复制对话框被取消时，我们这边只是"不再读流"，
// 服务侧照样会把文件（甚至整棵目录树）下完 —— 队列里任务还挂着、带宽白占，
// 用户看到的是"取消了却还在跑"。这条命令让取消**真的生效**。
inline BOOL FtpBridgeCancel(PCWSTR batchId, std::string &line)
{
    line.clear();
    if (!batchId || !batchId[0]) return FALSE;
    HANDLE pipe = FtpBridgeOpenPipe();
    if (pipe == INVALID_HANDLE_VALUE)
    {
        ProbeLog(L"[XFER] cancel: bridge unavailable batch='%s'", batchId);
        return FALSE;
    }
    BOOL sent = FtpBridgeWriteLine(pipe, L"CANCEL") && FtpBridgeWriteLine(pipe, batchId);
    BOOL got = sent && FtpBridgeReadReply(pipe, line, 5000);
    CloseHandle(pipe);
    BOOL confirmed = got && line.rfind("OK ", 0) == 0;
    ProbeLog(L"[XFER] cancel batch='%s' sent=%d confirmed=%d reply='%hs'",
             batchId, (int)sent, (int)confirmed, line.c_str());
    return confirmed;
}
// Explicit protocol outcome. Cancellation is not an error string: callers must
// not accidentally retry it as a transient download failure.
enum class FtpBridgeFetchState
{
    Failed,
    Done,
    Cancelled,
};

// 发起 + 等待完成。等待期间**不占管道**（每秒一次短查询），所以：
//   · 浏览请求不会被下载堵住；
//   · 用户在传输队列里点「取消」→ 服务侧中断下载 → 这里拿到明确 CANCELLED 终态。
inline FtpBridgeFetchState FtpBridgeFetchWait(PCWSTR op, PCWSTR site, PCWSTR remote, PCWSTR local,
                                              PCWSTR batchId, std::string &response)
{
    response.clear();
    std::string jobId;
    if (!FtpBridgeFetchStart(op, site, remote, local, batchId, jobId))
    {
        response = "FAIL: cannot start fetch";
        return FtpBridgeFetchState::Failed;
    }

    const ULONGLONG started = GetTickCount64();
    const ULONGLONG deadline = started + 120ull * 60ull * 1000ull;
    ProbeLog(L"[XFER] fetch started op='%s' remote='%s' batch='%s' job=%hs",
             op, remote ? remote : L"", batchId ? batchId : L"-", jobId.c_str());
    int tick = 0;
    for (;;)
    {
        Sleep(200);
        if (GetTickCount64() > deadline)
        {
            response = "FAIL: timeout";
            ProbeLog(L"[XFER] fetch terminal=failed reason=timeout job=%hs elapsed=%llu",
                     jobId.c_str(), GetTickCount64() - started);
            return FtpBridgeFetchState::Failed;
        }
        if (++tick % 5) continue;

        std::wstring wideJob(jobId.begin(), jobId.end());
        std::string st;
        if (!FtpBridgeFetchStatus(wideJob.c_str(), st)) continue;
        if (st.rfind("DONE", 0) == 0)
        {
            response = "OK";
            ProbeLog(L"[XFER] fetch terminal=done op='%s' remote='%s' job=%hs elapsed=%llu",
                     op, remote ? remote : L"", jobId.c_str(), GetTickCount64() - started);
            return FtpBridgeFetchState::Done;
        }
        if (st.rfind("CANCELLED", 0) == 0)
        {
            response = "CANCELLED";
            ProbeLog(L"[XFER] fetch terminal=cancelled job=%hs elapsed=%llu", jobId.c_str(), GetTickCount64() - started);
            return FtpBridgeFetchState::Cancelled;
        }
        if (st.rfind("FAILED", 0) == 0)
        {
            response = st;
            ProbeLog(L"[XFER] fetch terminal=failed job=%hs reply='%hs' elapsed=%llu",
                     jobId.c_str(), st.c_str(), GetTickCount64() - started);
            return FtpBridgeFetchState::Failed;
        }
        if (st.rfind("UNKNOWN", 0) == 0)
        {
            response = "FAIL: job lost";
            ProbeLog(L"[XFER] fetch terminal=failed reason=job-lost job=%hs elapsed=%llu",
                     jobId.c_str(), GetTickCount64() - started);
            return FtpBridgeFetchState::Failed;
        }
    }
}

inline FtpBridgeFetchState FtpBridgeFetch(PCWSTR site, PCWSTR remote, PCWSTR local, PCWSTR batchId, std::string &response)
{
    return FtpBridgeFetchWait(L"FETCH", site, remote, local, batchId, response);
}

inline FtpBridgeFetchState FtpBridgeFetchDir(PCWSTR site, PCWSTR remoteDir, PCWSTR localRoot, PCWSTR batchId, std::string &response)
{
    return FtpBridgeFetchWait(L"FETCHDIR", site, remoteDir, localRoot, batchId, response);
}

// 上传：把本地文件送到远程（粘贴 / 编辑回写 / 跨站点复制的上传段）。
// 与 FETCH 走完全相同的"发起 + 轮询 + 取消"链路，只是 op 为 PUT；
// 目标远程路径 = remote，本地源文件 = local。
inline FtpBridgeFetchState FtpBridgePut(PCWSTR site, PCWSTR remote, PCWSTR local, PCWSTR batchId, std::string &response)
{
    return FtpBridgeFetchWait(L"PUT", site, remote, local, batchId, response);
}
// 递归修改权限：走常驻服务（与 DELETE 同一条路）。
// 为什么不能在这里同步跑 CLI：属性页的「确定」在 Explorer 的 UI 线程上，树一大就整窗卡死，
// 而且 RunCli 有 30 秒超时会把 CLI 直接杀掉（大目录必然超时，改到一半就断）。
// 交给常驻服务后：遍历跑在它自己的线程上，带进度窗口和「取消」，我们只在完成回调里刷新视图。
inline BOOL FtpBridgeChmod(PCWSTR site, PCWSTR path, PCWSTR modeOctal, BOOL recursive, std::string &response)
{
    response.clear();
    const WCHAR pipeName[] = L"\\\\.\\pipe\\ExplorerRemoteFs.Bridge.v1";
    HANDLE pipe = INVALID_HANDLE_VALUE;
    DWORD lastError = ERROR_SUCCESS;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        pipe = CreateFileW(pipeName, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (pipe != INVALID_HANDLE_VALUE) break;
        lastError = GetLastError();
        if (lastError != ERROR_PIPE_BUSY || !WaitNamedPipeW(pipeName, 5000)) break;
    }
    if (pipe == INVALID_HANDLE_VALUE)
    {
        ProbeLog(L"[TERM] resident chmod bridge unavailable site='%s' path='%s' err=%lu",
                 site ? site : L"", path ? path : L"", lastError);
        return FALSE;
    }

    BOOL sent = FtpBridgeWriteLine(pipe, L"CHMOD") &&
                FtpBridgeWriteLine(pipe, site ? site : L"") &&
                FtpBridgeWriteLine(pipe, path ? path : L"/") &&
                FtpBridgeWriteLine(pipe, modeOctal ? modeOctal : L"644") &&
                FtpBridgeWriteLine(pipe, recursive ? L"1" : L"0");
    const ULONGLONG deadline = GetTickCount64() + 30ull * 60ull * 1000ull;
    if (sent)
    {
        while (GetTickCount64() < deadline)
        {
            DWORD avail = 0;
            if (!PeekNamedPipe(pipe, NULL, 0, NULL, &avail, NULL)) break;
            if (avail > 0)
            {
                char buf[1024]; DWORD got = 0;
                if (!ReadFile(pipe, buf, sizeof(buf), &got, NULL) || got == 0) break;
                response.append(buf, got);
                if (response.find('\n') != std::string::npos) break;
            }
            else Sleep(25);
        }
    }
    CloseHandle(pipe);
    BOOL ok = sent && response.rfind("OK", 0) == 0;
    ProbeLog(L"[TERM] resident chmod bridge site='%s' path='%s' mode=%s recursive=%d sent=%d ok=%d reply='%hs'",
             site ? site : L"", path ? path : L"/", modeOctal ? modeOctal : L"?", recursive, sent, ok, response.c_str());
    return ok;
}

// Tell the resident bridge service to drop its listing cache for one site
// ("*" = all). Best effort and FIRE-AND-FORGET: if the service is not running
// this is a no-op, and no reply is ever awaited — the bridge LIST cache TTL is
// only 1s anyway, so a missed clear self-heals on the next refresh.
//
// This routine runs on the Explorer UI thread (menu commands). It MUST never
// block on a server reply: any unexpected server-side stall would freeze
// Explorer. Write the three request lines and close; the server consumes them
// from its own instance and drops the cache entries.
inline void FtpBridgeClearCache(PCWSTR site)
{
    const WCHAR pipeName[] = L"\\\\.\\pipe\\ExplorerRemoteFs.Bridge.v1";
    HANDLE pipe = CreateFileW(pipeName, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (pipe == INVALID_HANDLE_VALUE) return;
    BOOL ok = FtpBridgeWriteLine(pipe, L"CACHE-CLEAR") &&
              FtpBridgeWriteLine(pipe, (site && site[0]) ? site : L"*") &&
              FtpBridgeWriteLine(pipe, L"");   // 3rd line: the server's reader expects three lines
    ProbeLog(L"[BRIDGE] cache-clear site='%s' sent=%d", site ? site : L"*", ok);
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

// ── 缓存新鲜期（2026-09-18 修"缓存不持久化"）────────────────────────────────
// 内存 TTL 原来是 30 秒：实测同一目录每 30 秒必然重拉一次远程 LIST（日志里两次
// [BRIDGE] 的间隔是 30703 / 34750 ms，正好卡在悬崖上），而用户从进目录到离开通常
// 远超 30 秒 —— 于是"刚刚才看过"的目录也一直在重拉。改成 5 分钟：正确性由
// "写操作后的精确失效 + 后台预取"保证，TTL 只用来兜住"别人在远端改了东西"。
//
// 磁盘 TTL 原来也是 30 秒，而且过期直接 DeleteFileW —— 这是"缓存不持久化"的根因：
// 磁盘条目与内存条目是**同一时刻**写入的（FtpCacheStore 一次写两边），
// 所以磁盘条目年龄恒 ≥ 内存条目年龄，磁盘层永远不可能比内存层活得更久，落盘等于白做。
// 现在磁盘快照不再因为"老"被删，只在超过 FTP_DISK_CACHE_MAX_AGE_MS 后不再用于首屏。
#define FTP_CACHE_TTL_MS            300000ULL              // 内存新鲜期：5 分钟
#define FTP_DISK_CACHE_MAX_AGE_MS   (24ULL * 3600 * 1000)  // 磁盘快照可用于首屏的上限：24 小时
#define FTP_CACHE_MAX_DIRS          128                    // 内存里最多保留多少个目录快照

// v2 (2026-09-20): carries the site+folder identity so a load NEVER serves a
// listing that belongs to another key. v1 files (case-folded-hash era) are
// rejected on load and deleted: accepting them would re-serve B listing for b.
struct FtpDiskCacheHeader { DWORD magic; DWORD version; DWORD count; WCHAR site[64]; WCHAR folder[600]; };
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
    // NOTE (2026-09-20): NO case folding here. Remote paths are case-sensitive
    // (Linux servers); the old tolower folded "B" and "b" onto ONE cache file,
    // so navigating to b showed B stale listing. Site names keep their own
    // comparison rules at lookup time; the file name must distinguish every key.
    ULONGLONG h = 1469598103934665603ULL; const WCHAR *parts[] = { site ? site : L"", L"|", (path && path[0]) ? path : L"/" };
    for (int i = 0; i < 3; ++i) for (const WCHAR *p = parts[i]; *p; ++p) { h ^= (ULONGLONG)(*p); h *= 1099511628211ULL; }
    return h;
}
inline BOOL FtpMetadataCacheFile(PCWSTR site, PCWSTR path, PWSTR out, UINT cch)
{
    WCHAR dir[MAX_PATH] = {}; if (!FtpMetadataCacheDirectory(dir, ARRAYSIZE(dir))) return FALSE;
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\ExplorerRemoteFs-meta-%016llX.bin", dir, FtpMetadataCacheHash(site, path)));
}
// maxAgeMs = 0 表示不限年龄；>0 且超龄时返回 FALSE，但**绝不删文件**（见上面
// FTP_CACHE_TTL_MS 的说明：过期即删会让磁盘层永远追不上内存层）。ageMsOut 回传年龄。
inline BOOL FtpDiskCacheLoad(PCWSTR site, PCWSTR path, std::vector<FTPENTRY> &items,
                             ULONGLONG maxAgeMs = 0, ULONGLONG *ageMsOut = NULL)
{
    items.clear(); if (ageMsOut) *ageMsOut = 0;
    WCHAR file[MAX_PATH] = {}; if (!FtpMetadataCacheFile(site, path, file, ARRAYSIZE(file))) return FALSE;
    WIN32_FILE_ATTRIBUTE_DATA a = {}; if (!GetFileAttributesExW(file, GetFileExInfoStandard, &a)) return FALSE;
    FILETIME ft = {}; GetSystemTimeAsFileTime(&ft); ULARGE_INTEGER now = {}, written = {}; now.LowPart = ft.dwLowDateTime; now.HighPart = ft.dwHighDateTime; written.LowPart = a.ftLastWriteTime.dwLowDateTime; written.HighPart = a.ftLastWriteTime.dwHighDateTime;
    ULONGLONG ageMs = (now.QuadPart > written.QuadPart) ? ((now.QuadPart - written.QuadPart) / 10000ULL) : 0ULL;
    if (ageMsOut) *ageMsOut = ageMs;
    if (maxAgeMs && ageMs > maxAgeMs) return FALSE;
    HANDLE f = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL); if (f == INVALID_HANDLE_VALUE) return FALSE;
    FtpDiskCacheHeader head = {}; DWORD got = 0;
    BOOL ok = ReadFile(f, &head, sizeof(head), &got, NULL) && got == sizeof(head) &&
              head.magic == 0x45524653 && head.version == 2 && head.count <= 100000 &&
              0 == StrCmp(head.site, site ? site : L"") &&
              0 == StrCmp(head.folder, (path && path[0]) ? path : L"/");
    if (ok && head.count) { items.resize(head.count); DWORD bytes = head.count * (DWORD)sizeof(FTPENTRY); ok = ReadFile(f, items.data(), bytes, &got, NULL) && got == bytes; }
    CloseHandle(f);
    if (!ok)
    {
        items.clear();
        // Stale v1 (case-folded era), foreign-identity or truncated file: drop
        // it so the next load refetches. Store is atomic (temp + move), so a
        // present-but-unreadable file is never a half-written one.
        DeleteFileW(file);
    }
    return ok;
}
inline void FtpDiskCacheStore(PCWSTR site, PCWSTR path, const FTPENTRY *items, int count)
{
    if (count < 0 || count > 100000 || (count && !items)) return; WCHAR file[MAX_PATH] = {}, temp[MAX_PATH] = {}; if (!FtpMetadataCacheFile(site, path, file, ARRAYSIZE(file))) return;
    if (FAILED(StringCchPrintfW(temp, ARRAYSIZE(temp), L"%s.%lu.%lu.tmp", file, GetCurrentProcessId(), GetCurrentThreadId()))) return;
    HANDLE f = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL); if (f == INVALID_HANDLE_VALUE) return;
    FtpDiskCacheHeader head = {}; head.magic = 0x45524653; head.version = 2; head.count = (DWORD)count;
    StringCchCopy(head.site, ARRAYSIZE(head.site), site ? site : L"");
    StringCchCopy(head.folder, ARRAYSIZE(head.folder), (path && path[0]) ? path : L"/");
    DWORD wrote = 0; BOOL ok = WriteFile(f, &head, sizeof(head), &wrote, NULL) && wrote == sizeof(head);
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
    ProbeLog(L"[CACHE] clear: mem lock enter");
    AcquireSRWLockExclusive(&FtpCacheLock()); FtpCacheEntries().clear(); ReleaseSRWLockExclusive(&FtpCacheLock());
    ProbeLog(L"[CACHE] clear: mem done");
    FtpDiskCacheClear();
    ProbeLog(L"[CACHE] clear: disk done");
    // NOTE (2026-09-03): FtpBridgeClearCache() was REMOVED from this path. A
    // synchronous pipe call here proved to be the Explorer freeze: when the
    // resident bridge is busy/stalled, CreateFile/WriteFile on the named pipe
    // blocks the UI thread forever (seen in logs: 'disk done' logged, 'bridge
    // done' never). The bridge LIST cache TTL is only 1s, so a missed clear
    // self-heals on the next refresh; freshness is not worth a hang.
}

// Post-mutation refresh, WinSCP-style (2026-09-03): PREFETCH the fresh listing
// into the metadata cache on a worker thread FIRST, then ask the shell to
// re-enumerate. A "clear cache then notify" ordering lets the view re-enumerate
// into an empty cache and can show an empty listing; prefetching guarantees the
// re-enumeration hits a ready cache and always shows the new state.
// Runs entirely off the caller's thread (cache clear + network fetch + notify).
inline BOOL FtpListCachedAll(PCWSTR site, PCWSTR path, std::vector<FTPENTRY> &out, bool waitForWarm = false);   // fwd (defined below)
struct FtpRefreshCtx
{
    WCHAR site[64];
    WCHAR folder[600];
    PIDLIST_ABSOLUTE pidl;
};
static DWORD WINAPI FtpRefreshThreadProc(LPVOID p)
{
    FtpRefreshCtx *c = static_cast<FtpRefreshCtx *>(p);
    FtpCacheClear();
    if (c->site[0] && c->folder[0])
    {
        std::vector<FTPENTRY> warm;
        FtpListCachedAll(c->site, c->folder, warm);   // real fetch -> fills the cache
        ProbeLog(L"[MUT] prefetch site='%s' path='%s' n=%u", c->site, c->folder, (UINT)warm.size());
    }
    if (c->pidl)
    {
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_IDLIST, (PCIDLIST_ABSOLUTE)c->pidl, NULL);
        ILFree(c->pidl);
    }
    delete c;
    DllRelease();
    return 0;
}
inline void FtpRefreshDirBackground(PCWSTR site, PCWSTR folder, PIDLIST_ABSOLUTE notifyPidl)
{
    FtpRefreshCtx *c = new (std::nothrow) FtpRefreshCtx{};
    if (!c) { FtpCacheClear(); return; }
    StringCchCopy(c->site, ARRAYSIZE(c->site), site ? site : L"");
    StringCchCopy(c->folder, ARRAYSIZE(c->folder), (folder && folder[0]) ? folder : L"/");
    c->pidl = notifyPidl ? ILCloneFull(notifyPidl) : NULL;
    DllAddRef();
    HANDLE h = CreateThread(NULL, 0, FtpRefreshThreadProc, c, 0, NULL);
    if (h) CloseHandle(h);
    else
    {
        DllRelease();
        if (c->pidl) ILFree(c->pidl);
        delete c;
        FtpCacheClear();
    }
}

// ---- optimistic in-memory patch of the cached listing (fast refresh) --------
// FTP answers a successful MKD/DELE/RNFR/STOR with success only (no metadata
// for the new item), but "success" is enough to patch the VIEWED directory's
// cache entry in place (remove/add/rename an item) so the immediate notify
// shows the change with zero network latency. A quiet background prefetch
// (FtpPrefetchQuiet) then REPLACES the cache with the real listing (accurate
// metadata) for later enumerations — FtpCacheStore already replaces the key.
inline void FtpCachePatchRemove(PCWSTR site, PCWSTR folder, PCWSTR name)
{
    if (!site || !site[0] || !folder || !name) return;
    AcquireSRWLockExclusive(&FtpCacheLock());
    for (auto &e : FtpCacheEntries())
        if (0 == StrCmp(e.site, site) && 0 == StrCmp(e.path, folder))
        {
            e.tick = GetTickCount64();
            e.items.erase(std::remove_if(e.items.begin(), e.items.end(),
                [&](FTPENTRY const &it) { return 0 == StrCmp(it.szName, name); }), e.items.end());
            break;
        }
    ReleaseSRWLockExclusive(&FtpCacheLock());
}
inline void FtpCachePatchAdd(PCWSTR site, PCWSTR folder, PCWSTR name, BOOL isFolder, ULONGLONG size)
{
    if (!site || !site[0] || !folder || !name) return;
    AcquireSRWLockExclusive(&FtpCacheLock());
    for (auto &e : FtpCacheEntries())
        if (0 == StrCmp(e.site, site) && 0 == StrCmp(e.path, folder))
        {
            bool exists = false;
            FTPENTRY sibling = {};   // inherit owner fields from a sibling when present
            for (auto &it : e.items)
            {
                if (0 == StrCmp(it.szName, name)) { exists = true; break; }
                if (!sibling.szOwner[0] && it.szOwner[0]) sibling = it;
            }
            if (!exists)
            {
                e.tick = GetTickCount64();
                FTPENTRY it = {};
                it.fIsFolder = isFolder;
                it.fIsSymlink = FALSE;
                it.dwSize = size;
                // Real Unix time (GetTickCount64()/1000 = uptime seconds ~ 1970).
                it.dwMtime = (DWORD)time(NULL);
                it.dwMode = isFolder ? 0x1FF : 0x1A4;             // 0777 / 0644 guess
                it.dwUid = sibling.szOwner[0] ? sibling.dwUid : 0xFFFFFFFF;
                it.dwGid = sibling.szOwner[0] ? sibling.dwGid : 0xFFFFFFFF;
                StringCchCopy(it.szOwner, ARRAYSIZE(it.szOwner), sibling.szOwner);
                StringCchCopy(it.szGroup, ARRAYSIZE(it.szGroup), sibling.szGroup);
                StringCchCopy(it.szName, ARRAYSIZE(it.szName), name);
                e.items.push_back(it);
            }
            break;
        }
    ReleaseSRWLockExclusive(&FtpCacheLock());
}
inline void FtpCachePatchRename(PCWSTR site, PCWSTR folder, PCWSTR oldName, PCWSTR newName)
{
    if (!site || !site[0] || !folder || !oldName || !newName) return;
    AcquireSRWLockExclusive(&FtpCacheLock());
    for (auto &e : FtpCacheEntries())
        if (0 == StrCmp(e.site, site) && 0 == StrCmp(e.path, folder))
        {
            e.tick = GetTickCount64();
            for (auto &it : e.items)
                if (0 == StrCmp(it.szName, oldName)) { StringCchCopy(it.szName, ARRAYSIZE(it.szName), newName); break; }
            break;
        }
    ReleaseSRWLockExclusive(&FtpCacheLock());
}

// Cache entry age check. Entries are stamped with GetTickCount64() when they
// are STORED, which can be later than a `now` sampled earlier in the same call
// (a fetch sits between the two). `now - tick` in ULONGLONG then underflows to
// ~2^64, which is ">= TTL" for any TTL, so a just-stored entry looked expired:
// FtpListCachedAll() filled the cache and then reported a listing failure,
// which is why expanding a COLD folder into a data object produced one item
// (the folder itself) and pasting it created an empty directory.
// Compare SIGNED so a tick slightly in the future reads as age <= 0 (fresh).
inline BOOL FtpCacheFresh(ULONGLONG now, ULONGLONG tick, ULONGLONG ttlMs)
{
    return (LONGLONG)(now - tick) < (LONGLONG)ttlMs;
}

// Single-item lookup from the in-memory cache WITHOUT copying the whole
// listing. Large-directory performance: Explorer queries metadata for every
// item (icon, each Details cell, properties), so copying the full vector per
// query would be O(n) per query -> O(n^2) rendering. Returns TRUE on a hit;
// FALSE when the directory is not cached (caller populates then retries).
inline BOOL FtpCacheFindOne(PCWSTR site, PCWSTR folder, PCWSTR name, FTPENTRY *out)
{
    if (!site || !site[0] || !name || !name[0] || !out) return FALSE;
    PCWSTR key = (folder && folder[0]) ? folder : L"/";
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockShared(&FtpCacheLock());
    for (auto const &e : FtpCacheEntries())
        if (0 == StrCmp(e.site, site) && 0 == StrCmp(e.path, key))
        {
            if (FtpCacheFresh(now, e.tick, FTP_CACHE_TTL_MS))
                for (auto const &it : e.items)
                    if (0 == StrCmp(it.szName, name))
                    {
                        *out = it;
                        ReleaseSRWLockShared(&FtpCacheLock());
                        return TRUE;
                    }
            break;
        }
    ReleaseSRWLockShared(&FtpCacheLock());
    return FALSE;
}

// Pure in-memory cache peek (NO disk-cache fallback, NO network/pipe fetch).
// Returns TRUE and deep-copies the FULL listing if the directory is currently
// cached (within TTL, i.e. the in-memory contents are the real remote listing).
// Returns FALSE when the directory is not in memory — the caller must NOT then
// do a synchronous FtpListCachedAll on the shell UI thread (a cold 100k-dir
// listing is a ~5-9MB / multi-second pipe read that freezes Explorer, which is
// exactly what Explorer's delete pre-count and a large-dir navigation hit).
// Callers that get FALSE should enumerate EMPTY and let FtpPrefetchQuiet fill
// the cache asynchronously + SHChangeNotify refresh the view.
inline BOOL FtpCachePeekAll(PCWSTR site, PCWSTR path, std::vector<FTPENTRY> &out)
{
    out.clear();
    if (!site || !site[0]) return FALSE;
    PCWSTR key = (path && path[0]) ? path : L"/";
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockShared(&FtpCacheLock());
    for (auto const &entry : FtpCacheEntries())
    {
        if (0 == StrCmp(entry.path, key) && 0 == StrCmp(entry.site, site) && FtpCacheFresh(now, entry.tick, FTP_CACHE_TTL_MS))
        {
            out = entry.items;
            ReleaseSRWLockShared(&FtpCacheLock());
            return TRUE;
        }
    }
    ReleaseSRWLockShared(&FtpCacheLock());
    return FALSE;
}

// ---------------------------------------------------------------------------
// UI-thread fetch suppression window.
//
// Explorer builds the *default* context menu (and through it the Delete /
// Properties command-bar verbs) by calling GetUIObjectOf(IID_IContextMenu) ->
// SHCreateDefaultContextMenu, which queries our attached IDataObject. That query
// reaches CRemoteDataObject::ExpandIfNeeded -> ExpandInto -> FtpListCachedAll,
// which on a COLD directory is a synchronous ~5-9MB / multi-second pipe read on
// the shell UI thread (log: "[BRIDGE] path=.../big-5 bytes=9188907
// elapsed=6875" fired inside the IContextMenu build). That is the "转圈圈很久"
// before the Delete / Properties / right-click UI appears.
//
// While this counter is > 0 the data object must NOT start a network/pipe fetch;
// it reads the cache only and lets the background prefetch warm it. Ctrl+C copy
// does NOT go through the context menu, so it is unaffected and keeps its full
// eager expansion (copy needs the complete file list).
inline volatile LONG *UiFetchSuppressCounter()
{
    static volatile LONG c = 0;
    return &c;
}
inline BOOL UiFetchSuppressed()
{
    return *UiFetchSuppressCounter() > 0;
}
struct UiFetchSuppressGuard
{
    UiFetchSuppressGuard() { InterlockedIncrement(UiFetchSuppressCounter()); }
    ~UiFetchSuppressGuard() { InterlockedDecrement(UiFetchSuppressCounter()); }
};

// Immediate view notify (background). The patched cache entry already exists,
// so the re-enumeration shows the change at once (no network involved).
inline void FtpNotifyUpdateDir(PIDLIST_ABSOLUTE notifyPidl)
{
    if (!notifyPidl) return;
    struct Runner
    {
        static DWORD WINAPI Run(LPVOID p)
        {
            SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_IDLIST, (PCIDLIST_ABSOLUTE)p, NULL);
            ILFree((PIDLIST_ABSOLUTE)p);
            DllRelease();
            return 0;
        }
    };
    DllAddRef();   // 2026-09-20: pin the module until the runner below returns
    HANDLE h = CreateThread(NULL, 0, Runner::Run, ILCloneFull(notifyPidl), 0, NULL);
    if (h) CloseHandle(h);
    else DllRelease();
}

// Background prefetch: replace the (possibly stale or empty) cache with the
// real listing (accurate metadata). Runs entirely off the caller's thread.
// When `notifyPidl` is non-NULL, fires SHCNE_UPDATEDIR after a successful fill
// so an empty view that was returned while the cache was cold repopulates as
// soon as the listing is ready (e.g. async enumeration of a big directory).
struct FtpPrefetchCtx
{
    WCHAR site[64];
    WCHAR folder[600];
    WCHAR key[700];                // "site|folder", removed from the in-flight set on exit
    PIDLIST_ABSOLUTE notifyPidl;   // cloned by FtpPrefetchQuiet, freed here
};

// In-flight prefetch set: many cold GetItemMeta calls (per rendered item / per
// sort comparison / per Details cell) must not spawn one thread each.
inline SRWLOCK &FtpPrefetchLock()
{
    static SRWLOCK l = SRWLOCK_INIT;
    return l;
}
inline std::vector<std::wstring> &FtpPrefetchInFlight()
{
    static std::vector<std::wstring> v;
    return v;
}
inline BOOL FtpPrefetchBegin(PCWSTR key)
{
    AcquireSRWLockExclusive(&FtpPrefetchLock());
    std::vector<std::wstring> &v = FtpPrefetchInFlight();
    if (std::find(v.begin(), v.end(), key) != v.end())
    {
        ReleaseSRWLockExclusive(&FtpPrefetchLock());
        return FALSE;
    }
    v.push_back(key);
    ReleaseSRWLockExclusive(&FtpPrefetchLock());
    return TRUE;
}
// 只读查询：某个键的预取/预热是否正在跑。
inline BOOL FtpPrefetchInFlightHas(PCWSTR key)
{
    BOOL found = FALSE;
    AcquireSRWLockShared(&FtpPrefetchLock());
    std::vector<std::wstring> &v = FtpPrefetchInFlight();
    found = (std::find(v.begin(), v.end(), std::wstring(key)) != v.end());
    ReleaseSRWLockShared(&FtpPrefetchLock());
    return found;
}
inline void FtpPrefetchEnd(PCWSTR key)
{
    AcquireSRWLockExclusive(&FtpPrefetchLock());
    std::vector<std::wstring> &v = FtpPrefetchInFlight();
    v.erase(std::remove(v.begin(), v.end(), std::wstring(key)), v.end());
    ReleaseSRWLockExclusive(&FtpPrefetchLock());
}
// Pending UPDATEDIR notifications for keys that are ALREADY being prefetched.
// Race fixed 2026-09-20: mkdir/rename start a prefetch WITHOUT a notify PIDL;
// the view re-enumeration that follows calls FtpPrefetchQuiet WITH one, but the
// in-flight dedup used to drop it on the floor — the prefetch then completed
// silently and the view stayed on the "loading..." placeholder forever (mkdir
// showed nothing / the new directory looked empty). Late notifies are queued
// here and ALL fired when the in-flight fetch completes.
inline SRWLOCK &FtpPrefetchNotifyLock()
{
    static SRWLOCK l = SRWLOCK_INIT;
    return l;
}
struct FtpPrefetchNotify
{
    std::wstring key;
    PIDLIST_ABSOLUTE pidl;   // cloned, freed on fire
};
inline std::vector<FtpPrefetchNotify> &FtpPrefetchNotifyPending()
{
    static std::vector<FtpPrefetchNotify> v;
    return v;
}
inline void FtpPrefetchNotifyAdd(PCWSTR key, PIDLIST_ABSOLUTE pidl)
{
    if (!key || !key[0] || !pidl) return;
    PIDLIST_ABSOLUTE c = ILCloneFull(pidl);
    if (!c) return;
    AcquireSRWLockExclusive(&FtpPrefetchNotifyLock());
    auto &v = FtpPrefetchNotifyPending();
    int same = 0;
    for (auto &n : v) if (n.key == key && ++same >= 8) break;
    if (same >= 8)
    {
        // Bound the queue: stale views may pile up; drop the oldest for this key.
        for (auto it = v.begin(); it != v.end(); ++it)
            if (it->key == key) { ILFree(it->pidl); v.erase(it); break; }
    }
    FtpPrefetchNotify n; n.key = key; n.pidl = c;
    v.push_back(n);
    ReleaseSRWLockExclusive(&FtpPrefetchNotifyLock());
}
// Fire every pending notify for key. Caller holds no locks (SHChangeNotify out).
inline void FtpPrefetchNotifyFire(PCWSTR key)
{
    std::vector<PIDLIST_ABSOLUTE> fire;
    AcquireSRWLockExclusive(&FtpPrefetchNotifyLock());
    auto &v = FtpPrefetchNotifyPending();
    for (auto it = v.begin(); it != v.end();)
    {
        if (it->key == key) { fire.push_back(it->pidl); it = v.erase(it); }
        else ++it;
    }
    ReleaseSRWLockExclusive(&FtpPrefetchNotifyLock());
    for (auto p : fire)
    {
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_IDLIST, (PCIDLIST_ABSOLUTE)p, NULL);
        ILFree(p);
    }
}

static DWORD WINAPI FtpPrefetchThreadProc(LPVOID p)
{
    FtpPrefetchCtx *c = static_cast<FtpPrefetchCtx *>(p);
    // A stuck in-flight key makes every later enumeration return the loading
    // placeholder forever, so the key MUST be released on every exit path —
    // including a C++ exception escaping the listing below. catch(...) here
    // keeps the key set consistent; the view simply retries on next navigate.
    try
    {
        std::vector<FTPENTRY> warm;
        if (!FtpListCachedAll(c->site, c->folder, warm) || warm.empty())
        {
            // Transient bridge/network hiccup: the optimistic patch entries would
            // otherwise linger with guessed metadata. Retry once after a beat.
            Sleep(300);
            warm.clear();
            FtpListCachedAll(c->site, c->folder, warm);
        }
        ProbeLog(L"[MUT] quiet prefetch site='%s' path='%s' n=%u", c->site, c->folder, (UINT)warm.size());
    }
    catch (...)
    {
        ProbeLog(L"[MUT] quiet prefetch threw site='%s' path='%s'", c->site, c->folder);
    }
    // Notify even when the fetch failed: the view re-enumerates (cold again ->
    // a fresh prefetch) instead of sitting on a stale placeholder.
    if (c->notifyPidl)
    {
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_IDLIST, c->notifyPidl, NULL);
        ILFree(c->notifyPidl);
    }
    FtpPrefetchNotifyFire(c->key);
    FtpPrefetchEnd(c->key);
    delete c;
    DllRelease();
    return 0;
}
inline void FtpPrefetchQuiet(PCWSTR site, PCWSTR folder, PIDLIST_ABSOLUTE notifyPidl = NULL)
{
    if (!site || !site[0]) return;
    PCWSTR dir = (folder && folder[0]) ? folder : L"/";
    WCHAR key[700] = {};
    StringCchPrintf(key, ARRAYSIZE(key), L"%s|%s", site, dir);
    if (!FtpPrefetchBegin(key)) { FtpPrefetchNotifyAdd(key, notifyPidl); return; }  // already being fetched
    FtpPrefetchCtx *c = new (std::nothrow) FtpPrefetchCtx{};
    if (!c) { FtpPrefetchEnd(key); return; }
    StringCchCopy(c->site, ARRAYSIZE(c->site), site);
    StringCchCopy(c->folder, ARRAYSIZE(c->folder), dir);
    StringCchCopy(c->key, ARRAYSIZE(c->key), key);
    c->notifyPidl = notifyPidl ? ILCloneFull(notifyPidl) : NULL;
    DllAddRef();
    HANDLE h = CreateThread(NULL, 0, FtpPrefetchThreadProc, c, 0, NULL);
    if (h) CloseHandle(h);
    else
    {
        DllRelease();
        if (c->notifyPidl) ILFree(c->notifyPidl);
        FtpPrefetchEnd(key);
        delete c;
    }
}

// ---- 递归后台预热（2026-09-20：源端复制冻结取证后的缓解） ---------------------
// 复制一个远程目录时，Shell 在**发起窗口的 UI 线程**上查询 CFSTR_FILEDESCRIPTORW，
// 我们必须在返回前枚举整棵树；冷目录每多一个就多一次同步桥接 LIST。实测一个
// 26535 项的**扁平**目录，一次 LIST 就占住 UI 线程 2750 ms（探针：
// `[DATAOBJ] expand done cacheOnly=0 ... elapsedMs=2750`）。
// 这里在数据对象刚建立（Ctrl+C / 拖拽开始 / 菜单探测）时，就把子树在后台逐个列进
// 内存+磁盘缓存，等真正粘贴时展开基本全命中，不再在 UI 线程上等网络。
struct FtpWarmTreeCtx
{
    WCHAR site[64];
    WCHAR folder[600];
    WCHAR key[760];          // "tree|site|folder"，与普通预取共用去重表
    int maxDirs;
    int maxEntries;
};

static DWORD WINAPI FtpWarmTreeThreadProc(LPVOID p)
{
    FtpWarmTreeCtx *c = static_cast<FtpWarmTreeCtx *>(p);
    int dirs = 0, entries = 0;
    try
    {
        std::vector<std::wstring> queue;
        queue.push_back(c->folder);
        for (size_t qi = 0; qi < queue.size() && dirs < c->maxDirs; ++qi)
        {
            std::vector<FTPENTRY> kids;
            // 失败（网络/管道）就跳过这一棵，绝不影响别的目录。
            if (!FtpListCachedAll(c->site, queue[qi].c_str(), kids)) continue;
            ++dirs;
            entries += (int)kids.size();
            if (entries >= c->maxEntries) break;
            for (auto const &k : kids)
            {
                if (!k.fIsFolder || k.fIsSymlink) continue;   // 符号链接目录不跟随
                std::wstring child = queue[qi];
                if (child.empty() || child[child.size() - 1] != L'/') child += L'/';
                child += k.szName;
                queue.push_back(child);
            }
        }
        ProbeLog(L"[WARM] tree site='%s' root='%s' dirs=%d entries=%d", c->site, c->folder, dirs, entries);
    }
    catch (...)
    {
        ProbeLog(L"[WARM] tree threw site='%s' root='%s'", c->site, c->folder);
    }
    FtpPrefetchEnd(c->key);
    delete c;
    DllRelease();
    return 0;
}

// 后台递归把 `folder` 子树列进缓存。重复调用同一个子树会被去重表挡掉。
inline void FtpPrefetchTreeQuiet(PCWSTR site, PCWSTR folder, int maxDirs = 512, int maxEntries = 300000)
{
    if (!site || !site[0]) return;
    PCWSTR dir = (folder && folder[0]) ? folder : L"/";
    WCHAR key[760] = {};
    StringCchPrintf(key, ARRAYSIZE(key), L"tree|%s|%s", site, dir);
    if (!FtpPrefetchBegin(key)) return;                 // 已在预热这棵树
    FtpWarmTreeCtx *c = new (std::nothrow) FtpWarmTreeCtx{};
    if (!c) { FtpPrefetchEnd(key); return; }
    StringCchCopy(c->site, ARRAYSIZE(c->site), site);
    StringCchCopy(c->folder, ARRAYSIZE(c->folder), dir);
    StringCchCopy(c->key, ARRAYSIZE(c->key), key);
    c->maxDirs = maxDirs; c->maxEntries = maxEntries;
    DllAddRef();   // 2026-09-20: 线程自己 pin 模块
    HANDLE h = CreateThread(NULL, 0, FtpWarmTreeThreadProc, c, 0, NULL);
    if (h) CloseHandle(h);
    else { DllRelease(); FtpPrefetchEnd(key); delete c; }
}

// Returns count of cached entries for site+path (0 = miss/expired).
inline int FtpCacheLookup(PCWSTR site, PCWSTR path, FTPENTRY *out, int maxOut)
{
    if (maxOut <= 0) return 0; PCWSTR key = (path && path[0]) ? path : L"/"; ULONGLONG now = GetTickCount64(); int got = 0;
    AcquireSRWLockShared(&FtpCacheLock());
    for (auto &e : FtpCacheEntries()) if (0 == StrCmp(e.path, key) && 0 == StrCmp(e.site, site)) { if (FtpCacheFresh(now, e.tick, FTP_CACHE_TTL_MS)) { got = (int)e.items.size(); if (got > maxOut) got = maxOut; for (int i = 0; i < got; ++i) out[i] = e.items[i]; } break; }
    ReleaseSRWLockShared(&FtpCacheLock()); if (got) return got;
    std::vector<FTPENTRY> disk; if (!FtpDiskCacheLoad(site, key, disk, FTP_DISK_CACHE_MAX_AGE_MS)) return 0; got = (int)disk.size(); if (got > maxOut) got = maxOut; for (int i = 0; i < got; ++i) out[i] = disk[i]; return got;
}

inline void FtpCacheStore(PCWSTR site, PCWSTR path, const FTPENTRY *items, int count)
{
    if (!site || !site[0] || count < 0) return; PCWSTR key = (path && path[0]) ? path : L"/"; ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&FtpCacheLock()); auto &v = FtpCacheEntries();
    for (auto it = v.begin(); it != v.end();) { if (0 == StrCmp(it->path, key) && 0 == StrCmp(it->site, site)) it = v.erase(it); else ++it; }
    FtpCacheEntry e = {}; StringCchCopy(e.site, ARRAYSIZE(e.site), site); StringCchCopy(e.path, ARRAYSIZE(e.path), key); e.tick = now; for (int i = 0; i < count; ++i) e.items.push_back(items[i]); v.push_back(std::move(e));
    for (auto it = v.begin(); it != v.end();) { if (!FtpCacheFresh(now, it->tick, FTP_CACHE_TTL_MS)) it = v.erase(it); else ++it; } while (v.size() > FTP_CACHE_MAX_DIRS) v.erase(v.begin()); ReleaseSRWLockExclusive(&FtpCacheLock());
    FtpDiskCacheStore(site, key, items, count);
}

// 把"刚从磁盘快照读回来"的内容提升进内存缓存。磁盘命中回填是必须的：
// 以前 FtpListCachedAll 命中磁盘后直接 return，内存仍然是冷的，于是紧接着的
// EnumObjects（只读内存）又 miss，视图永远热不起来。
// 不落盘 —— 内容本来就是从磁盘读出来的。
inline void FtpCachePromote(PCWSTR site, PCWSTR path, const std::vector<FTPENTRY> &items)
{
    if (!site || !site[0] || items.empty()) return;
    PCWSTR key = (path && path[0]) ? path : L"/";
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&FtpCacheLock()); auto &v = FtpCacheEntries();
    for (auto it = v.begin(); it != v.end();) { if (0 == StrCmp(it->path, key) && 0 == StrCmp(it->site, site)) it = v.erase(it); else ++it; }
    FtpCacheEntry e = {}; StringCchCopy(e.site, ARRAYSIZE(e.site), site); StringCchCopy(e.path, ARRAYSIZE(e.path), key); e.tick = now; e.items = items; v.push_back(std::move(e));
    for (auto it = v.begin(); it != v.end();) { if (!FtpCacheFresh(now, it->tick, FTP_CACHE_TTL_MS)) it = v.erase(it); else ++it; } while (v.size() > FTP_CACHE_MAX_DIRS) v.erase(v.begin());
    ReleaseSRWLockExclusive(&FtpCacheLock());
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
        // Health probe (2026-09-06): a listing where nearly every item has
        // mtime==0 AND size==0 AND no owner is almost certainly a corrupt
        // response/parse — it would blank all columns and scramble sorting
        // (folders no longer first) until the 3s TTL expires. Log any such
        // batch so the intermittent blank-columns report can be pinned.
        if (!listed.empty())
        {
            int zeroed = 0;
            for (auto const &it : listed)
                if (it.dwMtime == 0 && it.dwSize == 0 && !it.fIsFolder && !it.szOwner[0]) zeroed++;
            if (zeroed * 2 >= (int)listed.size())
                ProbeLog(L"[HEALTH] suspicious listing site='%s' path='%s' n=%u zeroed=%u",
                         site, pszPath, (UINT)listed.size(), (UINT)zeroed);
        }
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
inline BOOL FtpListCachedAll(PCWSTR site, PCWSTR path, std::vector<FTPENTRY> &out, bool waitForWarm)
{
    out.clear();
    PCWSTR key = (path && path[0]) ? path : L"/";
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockShared(&FtpCacheLock());
    for (auto const &entry : FtpCacheEntries())
    {
        if (0 == StrCmp(entry.path, key) && 0 == StrCmp(entry.site, site) && FtpCacheFresh(now, entry.tick, FTP_CACHE_TTL_MS))
        {
            out = entry.items;
            ReleaseSRWLockShared(&FtpCacheLock());
            return TRUE;
        }
    }
    ReleaseSRWLockShared(&FtpCacheLock());

    // 磁盘快照命中：立刻用它（"关掉资源管理器窗口再打开还是热目录"就是从这儿来的），
    // 并提升进内存，让紧接着的 EnumObjects（只读内存）也能命中。
    if (FtpDiskCacheLoad(site, key, out, FTP_DISK_CACHE_MAX_AGE_MS))
    {
        FtpCachePromote(site, key, out);
        return TRUE;
    }

    // 同一个目录的**后台预热**（tree|site|dir）正在跑时，先等它把内存缓存填好，
    // 而不是再发一次同步网络 LIST。实测同一次数据对象展开里"预热 + 同步 LIST"
    // 会各拉一遍（扁平 4469 项目录：预热 ~0.5 s，同步 LIST 占住 UI 线程 2.6 s）。
    // waitForWarm 只由数据对象展开路径传入 —— 预热线程自身调用时必须为 false，
    // 否则会等自己（那个键就是它持有的）。
    if (waitForWarm)
    {
        WCHAR warmKey[760] = {};
        StringCchPrintf(warmKey, ARRAYSIZE(warmKey), L"tree|%s|%s", site, key);
        if (FtpPrefetchInFlightHas(warmKey))
        {
            const ULONGLONG deadline = GetTickCount64() + 5000;
            while (GetTickCount64() < deadline)
            {
                Sleep(50);
                ULONGLONG t = GetTickCount64();
                BOOL hit = FALSE;
                AcquireSRWLockShared(&FtpCacheLock());
                for (auto const &entry : FtpCacheEntries())
                {
                    if (0 == StrCmp(entry.path, key) && 0 == StrCmp(entry.site, site) && FtpCacheFresh(t, entry.tick, FTP_CACHE_TTL_MS))
                    {
                        out = entry.items;
                        hit = TRUE;
                        break;
                    }
                }
                ReleaseSRWLockShared(&FtpCacheLock());
                if (hit)
                {
                    ProbeLog(L"[CACHE] expand waited for warm site='%s' path='%s' n=%u", site, key, (UINT)out.size());
                    return TRUE;
                }
            }
            ProbeLog(L"[CACHE] expand warm wait timed out site='%s' path='%s' -> sync LIST", site, key);
        }
    }

    FTPENTRY first = {};
    FtpListCached(site, key, &first, 1);

    AcquireSRWLockShared(&FtpCacheLock());
    for (auto const &entry : FtpCacheEntries())
    {
        if (0 == StrCmp(entry.path, key) && 0 == StrCmp(entry.site, site) && FtpCacheFresh(now, entry.tick, FTP_CACHE_TTL_MS))
        {
            out = entry.items;
            ReleaseSRWLockShared(&FtpCacheLock());
            return TRUE;
        }
    }
    ReleaseSRWLockShared(&FtpCacheLock());
    return FALSE;
}

// 枚举用的"先内存、再磁盘"快照读取（EnumObjects 走这条）。
//
// 为什么必须读磁盘：EnumObjects 以前只调 FtpCachePeekAll（纯内存、明确无磁盘回退），
// 于是"关掉资源管理器窗口再打开"必然是冷目录 —— 哪怕磁盘上的快照是几秒前刚写的，
// 首屏也得先显示"正在载入…"占位、再等后台把同一个目录重新拉一遍。
// 用户看到的就是"缓存完全没有持久化"。
//
// 保护：磁盘快照超过 8 MB（≈1.1 万项）就不在 UI 线程上搬，仍然交给后台预取 ——
// 本地文件读虽然快，也不能让一个超大目录把资源管理器的 UI 线程占住。
inline BOOL FtpCachePeekAllOrDisk(PCWSTR site, PCWSTR path, std::vector<FTPENTRY> &out)
{
    if (FtpCachePeekAll(site, path, out)) return TRUE;

    WCHAR file[MAX_PATH] = {};
    if (FtpMetadataCacheFile(site, path, file, ARRAYSIZE(file)))
    {
        WIN32_FILE_ATTRIBUTE_DATA a = {};
        if (GetFileAttributesExW(file, GetFileExInfoStandard, &a))
        {
            ULONGLONG size = ((ULONGLONG)a.nFileSizeHigh << 32) | a.nFileSizeLow;
            if (size > 8ULL * 1024 * 1024) { out.clear(); return FALSE; }
        }
    }

    if (!FtpDiskCacheLoad(site, path, out, FTP_DISK_CACHE_MAX_AGE_MS) || out.empty()) return FALSE;
    FtpCachePromote(site, path, out);   // 回填内存，后续查询直接命中
    ProbeLog(L"[CACHE] disk snapshot hit site='%s' path='%s' n=%u", site, path, (UINT)out.size());
    return TRUE;
}
