#pragma once
// 修改时间的显示口径 —— 与 C# 的 ExplorerRemoteFs.Utils.TimeDisplay 是**同一套规则**
// （同一注册表值 HKCU\Software\ExplorerRemoteFs\TimeDisplayMode），两边必须保持一致。
//
// 为什么要有"口径"：SFTP 给的是 Unix 秒（绝对时刻，无歧义），而 FTP 的 LIST 只给
// "服务器本地时间"的字面值，协议不带时区（RFC 3659 只规定 MDTM 用 UTC）。
// 同一个文件按本地 / 服务器 / UTC 三种口径显示可以差好几小时，所以让用户选，
// 默认本地时区（与同一窗口里的本地文件列一致）。
//
// 服务侧在 ITEM 行里带上"服务器时区偏移（分钟，-1 = 未探测）"，
// 这里据此渲染 —— DLL 不必自己去读 connections.json。

#include <windows.h>
#include <time.h>
#include <strsafe.h>

enum ErfTimeDisplayMode
{
    ErfTimeLocal = 0,    // 本地时区（默认）
    ErfTimeServer = 1,   // 服务器时区；偏移未知时退回本地
    ErfTimeUtc = 2,      // UTC+0
};

// 当前口径。缓存 2 秒 —— 列渲染按行按列调用，不能每次都读注册表。
inline ErfTimeDisplayMode ErfTimeDisplayModeRead()
{
    static ErfTimeDisplayMode cached = ErfTimeLocal;
    static ULONGLONG cachedAt = 0;
    ULONGLONG now = GetTickCount64();
    if (cachedAt != 0 && now - cachedAt < 2000) return cached;

    WCHAR value[32] = {};
    DWORD cb = sizeof(value) - sizeof(WCHAR);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\ExplorerRemoteFs", L"TimeDisplayMode",
                     RRF_RT_REG_SZ, NULL, value, &cb) == ERROR_SUCCESS)
    {
        if (0 == _wcsicmp(value, L"utc")) cached = ErfTimeUtc;
        else if (0 == _wcsicmp(value, L"server")) cached = ErfTimeServer;
        else cached = ErfTimeLocal;   // 非法/空一律回退默认（与 C# 侧一致）
    }
    else cached = ErfTimeLocal;
    cachedAt = now;
    return cached;
}

// 把 unix epoch 秒按口径渲染成 "yyyy-MM-dd HH:mm"。
// serverOffsetMinutes = -1（未探测）时，服务器口径退回本地 —— 与 C# 侧同一条规则。
inline void ErfFormatMtime(DWORD mtime, int serverOffsetMinutes, PWSTR out, UINT cch)
{
    __time64_t t = (__time64_t)mtime;
    struct tm tmv;
    BOOL ok = FALSE;

    switch (ErfTimeDisplayModeRead())
    {
    case ErfTimeUtc:
        ok = (0 == _gmtime64_s(&tmv, &t));
        break;
    case ErfTimeServer:
        if (serverOffsetMinutes == -1)
        {
            ok = (0 == _localtime64_s(&tmv, &t));
        }
        else
        {
            t += (__time64_t)serverOffsetMinutes * 60;
            ok = (0 == _gmtime64_s(&tmv, &t));
        }
        break;
    default:
        ok = (0 == _localtime64_s(&tmv, &t));
        break;
    }

    if (!ok) { StringCchCopy(out, cch, L"-"); return; }
    StringCchPrintf(out, cch, L"%04d-%02d-%02d %02d:%02d",
                    tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
}
