/**************************************************************************
    RemoteFsShell - Explorer Remote Filesystem Shell Namespace Extension
    Phase 1 PoC: hardcoded RemoteFS metadata (connections -> dirs -> files)
    with custom columns: Name | Permissions | Owner | Group | Size | Modified

    Based on Microsoft's ExplorerDataProvider sample (MIT-ish sample license).
**************************************************************************/

#include <windows.h>
#include <shlobj.h>
#include <propkey.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <shellapi.h>
#include <time.h>
#include <new>  // std::nothrow

#include "resource.h"
#include "Utils.h"
#include "Guid.h"
#include "fvcommands.h"
#include "ItemData.h"
#include "ContextMenu.h"
#include "FtpSource.h"

const int g_nMaxLevel = 10;  // connection plus up to nine remote directory levels

HRESULT CFolderViewCB_CreateInstance(REFIID riid, void **ppv);
HRESULT CFolderViewImplContextMenu_CreateInstance(REFIID riid, void **ppv);

// ---------------------------------------------------------------------------
// Data source: level 0 = configured connections (hardcoded for Phase 2-A),
// deeper levels = real FTP enumeration via FtpSource (WinINet).
// ---------------------------------------------------------------------------
// level 0: configured connections
static const ITEMDATA c_rgConnections[] =
{
    { 0, 0755, 0, 0, TRUE, FALSE, L"ftpuser", L"ftp", L"local-ftp" },
    { 0, 0755, 0, 0, TRUE, FALSE, L"control", L"control", L"static-control" },
};

static int CopyStaticItems(const ITEMDATA *items, int count, ITEMDATA *out, int maxItems)
{
    int n = min(count, maxItems);
    for (int i = 0; i < n; i++) out[i] = items[i];
    return n;
}

// Controlled in-memory hierarchy. It uses the exact same PIDL, BindToObject,
// IShellView and persistence implementation as local-ftp, but never invokes
// the FTP bridge or its cache.
static int GetStaticItems(PCWSTR pszPath, ITEMDATA *out, int maxItems)
{
    static const ITEMDATA root[] =
    {
        { 0, 0755, 0, 0, TRUE, FALSE, L"control", L"control", L"alpha" },
        { 0, 0755, 0, 0, TRUE, FALSE, L"control", L"control", L"beta" },
        { 0, 0644, 32, 0, FALSE, FALSE, L"control", L"control", L"root-file.txt" },
    };
    static const ITEMDATA alpha[] =
    {
        { 0, 0755, 0, 0, TRUE, FALSE, L"control", L"control", L"nested" },
        { 0, 0644, 32, 0, FALSE, FALSE, L"control", L"control", L"inside-alpha.txt" },
    };
    static const ITEMDATA nested[] =
    {
        { 0, 0644, 32, 0, FALSE, FALSE, L"control", L"control", L"deep-static.txt" },
    };
    static const ITEMDATA beta[] =
    {
        { 0, 0644, 32, 0, FALSE, FALSE, L"control", L"control", L"inside-beta.txt" },
    };
    if (0 == StrCmp(pszPath, L"@static")) return CopyStaticItems(root, ARRAYSIZE(root), out, maxItems);
    if (0 == StrCmp(pszPath, L"@static/alpha")) return CopyStaticItems(alpha, ARRAYSIZE(alpha), out, maxItems);
    if (0 == StrCmp(pszPath, L"@static/alpha/nested")) return CopyStaticItems(nested, ARRAYSIZE(nested), out, maxItems);
    if (0 == StrCmp(pszPath, L"@static/beta")) return CopyStaticItems(beta, ARRAYSIZE(beta), out, maxItems);
    return 0;
}

// Enumerate items for a folder level. Level 0 returns connections; @static
// paths use controlled memory data; all other levels use the real FTP bridge.
static int GetLevelItems(int nLevel, PCWSTR pszPath, ITEMDATA *out, int maxItems)
{
    if (nLevel == 0)
    {
        return CopyStaticItems(c_rgConnections, ARRAYSIZE(c_rgConnections), out, maxItems);
    }
    if (pszPath && 0 == StrCmpN(pszPath, L"@static", 7))
    {
        return GetStaticItems(pszPath, out, maxItems);
    }
    return FtpListDirectory(pszPath, out, maxItems);
}

// Probe helper: rebuild the remote path from an absolute PIDL (mirror of the
// static PidlToRemotePath in ContextMenu.cpp; kept local to avoid a header
// dependency).
static void RemotePathFromPidl(PCIDLIST_ABSOLUTE pidlAbs, PWSTR szOut, UINT cch)
{
    szOut[0] = 0;
    PCUIDLIST_RELATIVE pidl = (PCUIDLIST_RELATIVE)pidlAbs;
    while (pidl && pidl->mkid.cb)
    {
        PCFVITEMID item = (PCFVITEMID)pidl;
        if (item->MyObjID == MYOBJID && item->nLevel >= 1)
        {
            WCHAR szName[256] = {};
            for (UINT i = 0; i < 255 && item->szName[i]; i++)
            {
                szName[i] = item->szName[i];
            }
            if (szName[0] && lstrlen(szOut) + lstrlen(szName) + 2 < cch)
            {
                StringCchCat(szOut, cch, L"/");
                StringCchCat(szOut, cch, szName);
            }
        }
        pidl = ILNext(pidl);
    }
}

// ---------------------------------------------------------------------------
// The shell folder implementation
// ---------------------------------------------------------------------------
class CFolderViewImplFolder : public IShellFolder2,
                              public IPersistFolder2
{
public:
    CFolderViewImplFolder(UINT nLevel, PCWSTR pszRemotePath);

    // IUnknown methods
    IFACEMETHODIMP QueryInterface(REFIID riid, void **ppv);
    IFACEMETHODIMP_(ULONG) AddRef();
    IFACEMETHODIMP_(ULONG) Release();

    // IShellFolder
    IFACEMETHODIMP ParseDisplayName(HWND hwnd, IBindCtx *pbc, PWSTR pszName,
                                    ULONG *pchEaten, PIDLIST_RELATIVE *ppidl, ULONG *pdwAttributes);
    IFACEMETHODIMP EnumObjects(HWND hwnd, DWORD grfFlags, IEnumIDList **ppenumIDList);
    IFACEMETHODIMP BindToObject(PCUIDLIST_RELATIVE pidl, IBindCtx *pbc, REFIID riid, void **ppv);
    IFACEMETHODIMP BindToStorage(PCUIDLIST_RELATIVE pidl, IBindCtx *pbc, REFIID riid, void **ppv);
    IFACEMETHODIMP CompareIDs(LPARAM lParam, PCUIDLIST_RELATIVE pidl1, PCUIDLIST_RELATIVE pidl2);
    IFACEMETHODIMP CreateViewObject(HWND hwnd, REFIID riid, void **ppv);
    IFACEMETHODIMP GetAttributesOf(UINT cidl, PCUITEMID_CHILD_ARRAY apidl, ULONG *rgfInOut);
    IFACEMETHODIMP GetUIObjectOf(HWND hwnd, UINT cidl, PCUITEMID_CHILD_ARRAY apidl,
                                 REFIID riid, UINT* prgfInOut, void **ppv);
    IFACEMETHODIMP GetDisplayNameOf(PCUITEMID_CHILD pidl, SHGDNF shgdnFlags, STRRET *pName);
    IFACEMETHODIMP SetNameOf(HWND hwnd, PCUITEMID_CHILD pidl, PCWSTR pszName, DWORD uFlags, PITEMID_CHILD * ppidlOut);

    // IShellFolder2
    IFACEMETHODIMP GetDefaultSearchGUID(GUID *pGuid);
    IFACEMETHODIMP EnumSearches(IEnumExtraSearch **ppenum);
    IFACEMETHODIMP GetDefaultColumn(DWORD dwRes, ULONG *pSort, ULONG *pDisplay);
    IFACEMETHODIMP GetDefaultColumnState(UINT iColumn, SHCOLSTATEF *pbState);
    IFACEMETHODIMP GetDetailsEx(PCUITEMID_CHILD pidl, const PROPERTYKEY *pkey, VARIANT *pv);
    IFACEMETHODIMP GetDetailsOf(PCUITEMID_CHILD pidl, UINT iColumn, SHELLDETAILS *pDetails);
    IFACEMETHODIMP MapColumnToSCID(UINT iColumn, PROPERTYKEY *pkey);

    // IPersist
    IFACEMETHODIMP GetClassID(CLSID *pClassID);

    // IPersistFolder
    IFACEMETHODIMP Initialize(PCIDLIST_ABSOLUTE pidl);

    // IPersistFolder2
    IFACEMETHODIMP GetCurFolder(PIDLIST_ABSOLUTE *ppidl);

    // IDList constructor public for the enumerator object
    HRESULT CreateChildID(PCWSTR pszName, int nLevel, DWORD dwMode, DWORD dwSize, DWORD dwMtime,
                          PCWSTR pszOwner, PCWSTR pszGroup, BOOL fIsFolder, BOOL fIsSymlink, PITEMID_CHILD *ppidl);

    // Read-only accessor for the enumerator (RememberEnumPath needs the full
    // absolute PIDL of the folder being listed).
    PCIDLIST_ABSOLUTE GetPidl() const { return m_pidl; }

private:
    ~CFolderViewImplFolder();

    HRESULT _GetName(PCUIDLIST_RELATIVE pidl, PWSTR pszName, int cchMax);
    HRESULT _GetName(PCUIDLIST_RELATIVE pidl, PWSTR *pszName);
    HRESULT _GetLevel(PCUIDLIST_RELATIVE pidl, int* pLevel);
    HRESULT _GetMode(PCUIDLIST_RELATIVE pidl, DWORD* pdwMode);
    HRESULT _GetSize(PCUIDLIST_RELATIVE pidl, DWORD* pdwSize);
    HRESULT _GetMtime(PCUIDLIST_RELATIVE pidl, DWORD* pdwMtime);
    HRESULT _GetOwner(PCUIDLIST_RELATIVE pidl, PWSTR pszOwner, int cch);
    HRESULT _GetGroup(PCUIDLIST_RELATIVE pidl, PWSTR pszGroup, int cch);
    HRESULT _GetFolderness(PCUIDLIST_RELATIVE pidl, BOOL* pfIsFolder);
    HRESULT _GetSymlink(PCUIDLIST_RELATIVE pidl, BOOL* pfIsSymlink);
    HRESULT _ValidatePidl(PCUIDLIST_RELATIVE pidl);
    PCFVITEMID _IsValid(PCUIDLIST_RELATIVE pidl);

    HRESULT _GetColumnDisplayName(PCUITEMID_CHILD pidl, const PROPERTYKEY* pkey, VARIANT* pv, PWSTR pszRet, UINT cch);

    long                m_cRef;
    int                 m_nLevel;
    WCHAR               m_szRemotePath[512];   // remote path of this folder
    PIDLIST_ABSOLUTE    m_pidl;             // where this folder is in the name space
};

class CFolderViewImplEnumIDList : public IEnumIDList
{
public:
    CFolderViewImplEnumIDList(DWORD grfFlags, int nCurrent, PCWSTR pszPath, CFolderViewImplFolder *pFolderViewImplShellFolder);

    // IUnknown methods
    IFACEMETHODIMP QueryInterface(REFIID riid, void **ppv);
    IFACEMETHODIMP_(ULONG) AddRef();
    IFACEMETHODIMP_(ULONG) Release();

    // IEnumIDList
    IFACEMETHODIMP Next(ULONG celt, PITEMID_CHILD *rgelt, ULONG *pceltFetched);
    IFACEMETHODIMP Skip(DWORD celt);
    IFACEMETHODIMP Reset();
    IFACEMETHODIMP Clone(IEnumIDList **ppenum);

    HRESULT Initialize();

private:
    ~CFolderViewImplEnumIDList();

    long m_cRef;
    DWORD m_grfFlags;
    int m_nItem;
    int m_nLevel;
    WCHAR m_szPath[512];
    ITEMDATA m_aData[MAX_OBJS];

    CFolderViewImplFolder *m_pFolder;
};

// Minimal ICategoryProvider: declares support (so explorer does not fall back
// during view creation) but provides no grouping categories.
class CFolderViewImplCategoryProvider : public ICategoryProvider
{
public:
    CFolderViewImplCategoryProvider() : m_cRef(1) { DllAddRef(); }
    IFACEMETHODIMP QueryInterface(REFIID riid, void **ppv)
    {
        *ppv = NULL;
        if (riid == IID_IUnknown || riid == IID_ICategoryProvider)
        {
            *ppv = static_cast<ICategoryProvider *>(this);
        }
        else
        {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_cRef); }
    IFACEMETHODIMP_(ULONG) Release()
    {
        long cRef = InterlockedDecrement(&m_cRef);
        if (!cRef) { delete this; }
        return cRef;
    }
    IFACEMETHODIMP CanCategorizeOnSCID(const PROPERTYKEY *) { return S_FALSE; }
    IFACEMETHODIMP GetDefaultCategory(GUID *pguid, PROPERTYKEY *pkey) { return E_NOTIMPL; }
    IFACEMETHODIMP GetCategoryForSCID(const PROPERTYKEY *pkey, GUID *pguid) { return E_NOTIMPL; }
    IFACEMETHODIMP EnumCategories(IEnumGUID **ppenum) { *ppenum = NULL; return E_NOTIMPL; }
    IFACEMETHODIMP GetCategoryName(const GUID *pguid, PWSTR pszName, UINT cch) { return E_NOTIMPL; }
    IFACEMETHODIMP CreateCategory(const GUID *pguid, REFIID riid, void **ppv) { *ppv = NULL; return E_NOTIMPL; }
private:
    ~CFolderViewImplCategoryProvider() { DllRelease(); }
    long m_cRef;
};

HRESULT CFolderViewImplFolder_CreateInstance(REFIID riid, void **ppv)
{
    *ppv = NULL;
    CFolderViewImplFolder* pFolderViewImplShellFolder = new (std::nothrow) CFolderViewImplFolder(0, L"");
    HRESULT hr = pFolderViewImplShellFolder ? S_OK : E_OUTOFMEMORY;
    if (SUCCEEDED(hr))
    {
        hr = pFolderViewImplShellFolder->QueryInterface(riid, ppv);
        pFolderViewImplShellFolder->Release();
    }
    return hr;
}

CFolderViewImplFolder::CFolderViewImplFolder(UINT nLevel, PCWSTR pszRemotePath) : m_cRef(1), m_nLevel(nLevel), m_pidl(NULL)
{
    DllAddRef();
    if (pszRemotePath)
    {
        StringCchCopy(m_szRemotePath, ARRAYSIZE(m_szRemotePath), pszRemotePath);
    }
    else
    {
        m_szRemotePath[0] = L'\0';
    }
}

CFolderViewImplFolder::~CFolderViewImplFolder()
{
    CoTaskMemFree(m_pidl);
    DllRelease();
}

HRESULT CFolderViewImplFolder::QueryInterface(REFIID riid, void **ppv)
{
    *ppv = NULL;
    if (riid == IID_IUnknown || riid == IID_IShellFolder)
    {
        *ppv = static_cast<IShellFolder *>(this);
    }
    else if (riid == IID_IShellFolder2)
    {
        *ppv = static_cast<IShellFolder2 *>(this);
    }
    else if (riid == IID_IPersist)
    {
        *ppv = static_cast<IPersist *>(static_cast<IPersistFolder2 *>(this));
    }
    else if (riid == IID_IPersistFolder)
    {
        *ppv = static_cast<IPersistFolder *>(this);
    }
    else if (riid == IID_IPersistFolder2)
    {
        *ppv = static_cast<IPersistFolder2 *>(this);
    }
    else
    {
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

ULONG CFolderViewImplFolder::AddRef()
{
    return InterlockedIncrement(&m_cRef);
}

ULONG CFolderViewImplFolder::Release()
{
    long cRef = InterlockedDecrement(&m_cRef);
    if (0 == cRef)
    {
        delete this;
    }
    return cRef;
}

//  Translates a display name into an item identifier list.
HRESULT CFolderViewImplFolder::ParseDisplayName(HWND hwnd, IBindCtx *pbc, PWSTR pszName,
                                                ULONG *pchEaten, PIDLIST_RELATIVE *ppidl, ULONG *pdwAttributes)
{
    HRESULT hr = E_INVALIDARG;

    if (NULL != pszName)
    {
        WCHAR szNameComponent[MAX_PATH] = {};

        PWSTR pszNext = PathFindNextComponent(pszName);
        if (pszNext && *pszNext)
        {
            hr = StringCchCopyN(szNameComponent, ARRAYSIZE(szNameComponent), pszName, lstrlen(pszName) - lstrlen(pszNext));
        }
        else
        {
            hr = StringCchCopy(szNameComponent, ARRAYSIZE(szNameComponent), pszName);
        }

        if (SUCCEEDED(hr))
        {
            PathRemoveBackslash(szNameComponent);

            ITEMDATA items[MAX_OBJS];
            int n = GetLevelItems(m_nLevel, m_szRemotePath, items, MAX_OBJS);
            BOOL fFound = FALSE;
            for (int i = 0; i < n; i++)
            {
                if (0 == StrCmp(items[i].szName, szNameComponent))
                {
                    PIDLIST_RELATIVE pidlCurrent = NULL;
                    hr = CreateChildID(items[i].szName, m_nLevel, items[i].dwMode, items[i].dwSize,
                                       items[i].dwMtime, items[i].szOwner, items[i].szGroup,
                                       items[i].fIsFolder, items[i].fIsSymlink, &pidlCurrent);
                    if (SUCCEEDED(hr))
                    {
                        if (pszNext && *pszNext)
                        {
                            IShellFolder *psf;
                            hr = BindToObject(pidlCurrent, pbc, IID_PPV_ARGS(&psf));
                            if (SUCCEEDED(hr))
                            {
                                PIDLIST_RELATIVE pidlNext = NULL;
                                hr = psf->ParseDisplayName(hwnd, pbc, pszNext, pchEaten, &pidlNext, pdwAttributes);
                                if (SUCCEEDED(hr))
                                {
                                    *ppidl = ILCombine(pidlCurrent, pidlNext);
                                    CoTaskMemFree(pidlNext);
                                }
                                psf->Release();
                            }
                            CoTaskMemFree(pidlCurrent);
                        }
                        else
                        {
                            *ppidl = pidlCurrent;
                        }
                    }
                    fFound = TRUE;
                    break;
                }
            }

            if (!fFound)
            {
                hr = E_FAIL;
            }
        }
    }
    return hr;
}

HRESULT CFolderViewImplFolder::EnumObjects(HWND hwnd, DWORD grfFlags, IEnumIDList **ppenumIDList)
{
    DebugLog(L"[SF] EnumObjects level=%d flags=0x%X hwnd=%p", m_nLevel, grfFlags, hwnd);
    HRESULT hr;
    if (m_nLevel >= g_nMaxLevel)
    {
        *ppenumIDList = NULL;
        hr = S_FALSE; // S_FALSE is allowed with NULL out param to indicate no contents.
    }
    else
    {
        // Match Microsoft's ExplorerDataProvider contract: each enumerated
        // child carries its own level, one greater than the parent folder.
        CFolderViewImplEnumIDList *penum = new (std::nothrow) CFolderViewImplEnumIDList(grfFlags, m_nLevel + 1, m_szRemotePath, this);
        hr = penum ? S_OK : E_OUTOFMEMORY;
        if (SUCCEEDED(hr))
        {
            hr = penum->Initialize();
            if (SUCCEEDED(hr))
            {
                hr = penum->QueryInterface(IID_PPV_ARGS(ppenumIDList));
            }
            penum->Release();
        }
    }

    return hr;
}

//  Factory for handlers for the specified item.
HRESULT CFolderViewImplFolder::BindToObject(PCUIDLIST_RELATIVE pidl,
                                            IBindCtx *pbc, REFIID riid, void **ppv)
{
    *ppv = NULL;
    HRESULT hr = _ValidatePidl(pidl);
    if (SUCCEEDED(hr))
    {
        WCHAR szName[256];
        hr = _GetName(pidl, szName, ARRAYSIZE(szName));
        if (SUCCEEDED(hr))
        {
            WCHAR szChildPath[512];
            if (m_szRemotePath[0] == 0)
            {
                StringCchCopy(szChildPath, ARRAYSIZE(szChildPath),
                              0 == StrCmp(szName, L"static-control") ? L"@static" : L"/");
            }
            else if (m_szRemotePath[0] == L'/' && m_szRemotePath[1] == 0)
            {
                StringCchPrintf(szChildPath, ARRAYSIZE(szChildPath), L"/%s", szName);
            }
            else if (0 == StrCmpN(m_szRemotePath, L"@static", 7))
            {
                StringCchPrintf(szChildPath, ARRAYSIZE(szChildPath), L"%s/%s", m_szRemotePath, szName);
            }
            else
            {
                StringCchPrintf(szChildPath, ARRAYSIZE(szChildPath), L"%s/%s", m_szRemotePath, szName);
            }
            DebugLog(L"[SF] BindToObject sample-aligned folderLevel=%d name='%s' childPath='%s' m_pidl=%p",
                     m_nLevel, szName, szChildPath, m_pidl);

            // Match Microsoft's sample exactly: child folder depth follows the
            // parent object, never metadata recovered from a child PIDL.
            CFolderViewImplFolder *pFolder = new (std::nothrow) CFolderViewImplFolder(m_nLevel + 1, szChildPath);
            hr = pFolder ? S_OK : E_OUTOFMEMORY;
            if (SUCCEEDED(hr))
            {
                PITEMID_CHILD pidlFirst = ILCloneFirst(pidl);
                hr = pidlFirst ? S_OK : E_OUTOFMEMORY;
                if (SUCCEEDED(hr))
                {
                    PIDLIST_ABSOLUTE pidlBind = ILCombine(m_pidl, pidlFirst);
                    hr = pidlBind ? S_OK : E_OUTOFMEMORY;
                    if (SUCCEEDED(hr))
                    {
                        hr = pFolder->Initialize(pidlBind);
                        if (SUCCEEDED(hr))
                        {
                            PCUIDLIST_RELATIVE pidlNext = ILNext(pidl);
                            hr = ILIsEmpty(pidlNext)
                                ? pFolder->QueryInterface(riid, ppv)
                                : pFolder->BindToObject(pidlNext, pbc, riid, ppv);
                        }
                        CoTaskMemFree(pidlBind);
                    }
                    ILFree(pidlFirst);
                }
                pFolder->Release();
            }
        }
    }
    return hr;
}

HRESULT CFolderViewImplFolder::BindToStorage(PCUIDLIST_RELATIVE pidl,
                                             IBindCtx *pbc, REFIID riid, void **ppv)
{
    return BindToObject(pidl, pbc, riid, ppv);
}

//  Helper function to help compare relative IDs.
HRESULT _ILCompareRelIDs(IShellFolder *psfParent, PCUIDLIST_RELATIVE pidl1, PCUIDLIST_RELATIVE pidl2,
                         LPARAM lParam)
{
    HRESULT hr;
    PCUIDLIST_RELATIVE pidlRel1 = ILNext(pidl1);
    PCUIDLIST_RELATIVE pidlRel2 = ILNext(pidl2);

    if (ILIsEmpty(pidlRel1))
    {
        if (ILIsEmpty(pidlRel2))
        {
            hr = ResultFromShort(0);  // Both empty
        }
        else
        {
            hr = ResultFromShort(-1);   // 1 is empty, 2 is not.
        }
    }
    else
    {
        if (ILIsEmpty(pidlRel2))
        {
            hr = ResultFromShort(1);  // 2 is empty, 1 is not
        }
        else
        {
            PIDLIST_RELATIVE pidlNext = ILCloneFirst(pidl1);    // pidl2 would work as well
            hr = pidlNext ? S_OK : E_OUTOFMEMORY;
            if (pidlNext)
            {
                IShellFolder *psfNext;
                hr = psfParent->BindToObject(pidlNext, NULL, IID_PPV_ARGS(&psfNext));
                if (SUCCEEDED(hr))
                {
                    IShellFolder2 *psf2;
                    if (SUCCEEDED(psfNext->QueryInterface(&psf2)))
                    {
                        psf2->Release();  // We can use the lParam
                    }
                    else
                    {
                        lParam = 0;       // We can't use the lParam
                    }

                    hr = psfNext->CompareIDs((lParam & ~SHCIDS_COLUMNMASK), pidlRel1, pidlRel2);
                    psfNext->Release();
                }
                CoTaskMemFree(pidlNext);
            }
        }
    }
    return hr;
}

//  Called to determine the equivalence and/or sort order of two idlists.
HRESULT CFolderViewImplFolder::CompareIDs(LPARAM lParam, PCUIDLIST_RELATIVE pidl1, PCUIDLIST_RELATIVE pidl2)
{
    HRESULT hr;

    if (lParam & (SHCIDS_CANONICALONLY | SHCIDS_ALLFIELDS))
    {
        // First do a "canonical" comparison: compare item identity by name.
        PWSTR psz1 = NULL, psz2 = NULL;
        hr = _GetName(pidl1, &psz1);
        if (SUCCEEDED(hr))
        {
            hr = _GetName(pidl2, &psz2);
            if (SUCCEEDED(hr))
            {
                hr = ResultFromShort(StrCmp(psz1, psz2));
                CoTaskMemFree(psz2);
            }
            CoTaskMemFree(psz1);
        }

        if ((ResultFromShort(0) == hr) && (lParam & SHCIDS_ALLFIELDS))
        {
            DWORD dwM1 = 0, dwM2 = 0;
            hr = _GetMtime(pidl1, &dwM1);
            if (SUCCEEDED(hr))
            {
                hr = _GetMtime(pidl2, &dwM2);
                if (SUCCEEDED(hr))
                {
                    hr = ResultFromShort((dwM1 > dwM2) ? 1 : (dwM1 < dwM2) ? -1 : 0);
                }
            }
        }
    }
    else
    {
        // Compare child ids by column data (lParam & SHCIDS_COLUMNMASK).
        int nResult = 0;
        switch (lParam & SHCIDS_COLUMNMASK)
        {
            case 0: // Column 0: Name
            {
                PWSTR psz1 = NULL, psz2 = NULL;
                hr = _GetName(pidl1, &psz1);
                if (SUCCEEDED(hr))
                {
                    hr = _GetName(pidl2, &psz2);
                    if (SUCCEEDED(hr))
                    {
                        nResult = StrCmp(psz1, psz2);
                        CoTaskMemFree(psz2);
                    }
                    CoTaskMemFree(psz1);
                }
                break;
            }
            case 1: // Column 1: Permissions (by mode bits)
            {
                DWORD dwM1 = 0, dwM2 = 0;
                hr = _GetMode(pidl1, &dwM1);
                if (SUCCEEDED(hr))
                {
                    hr = _GetMode(pidl2, &dwM2);
                    if (SUCCEEDED(hr))
                    {
                        nResult = (dwM1 > dwM2) ? 1 : (dwM1 < dwM2) ? -1 : 0;
                    }
                }
                break;
            }
            case 2: // Column 2: Owner (string compare)
            {
                WCHAR szO1[33], szO2[33];
                hr = _GetOwner(pidl1, szO1, ARRAYSIZE(szO1));
                if (SUCCEEDED(hr))
                {
                    hr = _GetOwner(pidl2, szO2, ARRAYSIZE(szO2));
                    if (SUCCEEDED(hr))
                    {
                        nResult = StrCmp(szO1, szO2);
                    }
                }
                break;
            }
            case 3: // Column 3: Group (string compare)
            {
                WCHAR szG1[33], szG2[33];
                hr = _GetGroup(pidl1, szG1, ARRAYSIZE(szG1));
                if (SUCCEEDED(hr))
                {
                    hr = _GetGroup(pidl2, szG2, ARRAYSIZE(szG2));
                    if (SUCCEEDED(hr))
                    {
                        nResult = StrCmp(szG1, szG2);
                    }
                }
                break;
            }
            case 4: // Column 4: Size (numeric)
            {
                DWORD dwS1 = 0, dwS2 = 0;
                hr = _GetSize(pidl1, &dwS1);
                if (SUCCEEDED(hr))
                {
                    hr = _GetSize(pidl2, &dwS2);
                    if (SUCCEEDED(hr))
                    {
                        nResult = (dwS1 > dwS2) ? 1 : (dwS1 < dwS2) ? -1 : 0;
                    }
                }
                break;
            }
            case 5: // Column 5: Modified (numeric)
            {
                DWORD dwT1 = 0, dwT2 = 0;
                hr = _GetMtime(pidl1, &dwT1);
                if (SUCCEEDED(hr))
                {
                    hr = _GetMtime(pidl2, &dwT2);
                    if (SUCCEEDED(hr))
                    {
                        nResult = (dwT1 > dwT2) ? 1 : (dwT1 < dwT2) ? -1 : 0;
                    }
                }
                break;
            }
            default:
                hr = S_OK;
                break;
        }

        if (SUCCEEDED(hr))
        {
            hr = ResultFromShort(nResult);
        }

        if ((ResultFromShort(0) == hr) && SUCCEEDED(hr))
        {
            // Continue on by binding to the next level to keep the sort stable.
            hr = _ILCompareRelIDs(this, pidl1, pidl2, lParam);
        }
    }
    return hr;
}

//  Called by the Shell to create the View Object and return it.
HRESULT CFolderViewImplFolder::CreateViewObject(HWND hwnd, REFIID riid, void **ppv)
{
    *ppv = NULL;
    DebugLog(L"[SF] CreateViewObject riid=%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X level=%d",
             riid.Data1, riid.Data2, riid.Data3, riid.Data4[0], riid.Data4[1],
             riid.Data4[2], riid.Data4[3], riid.Data4[4], riid.Data4[5],
             riid.Data4[6], riid.Data4[7], m_nLevel);

    HRESULT hr = E_NOINTERFACE;
    if (riid == IID_IShellView)
    {
        // Probe: which instance does explorer create the view on?  Dump the
        // instance's PIDL to see whether the www segment is present.
        WCHAR szPath[512];
        RemotePathFromPidl(m_pidl, szPath, ARRAYSIZE(szPath));
        DebugLog(L"[SF]   CreateViewObject(IShellView) instance level=%d path='%s' m_pidl=%p rebuilt='%s'",
                 m_nLevel, m_szRemotePath, m_pidl, szPath);
        {
            UINT i = 0;
            PCUIDLIST_RELATIVE p = (PCUIDLIST_RELATIVE)m_pidl;
            while (p && p->mkid.cb && i < 4)
            {
                PCFVITEMID item = (PCFVITEMID)p;
                WCHAR szName[48] = {};
                for (UINT j = 0; j < 47 && item->szName[j]; j++) { szName[j] = item->szName[j]; }
                DebugLog(L"[SF]     view-instance seg%u cb=%u MyObjID=0x%04X nLevel=%u name='%s'",
                         i, p->mkid.cb, item->MyObjID, item->nLevel, szName);
                p = ILNext(p);
                i++;
            }
        }
        SFV_CREATE csfv = { sizeof(csfv), 0 };
        hr = QueryInterface(IID_PPV_ARGS(&csfv.pshf));
        DebugLog(L"[SF]   QI pshf hr=0x%08X", hr);
        if (SUCCEEDED(hr))
        {
            hr = CFolderViewCB_CreateInstance(IID_PPV_ARGS(&csfv.psfvcb));
            DebugLog(L"[SF]   CFolderViewCB hr=0x%08X", hr);
            if (SUCCEEDED(hr))
            {
                hr = SHCreateShellFolderView(&csfv, (IShellView**)ppv);
                DebugLog(L"[SF]   SHCreateShellFolderView hr=0x%08X view=%p", hr, ppv ? *ppv : NULL);
                csfv.psfvcb->Release();
            }
            csfv.pshf->Release();
        }
    }
    else if (riid == IID_ICategoryProvider)
    {
        // Control variable: the Microsoft sample's provider is tightly coupled
        // to its own columns/categories. Our placeholder advertises support but
        // cannot create any category, so fail explicitly instead of returning
        // a semantically incomplete object.
        hr = E_NOINTERFACE;
        DebugLog(L"[SF]   CreateViewObject(ICategoryProvider control) hr=0x%08X", hr);
    }
    else if (riid == IID_IContextMenu)
    {
        // This is the background context menu for the folder itself.
        DEFCONTEXTMENU dcm = { hwnd, NULL, m_pidl, static_cast<IShellFolder2 *>(this), 0, NULL, NULL, 0, NULL };
        hr = SHCreateDefaultContextMenu(&dcm, riid, ppv);
    }
    else if (riid == IID_IExplorerCommandProvider)
    {
        CFolderViewCommandProvider *pProvider = new (std::nothrow) CFolderViewCommandProvider();
        hr = pProvider ? S_OK : E_OUTOFMEMORY;
        if (SUCCEEDED(hr))
        {
            hr = pProvider->QueryInterface(riid, ppv);
            pProvider->Release();
        }
    }
    return hr;
}

//  Retrieves the attributes of one or more file objects or subfolders.
HRESULT CFolderViewImplFolder::GetAttributesOf(UINT cidl, PCUITEMID_CHILD_ARRAY apidl, ULONG *rgfInOut)
{
    DebugLog(L"[SF] GetAttributesOf cidl=%u in=0x%08X", cidl, rgfInOut ? *rgfInOut : 0);
    HRESULT hr = E_INVALIDARG;
    if (1 == cidl)
    {
        WCHAR szDbg[256] = {};
        PCFVITEMID pDbg = _IsValid(apidl[0]);
        if (pDbg)
        {
            int i = 0;
            while (i < 255 && pDbg->szName[i]) { szDbg[i] = pDbg->szName[i]; i++; }
        }
        BOOL fIsFolder = FALSE;
        hr = _GetFolderness(apidl[0], &fIsFolder);
        if (SUCCEEDED(hr))
        {
            DWORD dwAttribs = 0;
            if (fIsFolder)
            {
                dwAttribs |= SFGAO_FOLDER;
                // NOTE: do NOT add SFGAO_BROWSABLE here (RESEARCH_LOG):
                // it makes explorer request the private view interface
                // 93F81976 instead of falling back to IID_IShellView,
                // which breaks deep-folder view creation.
                int nLevel = 0;
                if (SUCCEEDED(_GetLevel(apidl[0], &nLevel)) && nLevel < g_nMaxLevel)
                {
                    dwAttribs |= SFGAO_HASSUBFOLDER;
                }
            }
            // EXPERIMENT (sample-alignment): Microsoft's original sample
            // returns ONLY SFGAO_FOLDER|SFGAO_HASSUBFOLDER here (no CAN*).
            // Our CAN* attributes may make explorer treat deep folders as
            // ordinary operable items instead of navigable locations,
            // breaking the current-location state.  Testing without them;
            // the context menu verbs come from the registered
            // ContextMenuHandlers, so they should still appear.
            *rgfInOut &= dwAttribs;
            DebugLog(L"[SF] GetAttributesOf name='%s' folder=%d out=0x%08X (no CAN*, sample-aligned)", szDbg, fIsFolder ? 1 : 0, *rgfInOut);
        }
    }
    return hr;
}

//  Retrieves an OLE interface that can be used to carry out
//  actions on the specified file objects or folders.
HRESULT CFolderViewImplFolder::GetUIObjectOf(HWND hwnd, UINT cidl, PCUITEMID_CHILD_ARRAY apidl,
                                             REFIID riid, UINT * /* prgfInOut */, void **ppv)
{
    *ppv = NULL;
    HRESULT hr;

    if (riid == IID_IContextMenu)
    {
        // Return Explorer's default menu directly. Wrapping it makes Explorer
        // call BindToObject without committing a deep-folder view on Win10.
        // Custom verbs are appended by the registered ContextMenuHandlers class.
        DEFCONTEXTMENU const dcm = { hwnd, NULL, m_pidl, static_cast<IShellFolder2 *>(this),
                               cidl, apidl, NULL, 0, NULL };
        hr = SHCreateDefaultContextMenu(&dcm, riid, ppv);
    }
    else if (riid == IID_IExtractIconW)
    {
        if (cidl < 1 || !apidl || !apidl[0])
        {
            return E_INVALIDARG;
        }
        IDefaultExtractIconInit *pdxi;
        hr = SHCreateDefaultExtractIcon(IID_PPV_ARGS(&pdxi));
        if (SUCCEEDED(hr))
        {
            BOOL fIsFolder = FALSE;
            hr = _GetFolderness(apidl[0], &fIsFolder);
            if (SUCCEEDED(hr))
            {
                hr = pdxi->SetNormalIcon(L"shell32.dll", fIsFolder ? 4 : 1);
            }
            if (SUCCEEDED(hr))
            {
                hr = pdxi->QueryInterface(riid, ppv);
            }
            pdxi->Release();
        }
    }
    else if (riid == IID_IDataObject)
    {
        // Probe: the shell folder's PIDL is complete here (m_pidl includes
        // the current directory segment, e.g. www).  Explorer builds the
        // context menu from this data object; we can attach a custom
        // clipboard format carrying the remote path so handlers never need
        // to reconstruct it from explorer's truncated CIDA.
        WCHAR szDbgPath[512];
        RemotePathFromPidl(m_pidl, szDbgPath, ARRAYSIZE(szDbgPath));
        DebugLog(L"[SF] GetUIObjectOf(IDataObject) cidl=%u m_pidl=%p remotePath='%s'", cidl, m_pidl, szDbgPath);
        hr = SHCreateDataObject(m_pidl, cidl, apidl, NULL, riid, ppv);
        DebugLog(L"[SF] GetUIObjectOf(IDataObject) -> pdo=%p hr=0x%08X", *ppv, hr);
    }
    else if (riid == IID_IQueryAssociations)
    {
        DebugLog(L"[SF] GetUIObjectOf(IQueryAssociations) cidl=%u", cidl);
        if (cidl < 1 || !apidl || !apidl[0])
        {
            return E_INVALIDARG;
        }
        BOOL fIsFolder = FALSE;
        hr = _GetFolderness(apidl[0], &fIsFolder);
        if (SUCCEEDED(hr))
        {
            if (fIsFolder)
            {
                ASSOCIATIONELEMENT const rgAssocFolder[] =
                {
                    { ASSOCCLASS_PROGID_STR, NULL, L"RemoteFsShellType"},
                    { ASSOCCLASS_FOLDER, NULL, NULL},
                };
                hr = AssocCreateForClasses(rgAssocFolder, ARRAYSIZE(rgAssocFolder), riid, ppv);
                DebugLog(L"[SF]   AssocCreateForClasses(folder) hr=0x%08X", hr);
            }
            else
            {
                ASSOCIATIONELEMENT const rgAssocItem[] =
                {
                    { ASSOCCLASS_PROGID_STR, NULL, L"RemoteFsShellType"},
                };
                hr = AssocCreateForClasses(rgAssocItem, ARRAYSIZE(rgAssocItem), riid, ppv);
            }
        }
    }
    else
    {
        WCHAR szIID[64] = {};
        StringFromGUID2(riid, szIID, ARRAYSIZE(szIID));
        DebugLog(L"[SF] GetUIObjectOf UNSUPPORTED riid=%s cidl=%u (probe: IShellItem=%d IShellItem2=%d IShellFolder=%d)",
                 szIID, cidl,
                 IsEqualGUID(riid, IID_IShellItem) ? 1 : 0,
                 IsEqualGUID(riid, IID_IShellItem2) ? 1 : 0,
                 IsEqualGUID(riid, IID_IShellFolder) ? 1 : 0);
        hr = E_NOINTERFACE;
    }
    return hr;
}

//  Retrieves the display name for the specified file object or subfolder.
HRESULT CFolderViewImplFolder::GetDisplayNameOf(PCUITEMID_CHILD pidl, SHGDNF shgdnFlags, STRRET *pName)
{
    HRESULT hr = S_OK;
    WCHAR szDbg[256] = {};
    if (pidl)
    {
        PCFVITEMID pItem = _IsValid(pidl);
        if (pItem)
        {
            int i = 0;
            while (i < 255 && pItem->szName[i]) { szDbg[i] = pItem->szName[i]; i++; }
        }
    }
    DebugLog(L"[SF] GetDisplayNameOf name='%s' flags=0x%X (FORPARSING=%d INFOLDER=%d FORADDRESSBAR=%d)",
             szDbg, (UINT)shgdnFlags,
             (shgdnFlags & SHGDN_FORPARSING) ? 1 : 0,
             (shgdnFlags & SHGDN_INFOLDER) ? 1 : 0,
             (shgdnFlags & SHGDN_FORADDRESSBAR) ? 1 : 0);
    if (shgdnFlags & SHGDN_FORPARSING)
    {
        WCHAR szDisplayName[MAX_PATH];
        if (shgdnFlags & SHGDN_INFOLDER)
        {
            hr = _GetName(pidl, szDisplayName, ARRAYSIZE(szDisplayName));
        }
        else
        {
            PWSTR pszThisFolder;
            hr = SHGetNameFromIDList(m_pidl, (shgdnFlags & SHGDN_FORADDRESSBAR) ? SIGDN_DESKTOPABSOLUTEEDITING : SIGDN_DESKTOPABSOLUTEPARSING, &pszThisFolder);
            if (SUCCEEDED(hr))
            {
                StringCchCopy(szDisplayName, ARRAYSIZE(szDisplayName), pszThisFolder);
                StringCchCat(szDisplayName, ARRAYSIZE(szDisplayName), L"\\");

                WCHAR szName[MAX_PATH];
                hr = _GetName(pidl, szName, ARRAYSIZE(szName));
                if (SUCCEEDED(hr))
                {
                    StringCchCat(szDisplayName, ARRAYSIZE(szDisplayName), szName);
                }
                CoTaskMemFree(pszThisFolder);
            }
        }
        if (SUCCEEDED(hr))
        {
            hr = StringToStrRet(szDisplayName, pName);
        }
    }
    else
    {
        PWSTR pszName;
        hr = _GetName(pidl, &pszName);
        if (SUCCEEDED(hr))
        {
            hr = StringToStrRet(pszName, pName);
            CoTaskMemFree(pszName);
        }
    }
    DebugLog(L"[SF] GetDisplayNameOf -> hr=0x%08X type=%d", hr, pName ? pName->uType : -1);
    return hr;
}

//  Sets the display name of a file object or subfolder, changing
//  the item identifier in the process.
HRESULT CFolderViewImplFolder::SetNameOf(HWND /* hwnd */, PCUITEMID_CHILD /* pidl */,
                                         PCWSTR /* pszName */,  DWORD /* uFlags */, PITEMID_CHILD *ppidlOut)
{
    HRESULT hr = E_NOTIMPL;
    *ppidlOut = NULL;
    return hr;
}

//  IPersist method
HRESULT CFolderViewImplFolder::GetClassID(CLSID *pClassID)
{
    *pClassID = CLSID_RemoteFsShell;
    return S_OK;
}

//  IPersistFolder method
HRESULT CFolderViewImplFolder::Initialize(PCIDLIST_ABSOLUTE pidl)
{
    CoTaskMemFree(m_pidl);
    m_pidl = ILCloneFull(pidl);
    DebugLog(L"[SF] Initialize(IPersistFolder) pidl=%p level=%d -> m_pidl=%p", pidl, m_nLevel, m_pidl);
    if (m_pidl)
    {
        UINT i = 0;
        PCUIDLIST_RELATIVE p = (PCUIDLIST_RELATIVE)m_pidl;
        while (p && p->mkid.cb && i < 8)
        {
            PCFVITEMID item = (PCFVITEMID)p;
            WCHAR szName[64] = {};
            for (UINT j = 0; j < 63 && item->szName[j]; j++) { szName[j] = item->szName[j]; }
            DebugLog(L"[SF]   Initialize pidl seg%u cb=%u MyObjID=0x%04X nLevel=%u name='%s'",
                     i, p->mkid.cb, item->MyObjID, item->nLevel, szName);
            p = ILNext(p);
            i++;
        }
    }
    return m_pidl ? S_OK : E_FAIL;
}

//  IShellFolder2 methods
HRESULT CFolderViewImplFolder::EnumSearches(IEnumExtraSearch **ppEnum)
{
    *ppEnum = NULL;
    return E_NOINTERFACE;
}

//  Retrieves the default sorting and display column (indices from GetDetailsOf).
HRESULT CFolderViewImplFolder::GetDefaultColumn(DWORD /* dwRes */,
                                                ULONG *pSort,
                                                ULONG *pDisplay)
{
    *pSort = 0;      // Name
    *pDisplay = 0;   // Name
    return S_OK;
}

//  Retrieves the default state for a specified column.
HRESULT CFolderViewImplFolder::GetDefaultColumnState(UINT iColumn, SHCOLSTATEF *pcsFlags)
{
    HRESULT hr = (iColumn < 6) ? S_OK : E_INVALIDARG;
    if (SUCCEEDED(hr))
    {
        *pcsFlags = SHCOLSTATE_ONBYDEFAULT;
        switch (iColumn)
        {
        case 0: *pcsFlags |= SHCOLSTATE_TYPE_STR; break;   // Name
        case 1: *pcsFlags |= SHCOLSTATE_TYPE_STR; break;                              // Permissions
        case 2: *pcsFlags |= SHCOLSTATE_TYPE_STR; break;                              // Owner
        case 3: *pcsFlags |= SHCOLSTATE_TYPE_STR; break;                              // Group
        case 4: *pcsFlags |= SHCOLSTATE_TYPE_INT; break;                              // Size
        case 5: *pcsFlags |= SHCOLSTATE_TYPE_DATE; break;                             // Modified
        }
    }
    return hr;
}

//  Requests the GUID of the default search object for the folder.
HRESULT CFolderViewImplFolder::GetDefaultSearchGUID(GUID * /* pguid */)
{
    return E_NOTIMPL;
}

//  Helper function for getting the display name for a column.
HRESULT CFolderViewImplFolder::_GetColumnDisplayName(PCUITEMID_CHILD pidl,
                                                     const PROPERTYKEY *pkey,
                                                     VARIANT *pv,
                                                     PWSTR pszRet,
                                                     UINT cch)
{
    BOOL fIsFolder = FALSE;
    HRESULT hr = _GetFolderness(pidl, &fIsFolder);
    if (SUCCEEDED(hr))
    {
        WCHAR szValue[64] = {};

        if (IsEqualPropertyKey(*pkey, PKEY_ItemNameDisplay))
        {
            hr = _GetName(pidl, szValue, ARRAYSIZE(szValue));
        }
        else if (IsEqualPropertyKey(*pkey, PKEY_Remote_Permissions))
        {
            DWORD dwMode = 0;
            BOOL fSymlink = FALSE;
            hr = _GetMode(pidl, &dwMode);
            if (SUCCEEDED(hr))
            {
                hr = _GetSymlink(pidl, &fSymlink);
                if (SUCCEEDED(hr))
                {
                    FormatMode(dwMode, fIsFolder, fSymlink, szValue, ARRAYSIZE(szValue));
                }
            }
        }
        else if (IsEqualPropertyKey(*pkey, PKEY_Remote_Owner))
        {
            hr = _GetOwner(pidl, szValue, ARRAYSIZE(szValue));
        }
        else if (IsEqualPropertyKey(*pkey, PKEY_Remote_Group))
        {
            hr = _GetGroup(pidl, szValue, ARRAYSIZE(szValue));
        }
        else if (IsEqualPropertyKey(*pkey, PKEY_Remote_Size))
        {
            DWORD dwSize = 0;
            hr = _GetSize(pidl, &dwSize);
            if (SUCCEEDED(hr))
            {
                FormatSize(dwSize, fIsFolder, szValue, ARRAYSIZE(szValue));
            }
        }
        else if (IsEqualPropertyKey(*pkey, PKEY_Remote_Modified))
        {
            DWORD dwMtime = 0;
            hr = _GetMtime(pidl, &dwMtime);
            if (SUCCEEDED(hr))
            {
                FormatMtime(dwMtime, szValue, ARRAYSIZE(szValue));
            }
        }
        else
        {
            if (pv)
            {
                VariantInit(pv);
            }
            if (pszRet)
            {
                *pszRet = '\0';
            }
        }

        if (SUCCEEDED(hr) && szValue[0])
        {
            if (pv != NULL)
            {
                pv->vt = VT_BSTR;
                pv->bstrVal = SysAllocString(szValue);
                hr = pv->bstrVal ? S_OK : E_OUTOFMEMORY;
            }
            else if (pszRet)
            {
                hr = StringCchCopy(pszRet, cch, szValue);
            }
        }
    }
    return hr;
}

//  Retrieves detailed information, identified by a
//  property set ID (FMTID) and property ID (PID),
//  on an item in a Shell folder.
HRESULT CFolderViewImplFolder::GetDetailsEx(PCUITEMID_CHILD pidl,
                                            const PROPERTYKEY *pkey,
                                            VARIANT *pv)
{
    BOOL pfIsFolder = FALSE;
    HRESULT hr = _GetFolderness(pidl, &pfIsFolder);
    if (SUCCEEDED(hr))
    {
        if (IsEqualPropertyKey(*pkey, PKEY_PropList_PreviewDetails))
        {
            // This proplist indicates what properties are shown in the details pane at the bottom of the explorer browser.
            pv->vt = VT_BSTR;
            pv->bstrVal = SysAllocString(L"prop:Remote.Permissions;Remote.Owner;Remote.Group;Remote.Size;Remote.Modified");
            hr = pv->bstrVal ? S_OK : E_OUTOFMEMORY;
        }
        else
        {
            hr = _GetColumnDisplayName(pidl, pkey, pv, NULL, 0);
        }
    }
    return hr;
}

//  Retrieves detailed information, identified by a
//  column index, on an item in a Shell folder.
HRESULT CFolderViewImplFolder::GetDetailsOf(PCUITEMID_CHILD pidl,
                                            UINT iColumn,
                                            SHELLDETAILS *pDetails)
{
    PROPERTYKEY key;
    HRESULT hr = MapColumnToSCID(iColumn, &key);
    WCHAR szRet[MAX_PATH];

    if (!pidl)
    {
        // No item means we're returning information about the column itself.
        switch (iColumn)
        {
        case 0:
            pDetails->fmt = LVCFMT_LEFT;
            pDetails->cxChar = 28;
            hr = StringCchCopy(szRet, ARRAYSIZE(szRet), L"Name");
            break;
        case 1:
            pDetails->fmt = LVCFMT_LEFT;
            pDetails->cxChar = 13;
            hr = StringCchCopy(szRet, ARRAYSIZE(szRet), L"Permissions");
            break;
        case 2:
            pDetails->fmt = LVCFMT_LEFT;
            pDetails->cxChar = 11;
            hr = StringCchCopy(szRet, ARRAYSIZE(szRet), L"Owner");
            break;
        case 3:
            pDetails->fmt = LVCFMT_LEFT;
            pDetails->cxChar = 11;
            hr = StringCchCopy(szRet, ARRAYSIZE(szRet), L"Group");
            break;
        case 4:
            pDetails->fmt = LVCFMT_RIGHT;
            pDetails->cxChar = 12;
            hr = StringCchCopy(szRet, ARRAYSIZE(szRet), L"Size");
            break;
        case 5:
            pDetails->fmt = LVCFMT_LEFT;
            pDetails->cxChar = 18;
            hr = StringCchCopy(szRet, ARRAYSIZE(szRet), L"Modified");
            break;
        default:
            // GetDetailsOf is called with increasing column indices until failure.
            hr = E_FAIL;
            break;
        }
    }
    else if (SUCCEEDED(hr))
    {
        hr = _GetColumnDisplayName(pidl, &key, NULL, szRet, ARRAYSIZE(szRet));
    }

    if (SUCCEEDED(hr))
    {
        hr = StringToStrRet(szRet, &pDetails->str);
    }

    return hr;
}

//  Converts a column name to the appropriate
//  property set ID (FMTID) and property ID (PID).
HRESULT CFolderViewImplFolder::MapColumnToSCID(UINT iColumn, PROPERTYKEY *pkey)
{
    HRESULT hr = S_OK;
    switch (iColumn)
    {
    case 0:
        *pkey = PKEY_ItemNameDisplay;
        break;
    case 1:
        *pkey = PKEY_Remote_Permissions;
        break;
    case 2:
        *pkey = PKEY_Remote_Owner;
        break;
    case 3:
        *pkey = PKEY_Remote_Group;
        break;
    case 4:
        *pkey = PKEY_Remote_Size;
        break;
    case 5:
        *pkey = PKEY_Remote_Modified;
        break;
    default:
        hr = E_FAIL;
        break;
    }
    return hr;
}

//IPersistFolder2 methods
//  Retrieves the PIDLIST_ABSOLUTE for the folder object.
HRESULT CFolderViewImplFolder::GetCurFolder(PIDLIST_ABSOLUTE *ppidl)
{
    *ppidl = NULL;
    HRESULT hr = m_pidl ? S_OK : E_FAIL;
    if (SUCCEEDED(hr))
    {
        *ppidl = ILCloneFull(m_pidl);
        hr = *ppidl ? S_OK : E_OUTOFMEMORY;
    }
    DebugLog(L"[SF] GetCurFolder hr=0x%08X m_pidl=%p level=%d path='%s'", hr, m_pidl, m_nLevel, m_szRemotePath);
    if (m_pidl)
    {
        UINT i = 0;
        PCUIDLIST_RELATIVE p = (PCUIDLIST_RELATIVE)m_pidl;
        while (p && p->mkid.cb && i < 8)
        {
            PCFVITEMID item = (PCFVITEMID)p;
            WCHAR szName[64] = {};
            for (UINT j = 0; j < 63 && item->szName[j]; j++) { szName[j] = item->szName[j]; }
            DebugLog(L"[SF]   GetCurFolder m_pidl seg%u cb=%u MyObjID=0x%04X nLevel=%u name='%s'",
                     i, p->mkid.cb, item->MyObjID, item->nLevel, szName);
            p = ILNext(p);
            i++;
        }
    }
    return hr;
}

// Item idlists passed to folder methods are guaranteed to have accessible memory as specified
// by the cbSize in the itemid.  However they may be loaded from a persisted form (for example
// shortcuts on disk) where they could be corrupted.  It is the shell folder's responsibility
// to make sure it's safe against corrupted or malicious itemids.
PCFVITEMID CFolderViewImplFolder::_IsValid(PCUIDLIST_RELATIVE pidl)
{
    PCFVITEMID pidmine = NULL;
    if (pidl)
    {
        pidmine = (PCFVITEMID)pidl;
        if (!(pidmine->cb && MYOBJID == pidmine->MyObjID && pidmine->nLevel <= g_nMaxLevel))
        {
            pidmine = NULL;
        }
    }
    return pidmine;
}

HRESULT CFolderViewImplFolder::_GetName(PCUIDLIST_RELATIVE pidl, PWSTR pszName, int cchMax)
{
    PCFVITEMID pMyObj = _IsValid(pidl);
    HRESULT hr = pMyObj ? S_OK : E_INVALIDARG;
    if (SUCCEEDED(hr))
    {
        // StringCchCopy requires aligned strings, and itemids are not necessarily aligned.
        int i = 0;
        for ( ; i < cchMax; i++)
        {
            pszName[i] = pMyObj->szName[i];
            if (0 == pszName[i])
            {
                break;
            }
        }

        // Make sure the string is null-terminated.
        if (i == cchMax)
        {
            pszName[cchMax - 1] = 0;
        }
    }
    return hr;
}

HRESULT CFolderViewImplFolder::_GetName(PCUIDLIST_RELATIVE pidl, PWSTR *ppsz)
{
    *ppsz = 0;
    PCFVITEMID pMyObj = _IsValid(pidl);
    HRESULT hr = pMyObj ? S_OK : E_INVALIDARG;
    if (SUCCEEDED(hr))
    {
        int const cch = pMyObj->cchName;
        *ppsz = (PWSTR)CoTaskMemAlloc(cch * sizeof(**ppsz));
        hr = *ppsz ? S_OK : E_OUTOFMEMORY;
        if (SUCCEEDED(hr))
        {
            hr = _GetName(pidl, *ppsz, cch);
        }
    }
    return hr;
}

HRESULT CFolderViewImplFolder::_GetLevel(PCUIDLIST_RELATIVE pidl, int *pLevel)
{
    PCFVITEMID pMyObj = _IsValid(pidl);
    HRESULT hr = pMyObj ? S_OK : E_INVALIDARG;
    if (SUCCEEDED(hr))
    {
        *pLevel = pMyObj->nLevel;
    }
    else  // If this fails we are at level zero.
    {
        *pLevel = 0;
    }
    return hr;
}

HRESULT CFolderViewImplFolder::_GetMode(PCUIDLIST_RELATIVE pidl, DWORD *pdwMode)
{
    PCFVITEMID pMyObj = _IsValid(pidl);
    HRESULT hr = pMyObj ? S_OK : E_INVALIDARG;
    if (SUCCEEDED(hr))
    {
        *pdwMode = pMyObj->dwMode;
    }
    return hr;
}

HRESULT CFolderViewImplFolder::_GetSize(PCUIDLIST_RELATIVE pidl, DWORD *pdwSize)
{
    PCFVITEMID pMyObj = _IsValid(pidl);
    HRESULT hr = pMyObj ? S_OK : E_INVALIDARG;
    if (SUCCEEDED(hr))
    {
        *pdwSize = pMyObj->dwSize;
    }
    return hr;
}

HRESULT CFolderViewImplFolder::_GetMtime(PCUIDLIST_RELATIVE pidl, DWORD *pdwMtime)
{
    PCFVITEMID pMyObj = _IsValid(pidl);
    HRESULT hr = pMyObj ? S_OK : E_INVALIDARG;
    if (SUCCEEDED(hr))
    {
        *pdwMtime = pMyObj->dwMtime;
    }
    return hr;
}

HRESULT CFolderViewImplFolder::_GetOwner(PCUIDLIST_RELATIVE pidl, PWSTR pszOwner, int cch)
{
    PCFVITEMID pMyObj = _IsValid(pidl);
    HRESULT hr = pMyObj ? S_OK : E_INVALIDARG;
    if (SUCCEEDED(hr))
    {
        const wchar_t *psz = L"";
        auto &t = OwnerTables();
        AcquireSRWLockShared(&t.lock);
        psz = TableGet(t.owners, pMyObj->nOwner);
        hr = StringCchCopy(pszOwner, cch, psz);
        ReleaseSRWLockShared(&t.lock);
    }
    return hr;
}

HRESULT CFolderViewImplFolder::_GetGroup(PCUIDLIST_RELATIVE pidl, PWSTR pszGroup, int cch)
{
    PCFVITEMID pMyObj = _IsValid(pidl);
    HRESULT hr = pMyObj ? S_OK : E_INVALIDARG;
    if (SUCCEEDED(hr))
    {
        const wchar_t *psz = L"";
        auto &t = OwnerTables();
        AcquireSRWLockShared(&t.lock);
        psz = TableGet(t.groups, pMyObj->nGroup);
        hr = StringCchCopy(pszGroup, cch, psz);
        ReleaseSRWLockShared(&t.lock);
    }
    return hr;
}

HRESULT CFolderViewImplFolder::_GetFolderness(PCUIDLIST_RELATIVE pidl, BOOL *pfIsFolder)
{
    PCFVITEMID pMyObj = _IsValid(pidl);
    HRESULT hr = pMyObj ? S_OK : E_INVALIDARG;
    if (SUCCEEDED(hr))
    {
        *pfIsFolder = pMyObj->fIsFolder;
    }
    return hr;
}

HRESULT CFolderViewImplFolder::_GetSymlink(PCUIDLIST_RELATIVE pidl, BOOL *pfIsSymlink)
{
    PCFVITEMID pMyObj = _IsValid(pidl);
    HRESULT hr = pMyObj ? S_OK : E_INVALIDARG;
    if (SUCCEEDED(hr))
    {
        *pfIsSymlink = pMyObj->fIsSymlink;
    }
    return hr;
}

HRESULT CFolderViewImplFolder::_ValidatePidl(PCUIDLIST_RELATIVE pidl)
{
    PCFVITEMID pMyObj = _IsValid(pidl);
    return pMyObj ? S_OK : E_INVALIDARG;
}

HRESULT CFolderViewImplFolder::CreateChildID(PCWSTR pszName, int nLevel, DWORD dwMode, DWORD dwSize, DWORD dwMtime,
                                             PCWSTR pszOwner, PCWSTR pszGroup, BOOL fIsFolder, BOOL fIsSymlink, PITEMID_CHILD *ppidl)
{
    // Cap the display name at 250 chars: cchName is a BYTE, so names >= 255
    // would wrap around and truncate the PIDL string buffer.
    WCHAR szNameBuf[256];
    int nLen = pszName ? lstrlen(pszName) : 0;
    if (nLen > 250)
    {
        nLen = 250;
    }
    StringCchCopyN(szNameBuf, ARRAYSIZE(szNameBuf), pszName ? pszName : L"", nLen);
    szNameBuf[nLen] = 0;   // null terminator

    // Sizeof an object plus the next cb plus the characters in the string.
    UINT nIDSize = sizeof(FVITEMID) +
                   sizeof(USHORT) +
                   (nLen * sizeof(WCHAR)) +
                   sizeof(WCHAR);

    // Allocate and zero the memory.
    FVITEMID *lpMyObj = (FVITEMID *)CoTaskMemAlloc(nIDSize);

    HRESULT hr = lpMyObj ? S_OK : E_OUTOFMEMORY;
    if (SUCCEEDED(hr))
    {
        ZeroMemory(lpMyObj, nIDSize);
        lpMyObj->cb = static_cast<short>(nIDSize - sizeof(lpMyObj->cb));
        lpMyObj->MyObjID    = MYOBJID;
        lpMyObj->cchName    = (BYTE)(nLen + 1);   // <= 251, no overflow
        lpMyObj->nLevel     = (BYTE)nLevel;
        lpMyObj->dwMode     = dwMode;
        lpMyObj->dwSize     = dwSize;
        lpMyObj->dwMtime    = dwMtime;
        // Owner/group are stored as BYTE indexes into the dynamic tables so the
        // PIDL stays small (~30 bytes, like the Microsoft sample).  Real FTP
        // servers expose arbitrary owner/group strings; the tables are filled
        // here and read back in _GetOwner/_GetGroup.
        {
            auto &t = OwnerTables();
            AcquireSRWLockExclusive(&t.lock);
            lpMyObj->nOwner = TableAdd(t.owners, pszOwner);
            lpMyObj->nGroup = TableAdd(t.groups, pszGroup);
            ReleaseSRWLockExclusive(&t.lock);
        }
        lpMyObj->fIsFolder  = fIsFolder;
        lpMyObj->fIsSymlink = fIsSymlink;

        hr = StringCchCopy(lpMyObj->szName, lpMyObj->cchName, szNameBuf);
        if (SUCCEEDED(hr))
        {
            *ppidl = (PITEMID_CHILD)lpMyObj;
        }
    }
    return hr;
}

CFolderViewImplEnumIDList::CFolderViewImplEnumIDList(DWORD grfFlags, int nLevel, PCWSTR pszPath, CFolderViewImplFolder *pFolderViewImplShellFolder) :
    m_cRef(1), m_grfFlags(grfFlags), m_nLevel(nLevel), m_nItem(0), m_pFolder(pFolderViewImplShellFolder)
{
    m_pFolder->AddRef();
    if (pszPath)
    {
        StringCchCopy(m_szPath, ARRAYSIZE(m_szPath), pszPath);
    }
    else
    {
        m_szPath[0] = L'\0';
    }
}

CFolderViewImplEnumIDList::~CFolderViewImplEnumIDList()
{
    m_pFolder->Release();
}

HRESULT CFolderViewImplEnumIDList::QueryInterface(REFIID riid, void **ppv)
{
    static const QITAB qit[] = {
        QITABENT (CFolderViewImplEnumIDList, IEnumIDList),
        { 0 },
    };
    return QISearch(this, qit, riid, ppv);
}

ULONG CFolderViewImplEnumIDList::AddRef()
{
    return InterlockedIncrement(&m_cRef);
}

ULONG CFolderViewImplEnumIDList::Release()
{
    long cRef = InterlockedDecrement(&m_cRef);
    if (0 == cRef)
    {
        delete this;
    }
    return cRef;
}

// This initializes the enumerator from the hardcoded RemoteFS data source.
// In Phase 2 the array would be backed by the C# Provider via IPC.
HRESULT CFolderViewImplEnumIDList::Initialize()
{
    ZeroMemory(m_aData, sizeof(m_aData));
    // m_nLevel is the level encoded into child PIDLs. The listed directory is
    // therefore the parent at m_nLevel - 1.
    int n = GetLevelItems(m_nLevel - 1, m_szPath, m_aData, MAX_OBJS);
    // Record this directory (path + full PIDL) so the context menu handler
    // can recover the current folder even though explorer hands it a
    // truncated PIDL.
    RememberEnumPath(m_nLevel - 1, m_szPath, m_pFolder->GetPidl());
    return (n >= 0) ? S_OK : S_OK;  // n<0 = connection error -> empty listing
}

// Retrieves the specified number of item identifiers in
// the enumeration sequence and advances the current position
// by the number of items retrieved.
HRESULT CFolderViewImplEnumIDList::Next(ULONG celt, PITEMID_CHILD *rgelt, ULONG *pceltFetched)
{
    ULONG celtFetched = 0;

    HRESULT hr = (pceltFetched || celt <= 1) ? S_OK : E_INVALIDARG;
    if (SUCCEEDED(hr))
    {
        ULONG i = 0;
        while (SUCCEEDED(hr) && i < celt && m_nItem < MAX_OBJS && m_aData[m_nItem].szName[0])
        {
            BOOL fSkip = FALSE;
            if (!(m_grfFlags & SHCONTF_STORAGE))
            {
                if (m_aData[m_nItem].fIsFolder)
                {
                    if (!(m_grfFlags & SHCONTF_FOLDERS))
                    {
                        fSkip = TRUE;
                    }
                }
                else
                {
                    if (!(m_grfFlags & SHCONTF_NONFOLDERS))
                    {
                        fSkip = TRUE;
                    }
                }
            }

            if (!fSkip)
            {
                hr = m_pFolder->CreateChildID(m_aData[m_nItem].szName, m_nLevel, m_aData[m_nItem].dwMode,
                                              m_aData[m_nItem].dwSize, m_aData[m_nItem].dwMtime,
                                              m_aData[m_nItem].szOwner, m_aData[m_nItem].szGroup,
                                              m_aData[m_nItem].fIsFolder, m_aData[m_nItem].fIsSymlink,
                                              &rgelt[i]);
                if (SUCCEEDED(hr))
                {
                    celtFetched++;
                    i++;
                }
            }

            m_nItem++;
        }
    }

    if (pceltFetched)
    {
        *pceltFetched = celtFetched;
    }

    return (celtFetched == celt) ? S_OK : S_FALSE;
}

HRESULT CFolderViewImplEnumIDList::Skip(DWORD celt)
{
    m_nItem += celt;
    return S_OK;
}

HRESULT CFolderViewImplEnumIDList::Reset()
{
    m_nItem = 0;
    return S_OK;
}

HRESULT CFolderViewImplEnumIDList::Clone(IEnumIDList **ppenum)
{
    // this method is rarely used and it's acceptable to not implement it.
    *ppenum = NULL;
    return E_NOTIMPL;
}

class CFolderViewCB : public IShellFolderViewCB,
                      public IFolderViewSettings
{
public:
    CFolderViewCB() : _cRef(1) { }
    IFACEMETHODIMP QueryInterface(REFIID riid, void **ppv)
    {
        static const QITAB qit[] = { QITABENT(CFolderViewCB, IShellFolderViewCB), QITABENT(CFolderViewCB, IFolderViewSettings), { 0 } };
        return QISearch(this, qit, riid, ppv);
    }
    IFACEMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&_cRef); }
    IFACEMETHODIMP_(ULONG) Release() { long c = InterlockedDecrement(&_cRef); if (!c) delete this; return c; }
    IFACEMETHODIMP MessageSFVCB(UINT, WPARAM, LPARAM) { return E_NOTIMPL; }
    IFACEMETHODIMP GetColumnPropertyList(REFIID, void **ppv) { *ppv = NULL; return E_NOTIMPL; }
    IFACEMETHODIMP GetGroupByProperty(PROPERTYKEY *, BOOL *) { return E_NOTIMPL; }
    IFACEMETHODIMP GetViewMode(FOLDERLOGICALVIEWMODE *) { return E_NOTIMPL; }
    IFACEMETHODIMP GetIconSize(UINT *) { return E_NOTIMPL; }
    IFACEMETHODIMP GetFolderFlags(FOLDERFLAGS *pfolderMask, FOLDERFLAGS *pfolderFlags)
    {
        if (pfolderMask) *pfolderMask = FWF_USESEARCHFOLDER;
        if (pfolderFlags) *pfolderFlags = FWF_USESEARCHFOLDER;
        return S_OK;
    }
    IFACEMETHODIMP GetSortColumns(SORTCOLUMN *, UINT, UINT *) { return E_NOTIMPL; }
    IFACEMETHODIMP GetGroupSubsetCount(UINT *) { return E_NOTIMPL; }
private:
    ~CFolderViewCB() { }
    long _cRef;
};

HRESULT CFolderViewCB_CreateInstance(REFIID riid, void **ppv)
{
    *ppv = NULL;

    HRESULT hr = E_OUTOFMEMORY;
    CFolderViewCB *pfvcb = new (std::nothrow) CFolderViewCB();
    if (pfvcb)
    {
        hr = pfvcb->QueryInterface(riid, ppv);
        pfvcb->Release();
    }
    return hr;
}
