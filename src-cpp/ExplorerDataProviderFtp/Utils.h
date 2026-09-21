/**************************************************************************
THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
PARTICULAR PURPOSE.

(c) Microsoft Corporation. All Rights Reserved.
**************************************************************************/

#pragma once

#define ISFOLDERFROMINDEX(u) (BOOL)(u % 2)

HRESULT LoadFolderViewImplDisplayString(UINT uIndex, PWSTR psz, UINT cch);
HRESULT LoadFolderViewImplDisplayStrings(PWSTR wszArrStrings[], UINT cArray);
HRESULT GetIndexFromDisplayString(PCWSTR psz, UINT *puIndex);
STDAPI StringToStrRet(PCWSTR pszName, STRRET *pStrRet);
HRESULT DisplayItem(IShellItemArray *psia, HWND hwndParent);

#ifndef ResultFromShort
#define ResultFromShort(i)      MAKE_HRESULT(SEVERITY_SUCCESS, 0, (USHORT)(i))
#endif

__inline HRESULT ResultFromKnownLastError() { const DWORD err = GetLastError(); return err == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32(err); }

extern HINSTANCE g_hInst;

void DllAddRef();
void DllRelease();

// ── 宿主进程保护（2026-09-21：Office 叮咚声/崩溃）───────────────────────
// 我们的命名空间文件夹钉在桌面命名空间下，任何进程的文件对话框枚举桌面时
// 都会把本 DLL 加载进来。explorer.exe 之外（Office 等）的宿主里：
//   * 绝不弹窗（MessageBoxW(NULL,...) 在异进程里是叮咚声/崩溃的嫌疑）——只记日志；
//   * 需要用户输入的一律 fail-closed（见 ContextMenu 的 PromptText 守卫）。
// 本 DLL 自己的工具进程（服务/CLI/测试具）不受影响。
#include "ProbeLog.h"
inline bool ErfHostIsExplorer()
{
    static LONG s_state = -1;   // -1=未判定 0=异进程 1=explorer
    if (s_state >= 0) return s_state == 1;
    WCHAR path[MAX_PATH] = {};
    GetModuleFileNameW(NULL, path, MAX_PATH);
    const WCHAR *base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    bool explorer = (0 == _wcsicmp(base, L"explorer.exe"));
    s_state = explorer ? 1 : 0;
    if (!explorer)
        ProbeLog(L"[HOST] foreign host process='%s' — modal UI suppressed, input prompts fail closed", base);
    return explorer;
}
inline bool ErfHostIsOwnTool()
{
    WCHAR path[MAX_PATH] = {};
    GetModuleFileNameW(NULL, path, MAX_PATH);
    const WCHAR *base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    return 0 == _wcsicmp(base, L"RemoteFsClient.exe") ||
           0 == _wcsicmp(base, L"ExplorerRemoteFs.Cli.exe") ||
           0 == _wcsicmp(base, L"ErfHarness.exe");
}
// 本 DLL 里所有 MessageBoxW 都必须走这里（异进程只记日志不弹窗）。
inline int ErfMessageBoxW(HWND hwnd, LPCWSTR text, LPCWSTR caption, UINT type)
{
    if (ErfHostIsExplorer() || ErfHostIsOwnTool())
        return (int)MessageBoxW(hwnd, text, caption, type);
    ProbeLog(L"[UI-SUPPRESSED] foreign host caption='%s' text='%.128s'",
             caption ? caption : L"", text ? text : L"");
    return (type & MB_YESNO) ? IDNO : IDOK;   // 确认框一律按"否/取消"处理（ fail-closed）
}


// Legacy fixed storage used only for site/column helpers. Remote directory
// enumeration is dynamic and must not be limited by this value.
#define MAX_OBJS    256
