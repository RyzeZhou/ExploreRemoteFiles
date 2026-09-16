#pragma once
// Dynamic site list for the shell namespace root.
// Reads %APPDATA%\ExplorerRemoteFs\connections.json (same file the .NET
// provider and the GUI client use). Naive JSON scan is fine here: the file
// is a small array of flat objects with known keys.
#include <windows.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <string>
#include <vector>

typedef struct
{
    WCHAR name[64];
    WCHAR type[16];
    WCHAR host[128];
    int   port;              // 0 = protocol default
    WCHAR user[64];
    WCHAR keyPath[260];      // PrivateKeyPath：让"打开终端"能复用已有密钥认证
    WCHAR startPath[256];
    WCHAR terminal[16];      // 站点级终端：wt / powershell / vscode；空 = 跟随全局设置
    WCHAR sshAlias[96];      // 绑定的现有 SSH Host 别名（%USERPROFILE%\.ssh\config）；空 = 未绑定
    // 列顺序：9 个数字（0..8 的排列），第 i 位 = 显示列号 i 对应的语义属性号
    // （0=名称 1=类型 2=大小 3=修改时间 4=权限 5=所有者 6=所有者ID 7=组 8=组ID）。
    // 空/非法 = 默认 "012345678"。只影响新建或重置后的视图，Explorer 按文件夹记住用户拖后的顺序。
    char  columnOrder[16];
} FTPSITE;

inline SRWLOCK &FtpSitesLock()
{
    static SRWLOCK l = SRWLOCK_INIT;
    return l;
}
inline std::vector<FTPSITE> &FtpSitesVec()
{
    static std::vector<FTPSITE> v;
    return v;
}
inline ULONGLONG &FtpSitesTick()
{
    static ULONGLONG t = 0;
    return t;
}

inline void FtpSitesReload()
{
    WCHAR path[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, path))) return;
    StringCchCatW(path, MAX_PATH, L"\\ExplorerRemoteFs\\connections.json");

    std::vector<FTPSITE> fresh;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE)
    {
        std::string text;
        char buf[4096]; DWORD got = 0;
        while (ReadFile(h, buf, sizeof(buf), &got, NULL) && got) text.append(buf, got);
        CloseHandle(h);

        size_t pos = 0;
        while ((pos = text.find('{', pos)) != std::string::npos)
        {
            size_t end = text.find('}', pos);
            if (end == std::string::npos) break;
            std::string obj = text.substr(pos, end - pos);
            pos = end + 1;

            auto getStr = [&](const char *key, WCHAR *out, UINT cch)
            {
                std::string pat = std::string("\"") + key + "\"";
                size_t k = obj.find(pat);
                if (k == std::string::npos) return;
                k = obj.find(':', k + pat.size());
                if (k == std::string::npos) return;
                ++k;
                while (k < obj.size() && (obj[k] == ' ' || obj[k] == '\t')) ++k;
                // 只接受字符串值。null / 数字 / true / false 必须原样放弃 ——
                // 否则会一路找到下一个键的引号，把键名当成值：
                // 实测 "PrivateKeyPath": null 曾被解析成 "StartPath"，
                // 于是 ssh 收到 -i 'StartPath'（终端启动失败）。
                if (k >= obj.size() || obj[k] != '"') return;
                size_t e = obj.find('"', k + 1);
                if (e == std::string::npos) return;
                std::string val = obj.substr(k + 1, e - k - 1);
                int need = MultiByteToWideChar(CP_UTF8, 0, val.c_str(), -1, NULL, 0);
                if (need > 0) MultiByteToWideChar(CP_UTF8, 0, val.c_str(), -1, out, cch);
            };
            // 同上，但保留窄字符串（列顺序是纯 ASCII 数字串，不需要转宽字符）。
            auto getNarrow = [&](const char *key, char *out, UINT cch)
            {
                std::string pat = std::string("\"") + key + "\"";
                size_t k = obj.find(pat);
                if (k == std::string::npos) return;
                k = obj.find(':', k + pat.size());
                if (k == std::string::npos) return;
                ++k;
                while (k < obj.size() && (obj[k] == ' ' || obj[k] == '\t')) ++k;
                if (k >= obj.size() || obj[k] != '"') return;    // 只接受字符串值（同 getStr）
                size_t e = obj.find('"', k + 1);
                if (e == std::string::npos) return;
                std::string val = obj.substr(k + 1, e - k - 1);
                if (val.size() >= cch) val.resize(cch - 1);
                memcpy(out, val.c_str(), val.size() + 1);
            };

            FTPSITE s = {}; s.port = 0;
            getStr("Name", s.name, 64);
            if (!s.name[0]) continue;                       // not a site object
            getStr("Type", s.type, 16);
            if (!s.type[0]) StringCchCopyW(s.type, 16, L"sftp");
            getStr("Host", s.host, 128);
            getStr("Username", s.user, 64);
            getStr("PrivateKeyPath", s.keyPath, 260);
            getStr("StartPath", s.startPath, 256);
            getStr("Terminal", s.terminal, 16);
            getStr("SshHostAlias", s.sshAlias, 96);
            getNarrow("ColumnOrder", s.columnOrder, ARRAYSIZE(s.columnOrder));
            if (!s.startPath[0]) StringCchCopyW(s.startPath, 256, L"/");
            {
                // "Port": 2121  |  "Port": null
                std::string pat = "\"Port\"";
                size_t k = obj.find(pat);
                if (k != std::string::npos)
                {
                    k = obj.find(':', k + pat.size());
                    if (k != std::string::npos)
                    {
                        ++k;
                        while (k < obj.size() && obj[k] == ' ') ++k;
                        int val = 0; bool any = false;
                        while (k < obj.size() && obj[k] >= '0' && obj[k] <= '9')
                        { val = val * 10 + (obj[k] - '0'); ++k; any = true; }
                        if (any) s.port = val;
                    }
                }
            }
            fresh.push_back(s);
        }
    }

    AcquireSRWLockExclusive(&FtpSitesLock());
    FtpSitesVec().swap(fresh);
    FtpSitesTick() = GetTickCount64();
    ReleaseSRWLockExclusive(&FtpSitesLock());
}

// Site count; re-reads the file at most every 2 seconds.
inline int FtpSitesGet(FTPSITE *out, int maxOut)
{
    ULONGLONG now = GetTickCount64();
    {
        AcquireSRWLockShared(&FtpSitesLock());
        bool fresh = (FtpSitesTick() != 0 && now - FtpSitesTick() <= 2000);
        int n = 0;
        if (fresh)
        {
            for (auto &s : FtpSitesVec()) { if (n >= maxOut) break; out[n++] = s; }
        }
        ReleaseSRWLockShared(&FtpSitesLock());
        if (fresh) return n;
    }
    FtpSitesReload();
    AcquireSRWLockShared(&FtpSitesLock());
    int n = 0;
    for (auto &s : FtpSitesVec()) { if (n >= maxOut) break; out[n++] = s; }
    ReleaseSRWLockShared(&FtpSitesLock());
    return n;
}

inline const FTPSITE *FtpSiteFind(PCWSTR name)
{
    ULONGLONG now = GetTickCount64();
    {
        AcquireSRWLockShared(&FtpSitesLock());
        bool fresh = (FtpSitesTick() != 0 && now - FtpSitesTick() <= 2000);
        const FTPSITE *hit = NULL;
        if (fresh)
        {
            for (auto &s : FtpSitesVec())
                if (0 == StrCmpW(s.name, name)) { hit = &s; break; }
        }
        ReleaseSRWLockShared(&FtpSitesLock());
        if (fresh) return hit;
    }
    FtpSitesReload();
    AcquireSRWLockShared(&FtpSitesLock());
    const FTPSITE *hit = NULL;
    for (auto &s : FtpSitesVec())
        if (0 == StrCmpW(s.name, name)) { hit = &s; break; }
    ReleaseSRWLockShared(&FtpSitesLock());
    // NOTE: pointer into the static vector; caller must not hold it across reloads.
    return hit;
}
