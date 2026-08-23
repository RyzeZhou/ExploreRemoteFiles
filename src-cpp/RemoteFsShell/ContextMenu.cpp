/**************************************************************************
    RemoteFsShell - context menu for remote items.
    Data is injected by the shell folder via Init() (GetUIObjectOf path),
    NOT via IShellExtInit (that is only used when the class is instantiated
    through a registered ContextMenuHandlers entry).
    Provides: Open (default verb, also used by double-click) and Properties.
**************************************************************************/

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>
#include "utils.h"
#include "resource.h"
#include "ItemData.h"
#include "ContextMenu.h"
#include "FtpSource.h"
#include <new>  // std::nothrow

// Verbs appended by the ContextMenuHandlers path (offsets from idCmdFirst).
#define MENUVERB_DELETE      0
#define MENUVERB_RENAME      1
#define MENUVERB_NEWFOLDER   2
#define MENUVERB_PROPERTIES  3

HRESULT CFolderViewImplContextMenu_CreateInstance(REFIID riid, void **ppv)
{
    *ppv = NULL;
    CFolderViewImplContextMenu* pMenu = new (std::nothrow) CFolderViewImplContextMenu();
    HRESULT hr = pMenu ? S_OK : E_OUTOFMEMORY;
    if (SUCCEEDED(hr))
    {
        hr = pMenu->QueryInterface(riid, ppv);
        pMenu->Release();
    }
    return hr;
}

CFolderViewImplContextMenu::CFolderViewImplContextMenu() : _cRef(1), _punkSite(NULL), _pdtobj(NULL), _pidl(NULL), _pidlFolder(NULL), _pDefault(NULL), _cVerbsDefault(0), _idCmdFirst(0)
{
    DllAddRef();
}

CFolderViewImplContextMenu::~CFolderViewImplContextMenu()
{
    if (_pdtobj)
    {
        _pdtobj->Release();
    }
    if (_punkSite)
    {
        _punkSite->Release();
    }
    CoTaskMemFree(_pidl);
    CoTaskMemFree(_pidlFolder);
    if (_pDefault)
    {
        _pDefault->Release();
    }
    DllRelease();
}

HRESULT CFolderViewImplContextMenu::QueryInterface(REFIID riid, void **ppv)
{
    static const QITAB qit[] = {
        QITABENT(CFolderViewImplContextMenu, IContextMenu),
        QITABENT(CFolderViewImplContextMenu, IShellExtInit),
        QITABENT(CFolderViewImplContextMenu, IObjectWithSite),
        { 0 },
    };
    return QISearch(this, qit, riid, ppv);
}

ULONG CFolderViewImplContextMenu::AddRef()
{
    return InterlockedIncrement(&_cRef);
}

ULONG CFolderViewImplContextMenu::Release()
{
    long cRef = InterlockedDecrement(&_cRef);
    if (!cRef)
    {
        delete this;
    }
    return cRef;
}

// ---------------------------------------------------------------------------
// Rebuild the remote path ("/www/sub") from an absolute PIDL by walking its
// FVITEMID segments.  Segments with MyObjID==MYOBJID and nLevel>=1 contribute
// a "/name" component.  Itemids are not aligned; names are copied char by char.
// ---------------------------------------------------------------------------
static void CopyItemName(PCFVITEMID item, PWSTR szOut, UINT cch)
{
    UINT i = 0;
    while (i + 1 < cch && item->szName[i])
    {
        szOut[i] = item->szName[i];
        i++;
    }
    szOut[i] = 0;
}

static void PidlToRemotePath(PCIDLIST_ABSOLUTE pidlAbs, PWSTR szOut, UINT cch)
{
    szOut[0] = 0;
    if (!pidlAbs)
    {
        DebugLog(L"[CM] PidlToRemotePath: pidlAbs=NULL -> EMPTY PATH");
        return;
    }
    PCUIDLIST_RELATIVE pidl = (PCUIDLIST_RELATIVE)pidlAbs;
    UINT nSeg = 0, nDump = 0;
    while (pidl && pidl->mkid.cb)
    {
        if (nDump++ < 8)
        {
            PCFVITEMID item = (PCFVITEMID)pidl;
            WCHAR szName[64] = {};
            UINT i = 0;
            while (i < 63 && item->szName[i]) { szName[i] = item->szName[i]; i++; }
            DebugLog(L"[CM] PidlToRemotePath seg#%u cb=%u MyObjID=0x%04X nLevel=%u name='%s'",
                     nDump - 1, pidl->mkid.cb, item->MyObjID, item->nLevel, szName);
        }
        PCFVITEMID item = (PCFVITEMID)pidl;
        if (item->MyObjID == MYOBJID && item->nLevel >= 1)
        {
            WCHAR szName[256];
            CopyItemName(item, szName, ARRAYSIZE(szName));
            if (szName[0] && lstrlen(szOut) + lstrlen(szName) + 2 < cch)
            {
                StringCchCat(szOut, cch, L"/");
                StringCchCat(szOut, cch, szName);
            }
            nSeg++;
        }
        pidl = ILNext(pidl);
    }
    DebugLog(L"[CM] PidlToRemotePath segs=%u -> '%s'", nSeg, szOut);
}

// Dump every segment of a PIDL (any relative/absolute) for diagnosis.
static void DumpPidlSegments(PCUIDLIST_RELATIVE pidl, const wchar_t *label)
{
    UINT i = 0;
    while (pidl && pidl->mkid.cb && i < 16)
    {
        PCFVITEMID item = (PCFVITEMID)pidl;
        WCHAR szName[64] = {};
        for (UINT j = 0; j < 63 && item->szName[j]; j++)
        {
            szName[j] = item->szName[j];
        }
        DebugLog(L"[CM] Dump[%s] seg%u cb=%u MyObjID=0x%04X nLevel=%u name='%s'",
                 label, i, pidl->mkid.cb, item->MyObjID, item->nLevel, szName);
        pidl = ILNext(pidl);
        i++;
    }
}

// Join a remote folder path and an item name without duplicating '/'
// (folder "/" + "big.bin" -> "/big.bin", folder "/www" + "big.bin" -> "/www/big.bin").
static void JoinRemotePath(PCWSTR folder, PCWSTR name, PWSTR out, UINT cch)
{
    if (folder && folder[0] && folder[lstrlen(folder) - 1] == L'/')
    {
        StringCchPrintf(out, cch, L"%s%s", folder, name ? name : L"");
    }
    else
    {
        StringCchPrintf(out, cch, L"%s/%s", folder ? folder : L"", name ? name : L"");
    }
}

// ---------------------------------------------------------------------------
// Show a metadata dialog for a remote item (Phase 1 PoC stand-in for the
// real Property Sheet; Phase 2 replaces with IShellPropSheetExt).
// ---------------------------------------------------------------------------
static void FormatItemInfo(PCFVITEMID pItem, PWSTR pszOut, UINT cchOut)
{
    WCHAR szName[256] = {};
    // itemids are not necessarily aligned; copy char by char
    for (UINT i = 0; i < ARRAYSIZE(szName) - 1 && pItem->szName[i]; i++)
    {
        szName[i] = pItem->szName[i];
    }

    WCHAR szMode[16], szSize[32], szMtime[32];
    FormatMode(pItem->dwMode, pItem->fIsFolder, pItem->fIsSymlink, szMode, ARRAYSIZE(szMode));
    FormatSize(pItem->dwSize, pItem->fIsFolder, szSize, ARRAYSIZE(szSize));
    FormatMtime(pItem->dwMtime, szMtime, ARRAYSIZE(szMtime));

    const wchar_t *pszOwnerT = TableGet(OwnerTables().owners, pItem->nOwner);
    const wchar_t *pszGroupT = TableGet(OwnerTables().groups, pItem->nGroup);
    PCWSTR pszOwner = pszOwnerT[0] ? pszOwnerT : L"?";
    PCWSTR pszGroup = pszGroupT[0] ? pszGroupT : L"?";

    StringCchPrintf(pszOut, cchOut,
        L"Name:        %s\r\n"
        L"Type:        %s\r\n"
        L"Permissions: %s\r\n"
        L"Owner:       %s\r\n"
        L"Group:       %s\r\n"
        L"Size:        %s\r\n"
        L"Modified:    %s\r\n",
        szName,
        pItem->fIsFolder ? L"Folder" : (pItem->fIsSymlink ? L"Symbolic Link" : L"File"),
        szMode, pszOwner, pszGroup, szSize, szMtime);
}

static INT_PTR CALLBACK ItemInfoDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_INITDIALOG:
        if (lParam)
        {
            SetDlgItemTextW(hDlg, IDC_INFO_TEXT, (PCWSTR)lParam);
        }
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void ShowItemInfoDialog(HWND hwndParent, PCFVITEMID pItem)
{
    WCHAR szInfo[512];
    FormatItemInfo(pItem, szInfo, ARRAYSIZE(szInfo));
    DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_ITEMINFO), hwndParent, ItemInfoDlgProc, (LPARAM)szInfo);
}

// Modal permissions dialog (IDD_PERMBOX): metadata + read-only permission
// checkboxes. Phase 2 will make the checkboxes editable (chmod via IPC).
static INT_PTR CALLBACK PermBoxDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_INITDIALOG:
    {
        PCFVITEMID pItem = (PCFVITEMID)lParam;
        if (pItem)
        {
            WCHAR szName[256] = {};
            for (UINT i = 0; i < ARRAYSIZE(szName) - 1 && pItem->szName[i]; i++)
            {
                szName[i] = pItem->szName[i];
            }
            WCHAR szMode[16], szSize[32], szMtime[32];
            FormatMode(pItem->dwMode, pItem->fIsFolder, pItem->fIsSymlink, szMode, ARRAYSIZE(szMode));
            FormatSize(pItem->dwSize, pItem->fIsFolder, szSize, ARRAYSIZE(szSize));
            FormatMtime(pItem->dwMtime, szMtime, ARRAYSIZE(szMtime));
            const wchar_t *pszOwnerT = TableGet(OwnerTables().owners, pItem->nOwner);
            const wchar_t *pszGroupT = TableGet(OwnerTables().groups, pItem->nGroup);
            PCWSTR pszOwner = pszOwnerT[0] ? pszOwnerT : L"?";
            PCWSTR pszGroup = pszGroupT[0] ? pszGroupT : L"?";

            SetDlgItemTextW(hDlg, 3001, szName);
            SetDlgItemTextW(hDlg, 3002, pItem->fIsFolder ? L"Folder" : (pItem->fIsSymlink ? L"Symbolic Link" : L"File"));
            SetDlgItemTextW(hDlg, 3003, szMode);
            SetDlgItemTextW(hDlg, 3004, pszOwner);
            SetDlgItemTextW(hDlg, 3005, pszGroup);
            SetDlgItemTextW(hDlg, 3006, szSize);
            SetDlgItemTextW(hDlg, 3007, szMtime);

            CheckDlgButton(hDlg, 3011, (pItem->dwMode & 0400) ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, 3012, (pItem->dwMode & 0200) ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, 3013, (pItem->dwMode & 0100) ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, 3014, (pItem->dwMode & 0040) ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, 3015, (pItem->dwMode & 0020) ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, 3016, (pItem->dwMode & 0010) ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, 3017, (pItem->dwMode & 0004) ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, 3018, (pItem->dwMode & 0002) ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, 3019, (pItem->dwMode & 0001) ? BST_CHECKED : BST_UNCHECKED);
        }
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return TRUE;
        }
        break;
    }
    return FALSE;
}

struct NameBoxData
{
    PCWSTR title;
    PCWSTR initial;
    PWSTR  out;
    UINT   cchOut;
};

static INT_PTR CALLBACK NameBoxDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_INITDIALOG:
    {
        NameBoxData *pData = (NameBoxData *)lParam;
        if (pData)
        {
            SetWindowTextW(hDlg, pData->title);
            SetDlgItemTextW(hDlg, 3101, pData->initial);
            SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)pData->out);
        }
        SetFocus(GetDlgItem(hDlg, 3101));
        return FALSE;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDOK:
        {
            PWSTR pszOut = (PWSTR)GetWindowLongPtrW(hDlg, DWLP_USER);
            if (pszOut)
            {
                GetDlgItemTextW(hDlg, 3101, pszOut, 256);
            }
            EndDialog(hDlg, 1);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hDlg, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// Modal name input dialog.  Returns TRUE and fills pszOut when OK was pressed.
static BOOL ShowNameBox(HWND hwndParent, PCWSTR pszTitle, PCWSTR pszInitial,
                        PWSTR pszOut, UINT cchOut)
{
    if (!pszOut || cchOut < 2)
    {
        return FALSE;
    }
    pszOut[0] = 0;
    NameBoxData data = { pszTitle, pszInitial, pszOut, cchOut };
    INT_PTR ret = DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_NAMEBOX), hwndParent,
                                  NameBoxDlgProc, (LPARAM)&data);
    return ret == 1;
}

// ---------------------------------------------------------------------------
// Return the selected child PIDL parsed as a valid remote item.
// ---------------------------------------------------------------------------
HRESULT CFolderViewImplContextMenu::_GetSelectedItem(PCFVITEMID *ppItem)
{
    *ppItem = NULL;
    if (!_pidl)
    {
        return E_FAIL;
    }
    return IsValidRemoteItem(_pidl, ppItem) ? S_OK : E_INVALIDARG;
}

// ---------------------------------------------------------------------------
// Get the current folder's FULL absolute PIDL from the shell browser site.
// Explorer's IShellExtInit CIDA only carries {CLSID}+connection segments
// (measured: no www segment), so PidlToRemotePath on it yields an empty
// path.  The site's active view background IPersistFolder2::GetCurFolder
// returns the complete absolute PIDL including all directory segments.
// ---------------------------------------------------------------------------
HRESULT CFolderViewImplContextMenu::_GetFolderPidlFromSite(PIDLIST_ABSOLUTE *ppidl)
{
    *ppidl = NULL;
    if (!_punkSite)
    {
        DebugLog(L"[CM] GetFolderPidlFromSite: no site");
        return E_FAIL;
    }

    // Probe: what interfaces does the site actually support?
    static const struct { const IID *piid; const wchar_t *name; } rgProbe[] =
    {
        { &IID_IShellBrowser,     L"IShellBrowser" },
        { &IID_IServiceProvider,  L"IServiceProvider" },
        { &IID_IOleWindow,        L"IOleWindow" },
        { &IID_IShellView,        L"IShellView" },
    };
    for (int i = 0; i < (int)ARRAYSIZE(rgProbe); i++)
    {
        void *pv = NULL;
        HRESULT hrQ = _punkSite->QueryInterface(*rgProbe[i].piid, &pv);
        DebugLog(L"[CM] GetFolderPidlFromSite: QI %s -> hr=0x%08X", rgProbe[i].name, hrQ);
        if (SUCCEEDED(hrQ) && pv)
        {
            ((IUnknown *)pv)->Release();
        }
    }

    // Try IShellBrowser first.
    IShellBrowser *psb = NULL;
    HRESULT hr = _punkSite->QueryInterface(IID_PPV_ARGS(&psb));
    if (FAILED(hr))
    {
        // Fall back to IServiceProvider -> SID_STopLevelBrowser.
        IServiceProvider *psp = NULL;
        hr = _punkSite->QueryInterface(IID_PPV_ARGS(&psp));
        if (SUCCEEDED(hr))
        {
            static const GUID SID_STopLevelBrowser = { 0x4C96BE40, 0x915C, 0x11CF, { 0x99, 0xD3, 0x00, 0xAA, 0x00, 0x4A, 0xE8, 0x37 } };
            hr = psp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&psb));
            DebugLog(L"[CM] GetFolderPidlFromSite: QueryService(STopLevelBrowser) hr=0x%08X psb=%p", hr, psb);
            psp->Release();
        }
        else
        {
            DebugLog(L"[CM] GetFolderPidlFromSite: site QI IShellBrowser failed hr=0x%08X", hr);
            return hr;
        }
    }
    if (!psb)
    {
        return E_FAIL;
    }
    IShellView *psv = NULL;
    hr = psb->QueryActiveShellView(&psv);
    if (FAILED(hr))
    {
        DebugLog(L"[CM] GetFolderPidlFromSite: QueryActiveShellView failed hr=0x%08X", hr);
        psb->Release();
        return hr;
    }
    IPersistFolder2 *ppf2 = NULL;
    // Preferred: IFolderView::GetFolder (documented way to obtain the current
    // folder object).  Fall back to GetItemObject(SVGIO_BACKGROUND).
    IFolderView *pfv = NULL;
    hr = psv->QueryInterface(IID_PPV_ARGS(&pfv));
    if (SUCCEEDED(hr))
    {
        hr = pfv->GetFolder(IID_PPV_ARGS(&ppf2));
        DebugLog(L"[CM] GetFolderPidlFromSite: IFolderView::GetFolder(IPersistFolder2) hr=0x%08X", hr);
        pfv->Release();
    }
    else
    {
        DebugLog(L"[CM] GetFolderPidlFromSite: view QI IFolderView failed hr=0x%08X (fallback SVGIO)", hr);
        hr = psv->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&ppf2));
        DebugLog(L"[CM] GetFolderPidlFromSite: GetItemObject(IPersistFolder2) hr=0x%08X", hr);
    }
    if (FAILED(hr))
    {
        psv->Release();
        psb->Release();
        return hr;
    }
    hr = ppf2->GetCurFolder(ppidl);
    DebugLog(L"[CM] GetFolderPidlFromSite: GetCurFolder hr=0x%08X pidl=%p", hr, *ppidl);
    if (SUCCEEDED(hr) && *ppidl)
    {
        DumpPidlSegments((PCUIDLIST_RELATIVE)*ppidl, L"site-curfolder");
    }
    ppf2->Release();

    // Comparison probe: GetItemObject(SVGIO_BACKGROUND, IShellFolder) may
    // return OUR folder instance (complete PIDL) instead of explorer's.
    {
        IShellFolder *psfBg = NULL;
        HRESULT hrBg = psv->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&psfBg));
        DebugLog(L"[CM] GetFolderPidlFromSite: [BG] GetItemObject(IShellFolder) hr=0x%08X psf=%p", hrBg, psfBg);
        if (SUCCEEDED(hrBg) && psfBg)
        {
            IPersistFolder2 *ppf2b = NULL;
            if (SUCCEEDED(psfBg->QueryInterface(IID_PPV_ARGS(&ppf2b))))
            {
                PIDLIST_ABSOLUTE pidlB = NULL;
                HRESULT hrC = ppf2b->GetCurFolder(&pidlB);
                DebugLog(L"[CM] GetFolderPidlFromSite: [BG] GetCurFolder hr=0x%08X pidl=%p", hrC, pidlB);
                if (SUCCEEDED(hrC) && pidlB)
                {
                    DumpPidlSegments((PCUIDLIST_RELATIVE)pidlB, L"bg-curfolder");
                }
                CoTaskMemFree(pidlB);
                ppf2b->Release();
            }
            psfBg->Release();
        }
    }

    psv->Release();
    psb->Release();
    return hr;
}

// ---------------------------------------------------------------------------
// Open the selected item: folders navigate in place, files show the
// metadata dialog in Phase 1 (Phase 2: download + default handler).
// ---------------------------------------------------------------------------
HRESULT CFolderViewImplContextMenu::_OpenSelected()
{
    PCFVITEMID pItem = NULL;
    HRESULT hr = _GetSelectedItem(&pItem);
    if (FAILED(hr))
    {
        return hr;
    }

    DebugLog(L"[CM] OpenSelected name=%s folder=%d parent=%p child=%p", pItem->szName, pItem->fIsFolder ? 1 : 0, _pidlFolder, _pidl);

    if (pItem->fIsFolder)
    {
        // Navigate into the folder. Prefer the current shell browser if available,
        // otherwise fall back to SHOpenFolderAndSelectItems.
        PIDLIST_ABSOLUTE pidlFull = ILCombine(_pidlFolder, _pidl);
        hr = pidlFull ? S_OK : E_OUTOFMEMORY;
        if (SUCCEEDED(hr))
        {
            HRESULT hrNav = E_FAIL;
            if (_punkSite)
            {
                IShellBrowser *psb = NULL;
                hrNav = _punkSite->QueryInterface(IID_PPV_ARGS(&psb));
                if (SUCCEEDED(hrNav))
                {
                    hrNav = psb->BrowseObject(pidlFull, SBSP_SAMEBROWSER | SBSP_ABSOLUTE);
                    DebugLog(L"[CM] BrowseObject(ABSOLUTE) hr=0x%08X", hrNav);
                    psb->Release();
                }
                else
                {
                    DebugLog(L"[CM] site QI IShellBrowser failed hr=0x%08X", hrNav);
                }
            }
            else
            {
                DebugLog(L"[CM] no site available, fallback SHOpenFolderAndSelectItems");
            }
            if (FAILED(hrNav))
            {
                hrNav = SHOpenFolderAndSelectItems(pidlFull, 0, NULL, 0);
                DebugLog(L"[CM] SHOpenFolderAndSelectItems hr=0x%08X", hrNav);
            }
            hr = hrNav;
            CoTaskMemFree(pidlFull);
        }
    }
    else
    {
        // Phase 1: show metadata.  Phase 2 will download via IPC and open.
        ShowItemInfoDialog(_hwnd ? _hwnd : GetActiveWindow(), pItem);
        hr = S_OK;
    }
    return hr;
}

HRESULT CFolderViewImplContextMenu::QueryContextMenu(HMENU hmenu, UINT indexMenu, UINT idCmdFirst, UINT idCmdLast, UINT uFlags)
{
    PCFVITEMID pItem = NULL;
    HRESULT hr = _GetSelectedItem(&pItem);
    if (FAILED(hr))
    {
        DebugLog(L"[CM] QueryContextMenu FAILED pidl=%p hr=0x%08X", _pidl, hr);
        return hr;
    }

    _idCmdFirst = idCmdFirst;
    DebugLog(L"[CM] QueryContextMenu flags=0x%X idCmdFirst=%u idCmdLast=%u pDefault=%p name=%s folder=%d",
             uFlags, idCmdFirst, idCmdLast, _pDefault, pItem->szName, pItem->fIsFolder ? 1 : 0);

    UINT cVerbs = 0;

    // Delegate to the default context menu first (provides the standard
    // "Open" navigation behavior that explorer knows how to execute).
    if (_pDefault)
    {
        HRESULT hrDefault = _pDefault->QueryContextMenu(hmenu, indexMenu, idCmdFirst, idCmdLast, uFlags);
        if (SUCCEEDED(hrDefault))
        {
            cVerbs = HRESULT_CODE(hrDefault);
        }
        indexMenu += cVerbs;
    }
    _cVerbsDefault = cVerbs;

    if (uFlags & CMF_DEFAULTONLY)
    {
        // Explorer is only querying the default verb (double-click path).
        // Return 0 always: folder navigation is handled natively by
        // BindToObject (never steal the default verb via the menu handler),
        // and file open is a Phase 2 concern (default handler download).
        DebugLog(L"[CM] QueryContextMenu DEFAULTONLY -> return 0 (cVerbs=%u)", cVerbs);
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, (USHORT)0);
    }

    // Append our own verbs (works both for the delegate path with cVerbs>0
    // and the ContextMenuHandlers path with cVerbs==0).
    UINT idCmd = idCmdFirst + cVerbs;

    if (cVerbs > 0)
    {
        InsertMenuW(hmenu, indexMenu++, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    }
    // Basic file operations (executed via the C# bridge / RemoteFS.Service).
    InsertMenuW(hmenu, indexMenu++, MF_BYPOSITION, idCmd + MENUVERB_DELETE, L"Delete");
    InsertMenuW(hmenu, indexMenu++, MF_BYPOSITION, idCmd + MENUVERB_RENAME, L"Rename");
    InsertMenuW(hmenu, indexMenu++, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    InsertMenuW(hmenu, indexMenu++, MF_BYPOSITION, idCmd + MENUVERB_PROPERTIES, L"Properties");

    DebugLog(L"[CM] QueryContextMenu inserted Delete=%u Rename=%u Properties=%u -> return %u verbs",
             idCmd + MENUVERB_DELETE, idCmd + MENUVERB_RENAME, idCmd + MENUVERB_PROPERTIES, cVerbs + 3);
    return MAKE_HRESULT(SEVERITY_SUCCESS, 0, (USHORT)(cVerbs + 3));
}

HRESULT CFolderViewImplContextMenu::InvokeCommand(LPCMINVOKECOMMANDINFO pici)
{
    if (!pici)
    {
        return E_INVALIDARG;
    }

    BOOL fNamedVerb = !IS_INTRESOURCE(pici->lpVerb);
    UINT_PTR idCmd;
    if (fNamedVerb)
    {
        // Verb name given; map "open" -> MENUVERB_OPEN, "properties" -> MENUVERB_PROPERTIES
        if ((pici->fMask & CMIC_MASK_UNICODE) && ((LPCMINVOKECOMMANDINFOEX)pici)->lpVerbW)
        {
            if (0 == StrCmpIW(((LPCMINVOKECOMMANDINFOEX)pici)->lpVerbW, L"properties"))
            {
                idCmd = _cVerbsDefault;   // our Properties slot (probe: base offset 0)
                DebugLog(L"[CM] InvokeCommand NAMED verb='properties' -> idCmd=_cVerbsDefault=%u", (UINT)idCmd);
            }
            else
            {
                DebugLog(L"[CM] InvokeCommand NAMED verb='%s' (W) -> delegate default",
                         ((LPCMINVOKECOMMANDINFOEX)pici)->lpVerbW);
                // unknown named verb -> delegate (e.g. "open")
                if (_pDefault)
                {
                    return _pDefault->InvokeCommand(pici);
                }
                return E_INVALIDARG;
            }
        }
        else
        {
            if (0 == StrCmpIA(pici->lpVerb, "properties"))
            {
                idCmd = _cVerbsDefault;
                DebugLog(L"[CM] InvokeCommand NAMED verb='properties' (A) -> idCmd=_cVerbsDefault=%u", (UINT)idCmd);
            }
            else
            {
                DebugLog(L"[CM] InvokeCommand NAMED verb='%s' (A) -> delegate default", pici->lpVerb);
                if (_pDefault)
                {
                    return _pDefault->InvokeCommand(pici);
                }
                return E_INVALIDARG;
            }
        }
    }
    else
    {
        idCmd = LOWORD((UINT_PTR)pici->lpVerb);
    }

    // Probe: full command resolution context.
    // Explorer calls InvokeCommand with a RELATIVE offset (0/1/3) on the
    // ContextMenuHandlers path (measured: idCmd=0 for Delete, 1 for Rename
    // while idCmdFirst=31004), and with the ABSOLUTE menu id on other paths.
    // Accept both interpretations.
    UINT uBase = _idCmdFirst + _cVerbsDefault;
    UINT uOffsetAbs = (idCmd >= uBase) ? (UINT)(idCmd - uBase) : 0xFFFFFFFF;
    UINT uOffsetRel = (idCmd <= 3) ? (UINT)idCmd : 0xFFFFFFFF;
    UINT uOffset = (uOffsetAbs != 0xFFFFFFFF) ? uOffsetAbs : uOffsetRel;
    DebugLog(L"[CM] InvokeCommand probe: idCmd=%u _idCmdFirst=%u _cVerbsDefault=%u base=%u abs=0x%X rel=0x%X chosen=0x%X named=%d pDefault=%p pidlFolder=%p",
             (UINT)idCmd, _idCmdFirst, _cVerbsDefault, uBase, uOffsetAbs, uOffsetRel, uOffset,
             fNamedVerb ? 1 : 0, _pDefault, _pidlFolder);

    // Commands owned by the default menu are forwarded to it.  The default
    // menu's verbs occupy [idCmdFirst, idCmdFirst+cVerbsDefault).
    if (_pDefault && idCmd >= _idCmdFirst && idCmd < _idCmdFirst + _cVerbsDefault)
    {
        DebugLog(L"[CM] InvokeCommand idCmd=%u in default range -> forward", (UINT)idCmd);
        return _pDefault->InvokeCommand(pici);
    }

    // Our own commands.
    PCFVITEMID pItem = NULL;
    HRESULT hrSel = _GetSelectedItem(&pItem);
    WCHAR szFolderPath[512], szFullPath[600], szNewName[256];

    // Rebuild the current folder remote path.  Explorer's CIDA only carries
    // {CLSID}+connection segments, so prefer the site's full absolute PIDL;
    // fall back to the recovered _pidlFolder, and finally to the
    // last-enumerated directory path (the enumerator always knows the real
    // path it listed).
    szFolderPath[0] = 0;
    PIDLIST_ABSOLUTE pidlFolderFull = NULL;
    if (SUCCEEDED(_GetFolderPidlFromSite(&pidlFolderFull)) && pidlFolderFull)
    {
        PidlToRemotePath(pidlFolderFull, szFolderPath, ARRAYSIZE(szFolderPath));
        // Replace _pidlFolder with the full PIDL so SHChangeNotify targets
        // the correct directory.
        CoTaskMemFree(_pidlFolder);
        _pidlFolder = ILCloneFull(pidlFolderFull);
        CoTaskMemFree(pidlFolderFull);
    }
    else
    {
        PidlToRemotePath(_pidlFolder, szFolderPath, ARRAYSIZE(szFolderPath));
    }
    if (szFolderPath[0] == 0)
    {
        const wchar_t *pszLast = GetLastEnumPath();
        if (pszLast && pszLast[0])
        {
            StringCchCopy(szFolderPath, ARRAYSIZE(szFolderPath), pszLast);
            DebugLog(L"[CM] InvokeCommand: path recovered from last-enum cache -> '%s'", szFolderPath);
        }
    }
    if (szFolderPath[0] == 0)
    {
        // Connection root: an empty remote path means the FTP root.
        StringCchCopy(szFolderPath, ARRAYSIZE(szFolderPath), L"/");
    }

    if (uOffset == MENUVERB_DELETE)
    {
        DebugLog(L"[CM] InvokeCommand DELETE (offset=%u)", uOffset);
        if (FAILED(hrSel) || !pItem)
        {
            DebugLog(L"[CM] DELETE: hrSel=0x%08X pItem=%p -> E_INVALIDARG", hrSel, pItem);
            return E_INVALIDARG;
        }
        JoinRemotePath(szFolderPath, pItem->szName, szFullPath, ARRAYSIZE(szFullPath));
        DebugLog(L"[CM] DELETE fullPath='%s' (folder='%s' item='%s')", szFullPath, szFolderPath, pItem->szName);
        if (IDYES == MessageBoxW(pici->hwnd ? pici->hwnd : _hwnd,
                                 L"Delete the selected item on the remote server?",
                                 L"RemoteFS", MB_YESNO | MB_ICONWARNING))
        {
            int rc = FsOpDelete(szFullPath);
            DebugLog(L"[CM] FsOpDelete('%s') rc=%d", szFullPath, rc);
            if (rc == 0)
            {
                // Notify with the FULL directory PIDL (recovered via the
                // enumerator cache) so explorer refreshes the actual folder
                // instead of the connection root.
                PCIDLIST_ABSOLUTE pidlNotify = GetLastEnumPidl();
                if (!pidlNotify)
                {
                    pidlNotify = ILCloneFull(_pidlFolder);
                }
                if (pidlNotify)
                {
                    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_IDLIST, pidlNotify, 0);
                    CoTaskMemFree((LPVOID)pidlNotify);
                }
            }
            else
            {
                MessageBoxW(pici->hwnd ? pici->hwnd : _hwnd, L"Delete failed.", L"RemoteFS", MB_OK | MB_ICONERROR);
            }
        }
        return S_OK;
    }

    if (uOffset == MENUVERB_RENAME)
    {
        DebugLog(L"[CM] InvokeCommand RENAME (offset=%u)", uOffset);
        if (FAILED(hrSel) || !pItem)
        {
            DebugLog(L"[CM] RENAME: hrSel=0x%08X pItem=%p -> E_INVALIDARG", hrSel, pItem);
            return E_INVALIDARG;
        }
        JoinRemotePath(szFolderPath, pItem->szName, szFullPath, ARRAYSIZE(szFullPath));
        WCHAR szOldName[256];
        CopyItemName(pItem, szOldName, ARRAYSIZE(szOldName));
        if (NameBoxDlgProc != NULL && ShowNameBox(pici->hwnd ? pici->hwnd : _hwnd,
                                                  L"Rename", szOldName, szNewName, ARRAYSIZE(szNewName)))
        {
            WCHAR szNewPath[600];
            JoinRemotePath(szFolderPath, szNewName, szNewPath, ARRAYSIZE(szNewPath));
            DebugLog(L"[CM] RENAME from='%s' to='%s' (folder='%s')", szFullPath, szNewPath, szFolderPath);
            int rc = FsOpRename(szFullPath, szNewPath);
            DebugLog(L"[CM] FsOpRename('%s' -> '%s') rc=%d", szFullPath, szNewPath, rc);
            if (rc == 0)
            {
                // Notify with the FULL directory PIDL so explorer refreshes
                // the actual folder instead of the connection root.
                PCIDLIST_ABSOLUTE pidlNotify = GetLastEnumPidl();
                if (!pidlNotify)
                {
                    pidlNotify = ILCloneFull(_pidlFolder);
                }
                if (pidlNotify)
                {
                    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_IDLIST, pidlNotify, 0);
                    CoTaskMemFree((LPVOID)pidlNotify);
                }
            }
            else
            {
                MessageBoxW(pici->hwnd ? pici->hwnd : _hwnd, L"Rename failed.", L"RemoteFS", MB_OK | MB_ICONERROR);
            }
        }
        return S_OK;
    }

    if (uOffset == MENUVERB_PROPERTIES || idCmd == MENUVERB_PROPERTIES)
    {
        DebugLog(L"[CM] InvokeCommand PROPERTIES (offset=%u idCmd=%u hrSel=0x%08X)", uOffset, (UINT)idCmd, hrSel);
        if (SUCCEEDED(hrSel))
        {
            INT_PTR ret = DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_PERMBOX),
                            pici->hwnd ? pici->hwnd : _hwnd, PermBoxDlgProc, (LPARAM)pItem);
            DebugLog(L"[CM] PermBox DialogBoxParamW ret=%lld (1=ok,-1=fail)", (long long)ret);
            hrSel = (ret == -1) ? E_FAIL : S_OK;
        }
        return hrSel;
    }

    DebugLog(L"[CM] InvokeCommand unmatched idCmd=%u offset=0x%X -> E_INVALIDARG", (UINT)idCmd, uOffset);

    // MENUVERB_OPEN removed: folder open is native BindToObject navigation,
    // file open is a Phase 2 concern (download + default handler).
    return E_INVALIDARG;
}

HRESULT CFolderViewImplContextMenu::GetCommandString(UINT_PTR idCmd, UINT uType, UINT * /* pRes */, LPSTR pszName, UINT cchMax)
{
    if (_pDefault && idCmd < _cVerbsDefault)
    {
        return _pDefault->GetCommandString(idCmd, uType, NULL, pszName, cchMax);
    }

    HRESULT hr = E_INVALIDARG;
    switch (uType)
    {
    case GCS_VERBA:
        if (idCmd == _cVerbsDefault)
        {
            hr = StringCchCopyA(pszName, cchMax, "properties");
        }
        break;
    case GCS_VERBW:
        if (idCmd == _cVerbsDefault)
        {
            hr = StringCchCopyW((PWSTR)pszName, cchMax, L"properties");
        }
        break;
    }
    return hr;
}

// Only used when instantiated through a registered ContextMenuHandlers entry.
HRESULT CFolderViewImplContextMenu::Initialize(PCIDLIST_ABSOLUTE pidlFolder, IDataObject *pdtobj, HKEY /* hkeyProgID */)
{
    DebugLog(L"[CM] Initialize(IShellExtInit) pidlFolder=%p pdtobj=%p", pidlFolder, pdtobj);
    if (_pdtobj)
    {
        _pdtobj->Release();
        _pdtobj = NULL;
    }
    if (_pidlFolder)
    {
        CoTaskMemFree(_pidlFolder);
        _pidlFolder = NULL;
    }
    if (_pidl)
    {
        CoTaskMemFree(_pidl);
        _pidl = NULL;
    }
    _idCmdFirst = 0;
    _cVerbsDefault = 0;

    if (pidlFolder)
    {
        _pidlFolder = ILCloneFull(pidlFolder);
    }

    // Extract the first selected child PIDL from the data object.
    if (pdtobj)
    {
        FORMATETC fmte = { RegisterClipboardFormatW(CFSTR_SHELLIDLIST), NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM medium = {};
        if (SUCCEEDED(pdtobj->GetData(&fmte, &medium)))
        {
            CIDA *pida = (CIDA*)GlobalLock(medium.hGlobal);
            if (pida && pida->cidl >= 1)
            {
                DebugLog(L"[CM] Initialize: CIDA cidl=%u aoffset[0]=%u aoffset[1]=%u", pida->cidl, pida->aoffset[0], pida->aoffset[1]);
                // Probe: dump the FULL CIDA (parent + every selected child).
                DumpPidlSegments((PCUIDLIST_RELATIVE)((BYTE*)pida + pida->aoffset[0]), L"parent");
                for (UINT ci = 1; ci <= pida->cidl && ci <= 4; ci++)
                {
                    WCHAR szLabel[16];
                    StringCchPrintf(szLabel, ARRAYSIZE(szLabel), L"child%u", ci - 1);
                    DumpPidlSegments((PCUIDLIST_RELATIVE)((BYTE*)pida + pida->aoffset[ci]), szLabel);
                }
                // aoffset[0] is the parent folder PIDL.  Explorer passes
                // pidlFolder=NULL to IShellExtInit::Initialize in Win10/11,
                // so recover the folder from the data object instead.
                if (!_pidlFolder && pida->aoffset[0])
                {
                    _pidlFolder = ILCloneFull((PCIDLIST_ABSOLUTE)((BYTE*)pida + pida->aoffset[0]));
                    DebugLog(L"[CM] Initialize: pidlFolder recovered from CIDA aoffset[0] -> %p", _pidlFolder);
                }
                else
                {
                    DebugLog(L"[CM] Initialize: aoffset[0]=%u (skipped recovery)", pida->aoffset[0]);
                }
                _pidl = ILClone((PCUITEMID_CHILD)((BYTE*)pida + pida->aoffset[1]));
                DebugLog(L"[CM] Initialize: child pidl from aoffset[1] -> %p", _pidl);
            }
            else
            {
                DebugLog(L"[CM] Initialize: CIDA null or cidl=%u", pida ? pida->cidl : 0);
            }
            if (pida)
            {
                GlobalUnlock(medium.hGlobal);
            }
            ReleaseStgMedium(&medium);
        }
        else
        {
            DebugLog(L"[CM] Initialize: pdtobj->GetData(CFSTR_SHELLIDLIST) FAILED");
        }
        _pdtobj = pdtobj;
        _pdtobj->AddRef();
    }
    else
    {
        DebugLog(L"[CM] Initialize: pdtobj=NULL (no data object)");
    }

    // Probe: show what we recovered.
    WCHAR szFolderPath[512];
    PidlToRemotePath(_pidlFolder, szFolderPath, ARRAYSIZE(szFolderPath));
    if (_pidl)
    {
        PCFVITEMID pItem = NULL;
        if (IsValidRemoteItem(_pidl, &pItem))
        {
            DebugLog(L"[CM] Initialize probe: folderPath='%s' item='%s'", szFolderPath, pItem->szName);
        }
        else
        {
            DebugLog(L"[CM] Initialize probe: folderPath='%s' item=INVALID", szFolderPath);
        }
    }
    else
    {
        DebugLog(L"[CM] Initialize probe: folderPath='%s' item=NULL", szFolderPath);
    }
    return S_OK;
}

HRESULT CFolderViewImplContextMenu::Init(HWND hwnd, PCUITEMID_CHILD pidl, PCIDLIST_ABSOLUTE pidlFolder, IContextMenu *pDefaultMenu)
{
    _hwnd = hwnd;
    if (_pidl)
    {
        CoTaskMemFree(_pidl);
    }
    _pidl = pidl ? ILClone(pidl) : NULL;
    if (_pidlFolder)
    {
        CoTaskMemFree(_pidlFolder);
    }
    _pidlFolder = pidlFolder ? ILCloneFull(pidlFolder) : NULL;
    if (_pDefault)
    {
        _pDefault->Release();
        _pDefault = NULL;
    }
    _pDefault = pDefaultMenu;
    if (_pDefault)
    {
        _pDefault->AddRef();
    }
    _idCmdFirst = 0;
    _cVerbsDefault = 0;
    DebugLog(L"[CM] Init(GetUIObjectOf) hwnd=%p pidl=%p pidlFolder=%p pDefault=%p", hwnd, _pidl, _pidlFolder, _pDefault);
    return (_pidl && _pidlFolder) ? S_OK : E_OUTOFMEMORY;
}

HRESULT CFolderViewImplContextMenu::SetSite(IUnknown *punkSite)
{
    DebugLog(L"[CM] SetSite called punkSite=%p", punkSite);
    if (_punkSite)
    {
        _punkSite->Release();
        _punkSite = NULL;
    }

    _punkSite = punkSite;
    if (punkSite)
    {
        punkSite->AddRef();
    }
    return S_OK;
}

HRESULT CFolderViewImplContextMenu::GetSite(REFIID riid, void **ppvSite)
{
    return _punkSite ? _punkSite->QueryInterface(riid, ppvSite) : E_FAIL;
}
