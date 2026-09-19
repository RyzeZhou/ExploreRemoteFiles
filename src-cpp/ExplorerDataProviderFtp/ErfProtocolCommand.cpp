// In-process DelegateExecute handler for erf: addresses.
//
// Win11 exposes one CabinetWClass HWND for every tab in a tabbed Explorer
// window.  An out-of-process URI handler therefore cannot identify the tab
// that issued the address-bar request.  DelegateExecute runs at the Shell
// invocation point instead: IObjectWithSite gives us that tab's IShellBrowser,
// and BrowseObject(SBSP_SAMEBROWSER) navigates precisely that browser.
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <shellapi.h>
#include <string>
#include <new>
#include "Guid.h"
#include "Utils.h"
#include "ProbeLog.h"

namespace
{
static const GUID kSidTopLevelBrowser =
    { 0x4c96be40, 0x915c, 0x11cf, { 0x99, 0xd3, 0x00, 0xaa, 0x00, 0x4a, 0xe8, 0x37 } };
static const WCHAR kNamespaceRoot[] = L"::{C816CE0E-728C-4FC9-98E5-D0B35B384597}";

static bool IsErfAddress(PCWSTR address, std::wstring *target)
{
    if (!address || !target) return false;
    std::wstring value(address);
    const size_t first = value.find_first_not_of(L" \t\r\n\"");
    if (first == std::wstring::npos) return false;
    const size_t last = value.find_last_not_of(L" \t\r\n\"");
    value = value.substr(first, last - first + 1);
    if (value.size() < 7 || _wcsnicmp(value.c_str(), L"erf:", 4) != 0) return false;

    std::wstring decoded = value.substr(4);
    // The protocol supplies URI-escaped text; ParseDisplayName expects the
    // original site/path characters, exactly like Uri.UnescapeDataString in
    // the former resident-client handler.
    if (!decoded.empty())
    {
        HRESULT hr = UrlUnescapeW(&decoded[0], NULL, NULL, URL_UNESCAPE_INPLACE);
        if (FAILED(hr)) return false;
        decoded.resize(wcslen(decoded.c_str()));
    }
    const size_t colon = decoded.find(L':');
    if (colon == 0 || colon == std::wstring::npos || colon + 1 >= decoded.size()) return false;
    const std::wstring site = decoded.substr(0, colon);
    const std::wstring path = decoded.substr(colon + 1);
    if (site.find_first_of(L"/\\:") != std::wstring::npos || path.empty() || path[0] != L'/' ||
        path.find(L'\\') != std::wstring::npos) return false;
    *target = decoded;
    return true;
}

static HRESULT GetCurrentShellBrowser(IUnknown *site, IShellBrowser **browser)
{
    if (!browser) return E_POINTER;
    *browser = NULL;
    if (!site) return E_NOINTERFACE;
    HRESULT hr = site->QueryInterface(IID_PPV_ARGS(browser));
    if (SUCCEEDED(hr) && *browser) return S_OK;

    IServiceProvider *services = NULL;
    hr = site->QueryInterface(IID_PPV_ARGS(&services));
    if (SUCCEEDED(hr) && services)
    {
        hr = services->QueryService(kSidTopLevelBrowser, IID_PPV_ARGS(browser));
        services->Release();
    }
    return hr;
}

static HRESULT BuildTargetPidl(PCWSTR target, PIDLIST_ABSOLUTE *targetPidl)
{
    if (!targetPidl) return E_POINTER;
    *targetPidl = NULL;
    PIDLIST_ABSOLUTE root = NULL;
    PIDLIST_RELATIVE relative = NULL;
    IShellFolder *folder = NULL;
    HRESULT hr = SHParseDisplayName(kNamespaceRoot, NULL, &root, 0, NULL);
    if (SUCCEEDED(hr))
    {
        hr = SHBindToObject(NULL, root, NULL, IID_PPV_ARGS(&folder));
        if (SUCCEEDED(hr))
        {
            ULONG eaten = 0;
            DWORD attributes = 0;
            hr = folder->ParseDisplayName(NULL, NULL, const_cast<PWSTR>(target), &eaten, &relative, &attributes);
        }
    }
    if (SUCCEEDED(hr))
    {
        *targetPidl = ILCombine(root, relative);
        if (!*targetPidl) hr = E_OUTOFMEMORY;
    }
    if (folder) folder->Release();
    if (relative) ILFree(relative);
    if (root) ILFree(root);
    return hr;
}

static HRESULT StartResidentFallback(PCWSTR address)
{
    WCHAR client[MAX_PATH] = {};
    DWORD cch = ARRAYSIZE(client);
    LONG status = RegGetValueW(HKEY_CURRENT_USER, L"Software\\ExplorerRemoteFs", L"ClientPath",
                               RRF_RT_REG_SZ, NULL, client, &cch);
    if (status != ERROR_SUCCESS || !client[0]) return HRESULT_FROM_WIN32(status ? status : ERROR_FILE_NOT_FOUND);
    WCHAR parameters[2200] = {};
    HRESULT hr = StringCchPrintfW(parameters, ARRAYSIZE(parameters), L"--open-erf \"%s\"", address);
    if (FAILED(hr)) return hr;
    INT_PTR result = (INT_PTR)ShellExecuteW(NULL, L"open", client, parameters, NULL, SW_SHOWNORMAL);
    return result > 32 ? S_OK : HRESULT_FROM_WIN32((DWORD)result);
}

class CErfProtocolCommand final : public IExecuteCommand, public IObjectWithSelection, public IObjectWithSite
{
public:
    CErfProtocolCommand() : m_ref(1), m_selection(NULL), m_site(NULL) { DllAddRef(); }

    HRESULT QueryInterface(REFIID riid, void **ppv) override
    {
        static const QITAB qit[] = {
            QITABENT(CErfProtocolCommand, IExecuteCommand),
            QITABENT(CErfProtocolCommand, IObjectWithSelection),
            QITABENT(CErfProtocolCommand, IObjectWithSite),
            { 0 }
        };
        return QISearch(this, qit, riid, ppv);
    }
    ULONG AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG Release() override
    {
        const LONG refs = InterlockedDecrement(&m_ref);
        if (!refs) delete this;
        return refs;
    }

    HRESULT SetKeyState(DWORD) override { return S_OK; }
    HRESULT SetParameters(LPCWSTR parameters) override
    {
        m_parameters = parameters ? parameters : L"";
        ProbeLog(L"[ERF-DELEGATE] SetParameters length=%u", (unsigned)m_parameters.size());
        return S_OK;
    }
    HRESULT SetPosition(POINT) override { return S_OK; }
    HRESULT SetShowWindow(int) override { return S_OK; }
    HRESULT SetNoShowUI(BOOL) override { return S_OK; }
    HRESULT SetDirectory(LPCWSTR) override { return S_OK; }

    HRESULT Execute() override
    {
        std::wstring target;
        if (!IsErfAddress(m_parameters.c_str(), &target))
        {
            ProbeLog(L"[ERF-DELEGATE] rejected malformed address; length=%u", (unsigned)m_parameters.size());
            return E_INVALIDARG;
        }

        IShellBrowser *browser = NULL;
        HRESULT hr = GetCurrentShellBrowser(m_site, &browser);
        if (FAILED(hr) || !browser)
        {
            // An external caller may execute erf: without an Explorer site.
            // Preserve that use case through the resident client, but never use
            // this fallback for an Explorer tab: it cannot preserve tab identity.
            ProbeLog(L"[ERF-DELEGATE] no shell browser hr=0x%08X; resident fallback", hr);
            return StartResidentFallback(m_parameters.c_str());
        }

        PIDLIST_ABSOLUTE pidl = NULL;
        hr = BuildTargetPidl(target.c_str(), &pidl);
        if (SUCCEEDED(hr))
            hr = browser->BrowseObject(pidl, SBSP_SAMEBROWSER | SBSP_ABSOLUTE);
        ProbeLog(L"[ERF-DELEGATE] BrowseObject same-browser hr=0x%08X; target-length=%u",
                 hr, (unsigned)target.size());
        if (pidl) ILFree(pidl);
        browser->Release();
        return hr;
    }

    HRESULT SetSelection(IShellItemArray *selection) override
    {
        if (m_selection) m_selection->Release();
        m_selection = selection;
        if (m_selection) m_selection->AddRef();
        return S_OK;
    }
    HRESULT GetSelection(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = NULL;
        return m_selection ? m_selection->QueryInterface(riid, ppv) : E_FAIL;
    }
    HRESULT SetSite(IUnknown *site) override
    {
        if (m_site) m_site->Release();
        m_site = site;
        if (m_site) m_site->AddRef();
        ProbeLog(L"[ERF-DELEGATE] SetSite present=%d", m_site ? 1 : 0);
        return S_OK;
    }
    HRESULT GetSite(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = NULL;
        return m_site ? m_site->QueryInterface(riid, ppv) : E_FAIL;
    }

private:
    ~CErfProtocolCommand()
    {
        if (m_selection) m_selection->Release();
        if (m_site) m_site->Release();
        DllRelease();
    }
    LONG m_ref;
    IShellItemArray *m_selection;
    IUnknown *m_site;
    std::wstring m_parameters;
};
}

HRESULT CErfProtocolCommand_CreateInstance(REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    CErfProtocolCommand *command = new (std::nothrow) CErfProtocolCommand();
    if (!command) return E_OUTOFMEMORY;
    HRESULT hr = command->QueryInterface(riid, ppv);
    command->Release();
    return hr;
}