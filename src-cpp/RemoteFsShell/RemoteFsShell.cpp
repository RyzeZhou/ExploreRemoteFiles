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

const int g_nMaxLevel = 3;   // level 0 = connections, 1 = root, 2 = dir contents

HRESULT CFolderViewCB_CreateInstance(REFIID riid, void **ppv);
HRESULT CFolderViewImplContextMenu_CreateInstance(REFIID riid, void **ppv);

// ---------------------------------------------------------------------------
// Hardcoded RemoteFS data source (Phase 1 PoC).
// Phase 2 replaces this with real data from the C# Provider via IPC.
// ---------------------------------------------------------------------------
typedef struct
{
    int     nLevel;
    DWORD   dwMode;
    DWORD   dwSize;
    DWORD   dwMtime;
    int     nOwner;
    int     nGroup;
    BOOL    fIsFolder;
    BOOL    fIsSymlink;
    WCHAR   szName[MAX_PATH];
} ITEMDATA;

// level 0: configured connections
static const ITEMDATA c_rgConnections[] =
{
    { 0, 0755, 0,        1752910000, 3, 3, TRUE,  FALSE, L"local-ftp"  },
    { 0, 0755, 0,        1752910000, 1, 1, TRUE,  FALSE, L"local-sftp" },
};

// level 1: remote root contents (both connections share this in the PoC)
static const ITEMDATA c_rgRoot[] =
{
    { 1, 0755, 0,        1752900000, 2, 2, TRUE,  FALSE, L"www"         },
    { 1, 0755, 0,        1752890000, 0, 0, TRUE,  FALSE, L"home"        },
    { 1, 0755, 8192,     1752880000, 1, 1, FALSE, FALSE, L"deploy.sh"   },
    { 1, 0644, 12288,    1752870000, 0, 0, FALSE, FALSE, L"nginx.conf"  },
    { 1, 0777, 0,        1752860000, 0, 0, FALSE, TRUE,  L"current"     },
};

// level 2: www directory contents
static const ITEMDATA c_rgWww[] =
{
    { 2, 0644, 1024,     1752850000, 2, 2, FALSE, FALSE, L"index.html"  },
    { 2, 0644, 2097152,  1752840000, 2, 2, FALSE, FALSE, L"big.bin"     },
    { 2, 0755, 0,        1752830000, 2, 2, TRUE,  FALSE, L"assets"      },
};

// level 3: assets contents (leaf level in PoC)
static const ITEMDATA c_rgAssets[] =
{
    { 3, 0644, 51200,    1752820000, 2, 2, FALSE, FALSE, L"logo.png"    },
    { 3, 0644, 3072,     1752810000, 2, 2, FALSE, FALSE, L"style.css"   },
};

static const ITEMDATA *GetLevelData(int nLevel, int *pcItems)
{
    switch (nLevel)
    {
    case 0: *pcItems = ARRAYSIZE(c_rgConnections); return c_rgConnections;
    case 1: *pcItems = ARRAYSIZE(c_rgRoot);        return c_rgRoot;
    case 2: *pcItems = ARRAYSIZE(c_rgWww);         return c_rgWww;
    case 3: *pcItems = ARRAYSIZE(c_rgAssets);      return c_rgAssets;
    default: *pcItems = 0;                         return NULL;
    }
}

// ---------------------------------------------------------------------------
// The shell folder implementation
// ---------------------------------------------------------------------------
class CFolderViewImplFolder : public IShellFolder2,
                              public IPersistFolder2
{
public:
    CFolderViewImplFolder(UINT nLevel);

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
                          int nOwner, int nGroup, BOOL fIsFolder, BOOL fIsSymlink, PITEMID_CHILD *ppidl);

private:
    ~CFolderViewImplFolder();

    HRESULT _GetName(PCUIDLIST_RELATIVE pidl, PWSTR pszName, int cchMax);
    HRESULT _GetName(PCUIDLIST_RELATIVE pidl, PWSTR *pszName);
    HRESULT _GetLevel(PCUIDLIST_RELATIVE pidl, int* pLevel);
    HRESULT _GetMode(PCUIDLIST_RELATIVE pidl, DWORD* pdwMode);
    HRESULT _GetSize(PCUIDLIST_RELATIVE pidl, DWORD* pdwSize);
    HRESULT _GetMtime(PCUIDLIST_RELATIVE pidl, DWORD* pdwMtime);
    HRESULT _GetOwner(PCUIDLIST_RELATIVE pidl, int* pnOwner);
    HRESULT _GetGroup(PCUIDLIST_RELATIVE pidl, int* pnGroup);
    HRESULT _GetFolderness(PCUIDLIST_RELATIVE pidl, BOOL* pfIsFolder);
    HRESULT _GetSymlink(PCUIDLIST_RELATIVE pidl, BOOL* pfIsSymlink);
    HRESULT _ValidatePidl(PCUIDLIST_RELATIVE pidl);
    PCFVITEMID _IsValid(PCUIDLIST_RELATIVE pidl);

    HRESULT _GetColumnDisplayName(PCUITEMID_CHILD pidl, const PROPERTYKEY* pkey, VARIANT* pv, PWSTR pszRet, UINT cch);

    long                m_cRef;
    int                 m_nLevel;
    PIDLIST_ABSOLUTE    m_pidl;             // where this folder is in the name space
};

class CFolderViewImplEnumIDList : public IEnumIDList
{
public:
    CFolderViewImplEnumIDList(DWORD grfFlags, int nCurrent, CFolderViewImplFolder *pFolderViewImplShellFolder);

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
    ITEMDATA m_aData[MAX_OBJS];

    CFolderViewImplFolder *m_pFolder;
};

HRESULT CFolderViewImplFolder_CreateInstance(REFIID riid, void **ppv)
{
    *ppv = NULL;
    CFolderViewImplFolder* pFolderViewImplShellFolder = new (std::nothrow) CFolderViewImplFolder(0);
    HRESULT hr = pFolderViewImplShellFolder ? S_OK : E_OUTOFMEMORY;
    if (SUCCEEDED(hr))
    {
        hr = pFolderViewImplShellFolder->QueryInterface(riid, ppv);
        pFolderViewImplShellFolder->Release();
    }
    return hr;
}

CFolderViewImplFolder::CFolderViewImplFolder(UINT nLevel) : m_cRef(1), m_nLevel(nLevel), m_pidl(NULL)
{
    DllAddRef();
}

CFolderViewImplFolder::~CFolderViewImplFolder()
{
    CoTaskMemFree(m_pidl);
    DllRelease();
}

HRESULT CFolderViewImplFolder::QueryInterface(REFIID riid, void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CFolderViewImplFolder, IShellFolder),
        QITABENT(CFolderViewImplFolder, IShellFolder2),
        QITABENT(CFolderViewImplFolder, IPersist),
        QITABENT(CFolderViewImplFolder, IPersistFolder),
        QITABENT(CFolderViewImplFolder, IPersistFolder2),
        { 0 },
    };
    return QISearch(this, qit, riid, ppv);
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

        // extract first component of the display name
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

            // Find this component in the current level's data source.
            int cItems = 0;
            const ITEMDATA *pData = GetLevelData(m_nLevel, &cItems);
            BOOL fFound = FALSE;
            for (int i = 0; i < cItems; i++)
            {
                if (0 == StrCmp(pData[i].szName, szNameComponent))
                {
                    PIDLIST_RELATIVE pidlCurrent = NULL;
                    hr = CreateChildID(pData[i].szName, m_nLevel, pData[i].dwMode, pData[i].dwSize,
                                       pData[i].dwMtime, pData[i].nOwner, pData[i].nGroup,
                                       pData[i].fIsFolder, pData[i].fIsSymlink, &pidlCurrent);
                    if (SUCCEEDED(hr))
                    {
                        // If there are more components to parse, delegate to the child folder.
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

HRESULT CFolderViewImplFolder::EnumObjects(HWND /* hwnd */, DWORD grfFlags, IEnumIDList **ppenumIDList)
{
    DebugLog(L"[SF] EnumObjects level=%d flags=0x%X", m_nLevel, grfFlags);
    HRESULT hr;
    if (m_nLevel >= g_nMaxLevel)
    {
        *ppenumIDList = NULL;
        hr = S_FALSE; // S_FALSE is allowed with NULL out param to indicate no contents.
    }
    else
    {
        CFolderViewImplEnumIDList *penum = new (std::nothrow) CFolderViewImplEnumIDList(grfFlags, m_nLevel, this);
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
    DebugLog(L"[SF] BindToObject called pidl=%p riid=%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             pidl, riid.Data1, riid.Data2, riid.Data3,
             riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3],
             riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7]);
    HRESULT hr = _ValidatePidl(pidl);
    if (SUCCEEDED(hr))
    {
        int nLevel = 0;
        hr = _GetLevel(pidl, &nLevel);
        if (SUCCEEDED(hr))
        {
            CFolderViewImplFolder* pFolder = new (std::nothrow) CFolderViewImplFolder(nLevel + 1);
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
                            if (ILIsEmpty(pidlNext))
                            {
                                hr = pFolder->QueryInterface(riid, ppv);
                            }
                            else
                            {
                                hr = pFolder->BindToObject(pidlNext, pbc, riid, ppv);
                            }
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
            case 2: // Column 2: Owner (by table index)
            {
                int nO1 = 0, nO2 = 0;
                hr = _GetOwner(pidl1, &nO1);
                if (SUCCEEDED(hr))
                {
                    hr = _GetOwner(pidl2, &nO2);
                    if (SUCCEEDED(hr))
                    {
                        nResult = (nO1 > nO2) ? 1 : (nO1 < nO2) ? -1 : 0;
                    }
                }
                break;
            }
            case 3: // Column 3: Group (by table index)
            {
                int nG1 = 0, nG2 = 0;
                hr = _GetGroup(pidl1, &nG1);
                if (SUCCEEDED(hr))
                {
                    hr = _GetGroup(pidl2, &nG2);
                    if (SUCCEEDED(hr))
                    {
                        nResult = (nG1 > nG2) ? 1 : (nG1 < nG2) ? -1 : 0;
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
        SFV_CREATE csfv = { sizeof(csfv), 0 };
        hr = QueryInterface(IID_PPV_ARGS(&csfv.pshf));
        if (SUCCEEDED(hr))
        {
            hr = CFolderViewCB_CreateInstance(IID_PPV_ARGS(&csfv.psfvcb));
            if (SUCCEEDED(hr))
            {
                hr = SHCreateShellFolderView(&csfv, (IShellView**)ppv);
                csfv.psfvcb->Release();
            }
            csfv.pshf->Release();
        }
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
        BOOL fIsFolder = FALSE;
        hr = _GetFolderness(apidl[0], &fIsFolder);
        if (SUCCEEDED(hr))
        {
            DWORD dwAttribs = 0;
            if (fIsFolder)
            {
                dwAttribs |= SFGAO_FOLDER;
                int nLevel = 0;
                if (SUCCEEDED(_GetLevel(apidl[0], &nLevel)) && nLevel < g_nMaxLevel)
                {
                    dwAttribs |= SFGAO_HASSUBFOLDER;
                }
            }
            // 提供基本文件操作的菜单项（不影响导航路径）
            dwAttribs |= SFGAO_CANCOPY | SFGAO_CANMOVE | SFGAO_CANRENAME
                       | SFGAO_CANDELETE | SFGAO_CANLINK;
            *rgfInOut &= dwAttribs;
            DebugLog(L"[SF] GetAttributesOf out=0x%08X", *rgfInOut);
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
        // 自定义菜单：委托 SHCreateDefaultContextMenu（保留标准导航 verb），
        // 追加 "Properties" verb。之前导航失败是 SFGAO_BROWSABLE 导致，
        // 与菜单包装无关（BROWSABLE 已移除）。
        if (cidl >= 1 && apidl && apidl[0])
        {
            CFolderViewImplContextMenu* pMenu = new (std::nothrow) CFolderViewImplContextMenu();
            hr = pMenu ? S_OK : E_OUTOFMEMORY;
            if (SUCCEEDED(hr))
            {
                IContextMenu *pDefault = NULL;
                DEFCONTEXTMENU dcm = { hwnd, NULL, m_pidl, static_cast<IShellFolder2 *>(this),
                                       cidl, apidl, NULL, 0, NULL };
                if (FAILED(SHCreateDefaultContextMenu(&dcm, IID_PPV_ARGS(&pDefault))))
                {
                    pDefault = NULL;
                }
                hr = pMenu->Init(hwnd, apidl[0], m_pidl, pDefault);
                if (pDefault)
                {
                    pDefault->Release();
                }
                if (SUCCEEDED(hr))
                {
                    hr = pMenu->QueryInterface(riid, ppv);
                }
                pMenu->Release();
            }
        }
        else
        {
            hr = E_INVALIDARG;
        }
    }
    else if (riid == IID_IExtractIconW)
    {
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
        hr = SHCreateDataObject(m_pidl, cidl, apidl, NULL, riid, ppv);
    }
    else if (riid == IID_IQueryAssociations)
    {
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
        hr = E_NOINTERFACE;
    }
    return hr;
}

//  Retrieves the display name for the specified file object or subfolder.
HRESULT CFolderViewImplFolder::GetDisplayNameOf(PCUITEMID_CHILD pidl, SHGDNF shgdnFlags, STRRET *pName)
{
    HRESULT hr = S_OK;
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
    m_pidl = ILCloneFull(pidl);
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
            int nOwner = 0;
            hr = _GetOwner(pidl, &nOwner);
            if (SUCCEEDED(hr) && nOwner >= 0 && nOwner < (int)ARRAYSIZE(c_rgOwners))
            {
                StringCchCopy(szValue, ARRAYSIZE(szValue), c_rgOwners[nOwner]);
            }
        }
        else if (IsEqualPropertyKey(*pkey, PKEY_Remote_Group))
        {
            int nGroup = 0;
            hr = _GetGroup(pidl, &nGroup);
            if (SUCCEEDED(hr) && nGroup >= 0 && nGroup < (int)ARRAYSIZE(c_rgGroups))
            {
                StringCchCopy(szValue, ARRAYSIZE(szValue), c_rgGroups[nGroup]);
            }
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

HRESULT CFolderViewImplFolder::_GetOwner(PCUIDLIST_RELATIVE pidl, int *pnOwner)
{
    PCFVITEMID pMyObj = _IsValid(pidl);
    HRESULT hr = pMyObj ? S_OK : E_INVALIDARG;
    if (SUCCEEDED(hr))
    {
        *pnOwner = pMyObj->nOwner;
    }
    return hr;
}

HRESULT CFolderViewImplFolder::_GetGroup(PCUIDLIST_RELATIVE pidl, int *pnGroup)
{
    PCFVITEMID pMyObj = _IsValid(pidl);
    HRESULT hr = pMyObj ? S_OK : E_INVALIDARG;
    if (SUCCEEDED(hr))
    {
        *pnGroup = pMyObj->nGroup;
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
                                             int nOwner, int nGroup, BOOL fIsFolder, BOOL fIsSymlink, PITEMID_CHILD *ppidl)
{
    // Sizeof an object plus the next cb plus the characters in the string.
    UINT nIDSize = sizeof(FVITEMID) +
                   sizeof(USHORT) +
                   (lstrlen(pszName) * sizeof(WCHAR)) +
                   sizeof(WCHAR);

    // Allocate and zero the memory.
    FVITEMID *lpMyObj = (FVITEMID *)CoTaskMemAlloc(nIDSize);

    HRESULT hr = lpMyObj ? S_OK : E_OUTOFMEMORY;
    if (SUCCEEDED(hr))
    {
        ZeroMemory(lpMyObj, nIDSize);
        lpMyObj->cb = static_cast<short>(nIDSize - sizeof(lpMyObj->cb));
        lpMyObj->MyObjID    = MYOBJID;
        lpMyObj->cchName    = (BYTE)(lstrlen(pszName) + 1);
        lpMyObj->nLevel     = (BYTE)nLevel;
        lpMyObj->dwMode     = dwMode;
        lpMyObj->dwSize     = dwSize;
        lpMyObj->dwMtime    = dwMtime;
        lpMyObj->nOwner     = (BYTE)nOwner;
        lpMyObj->nGroup     = (BYTE)nGroup;
        lpMyObj->fIsFolder  = fIsFolder;
        lpMyObj->fIsSymlink = fIsSymlink;

        hr = StringCchCopy(lpMyObj->szName, lpMyObj->cchName, pszName);
        if (SUCCEEDED(hr))
        {
            *ppidl = (PITEMID_CHILD)lpMyObj;
        }
    }
    return hr;
}

CFolderViewImplEnumIDList::CFolderViewImplEnumIDList(DWORD grfFlags, int nLevel, CFolderViewImplFolder *pFolderViewImplShellFolder) :
    m_cRef(1), m_grfFlags(grfFlags), m_nLevel(nLevel), m_nItem(0), m_pFolder(pFolderViewImplShellFolder)
{
    m_pFolder->AddRef();
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

    int cItems = 0;
    const ITEMDATA *pData = GetLevelData(m_nLevel, &cItems);
    if (!pData)
    {
        return S_OK; // no contents at this level
    }

    int nCount = min(cItems, MAX_OBJS);
    for (int i = 0; i < nCount; i++)
    {
        m_aData[i] = pData[i];
    }
    return S_OK;
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
                                              m_aData[m_nItem].nOwner, m_aData[m_nItem].nGroup,
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

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void **ppv)
    {
        static const QITAB qit[] =
        {
            QITABENT(CFolderViewCB, IShellFolderViewCB),
            QITABENT(CFolderViewCB, IFolderViewSettings),
            { 0 },
        };
        return QISearch(this, qit, riid, ppv);
    }

    IFACEMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&_cRef); }
    IFACEMETHODIMP_(ULONG) Release()
    {
        long cRef = InterlockedDecrement(&_cRef);
        if (0 == cRef)
        {
            delete this;
        }
        return cRef;
    }

    // IShellFolderViewCB
    IFACEMETHODIMP MessageSFVCB(UINT /* uMsg */, WPARAM /* wParam */, LPARAM /* lParam */)
        { return E_NOTIMPL; }

    // IFolderViewSettings
    IFACEMETHODIMP GetColumnPropertyList(REFIID /* riid */, void **ppv)
        { *ppv = NULL; return E_NOTIMPL; }
    IFACEMETHODIMP GetGroupByProperty(PROPERTYKEY * /* pkey */, BOOL * /* pfGroupAscending */)
        { return E_NOTIMPL; }
    IFACEMETHODIMP GetViewMode(FOLDERLOGICALVIEWMODE * /* plvm */)
        { return E_NOTIMPL; }
    IFACEMETHODIMP GetIconSize(UINT * /* puIconSize */)
        { return E_NOTIMPL; }

    IFACEMETHODIMP GetFolderFlags(FOLDERFLAGS *pfolderMask, FOLDERFLAGS *pfolderFlags);

    IFACEMETHODIMP GetSortColumns(SORTCOLUMN * /* rgSortColumns */, UINT /* cColumnsIn */, UINT * /* pcColumnsOut */)
        { return E_NOTIMPL; }
    IFACEMETHODIMP GetGroupSubsetCount(UINT * /* pcVisibleRows */)
        { return E_NOTIMPL; }

private:
    ~CFolderViewCB() { };
    long _cRef;
};

// IFolderViewSettings
IFACEMETHODIMP CFolderViewCB::GetFolderFlags(FOLDERFLAGS *pfolderMask, FOLDERFLAGS *pfolderFlags)
{
    if (pfolderMask)
    {
        *pfolderMask = FWF_USESEARCHFOLDER;
    }

    if (pfolderFlags)
    {
        *pfolderFlags = FWF_USESEARCHFOLDER;
    }

    return S_OK;
}

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
