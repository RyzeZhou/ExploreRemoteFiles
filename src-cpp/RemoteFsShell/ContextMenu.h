/**************************************************************************
    RemoteFsShell - context menu class declaration.
**************************************************************************/
#pragma once

#include <windows.h>
#include <shlobj.h>

class CFolderViewImplContextMenu : public IContextMenu
                                 , public IShellExtInit
                                 , public IObjectWithSite
{
public:
    CFolderViewImplContextMenu();

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void **ppv);
    IFACEMETHODIMP_(ULONG) AddRef();
    IFACEMETHODIMP_(ULONG) Release();

    // IContextMenu
    IFACEMETHODIMP QueryContextMenu(HMENU hmenu, UINT indexMenu, UINT idCmdFirst, UINT idCmdLast, UINT uFlags);
    IFACEMETHODIMP InvokeCommand(LPCMINVOKECOMMANDINFO lpici);
    IFACEMETHODIMP GetCommandString(UINT_PTR idCmd, UINT uType, UINT *pRes, LPSTR pszName, UINT cchMax);

    // IShellExtInit
    IFACEMETHODIMP Initialize(PCIDLIST_ABSOLUTE pidlFolder, IDataObject *pdtobj, HKEY hkeyProgID);

    // IObjectWithSite
    IFACEMETHODIMP SetSite(IUnknown *punkSite);
    IFACEMETHODIMP GetSite(REFIID riid, void **ppvSite);

    // Injected by CFolderViewImplFolder::GetUIObjectOf
    HRESULT Init(HWND hwnd, PCUITEMID_CHILD pidl, PCIDLIST_ABSOLUTE pidlFolder, IContextMenu *pDefaultMenu);

private:
    ~CFolderViewImplContextMenu();

    long    _cRef;
    IDataObject *_pdtobj;
    IUnknown *_punkSite;
    HWND    _hwnd;
    PITEMID_CHILD _pidl;
    PIDLIST_ABSOLUTE _pidlFolder;
    IContextMenu *_pDefault;    // default context menu (SHCreateDefaultContextMenu)
    UINT    _cVerbsDefault;     // verbs added by _pDefault in the last QueryContextMenu

    HRESULT _GetSelectedItem(PCFVITEMID *ppItem);
    HRESULT _OpenSelected();
};

HRESULT CFolderViewImplContextMenu_CreateInstance(REFIID riid, void **ppv);
