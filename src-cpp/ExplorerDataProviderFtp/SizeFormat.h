#pragma once
// 文件大小的显示口径（单位与进制）。
//
// 背景：**同一串数字，三种"正确"写法**，混用会让人对不上账 ——
//   * Windows 资源管理器：1024 进制，但标签写 KB/MB（"1.00 MB (1,048,576 字节)"）；
//   * GNU coreutils 默认（ls -h / du -h / df -h）：1024 进制，标签 K/M/G；
//   * `--si` / `df -H` / GNOME 文件、macOS Finder：**1000** 进制，标签 kB/MB；
//   * 严格 IEC 80000-13：kB = 1000 字节，KiB = 1024 字节。
// 远端给的是精确字节数（SFTP 的 st_size / FTP 的 SIZE），进制只是显示问题，
// 所以这里让用户选口径，**默认取 Windows/Explorer 口径**（我们长在资源管理器里，
// 与同一窗口的本地文件列保持一致比"标签更严格"更重要）。
//
// 取值（HKCU\Software\ExplorerRemoteFs\SizeFormat，与客户端设置同一处）：
//   "auto"（默认）**交给 Windows 自己格式化**（StrFormatByteSizeW）：实测规则是
//          "3 位有效数字 + 数值不超过 999" —— 1..1023 直接给字节，1024 起
//          1.00 KB / 9.76 KB / 22.9 KB / 976 KB / 0.99 MB / 1.00 MB …
//          Win11 新版把 Details 与属性页改成"自适应单位"后我们**自动跟上**，
//          不必去追它没公开的阈值与小数位规则
//   "kb"          1024 进制 + 整 KB（Windows 属性页那种）
//   "si"          1000 进制 + kB/MB/GB（与 Nautilus / ls --si 一致）
//   "iec"         1024 进制 + KiB/MiB/GiB（严格 IEC）
//
// 属性页永远额外给出**精确字节数**（"1.2 MB (1,234,567 B)"），
// 口径怎么选都不会让信息丢失。

#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shlwapi.h>   // StrFormatByteSizeW：Windows 自己的文件大小格式化
#include <string.h>
#include <strsafe.h>

enum ErfSizeFormat
{
    ERF_SIZE_AUTO = 0,   // 系统口径：调用 Windows 自己的格式化
    ERF_SIZE_KB,         // 1024 + 整 KB
    ERF_SIZE_SI,         // 1000 + kB/MB/GB
    ERF_SIZE_IEC,        // 1024 + KiB/MiB/GiB
};

inline ErfSizeFormat ErfParseSizeFormat(PCWSTR value)
{
    if (!value || !value[0]) return ERF_SIZE_AUTO;
    // 用 CRT 的 _wcsicmp 而不是 Shlwapi 的 StrCmpIW：这个头文件要能被独立编译的测试直接包含。
    if (0 == _wcsicmp(value, L"kb")) return ERF_SIZE_KB;
    if (0 == _wcsicmp(value, L"si")) return ERF_SIZE_SI;
    if (0 == _wcsicmp(value, L"iec")) return ERF_SIZE_IEC;
    return ERF_SIZE_AUTO;
}

// 从注册表读口径。缓存 2 秒：这个函数按"每行每列"被调用，不能每次去戳注册表。
inline ErfSizeFormat ErfSizeFormatMode()
{
    static DWORD cached = 0xFFFFFFFF;
    static ULONGLONG tick = 0;
    ULONGLONG now = GetTickCount64();
    if (cached != 0xFFFFFFFF && now - tick < 2000) return (ErfSizeFormat)cached;

    WCHAR buf[32] = {};
    DWORD cb = sizeof(buf);
    ErfSizeFormat mode = ERF_SIZE_AUTO;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\ExplorerRemoteFs", L"SizeFormat",
                     RRF_RT_REG_SZ, NULL, buf, &cb) == ERROR_SUCCESS && buf[0])
        mode = ErfParseSizeFormat(buf);
    cached = (DWORD)mode;
    tick = now;
    return mode;
}

// 千位分隔的精确字节数。
inline void ErfFormatExactBytes(ULONGLONG value, PWSTR out, UINT cch)
{
    WCHAR raw[32] = {};
    StringCchPrintf(raw, ARRAYSIZE(raw), L"%llu", value);
    UINT digits = (UINT)wcslen(raw), first = digits % 3;
    if (!first) first = 3;
    WCHAR text[40] = {};
    StringCchCopyN(text, ARRAYSIZE(text), raw, first);
    for (UINT i = first; i < digits; i += 3)
    {
        StringCchCat(text, ARRAYSIZE(text), L",");
        WCHAR part[4] = { raw[i], raw[i + 1], raw[i + 2], 0 };
        StringCchCat(text, ARRAYSIZE(text), part);
    }
    StringCchCopy(out, cch, text);
}

// 十进制（SI）单位：kB/MB/GB…；二进制（IEC）单位：KiB/MiB/GiB…；
// "auto" 直接交给 Windows 自己格式化（见下）。
// 显式指定口径的版本：单元测试与"口径预览"用它，避免读注册表（那有 2 秒缓存）。
inline void ErfFormatSizeForMode(ULONGLONG size, BOOL folder, ErfSizeFormat mode, PWSTR out, UINT cch)
{
    if (folder) { StringCchCopy(out, cch, L"-"); return; }

    if (mode == ERF_SIZE_AUTO)
    {
        // **交给 Windows 自己**：资源管理器的 Details 列与属性页用的就是这套
        // （实测 StrFormatByteSizeW 与属性系统 PKEY_Size 的输出逐字相同）。
        // 这样 Win11 把显示改成"自适应单位"之后我们**自动跟上** —— 微软没公开
        // 阈值与小数位规则，与其猜，不如用它的实现。
        // 实测（Win10 22H2 19045.6466）：1..1023 → "999 字节"；1024 → "1.00 KB"；
        //   10000 → "9.76 KB"；23506 → "22.9 KB"；999999 → "976 KB"；
        //   1048575 → "0.99 MB"；1500000 → "1.42 MB"；1500000000 → "1.39 GB"。
        //   即"3 位有效数字 + 数值不超过 999"（超了进下一个单位）。
        //
        // 注意返回值：StrFormatByteSizeW 返回的是 **PWSTR**（缓冲区指针），不是 HRESULT！
        // 早先写成 SUCCEEDED(...) 判断，指针被截断成 HRESULT 后判成失败，
        // 于是静默退回 KiB 口径（列里显示 976.6 KiB 而不是 976 KB）—— 自检抓出来的。
        // 判据改成"缓冲区里有没有内容"。
        out[0] = 0;
        StrFormatByteSizeW(size, out, cch);
        if (out[0]) return;
        mode = ERF_SIZE_IEC;   // API 没写出东西时退回自己的二进制口径（标签用 KiB 系，不冒充系统）
    }

    if (mode == ERF_SIZE_KB)
    {
        // 旧版资源管理器那种：整 KB，向上取整。
        ULONGLONG kb = (size + 1023) / 1024;
        StringCchPrintf(out, cch, L"%llu KB", kb);
        return;
    }

    const double base = (mode == ERF_SIZE_SI) ? 1000.0 : 1024.0;
    static const WCHAR *siUnits[]  = { L"kB",  L"MB",  L"GB",  L"TB",  L"PB"  };
    static const WCHAR *iecUnits[] = { L"KiB", L"MiB", L"GiB", L"TiB", L"PiB" };
    const WCHAR **units = (mode == ERF_SIZE_SI) ? siUnits : iecUnits;

    if ((double)size < base)
    {
        StringCchPrintf(out, cch, L"%llu B", size);   // 小于一个单位：直接给字节，别写 0.9 kB
        return;
    }
    double shown = (double)size / base;
    int unit = 0;
    while (shown >= base && unit < 4) { shown /= base; ++unit; }
    StringCchPrintf(out, cch, L"%.1f %s", shown, units[unit]);
}

// 读注册表口径的版本（正常调用走这个）。
inline void ErfFormatSize(ULONGLONG size, BOOL folder, PWSTR out, UINT cch)
{
    ErfFormatSizeForMode(size, folder, ErfSizeFormatMode(), out, cch);
}

// 属性页/详情用：口径 + 精确字节，两者都给（"1.30 KB (1,340 B)"）。
inline void ErfFormatSizeWithExact(ULONGLONG size, BOOL folder, PWSTR out, UINT cch)
{
    if (folder) { StringCchCopy(out, cch, L"-"); return; }
    WCHAR human[64] = {};
    ErfFormatSize(size, FALSE, human, ARRAYSIZE(human));
    if (0 == wcscmp(human, L"-")) { StringCchCopy(out, cch, human); return; }

    // 已经写的就是精确字节数时不要重复（Windows 自己会把 <1KB 写成 "999 字节"）。
    // 判据用"数字是否出现"而不是猜单位：字符串里含精确数字（带或不带千位分隔）就跳过。
    WCHAR raw[32] = {};
    StringCchPrintf(raw, ARRAYSIZE(raw), L"%llu", size);
    WCHAR exact[40] = {};
    ErfFormatExactBytes(size, exact, ARRAYSIZE(exact));
    if (wcsstr(human, raw) || wcsstr(human, exact))
    {
        StringCchCopy(out, cch, human);
        return;
    }
    StringCchPrintf(out, cch, L"%s (%s B)", human, exact);
}
