// proptrigger.cpp - 触发 ERF 命名空间扩展的属性页（诊断工具，2026-09-19）
//
// 为什么需要它：
//   「属性页在前台时锁住其他资源管理器窗口」**只在 Win11 上复现**。那边只能靠
//   手工右键 -> 属性来复现，而手工操作说不清"每次是不是同一条路径"、也说不清
//   "到底从哪一刻开始锁的"。这个小工具用与资源管理器**完全相同**的路径打开
//   属性页（IShellFolder::GetUIObjectOf(IID_IShellPropSheetExt) + AddPages +
//   PropertySheet），于是探针（src-cpp 的 PropSheetProbe.h）留下的日志可以逐次对比。
//
// 它同时是"探针能不能用"的验证手段：日志里必须出现
//   [PROBE] ===== chain dump ...  /  [PROBE] sampler started  /  [PROBE] AddPages ...
//
// 用法：
//   PropTrigger.exe [shell 路径] [自动关闭秒数]
//   默认路径 = ::{C816CE0E-728C-4FC9-98E5-D0B35B384597}（"易远传"根，站点列表页）
//   例：
//     PropTrigger.exe
//     PropTrigger.exe "::{C816CE0E-728C-4FC9-98E5-D0B35B384597}\WSL-SFTP\home\zhou\AI_work\test.txt" 10
//
// 行为：打开属性页 -> 等 N 秒（默认 8）-> 只给**本线程自己的** #32770 发 WM_CLOSE。
//       全程不碰别的窗口。

#include <windows.h>
#include <shlobj.h>
#include <shlobj_core.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>

#define ERF_CLSID_PATH L"::{C816CE0E-728C-4FC9-98E5-D0B35B384597}"

// {5DD84779-FEF1-46A3-8FCF-9F1A9603BB8F} = CLSID_RemoteFsPropSheet（见 src-cpp 的 Guid.h）。
// 属性页扩展是一个**独立的 COM 类**，由资源管理器按注册表加载；
// 所以 IShellFolder::GetUIObjectOf(IID_IShellPropSheetExt) 会返回 E_NOINTERFACE ——
// 要复现 shell 的行为，得自己 CoCreateInstance 再喂 IDataObject 给它。
static const GUID CLSID_ErfPropSheet =
    { 0x5dd84779, 0xfef1, 0x46a3, { 0x8f, 0xcf, 0x9f, 0x1a, 0x96, 0x03, 0xbb, 0x8f } };

static HPROPSHEETPAGE g_pages[16];
static int g_count = 0;

static int CALLBACK AddPageProc(HPROPSHEETPAGE page, LPARAM /*lParam*/)
{
    if (g_count < 16) g_pages[g_count++] = page;
    return TRUE;
}

struct CloseCtx
{
    DWORD tid;
    DWORD delayMs;
};

static BOOL CALLBACK CloseEnumProc(HWND h, LPARAM /*lp*/)
{
    WCHAR cls[64] = {};
    GetClassNameW(h, cls, ARRAYSIZE(cls));
    if (0 == wcscmp(cls, L"#32770"))
    {
        wprintf(L"  closing prop sheet hwnd=%p\n", (void *)h);
        PostMessageW(h, WM_CLOSE, 0, 0);
        return FALSE;
    }
    return TRUE;
}

static DWORD WINAPI CloserProc(LPVOID p)
{
    CloseCtx *c = (CloseCtx *)p;
    Sleep(c->delayMs);
    EnumThreadWindows(c->tid, CloseEnumProc, 0);
    delete c;
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    const wchar_t *target = (argc > 1) ? argv[1] : ERF_CLSID_PATH;
    int seconds = (argc > 2) ? _wtoi(argv[2]) : 8;
    if (seconds < 2) seconds = 2;

    wprintf(L"target = %s\n", target);

    PIDLIST_ABSOLUTE pidl = NULL;
    HRESULT hr = SHParseDisplayName(target, NULL, &pidl, 0, NULL);
    if (FAILED(hr) || !pidl)
    {
        wprintf(L"SHParseDisplayName failed: 0x%08X\n", hr);
        return 2;
    }

    IShellFolder *parent = NULL;
    PCUITEMID_CHILD child = NULL;
    hr = SHBindToParent(pidl, IID_PPV_ARGS(&parent), &child);
    if (FAILED(hr) || !parent)
    {
        wprintf(L"SHBindToParent failed: 0x%08X\n", hr);
        CoTaskMemFree(pidl);
        return 2;
    }

    // 1) 从我们的 IShellFolder 取 IDataObject —— 属性页扩展的 IShellExtInit 要它。
    IDataObject *pdtobj = NULL;
    hr = parent->GetUIObjectOf(NULL, 1, &child, IID_IDataObject, NULL, (void **)&pdtobj);
    if (FAILED(hr) || !pdtobj)
    {
        wprintf(L"GetUIObjectOf(IDataObject) failed: 0x%08X\n", hr);
        parent->Release();
        CoTaskMemFree(pidl);
        return 3;
    }

    // 2) 创建属性页扩展（shell 走的就是这条路：注册表 -> CoCreateInstance）。
    IShellPropSheetExt *pse = NULL;
    hr = CoCreateInstance(CLSID_ErfPropSheet, NULL, CLSCTX_INPROC_SERVER,
                          IID_IShellPropSheetExt, (void **)&pse);
    if (FAILED(hr) || !pse)
    {
        wprintf(L"CoCreateInstance(prop sheet) failed: 0x%08X\n", hr);
        pdtobj->Release();
        parent->Release();
        CoTaskMemFree(pidl);
        return 3;
    }

    // 3) shell 在 AddPages 之前**一定**先调 IShellExtInit::Initialize —— 探针的
    //    CollectSelection 依赖它，跳过这一步就选不中任何东西。
    IShellExtInit *pinit = NULL;
    if (SUCCEEDED(pse->QueryInterface(IID_IShellExtInit, (void **)&pinit)) && pinit)
    {
        hr = pinit->Initialize(NULL, pdtobj, NULL);
        wprintf(L"IShellExtInit::Initialize -> 0x%08X\n", hr);
        pinit->Release();
    }
    else
    {
        wprintf(L"IShellExtInit not available on the prop sheet object\n");
    }

    hr = pse->AddPages(AddPageProc, 0);
    wprintf(L"AddPages -> 0x%08X, pages=%d\n", hr, g_count);
    if (g_count == 0)
    {
        wprintf(L"no pages added (selection is not ours?)\n");
        pse->Release();
        pdtobj->Release();
        parent->Release();
        CoTaskMemFree(pidl);
        return 4;
    }

    PROPSHEETHEADER psh = {};
    psh.dwSize = sizeof(psh);
    psh.dwFlags = PSH_DEFAULT | PSH_NOAPPLYNOW;
    psh.nPages = (UINT)g_count;
    psh.phpage = g_pages;
    psh.pszCaption = L"ERF property sheet (probe)";

    // 自动关闭：属性表跑在**本线程**上，所以只关本线程的窗口，不碰别的。
    CloseCtx *ctx = new CloseCtx{ GetCurrentThreadId(), (DWORD)seconds * 1000 };
    HANDLE h = CreateThread(NULL, 0, CloserProc, ctx, 0, NULL);
    if (h) CloseHandle(h);

    wprintf(L"opening property sheet for %d s ...\n", seconds);
    INT_PTR r = PropertySheet(&psh);
    wprintf(L"PropertySheet returned %d\n", (int)r);

    pse->Release();
    pdtobj->Release();
    parent->Release();
    CoTaskMemFree(pidl);
    CoUninitialize();
    return 0;
}
