/**************************************************************************
    RemoteFsShell - property sheet handler implementation.
    Displays remote Unix metadata (permissions / owner / group) in the
    Properties dialog. Phase 1: read-only view.
**************************************************************************/

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <prsht.h>
#include <strsafe.h>
#include "PropSheet.h"
#include "ItemData.h"
#include "resource.h"
#include "Utils.h"
#include <new>

// Property sheet page dialog resource
#define IDD_PERMPAGE   3000
// Checkbox / text control IDs
#define IDC_PP_NAME    3001
#define IDC_PP_TYPE    3002
#define IDC_PP_MODE    3003
#define IDC_PP_OWNER   3004
#define IDC_PP_GROUP   3005
#define IDC_PP_SIZE    3006
#define IDC_PP_MTIME   3007
#define IDC_PP_O_R     3011
#define IDC_PP_O_W     3012
#define IDC_PP_O_X     3013
#define IDC_PP_G_R     3014
#define IDC_PP_G_W     3015
#define IDC_PP_G_X     3016
#define IDC_PP_T_R     3017
#define IDC_PP_T_W     3018
#define IDC_PP_T_X     3019

CPropSheet::CPropSheet() : _cRef(1), _pdtobj(NULL), _pItem(NULL)
{
    ZeroMemory(_itemData, sizeof(_itemData));
    DllAddRef();
}

CPropSheet::~CPropSheet()
{
    if (_pdtobj)
    {
        _pdtobj->Release();
    }
    DllRelease();
}

HRESULT CPropSheet::QueryInterface(REFIID riid, void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CPropSheet, IShellPropSheetExt),
        QITABENT(CPropSheet, IShellExtInit),
        { 0 },
    };
    return QISearch(this, qit, riid, ppv);
}

ULONG CPropSheet::AddRef()
{
    return InterlockedIncrement(&_cRef);
}

ULONG CPropSheet::Release()
{
    long cRef = InterlockedDecrement(&_cRef);
    if (0 == cRef)
    {
        delete this;
    }
    return cRef;
}

HRESULT CPropSheet_CreateInstance(REFIID riid, void **ppv)
{
    *ppv = NULL;
    CPropSheet* pSheet = new (std::nothrow) CPropSheet();
    HRESULT hr = pSheet ? S_OK : E_OUTOFMEMORY;
    if (SUCCEEDED(hr))
    {
        hr = pSheet->QueryInterface(riid, ppv);
        pSheet->Release();
    }
    return hr;
}

// Extract the first selected item from the CFSTR_SHELLIDLIST data object
// into a self-contained copy (PIDL-embedded data + name).
HRESULT CPropSheet::_ExtractItem()
{
    _pItem = NULL;
    if (!_pdtobj)
    {
        return E_FAIL;
    }

    FORMATETC fmte = { RegisterClipboardFormatW(CFSTR_SHELLIDLIST), NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium = {};
    HRESULT hr = _pdtobj->GetData(&fmte, &medium);
    if (SUCCEEDED(hr))
    {
        CIDA *pida = (CIDA*)GlobalLock(medium.hGlobal);
        if (pida && pida->cidl >= 1)
        {
            PCUITEMID_CHILD pidl = (PCUITEMID_CHILD)((BYTE*)pida + pida->aoffset[1]);
            PCFVITEMID pSrc = NULL;
            if (IsValidRemoteItem(pidl, &pSrc))
            {
                // Copy the whole PIDL item including the name string.
                UINT cb = pSrc->cb + sizeof(USHORT);   // item + terminator
                if (cb <= sizeof(_itemData))
                {
                    CopyMemory(_itemData, pSrc, cb);
                    _pItem = (PCFVITEMID)_itemData;
                    hr = S_OK;
                }
                else
                {
                    hr = E_OUTOFMEMORY;
                }
            }
            else
            {
                hr = E_INVALIDARG;
            }
        }
        else
        {
            hr = E_UNEXPECTED;
        }
        if (pida)
        {
            GlobalUnlock(medium.hGlobal);
        }
        ReleaseStgMedium(&medium);
    }
    return hr;
}

HRESULT CPropSheet::Initialize(PCIDLIST_ABSOLUTE /* pidlFolder */, IDataObject *pdtobj, HKEY /* hkeyProgID */)
{
    if (_pdtobj)
    {
        _pdtobj->Release();
        _pdtobj = NULL;
    }
    _pdtobj = pdtobj;
    if (_pdtobj)
    {
        _pdtobj->AddRef();
    }
    return S_OK;
}

// ---------------------------------------------------------------------------
// Permissions page dialog procedure (read-only in Phase 1).
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK PermPageDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_INITDIALOG:
    {
        PCFVITEMID pItem = (PCFVITEMID)((LPPROPSHEETPAGE)lParam)->lParam;
        if (!pItem)
        {
            return TRUE;
        }

        // Name (itemids are not aligned; copy char by char)
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

        SetDlgItemTextW(hDlg, IDC_PP_NAME, szName);
        SetDlgItemTextW(hDlg, IDC_PP_TYPE, pItem->fIsFolder ? L"Folder" : (pItem->fIsSymlink ? L"Symbolic Link" : L"File"));
        SetDlgItemTextW(hDlg, IDC_PP_MODE, szMode);
        SetDlgItemTextW(hDlg, IDC_PP_OWNER, pszOwner);
        SetDlgItemTextW(hDlg, IDC_PP_GROUP, pszGroup);
        SetDlgItemTextW(hDlg, IDC_PP_SIZE, szSize);
        SetDlgItemTextW(hDlg, IDC_PP_MTIME, szMtime);

        // Permissions checkboxes (read-only display)
        BOOL fO[3] = { (pItem->dwMode & 0400) != 0, (pItem->dwMode & 0200) != 0, (pItem->dwMode & 0100) != 0 };
        BOOL fG[3] = { (pItem->dwMode & 0040) != 0, (pItem->dwMode & 0020) != 0, (pItem->dwMode & 0010) != 0 };
        BOOL fT[3] = { (pItem->dwMode & 0004) != 0, (pItem->dwMode & 0002) != 0, (pItem->dwMode & 0001) != 0 };
        CheckDlgButton(hDlg, IDC_PP_O_R, fO[0] ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_PP_O_W, fO[1] ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_PP_O_X, fO[2] ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_PP_G_R, fG[0] ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_PP_G_W, fG[1] ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_PP_G_X, fG[2] ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_PP_T_R, fT[0] ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_PP_T_W, fT[1] ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_PP_T_X, fT[2] ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    }
    }
    return FALSE;
}

HRESULT CPropSheet::AddPages(LPFNADDPROPSHEETPAGE pfnAddPage, LPARAM lParam)
{
    HRESULT hr = _ExtractItem();
    if (FAILED(hr))
    {
        return hr;
    }

    PROPSHEETPAGE psp = {};
    psp.dwSize = sizeof(psp);
    psp.dwFlags = PSP_USECALLBACK | PSP_DLGINDIRECT;
    psp.hInstance = g_hInst;
    psp.pResource = (LPCDLGTEMPLATE)MAKEINTRESOURCE(IDD_PERMPAGE);
    psp.pfnDlgProc = PermPageDlgProc;
    psp.lParam = (LPARAM)_pItem;
    psp.pfnCallback = NULL;   // PSP_USECALLBACK requires a callback; use plain template instead

    // Use a plain dialog template (no PSP_USECALLBACK) to keep it simple.
    psp.dwFlags = PSP_DLGINDIRECT;
    HPROPSHEETPAGE hPage = CreatePropertySheetPage(&psp);
    if (!hPage)
    {
        return E_OUTOFMEMORY;
    }

    hr = pfnAddPage(hPage, lParam) ? S_OK : E_FAIL;
    return hr;
}

HRESULT CPropSheet::ReplacePage(UINT /* uPageID */, LPFNADDPROPSHEETPAGE /* pfnReplacePage */, LPARAM /* lParam */)
{
    return E_NOTIMPL;
}
