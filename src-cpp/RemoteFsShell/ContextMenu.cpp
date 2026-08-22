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
#include <new>  // std::nothrow

#define MENUVERB_OPEN       0
#define MENUVERB_PROPERTIES 1

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

CFolderViewImplContextMenu::CFolderViewImplContextMenu() : _cRef(1), _punkSite(NULL), _pdtobj(NULL), _pidl(NULL), _pidlFolder(NULL), _pDefault(NULL), _cVerbsDefault(0)
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

    PCWSTR pszOwner = (pItem->nOwner < ARRAYSIZE(c_rgOwners)) ? c_rgOwners[pItem->nOwner] : L"?";
    PCWSTR pszGroup = (pItem->nGroup < ARRAYSIZE(c_rgGroups)) ? c_rgGroups[pItem->nGroup] : L"?";

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
            PCWSTR pszOwner = (pItem->nOwner < ARRAYSIZE(c_rgOwners)) ? c_rgOwners[pItem->nOwner] : L"?";
            PCWSTR pszGroup = (pItem->nGroup < ARRAYSIZE(c_rgGroups)) ? c_rgGroups[pItem->nGroup] : L"?";

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

    DebugLog(L"[CM] QueryContextMenu flags=0x%X name=%s folder=%d", uFlags, pItem->szName, pItem->fIsFolder ? 1 : 0);

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
        // Folders: the default menu provides "open" -> navigation works.
        // Files: if the default menu has no verb, expose our Open so
        // double-click reaches InvokeCommand.
        if (cVerbs == 0 && !pItem->fIsFolder)
        {
            return MAKE_HRESULT(SEVERITY_SUCCESS, 0, (USHORT)1);
        }
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, (USHORT)cVerbs);
    }

    UINT idCmd = idCmdFirst + cVerbs;

    // Append our own verb: "Properties" (metadata dialog).
    if (cVerbs > 0)
    {
        InsertMenuW(hmenu, indexMenu++, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    }
    InsertMenuW(hmenu, indexMenu++, MF_BYPOSITION, idCmd, L"Properties");

    return MAKE_HRESULT(SEVERITY_SUCCESS, 0, (USHORT)(cVerbs + 1));
}

HRESULT CFolderViewImplContextMenu::InvokeCommand(LPCMINVOKECOMMANDINFO pici)
{
    if (!pici)
    {
        return E_INVALIDARG;
    }

    UINT_PTR idCmd;
    if (!IS_INTRESOURCE(pici->lpVerb))
    {
        // Verb name given; map "open" -> MENUVERB_OPEN, "properties" -> MENUVERB_PROPERTIES
        if ((pici->fMask & CMIC_MASK_UNICODE) && ((LPCMINVOKECOMMANDINFOEX)pici)->lpVerbW)
        {
            if (0 == StrCmpIW(((LPCMINVOKECOMMANDINFOEX)pici)->lpVerbW, L"properties"))
            {
                idCmd = _cVerbsDefault;   // our Properties slot
            }
            else
            {
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
            }
            else
            {
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

    // Commands owned by the default menu are forwarded to it.
    if (_pDefault && idCmd < _cVerbsDefault)
    {
        return _pDefault->InvokeCommand(pici);
    }

    // Our own commands.
    if (idCmd == _cVerbsDefault || idCmd == MENUVERB_PROPERTIES)
    {
        DebugLog(L"[CM] InvokeCommand PROPERTIES");
        PCFVITEMID pItem = NULL;
        HRESULT hr = _GetSelectedItem(&pItem);
        if (SUCCEEDED(hr))
        {
            INT_PTR ret = DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_PERMBOX),
                            pici->hwnd ? pici->hwnd : _hwnd, PermBoxDlgProc, (LPARAM)pItem);
            DebugLog(L"[CM] PermBox DialogBoxParamW ret=%lld (0=ok,-1=fail)", (long long)ret);
            hr = (ret == -1) ? E_FAIL : S_OK;
        }
        return hr;
    }

    if (idCmd == MENUVERB_OPEN)
    {
        DebugLog(L"[CM] InvokeCommand OPEN");
        return _OpenSelected();
    }

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
                _pidl = ILClone((PCUITEMID_CHILD)((BYTE*)pida + pida->aoffset[1]));
            }
            if (pida)
            {
                GlobalUnlock(medium.hGlobal);
            }
            ReleaseStgMedium(&medium);
        }
        _pdtobj = pdtobj;
        _pdtobj->AddRef();
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
