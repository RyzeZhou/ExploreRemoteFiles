/**************************************************************************
    RemoteFsShell - property sheet handler (IShellPropSheetExt).
    Adds a "Permissions" page to the Properties dialog showing the remote
    Unix metadata (mode / owner / group / size / modified).
    Phase 1: read-only. Phase 2: editable (chmod via IPC provider).
**************************************************************************/
#pragma once

#include <windows.h>
#include <shlobj.h>
#include "ItemData.h"

class CPropSheet : public IShellPropSheetExt,
                   public IShellExtInit
{
public:
    CPropSheet();

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void **ppv);
    IFACEMETHODIMP_(ULONG) AddRef();
    IFACEMETHODIMP_(ULONG) Release();

    // IShellPropSheetExt
    IFACEMETHODIMP AddPages(LPFNADDPROPSHEETPAGE pfnAddPage, LPARAM lParam);
    IFACEMETHODIMP ReplacePage(UINT uPageID, LPFNADDPROPSHEETPAGE pfnReplacePage, LPARAM lParam);

    // IShellExtInit
    IFACEMETHODIMP Initialize(PCIDLIST_ABSOLUTE pidlFolder, IDataObject *pdtobj, HKEY hkeyProgID);

private:
    ~CPropSheet();

    long _cRef;
    IDataObject *_pdtobj;
    PCFVITEMID _pItem;   // pointer into _itemData
    BYTE _itemData[sizeof(FVITEMID) + 300]; // FVITEMID + name storage

    HRESULT _ExtractItem();
};

HRESULT CPropSheet_CreateInstance(REFIID riid, void **ppv);
