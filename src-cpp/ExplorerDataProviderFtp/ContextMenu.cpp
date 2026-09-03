// Context menu for the FTP Microsoft-Core namespace.
// WinSCP-grade verbs: Open / Edit / Download / Copy-to-clipboard / path-name
// copies / server-side copy & move / rename / delete / custom commands / properties.
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <string>
#include <time.h>
#include <shellapi.h>
#include "FtpMeta.h"
#include "FtpSites.h"
#include "Utils.h"
#include "resource.h"
#include "ProbeLog.h"
#include <new>

#define MENU_DELETE 0
#define MENU_PROPERTIES 1
#define MENU_COPY_NATIVE 2
#define MENU_COPY_FULL 3
#define MENU_OPEN 4
#define MENU_EDIT 5
#define MENU_DOWNLOAD 6
#define MENU_COPY_CLIP 7
#define MENU_COPY_NAME 8
#define MENU_RCOPY 9
#define MENU_RMOVE 10
#define MENU_RENAME 11
#define MENU_CUSTOM_BASE 100

#define MYOBJID 0x1234
#define MAX_SEL 64
#define MAX_CUSTOM 16
#pragma pack(1)
typedef struct tagCompactItem {
    USHORT cb; WORD MyObjID; BYTE nLevel; BYTE nSize; BYTE nSides; BYTE cchName;
    BOOL fIsFolder; WCHAR szName[1];
} COMPACTITEM;
#pragma pack()

static void CopyName(const COMPACTITEM *item, PWSTR out, UINT cch)
{
    UINT i=0; while(i+1<cch && i<item->cchName && item->szName[i]) { out[i]=item->szName[i]; i++; } out[i]=0;
}
static BOOL IsOurs(PCUIDLIST_RELATIVE p)
{
    return p && p->mkid.cb >= FIELD_OFFSET(COMPACTITEM, szName)+sizeof(WCHAR) && ((const COMPACTITEM*)p)->MyObjID==MYOBJID;
}
static void PidlPath(PCIDLIST_ABSOLUTE abs, PWSTR out, UINT cch)
{
    // First IsOurs segment is the site name (level 1); skip it, join the rest.
    out[0]=0; PCUIDLIST_RELATIVE p=(PCUIDLIST_RELATIVE)abs; BOOL first=TRUE;
    while(p && p->mkid.cb) {
        if(IsOurs(p)) { WCHAR name[256]; CopyName((const COMPACTITEM*)p,name,ARRAYSIZE(name));
            if(name[0] && !first) { StringCchCat(out,cch,L"/"); StringCchCat(out,cch,name); }
            first=FALSE;
        }
        p=ILNext(p);
    }
    if(!out[0]) StringCchCopy(out,cch,L"/");
}
static BOOL PidlSite(PCIDLIST_ABSOLUTE abs, PWSTR out, UINT cch)
{
    out[0]=0; PCUIDLIST_RELATIVE p=(PCUIDLIST_RELATIVE)abs;
    while(p && p->mkid.cb) {
        if(IsOurs(p)) { CopyName((const COMPACTITEM*)p,out,cch); return out[0]!=0; }
        p=ILNext(p);
    }
    return FALSE;
}
static void ApplySiteStartPath(PCWSTR site, PWSTR path, UINT cch)
{
    if (!site || !site[0] || !path || !path[0]) return;
    const FTPSITE *s = FtpSiteFind(site);
    if (!s || !s->startPath[0] || StrCmp(s->startPath, L"/") == 0) return;

    if (StrCmp(path, L"/") == 0)
    {
        StringCchCopy(path, cch, s->startPath);
        return;
    }

    WCHAR relative[512] = {}, base[256] = {};
    StringCchCopy(relative, ARRAYSIZE(relative), path);
    StringCchCopy(base, ARRAYSIZE(base), s->startPath);
    while (lstrlen(base) > 1 && base[lstrlen(base) - 1] == L'/')
        base[lstrlen(base) - 1] = L'\0';
    StringCchPrintf(path, cch, L"%s%s", base, relative);
}

static void JoinPath(PCWSTR folder, PCWSTR name, PWSTR out, UINT cch)
{
    if(folder[0]==L'/' && !folder[1]) StringCchPrintf(out,cch,L"/%s",name);
    else StringCchPrintf(out,cch,L"%s/%s",folder,name);
}
static void CopyTextToClipboard(HWND hwnd, PCWSTR text)
{
    if (!text || !text[0]) return;
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    size_t cch = wcslen(text) + 1;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, cch * sizeof(WCHAR));
    if (hMem)
    {
        PWSTR dst = (PWSTR)GlobalLock(hMem);
        if (dst) { StringCchCopy(dst, cch, text); GlobalUnlock(hMem); SetClipboardData(CF_UNICODETEXT, hMem); }
        else GlobalFree(hMem);
    }
    CloseClipboard();
}
static int RunCli(PCWSTR site, PCWSTR verb, PCWSTR p1, PCWSTR p2, std::string *captured)
{
    WCHAR cmd[2400];
    PCWSTR cli = GetCliPath();
    if (!cli || !cli[0])
    {
        ProbeLog(L"[FTP] RunCli: CLI path is empty");
        return -1;
    }
    if (p2 && p2[0])
        StringCchPrintf(cmd,ARRAYSIZE(cmd),L"\"%s\" %s \"%s\" \"%s\" \"%s\"",GetCliPath(),verb,site,p1,p2);
    else
        StringCchPrintf(cmd,ARRAYSIZE(cmd),L"\"%s\" %s \"%s\" \"%s\"",GetCliPath(),verb,site,p1);
    SECURITY_ATTRIBUTES sa={sizeof(sa),NULL,TRUE}; HANDLE rd=NULL,wr=NULL;
    if(captured && !CreatePipe(&rd,&wr,&sa,0)) return -1;
    if(captured) SetHandleInformation(rd,HANDLE_FLAG_INHERIT,0);
    STARTUPINFOW si={sizeof(si)}; if(captured){si.dwFlags=STARTF_USESTDHANDLES;si.hStdOutput=wr;si.hStdError=wr;si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);}
    PROCESS_INFORMATION pi={}; BOOL ok=CreateProcessW(NULL,cmd,NULL,NULL,captured?TRUE:FALSE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi);
    if(captured) CloseHandle(wr);
    if(!ok){
        ProbeLog(L"[FTP] RunCli CreateProcess failed err=%lu cmd='%s'", GetLastError(), cmd);
        if(rd)CloseHandle(rd);
        return -1;
    }
    if(captured){char b[4096];DWORD n=0;while(ReadFile(rd,b,sizeof(b),&n,NULL)&&n)captured->append(b,n);CloseHandle(rd);}
    DWORD wait=WaitForSingleObject(pi.hProcess,30000);
    if(wait==WAIT_TIMEOUT){
        ProbeLog(L"[FTP] RunCli timed out cmd='%s'", cmd);
        TerminateProcess(pi.hProcess,1);
        WaitForSingleObject(pi.hProcess,2000);
    }
    DWORD code=1;GetExitCodeProcess(pi.hProcess,&code);
    ProbeLog(L"[FTP] RunCli site='%s' verb='%s' p1='%s' p2='%s' wait=%lu exit=%u",
             site,verb,p1,p2?p2:L"",wait,code);
    CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return code==0?0:-1;
}

// Single refresh pipeline for EVERY successful remote mutation — right-click
// commands, background menu, drop target (Ctrl+V / drag-drop) all funnel here.
// UI thread only queues work (never touches cache/network/pipe); the worker
// prefetches the fresh listing into the cache and only then notifies the view,
// so the re-enumeration can never land on an empty cache (WinSCP-style).
//
// site+folder are the FULL remote path of the VIEWED directory, passed in
// explicitly by the caller (it already has them) — NEVER re-derived from the
// PIDL here, because PIDL paths lack the site StartPath and a prefetch would
// then LIST a non-existent path (see 53e6639 / 9aa5535 regression chain).
static void AfterRemoteMutation(PCWSTR site, PCWSTR folder, PIDLIST_ABSOLUTE notifyPidl)
{
    if (!notifyPidl) { FtpCacheClear(); return; }
    ProbeLog(L"[MUT] AfterRemoteMutation queue site='%s' path='%s'", site ? site : L"", folder ? folder : L"/");
    FtpRefreshDirBackground(site, folder, notifyPidl);
}

// View directory for a property-page mutation = parent of the mutated path.
static void PathParent(PCWSTR full, PWSTR out, UINT cch)
{
    StringCchCopy(out, cch, (full && full[0]) ? full : L"/");
    WCHAR *slash = wcsrchr(out, L'/');
    if (slash && slash != out) *slash = 0;
    else if (slash == out) out[1] = 0;
}

// WinSCP.com location for "script" custom commands: registry
// (HKCU\Software\ExplorerRemoteFs\WinScpPath, set by the GUI client) -> common
// install paths -> PATH. Supports portable/green WinSCP installs.
static const WCHAR *GetWinScpPath()
{
    static WCHAR s_path[MAX_PATH] = {};
    if (!s_path[0])
    {
        DWORD cb = sizeof(s_path);
        LONG r = RegGetValueW(HKEY_CURRENT_USER, L"Software\\ExplorerRemoteFs", L"WinScpPath",
                              RRF_RT_REG_SZ, NULL, s_path, &cb);
        if (r != ERROR_SUCCESS || !s_path[0])
        {
            s_path[0] = 0;
            static const WCHAR *probes[] = {
                L"C:\\Program Files (x86)\\WinSCP\\WinSCP.com",
                L"C:\\Program Files\\WinSCP\\WinSCP.com",
            };
            for (const WCHAR *p : probes)
            {
                if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES)
                { StringCchCopyW(s_path, ARRAYSIZE(s_path), p); break; }
            }
        }
    }
    return s_path;
}

#define BIT(v) ((v)?BST_CHECKED:BST_UNCHECKED)
typedef struct RemoteMeta {
    WCHAR name[MAX_PATH]; WCHAR type[24]; WCHAR mode[16]; WCHAR owner[40]; WCHAR group[40];
    WCHAR size[80]; WCHAR mtime[32]; DWORD bits; ULONGLONG dwSize; DWORD dwMtime;
    DWORD dwUid; DWORD dwGid;          // 0 = unknown
    BOOL fIsFolder; BOOL fIsSymlink;
} REMOTEMETA;
static DWORD ParseModeFromLine(const WCHAR *fields[])
{
    DWORD mode = 0;
    if (!fields || !fields[1]) return 0;
    const WCHAR *m = fields[1];
    for (int i = 0; i < 9; i++) { if (m[i+1] != L'-') mode |= (0400 >> i); }
    return mode;
}
static void FormatModeString(DWORD mode, BOOL folder, BOOL symlink, PWSTR out, UINT cch)
{
    WCHAR type = symlink ? L'l' : (folder ? L'd' : L'-');
    StringCchPrintf(out, cch, L"%c%c%c%c%c%c%c%c%c%c", type,
        (mode & 0400) ? L'r' : L'-', (mode & 0200) ? L'w' : L'-', (mode & 0100) ? L'x' : L'-',
        (mode & 0040) ? L'r' : L'-', (mode & 0020) ? L'w' : L'-', (mode & 0010) ? L'x' : L'-',
        (mode & 0004) ? L'r' : L'-', (mode & 0002) ? L'w' : L'-', (mode & 0001) ? L'x' : L'-');
}
static void FormatByteCount(ULONGLONG value, PWSTR out, UINT cch)
{
    WCHAR raw[32] = {}; StringCchPrintf(raw, ARRAYSIZE(raw), L"%llu", value);
    UINT digits = lstrlenW(raw), first = digits % 3; if (!first) first = 3;
    std::wstring text(raw, first);
    for (UINT i = first; i < digits; i += 3) { text += L','; text.append(raw + i, 3); }
    StringCchCopyW(out, cch, text.c_str());
}
static void FormatSizeString(ULONGLONG size, BOOL folder, PWSTR out, UINT cch)
{
    if (folder) { StringCchCopy(out, cch, L"-"); return; }
    if (size < 1024) { StringCchPrintf(out, cch, L"%llu B", size); return; }
    static const WCHAR *units[] = { L"KB", L"MB", L"GB", L"TB", L"PB" };
    double shown = (double)size / 1024.0; int unit = 0;
    while (shown >= 1024.0 && unit < 4) { shown /= 1024.0; ++unit; }
    WCHAR exact[40] = {}; FormatByteCount(size, exact, ARRAYSIZE(exact));
    StringCchPrintf(out, cch, L"%.1f %s (%s B)", shown, units[unit], exact);
}
static void FormatMtimeString(DWORD mtime, PWSTR out, UINT cch)
{
    __time64_t t = (__time64_t)mtime; struct tm tmLocal;
    if (_localtime64_s(&tmLocal,&t)==0) StringCchPrintf(out,cch,L"%04d-%02d-%02d %02d:%02d",
        tmLocal.tm_year+1900, tmLocal.tm_mon+1, tmLocal.tm_mday, tmLocal.tm_hour, tmLocal.tm_min);
    else StringCchCopy(out,cch,L"-");
}

static BOOL ReadRemoteMeta(PCWSTR site, PCWSTR folder, PCWSTR name, REMOTEMETA *meta)
{
    if (!name || !name[0]) return FALSE;
    std::vector<FTPENTRY> entries;
    if (!FtpListCachedAll(site, folder, entries)) return FALSE;
    for (auto const &item : entries)
    {
        if (0 != StrCmp(item.szName, name)) continue;
        ZeroMemory(meta, sizeof(*meta));
        StringCchCopy(meta->name, ARRAYSIZE(meta->name), item.szName);
        StringCchCopy(meta->owner, ARRAYSIZE(meta->owner), item.szOwner);
        StringCchCopy(meta->group, ARRAYSIZE(meta->group), item.szGroup);
        meta->bits = item.dwMode;
        meta->fIsFolder = item.fIsFolder;
        meta->fIsSymlink = item.fIsSymlink;
        meta->dwUid = item.dwUid;
        meta->dwGid = item.dwGid;
        WCHAR mode[16];
        FormatModeString(item.dwMode, item.fIsFolder, item.fIsSymlink, mode, ARRAYSIZE(mode));
        StringCchCopy(meta->mode, ARRAYSIZE(meta->mode), mode);
        meta->dwSize = item.dwSize;
        meta->dwMtime = item.dwMtime;
        StringCchCopy(meta->type, ARRAYSIZE(meta->type),
            item.fIsSymlink ? ExplorerText(L"type.symbolic_link", L"符号链接", L"Symbolic Link") : (item.fIsFolder ? ExplorerText(L"type.folder", L"文件夹", L"Folder") : ExplorerText(L"type.file", L"文件", L"File")));
        FormatSizeString(item.dwSize, item.fIsFolder, meta->size, ARRAYSIZE(meta->size));
        FormatMtimeString(item.dwMtime, meta->mtime, ARRAYSIZE(meta->mtime));
        return TRUE;
    }
    return FALSE;
}

// ---- chmod write-back: properties dialog + prompt dialog -------------------

typedef struct {
    REMOTEMETA meta;
    WCHAR site[64];
    WCHAR path[600];
    PIDLIST_ABSOLUTE notify;
    BOOL canSetOwner;
    BOOL modeless;  // heap-owned only for background-menu windows
} PROPMETA;

// FTP/FTPS have no standard owner/group mutation. SFTP can issue SETSTAT;
// whether the server ACL grants it is still confirmed when the user applies it.
static BOOL SiteCanSetOwner(PCWSTR site)
{
    const FTPSITE *s = FtpSiteFind(site);
    return s && 0 == StrCmpIW(s->type, L"sftp");
}

static void PermSetOwnerChangeVisible(HWND hDlg, BOOL visible)
{
    const int controls[] = { 3024, 3025, 3026, 3027 };
    for (int id : controls) ShowWindow(GetDlgItem(hDlg, id), visible ? SW_SHOW : SW_HIDE);
}

static void PermSetChecks(HWND hDlg, DWORD mode)
{
    CheckDlgButton(hDlg,3011,BIT(mode&0400)); CheckDlgButton(hDlg,3012,BIT(mode&0200)); CheckDlgButton(hDlg,3013,BIT(mode&0100));
    CheckDlgButton(hDlg,3014,BIT(mode&0040)); CheckDlgButton(hDlg,3015,BIT(mode&0020)); CheckDlgButton(hDlg,3016,BIT(mode&0010));
    CheckDlgButton(hDlg,3017,BIT(mode&0004)); CheckDlgButton(hDlg,3018,BIT(mode&0002)); CheckDlgButton(hDlg,3019,BIT(mode&0001));
}
static DWORD PermCollectChecks(HWND hDlg)
{
    DWORD mode = 0;
    if(IsDlgButtonChecked(hDlg,3011))mode|=0400; if(IsDlgButtonChecked(hDlg,3012))mode|=0200; if(IsDlgButtonChecked(hDlg,3013))mode|=0100;
    if(IsDlgButtonChecked(hDlg,3014))mode|=0040; if(IsDlgButtonChecked(hDlg,3015))mode|=0020; if(IsDlgButtonChecked(hDlg,3016))mode|=0010;
    if(IsDlgButtonChecked(hDlg,3017))mode|=0004; if(IsDlgButtonChecked(hDlg,3018))mode|=0002; if(IsDlgButtonChecked(hDlg,3019))mode|=0001;
    return mode;
}
static void PermSyncChecksToOctal(HWND hDlg)
{
    WCHAR buf[8]; StringCchPrintf(buf,ARRAYSIZE(buf),L"%03o",PermCollectChecks(hDlg));
    SetDlgItemTextW(hDlg,3022,buf);
}
static void PermSyncOctalToChecks(HWND hDlg)
{
    WCHAR buf[8]; GetDlgItemTextW(hDlg,3022,buf,ARRAYSIZE(buf));
    if(!buf[0]) return;
    WCHAR *end=NULL; long v=wcstol(buf,&end,8);
    if(end==buf || *end) return;                 // not a valid octal number
    if((DWORD)v != PermCollectChecks(hDlg)) PermSetChecks(hDlg,(DWORD)v);
}

// Owner/Group rows show "name [uid]" / "group [gid]"; the edit boxes below
// let the user change the numeric uid/gid (SFTP chown; FTP will fail cleanly).
static void PermInitOwnerGroup(HWND hDlg, const REMOTEMETA *m)
{
    WCHAR u[16] = {}, g[16] = {}, buf[128];
    if (m->dwUid != 0xFFFFFFFF) StringCchPrintf(u, ARRAYSIZE(u), L"%u", m->dwUid);
    if (m->dwGid != 0xFFFFFFFF) StringCchPrintf(g, ARRAYSIZE(g), L"%u", m->dwGid);
    StringCchPrintf(buf, ARRAYSIZE(buf), L"%s [%s]", m->owner[0] ? m->owner : L"-", u[0] ? u : L"-");
    SetDlgItemTextW(hDlg, 3004, buf);
    StringCchPrintf(buf, ARRAYSIZE(buf), L"%s [%s]", m->group[0] ? m->group : L"-", g[0] ? g : L"-");
    SetDlgItemTextW(hDlg, 3005, buf);
    SetDlgItemTextW(hDlg, 3024, u);
    SetDlgItemTextW(hDlg, 3025, g);
}

// Read the uid/gid edit boxes and chown if either differs from current.
// Returns TRUE when a change was submitted (even if it failed) so callers
// can decide whether to refresh.
static void PermApplyChown(HWND hDlg, PROPMETA *pm)
{
    if (!pm->canSetOwner) return;
    WCHAR newU[32] = {}, newG[32] = {}, curU[16] = {}, curG[16] = {};
    GetDlgItemTextW(hDlg, 3024, newU, ARRAYSIZE(newU));
    GetDlgItemTextW(hDlg, 3025, newG, ARRAYSIZE(newG));
    if (pm->meta.dwUid != 0xFFFFFFFF) StringCchPrintf(curU, ARRAYSIZE(curU), L"%u", pm->meta.dwUid);
    if (pm->meta.dwGid != 0xFFFFFFFF) StringCchPrintf(curG, ARRAYSIZE(curG), L"%u", pm->meta.dwGid);
    BOOL changeU = newU[0] && StrCmp(newU, curU) != 0;
    BOOL changeG = newG[0] && StrCmp(newG, curG) != 0;
    if (!changeU && !changeG) return;
    WCHAR spec[64];
    StringCchPrintf(spec, ARRAYSIZE(spec), L"%s:%s", changeU ? newU : L"-", changeG ? newG : L"-");
    if (RunCli(pm->site, L"chown", pm->path, spec, NULL) != 0)
        MessageBoxW(hDlg, ExplorerText(L"error.owner_group_rejected", L"SFTP 服务器拒绝了所有者/组更新。", L"Owner/group update was rejected by the SFTP server."), ExplorerText(L"dialog.remote", L"远程操作", L"Remote"), MB_OK | MB_ICONERROR);
    else
    {
        WCHAR parentDir[512]; PathParent(pm->path, parentDir, ARRAYSIZE(parentDir));
        AfterRemoteMutation(pm->site, parentDir, pm->notify);
    }
}

static PCWSTR PropertyDialogTitle(const REMOTEMETA *meta)
{
    return meta && meta->fIsFolder
        ? ExplorerText(L"property.directory_properties", L"目录属性", L"Directory properties")
        : ExplorerText(L"property.file_properties", L"文件属性", L"File properties");
}
static void LocalizePermissionDialog(HWND hDlg)
{
    SetDlgItemTextW(hDlg, IDC_PROP_NAME, ExplorerText(L"property.name", L"名称：", L"Name:"));
    SetDlgItemTextW(hDlg, IDC_PROP_TYPE, ExplorerText(L"property.type", L"类型：", L"Type:"));
    SetDlgItemTextW(hDlg, IDC_PROP_PERMISSIONS, ExplorerText(L"property.permissions", L"权限：", L"Permissions:"));
    SetDlgItemTextW(hDlg, IDC_PROP_OWNER, ExplorerText(L"property.owner", L"所有者：", L"Owner:"));
    SetDlgItemTextW(hDlg, IDC_PROP_GROUP, ExplorerText(L"property.group", L"组：", L"Group:"));
    SetDlgItemTextW(hDlg, IDC_PROP_SIZE, ExplorerText(L"property.size", L"大小：", L"Size:"));
    SetDlgItemTextW(hDlg, IDC_PROP_MODIFIED, ExplorerText(L"property.modified", L"修改日期：", L"Modified:"));
    SetDlgItemTextW(hDlg, 3026, ExplorerText(L"property.new_owner", L"新所有者 (UID)：", L"New owner (UID):"));
    SetDlgItemTextW(hDlg, 3027, ExplorerText(L"property.new_group", L"新组 (GID)：", L"New group (GID):"));
    SetDlgItemTextW(hDlg, IDC_PROP_PERMISSION_GROUP, ExplorerText(L"label.permissions", L"权限", L"Permissions"));
    SetDlgItemTextW(hDlg, IDC_PROP_OWNER_ROLE, ExplorerText(L"label.owner", L"所有者", L"Owner"));
    SetDlgItemTextW(hDlg, IDC_PROP_GROUP_ROLE, ExplorerText(L"label.group", L"组", L"Group"));
    SetDlgItemTextW(hDlg, IDC_PROP_OTHERS_ROLE, ExplorerText(L"label.others", L"其他", L"Others"));
    SetDlgItemTextW(hDlg, IDC_PROP_OCTAL_LABEL, ExplorerText(L"property.octal", L"八进制：", L"Octal:"));
    SetDlgItemTextW(hDlg, 3023, ExplorerText(L"property.recursive_subdirectories", L"递归应用到子目录", L"Recursive (subdirectories)"));
    const int reads[] = { 3011, 3014, 3017 }, writes[] = { 3012, 3015, 3018 }, execs[] = { 3013, 3016, 3019 };
    for (int id : reads) SetDlgItemTextW(hDlg, id, ExplorerText(L"property.read", L"读取", L"Read"));
    for (int id : writes) SetDlgItemTextW(hDlg, id, ExplorerText(L"property.write", L"写入", L"Write"));
    for (int id : execs) SetDlgItemTextW(hDlg, id, ExplorerText(L"property.execute", L"执行", L"Execute"));
    SetDlgItemTextW(hDlg, IDOK, ExplorerText(L"button.ok", L"确定", L"OK"));
    SetDlgItemTextW(hDlg, IDCANCEL, ExplorerText(L"button.cancel", L"取消", L"Cancel"));
}
static void LocalizeSiteDialog(HWND hDlg)
{
    SetDlgItemTextW(hDlg, IDC_SITE_NAME_LABEL, ExplorerText(L"property.name", L"名称：", L"Name:"));
    SetDlgItemTextW(hDlg, IDC_SITE_HOST_LABEL, ExplorerText(L"column.host", L"主机：", L"Host:"));
    SetDlgItemTextW(hDlg, IDC_SITE_PROTOCOL_LABEL, ExplorerText(L"property.protocol", L"协议：", L"Protocol:"));
    SetDlgItemTextW(hDlg, IDC_SITE_PORT_LABEL, ExplorerText(L"column.port", L"端口：", L"Port:"));
    SetDlgItemTextW(hDlg, IDC_SITE_USER_LABEL, ExplorerText(L"property.user", L"用户：", L"User:"));
    SetDlgItemTextW(hDlg, IDC_SITE_START_PATH_LABEL, ExplorerText(L"property.start_path", L"起始路径：", L"Start path:"));
    SetDlgItemTextW(hDlg, IDCANCEL, ExplorerText(L"button.close", L"关闭", L"Close"));
}
static INT_PTR CALLBACK PermDlgProc(HWND hDlg,UINT msg,WPARAM wp,LPARAM lp)
{
    switch(msg){
    case WM_INITDIALOG:{
        PROPMETA *pm=(PROPMETA*)lp; if(!pm)return TRUE;
        SetWindowLongPtrW(hDlg,DWLP_USER,(LONG_PTR)pm);
        SetWindowTextW(hDlg, PropertyDialogTitle(&pm->meta));
        LocalizePermissionDialog(hDlg);
        REMOTEMETA *m=&pm->meta;
        SetDlgItemTextW(hDlg,3001,m->name); SetDlgItemTextW(hDlg,3002,m->type);
        SetDlgItemTextW(hDlg,3003,m->mode); SetDlgItemTextW(hDlg,3006,m->size); SetDlgItemTextW(hDlg,3007,m->mtime);
        PermInitOwnerGroup(hDlg,m);
        PermSetOwnerChangeVisible(hDlg, pm->canSetOwner);
        PermSetChecks(hDlg,m->bits);
        PermSyncChecksToOctal(hDlg);
        EnableWindow(GetDlgItem(hDlg,3023), m->fIsFolder ? TRUE : FALSE);
        return TRUE;}
    case WM_COMMAND:
        if(HIWORD(wp)==BN_CLICKED && LOWORD(wp)>=3011 && LOWORD(wp)<=3019){ PermSyncChecksToOctal(hDlg); return TRUE; }
        if(HIWORD(wp)==EN_CHANGE && LOWORD(wp)==3022){ PermSyncOctalToChecks(hDlg); return TRUE; }
        if(LOWORD(wp)==IDCANCEL){
            PROPMETA *pm=(PROPMETA*)GetWindowLongPtrW(hDlg,DWLP_USER);
            if(pm && pm->modeless) DestroyWindow(hDlg); else EndDialog(hDlg,IDCANCEL);
            return TRUE;}
        if(LOWORD(wp)==IDOK){
            PROPMETA *pm=(PROPMETA*)GetWindowLongPtrW(hDlg,DWLP_USER);
            if(pm){
                PermSyncOctalToChecks(hDlg);
                DWORD mode = PermCollectChecks(hDlg);
                BOOL recursive = IsDlgButtonChecked(hDlg,3023)!=0;
                if(mode != pm->meta.bits || recursive){
                    WCHAR modeStr[8]; StringCchPrintf(modeStr,ARRAYSIZE(modeStr),L"%03o",mode);
                    if(RunCli(pm->site, recursive?L"chmodr":L"chmod", pm->path, modeStr, NULL)==0){
                        WCHAR parentDir[512]; PathParent(pm->path, parentDir, ARRAYSIZE(parentDir));
                        AfterRemoteMutation(pm->site, parentDir, pm->notify);
                    } else MessageBoxW(hDlg, ExplorerText(L"error.chmod_failed", L"权限修改失败。", L"Permission update failed."), ExplorerText(L"dialog.remote", L"远程操作", L"Remote"), MB_OK|MB_ICONERROR);
                }
                PermApplyChown(hDlg,pm);
                if(pm->modeless) DestroyWindow(hDlg); else EndDialog(hDlg,IDOK);
            }
            return TRUE;}
        break;
    case WM_NCDESTROY:{
        PROPMETA *pm=(PROPMETA*)GetWindowLongPtrW(hDlg,DWLP_USER);
        if(pm && pm->modeless){ if(pm->notify) CoTaskMemFree(pm->notify); CoTaskMemFree(pm); }
        SetWindowLongPtrW(hDlg,DWLP_USER,0);
        break;}
    }
    return FALSE;
}

// Background-menu helpers. The folder PIDL represents the current directory,
// so its metadata is found by listing the parent directory and selecting its leaf.
static void ShowCurrentFolderProperties(HWND hwnd, PCWSTR site, PCWSTR folder)
{
    if (!site || !site[0] || !folder || !folder[0]) return;
    WCHAR full[600] = {}, parent[600] = {}, name[MAX_PATH] = {};
    StringCchCopy(full, ARRAYSIZE(full), folder);
    while (lstrlen(full) > 1 && full[lstrlen(full) - 1] == L'/') full[lstrlen(full) - 1] = L'\0';
    WCHAR *slash = wcsrchr(full, L'/');
    if (!slash || !slash[1]){
        MessageBoxW(hwnd, ExplorerText(L"info.root_no_parent", L"远程根目录没有可用于读取 Unix 元数据的父目录项。", L"The remote root has no parent entry from which to read Unix metadata."), ExplorerText(L"property.directory_properties", L"目录属性", L"Directory properties"), MB_OK | MB_ICONINFORMATION); return; }
    StringCchCopy(name, ARRAYSIZE(name), slash + 1);
    if (slash == full) StringCchCopy(parent, ARRAYSIZE(parent), L"/");
    else { *slash = L'\0'; StringCchCopy(parent, ARRAYSIZE(parent), full); }

    PROPMETA *pm=(PROPMETA*)CoTaskMemAlloc(sizeof(*pm));
    if(!pm) return;
    ZeroMemory(pm,sizeof(*pm));
    StringCchCopy(pm->site, ARRAYSIZE(pm->site), site);
    StringCchCopy(pm->path, ARRAYSIZE(pm->path), folder);
    pm->canSetOwner = SiteCanSetOwner(site);
    pm->modeless = TRUE;
    if (!ReadRemoteMeta(site, parent, name, &pm->meta)){
        CoTaskMemFree(pm);
        MessageBoxW(hwnd, ExplorerText(L"info.directory_metadata_unavailable", L"无法获取当前目录的元数据。", L"Unable to retrieve current directory metadata."), ExplorerText(L"property.directory_properties", L"目录属性", L"Directory properties"), MB_OK | MB_ICONINFORMATION); return; }
    HWND dlg=CreateDialogParamW(g_hInst, MAKEINTRESOURCEW(IDD_PERMBOX), hwnd, PermDlgProc, (LPARAM)pm);
    if(dlg){ ShowWindow(dlg,SW_SHOWNORMAL); SetForegroundWindow(dlg); }
    else CoTaskMemFree(pm);
}

static void PopulateSiteInfo(HWND hDlg, PCWSTR site)
{
    const FTPSITE *s = FtpSiteFind(site);
    WCHAR buf[64] = {};
    SetDlgItemTextW(hDlg, 4001, (s && s->name[0]) ? s->name : site);
    SetDlgItemTextW(hDlg, 4002, s ? s->host : L"");
    SetDlgItemTextW(hDlg, 4003, s ? s->type : L"");
    if (s) StringCchPrintf(buf, ARRAYSIZE(buf), L"%d", s->port);
    SetDlgItemTextW(hDlg, 4004, buf);
    SetDlgItemTextW(hDlg, 4005, s ? s->user : L"");
    SetDlgItemTextW(hDlg, 4006, s ? s->startPath : L"");
}
typedef struct { WCHAR site[64]; } SITEINFOCTX;
static INT_PTR CALLBACK SiteInfoDlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    if(msg==WM_INITDIALOG){ SITEINFOCTX *ctx=(SITEINFOCTX*)lp; if(!ctx)return FALSE; SetWindowLongPtrW(hDlg,DWLP_USER,(LONG_PTR)ctx); SetWindowTextW(hDlg, ExplorerText(L"property.site_properties", L"站点属性", L"Site properties")); LocalizeSiteDialog(hDlg); PopulateSiteInfo(hDlg,ctx->site); return TRUE; }
    if(msg==WM_COMMAND && (LOWORD(wp)==IDCANCEL || LOWORD(wp)==IDOK)){ DestroyWindow(hDlg); return TRUE; }
    if(msg==WM_NCDESTROY){ SITEINFOCTX *ctx=(SITEINFOCTX*)GetWindowLongPtrW(hDlg,DWLP_USER); if(ctx)CoTaskMemFree(ctx); SetWindowLongPtrW(hDlg,DWLP_USER,0); }
    return FALSE;
}
static void ShowCurrentSiteInfo(HWND hwnd, PCWSTR site)
{
    if (!FtpSiteFind(site)){
        MessageBoxW(hwnd, ExplorerText(L"info.site_configuration_unavailable", L"当前站点配置已不可用。", L"The current site configuration is no longer available."), ExplorerText(L"property.site_properties", L"站点属性", L"Site properties"), MB_OK | MB_ICONINFORMATION); return; }
    SITEINFOCTX *ctx=(SITEINFOCTX*)CoTaskMemAlloc(sizeof(*ctx));
    if(!ctx)return;
    ZeroMemory(ctx,sizeof(*ctx)); StringCchCopy(ctx->site,ARRAYSIZE(ctx->site),site);
    HWND dlg=CreateDialogParamW(g_hInst,MAKEINTRESOURCEW(IDD_SITEBOX),hwnd,SiteInfoDlgProc,(LPARAM)ctx);
    if(dlg){ ShowWindow(dlg,SW_SHOWNORMAL); SetForegroundWindow(dlg); } else CoTaskMemFree(ctx);
}
typedef struct { WCHAR* buf; UINT cch; PCWSTR caption; PCWSTR initial; } PROMPTCTX;
static INT_PTR CALLBACK NameDlgProc(HWND h,UINT m,WPARAM w,LPARAM l)
{
    if(m==WM_INITDIALOG){PROMPTCTX*c=(PROMPTCTX*)l;SetWindowLongPtrW(h,DWLP_USER,(LONG_PTR)c);SetWindowTextW(h,c->caption);SetDlgItemTextW(h,3030,ExplorerText(L"property.name",L"名称：",L"Name:"));SetDlgItemTextW(h,IDOK,ExplorerText(L"button.ok",L"确定",L"OK"));SetDlgItemTextW(h,IDCANCEL,ExplorerText(L"button.cancel",L"取消",L"Cancel"));SetDlgItemTextW(h,3101,c->initial);return TRUE;}
    if(m==WM_COMMAND&&(LOWORD(w)==IDOK||LOWORD(w)==IDCANCEL)){
        if(LOWORD(w)==IDOK){PROMPTCTX*c=(PROMPTCTX*)GetWindowLongPtrW(h,DWLP_USER);GetDlgItemTextW(h,3101,c->buf,c->cch);}
        EndDialog(h,LOWORD(w));return TRUE;}
    return FALSE;
}
static BOOL PromptText(HWND parent, PCWSTR caption, PWSTR buf, UINT cch, PCWSTR initial)
{
    PROMPTCTX c={buf,cch,caption,initial?initial:L""};
    return DialogBoxParamW(g_hInst,MAKEINTRESOURCEW(IDD_NAMEBOX),parent,NameDlgProc,(LPARAM)&c)==IDOK && buf[0];
}

// ---- selection (multi-select aware) -----------------------------------------

typedef struct {
    WCHAR site[64];
    WCHAR folder[512];
    WCHAR names[MAX_SEL][256];
    int count;
    BOOL firstIsFolder;
    PIDLIST_ABSOLUTE notify;
} SELDATA;

static BOOL CollectSelection(IDataObject *data, SELDATA *out)
{
    out->count=0; out->site[0]=0; out->folder[0]=0; out->firstIsFolder=FALSE; out->notify=NULL;
    if(!data) return FALSE;
    FORMATETC f={(CLIPFORMAT)RegisterClipboardFormatW(CFSTR_SHELLIDLIST),NULL,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};
    STGMEDIUM st={};
    if(FAILED(data->GetData(&f,&st))) return FALSE;
    CIDA *cida=(CIDA*)GlobalLock(st.hGlobal);
    BOOL ok=FALSE;
    if(cida && cida->cidl>0){
        PCIDLIST_ABSOLUTE parent=(PCIDLIST_ABSOLUTE)((BYTE*)cida+cida->aoffset[0]);
        PidlSite(parent,out->site,ARRAYSIZE(out->site));
        PidlPath(parent,out->folder,ARRAYSIZE(out->folder));
        ApplySiteStartPath(out->site, out->folder, ARRAYSIZE(out->folder));
        out->notify=ILCloneFull(parent);
        for(UINT i=1;i<=cida->cidl && out->count<MAX_SEL;i++){
            PCUIDLIST_RELATIVE child=(PCUIDLIST_RELATIVE)((BYTE*)cida+cida->aoffset[i]);
            if(IsOurs(child)){ const COMPACTITEM *item=(const COMPACTITEM*)child; if(out->count==0) out->firstIsFolder=item->fIsFolder; CopyName(item,out->names[out->count],ARRAYSIZE(out->names[0])); out->count++; }
        }
        // Site-picker items (level 0) have no site segment in the folder PIDL;
        // accept a non-empty selection regardless so their property sheet works.
        ok = out->count>0;
    }
    if(cida) GlobalUnlock(st.hGlobal);
    ReleaseStgMedium(&st);
    if(!ok && out->notify){ CoTaskMemFree(out->notify); out->notify=NULL; }
    return ok;
}

// ---- temp staging for open / edit / download / clipboard --------------------

static BOOL GetConfiguredDirectory(PCWSTR valueName, PCWSTR fallbackLeaf, PWSTR out, UINT cch)
{
    DWORD cb = cch * sizeof(WCHAR); out[0] = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\ExplorerRemoteFs", valueName, RRF_RT_REG_SZ, NULL, out, &cb) != ERROR_SUCCESS || !out[0]) {
        WCHAR local[MAX_PATH] = {}; if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, local))) return FALSE;
        if (FAILED(StringCchPrintfW(out, cch, L"%s\\ExplorerRemoteFs\\%s", local, fallbackLeaf))) return FALSE;
    }
    if (!PathIsDirectoryW(out) && ERROR_SUCCESS != SHCreateDirectoryExW(NULL, out, NULL) && !PathIsDirectoryW(out)) return FALSE;
    return PathIsDirectoryW(out);
}
static BOOL TempDir(PCWSTR sub, PCWSTR site, PWSTR out, UINT cch)
{
    WCHAR root[MAX_PATH] = {}; if (!GetConfiguredDirectory(L"FileCachePath", L"FileCache", root, ARRAYSIZE(root))) return FALSE;
    if (FAILED(StringCchPrintfW(out, cch, L"%s\\%s\\%s\\", root, sub, site))) return FALSE;
    if(!PathIsDirectoryW(out)) SHCreateDirectoryExW(NULL,out,NULL);
    return PathIsDirectoryW(out);
}
static BOOL TempLocalPath(PCWSTR sub, PCWSTR site, PCWSTR name, PWSTR out, UINT cch)
{
    if(!TempDir(sub,site,out,cch)) return FALSE;
    StringCchCat(out,cch,name);
    return TRUE;
}
static void CleanupDir(PCWSTR dir)
{
    // Best-effort delete of temp staging dir contents (files only, no recursion depth 1).
    WCHAR pat[MAX_PATH]; StringCchPrintf(pat,ARRAYSIZE(pat),L"%s*",dir);
    WIN32_FIND_DATAW fd={}; HANDLE h=FindFirstFileW(pat,&fd);
    if(h!=INVALID_HANDLE_VALUE){
        do { if(!(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)){ WCHAR p[MAX_PATH]; StringCchPrintf(p,ARRAYSIZE(p),L"%s%s",dir,fd.cFileName); DeleteFileW(p);} }
        while(FindNextFileW(h,&fd)); FindClose(h); }
}

// Edit watcher: poll local mtime; on change, upload back to the server.
typedef struct { WCHAR site[64]; WCHAR remote[600]; WCHAR local[MAX_PATH]; } EDITCTX;
static DWORD WINAPI EditWatch(LPVOID p)
{
    EDITCTX *c=(EDITCTX*)p;
    HANDLE h=CreateFileW(c->local,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    FILETIME last={}; if(h!=INVALID_HANDLE_VALUE){ GetFileTime(h,NULL,NULL,&last); CloseHandle(h); }
    ULONGLONG lastT=((ULONGLONG)last.dwHighDateTime<<32)|last.dwLowDateTime;
    ULONGLONG start=GetTickCount64();
    while(GetTickCount64()-start < 30*60*1000){
        Sleep(3000);
        if(!PathFileExistsW(c->local)) break;   // editor deleted temp file / closed
        HANDLE h2=CreateFileW(c->local,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
        if(h2==INVALID_HANDLE_VALUE) break;
        FILETIME now={}; GetFileTime(h2,NULL,NULL,&now); CloseHandle(h2);
        ULONGLONG nowT=((ULONGLONG)now.dwHighDateTime<<32)|now.dwLowDateTime;
        if(nowT!=lastT && nowT!=0){
            lastT=nowT;
            // small settle delay, then upload
            Sleep(1200);
            RunCli(c->site,L"put",c->local,c->remote,NULL);
            FtpCacheClear();
        }
    }
    // File finished editing; upload a final time if it changed after the loop.
    RunCli(c->site,L"put",c->local,c->remote,NULL);
    FtpCacheClear();
    DeleteFileW(c->local);
    CoTaskMemFree(c);   // allocated via CoTaskMemAlloc
    return 0;
}
static void OpenRemote(HWND hwnd, PCWSTR site, PCWSTR folder, PCWSTR name, BOOL edit)
{
    WCHAR full[700]; JoinPath(folder,name,full,ARRAYSIZE(full));
    WCHAR local[MAX_PATH];
    if(!TempLocalPath(L"Open",site,name,local,ARRAYSIZE(local))) return;
    if(RunCli(site,L"get",full,local,NULL)!=0){ MessageBoxW(hwnd,ExplorerText(L"error.download_failed",L"下载失败。",L"Download failed."),ExplorerText(L"dialog.remote",L"远程操作",L"Remote"),MB_OK|MB_ICONERROR); return; }
    HINSTANCE hr=ShellExecuteW(hwnd,edit?L"open":L"open",local,NULL,NULL,SW_SHOWNORMAL);
    if((INT_PTR)hr<=32){ MessageBoxW(hwnd,ExplorerText(L"error.open_failed",L"打开失败。",L"Open failed."),ExplorerText(L"dialog.remote",L"远程操作",L"Remote"),MB_OK|MB_ICONERROR); DeleteFileW(local); return; }
    if(edit){
        EDITCTX *c=(EDITCTX*)CoTaskMemAlloc(sizeof(EDITCTX));
        if(c){ StringCchCopy(c->site,ARRAYSIZE(c->site),site); StringCchCopy(c->remote,ARRAYSIZE(c->remote),full);
               StringCchCopy(c->local,ARRAYSIZE(c->local),local);
               CloseHandle(CreateThread(NULL,0,EditWatch,c,0,NULL)); }
    }
}
static void DownloadFiles(HWND hwnd, PCWSTR site, PCWSTR folder, PCWSTR *names, int count)
{
    WCHAR dir[MAX_PATH];
    if(!TempDir(L"Download",site,dir,ARRAYSIZE(dir))) return;
    // Save dialog for single file
    if(count==1){
        WCHAR local[MAX_PATH]; StringCchPrintf(local,ARRAYSIZE(local),L"%s%s",dir,names[0]);
        WCHAR filter[64] = {}; StringCchPrintf(filter, ARRAYSIZE(filter), L"%s%c*.*%c", ExplorerText(L"filter.all_files", L"所有文件", L"All files"), 0, 0);
        OPENFILENAMEW ofn={sizeof(ofn)}; ofn.hwndOwner=hwnd; ofn.lpstrFilter=filter;
        ofn.lpstrFile=local; ofn.nMaxFile=ARRAYSIZE(local); ofn.Flags=OFN_OVERWRITEPROMPT; ofn.lpstrTitle=ExplorerText(L"dialog.download_to",L"下载到",L"Download to");
        if(!GetSaveFileNameW(&ofn)) return;
        WCHAR full[700]; JoinPath(folder,names[0],full,ARRAYSIZE(full));
        if(RunCli(site,L"get",full,local,NULL)!=0) MessageBoxW(hwnd,ExplorerText(L"error.download_failed",L"下载失败。",L"Download failed."),ExplorerText(L"dialog.remote",L"远程操作",L"Remote"),MB_OK|MB_ICONERROR);
        return;
    }
    // Multiple: save into the temp folder (already keyed by site).
    for(int i=0;i<count;i++){
        WCHAR full[700]; JoinPath(folder,names[i],full,ARRAYSIZE(full));
        WCHAR local[MAX_PATH]; StringCchPrintf(local,ARRAYSIZE(local),L"%s%s",dir,names[i]);
        RunCli(site,L"get",full,local,NULL);
    }
    WCHAR msg[512]; StringCchPrintf(msg,ARRAYSIZE(msg),ExplorerText(L"info.downloaded_to",L"已下载 %d 个文件到：\n%s",L"Downloaded %d files to:\n%s"),count,dir);
    MessageBoxW(hwnd,msg,ExplorerText(L"dialog.remote",L"远程操作",L"Remote"),MB_OK|MB_ICONINFORMATION);
}
static void CopyClipboard(HWND hwnd, PCWSTR site, PCWSTR folder, PCWSTR *names, int count)
{
    WCHAR dir[MAX_PATH];
    if(!TempDir(L"Clip",site,dir,ARRAYSIZE(dir))) return;
    CleanupDir(dir);
    // Download each file into temp dir; only successful downloads are offered to Explorer.
    PCWSTR *paths=(PCWSTR*)CoTaskMemAlloc(sizeof(PCWSTR)*count);
    if(!paths) return;
    WCHAR *buf=(WCHAR*)CoTaskMemAlloc(sizeof(WCHAR)*MAX_PATH*count);
    if(!buf){ CoTaskMemFree(paths); return; }
    int copied=0;
    for(int i=0;i<count;i++){
        WCHAR *local=&buf[copied*MAX_PATH];
        StringCchPrintf(local,MAX_PATH,L"%s%s",dir,names[i]);
        WCHAR full[700]; JoinPath(folder,names[i],full,ARRAYSIZE(full));
        if(RunCli(site,L"get",full,local,NULL)!=0) continue;
        paths[copied++]=local;
    }
    if(!copied){
        MessageBoxW(hwnd,ExplorerText(L"error.copy_to_clipboard_failed",L"无法下载选中的项目，未复制到剪贴板。",L"The selected item(s) could not be downloaded, so nothing was copied to the clipboard."),ExplorerText(L"dialog.remote",L"远程操作",L"Remote"),MB_OK|MB_ICONERROR);
        CoTaskMemFree(paths); CoTaskMemFree(buf); return;
    }
    // Build CF_HDROP.
    SIZE_T sz=sizeof(DROPFILES)+2;
    for(int i=0;i<copied;i++) sz+=(wcslen(paths[i])+1)*sizeof(WCHAR);
    HGLOBAL h=GlobalAlloc(GMEM_MOVEABLE,sz);
    if(h){
        DROPFILES *df=(DROPFILES*)GlobalLock(h);
        df->pFiles=sizeof(DROPFILES); df->fWide=TRUE; df->pt.x=0; df->pt.y=0;
        WCHAR *p=(WCHAR*)((BYTE*)df+sizeof(DROPFILES));
        for(int i=0;i<copied;i++){ StringCchCopy(p,(sz-((BYTE*)p-(BYTE*)df))/2,paths[i]); p+=wcslen(paths[i])+1; }
        *p=0;
        GlobalUnlock(h);
        if(OpenClipboard(hwnd)){ EmptyClipboard(); SetClipboardData(CF_HDROP,h); CloseClipboard(); }
        else GlobalFree(h);
    }
    CoTaskMemFree(paths); CoTaskMemFree(buf);
}
enum COPYTARGET { COPY_ORIGINAL = 0, COPY_SAME_SITE = 1, COPY_OTHER_SITE = 2, COPY_LOCAL_FOLDER = 3 };
typedef struct {
    WCHAR sourceSite[64]; WCHAR sourceFolder[600]; PCWSTR *names; int count;
    COPYTARGET target; WCHAR targetSite[64]; WCHAR targetPath[700];
} COPYCTX;
static void CopyDialogSetTarget(HWND hDlg, COPYCTX *ctx, COPYTARGET target)
{
    CheckRadioButton(hDlg, IDC_COPY_ORIGINAL, IDC_COPY_LOCAL, IDC_COPY_ORIGINAL + (int)target);
    BOOL needsPath = target != COPY_ORIGINAL, needsSite = target == COPY_OTHER_SITE, isLocal = target == COPY_LOCAL_FOLDER;
    EnableWindow(GetDlgItem(hDlg, IDC_COPY_SITE_LABEL), needsSite);
    EnableWindow(GetDlgItem(hDlg, IDC_COPY_SITE), needsSite);
    EnableWindow(GetDlgItem(hDlg, IDC_COPY_PATH_LABEL), needsPath);
    EnableWindow(GetDlgItem(hDlg, IDC_COPY_PATH), needsPath);
    EnableWindow(GetDlgItem(hDlg, IDC_COPY_BROWSE), isLocal);
    if (target == COPY_SAME_SITE) SetDlgItemTextW(hDlg, IDC_COPY_PATH, ctx->sourceFolder);
    else if (target == COPY_OTHER_SITE) SetDlgItemTextW(hDlg, IDC_COPY_PATH, L"/");
    else if (target == COPY_LOCAL_FOLDER) {
        WCHAR profile[MAX_PATH] = {}; DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, ARRAYSIZE(profile));
        SetDlgItemTextW(hDlg, IDC_COPY_PATH, n && n < ARRAYSIZE(profile) ? profile : L"");
    }
}
static void CopyDialogBrowseLocal(HWND hDlg)
{
    BROWSEINFOW bi = {}; bi.hwndOwner = hDlg; bi.lpszTitle = ExplorerText(L"dialog.select_local_folder", L"选择本地目标文件夹", L"Select local destination folder");
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&bi);
    if (!id) return;
    WCHAR path[MAX_PATH] = {}; if (SHGetPathFromIDListW(id, path)) SetDlgItemTextW(hDlg, IDC_COPY_PATH, path);
    CoTaskMemFree(id);
}
static INT_PTR CALLBACK CopyDlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    COPYCTX *ctx = (COPYCTX *)GetWindowLongPtrW(hDlg, DWLP_USER);
    switch (msg) {
    case WM_INITDIALOG: {
        ctx = (COPYCTX *)lp; SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)ctx);
        SetWindowTextW(hDlg, ExplorerText(L"dialog.copy_to", L"复制到...", L"Copy to..."));
        SetDlgItemTextW(hDlg, IDC_COPY_SOURCE_LABEL, ExplorerText(L"copy.source_path", L"原文件路径：", L"Source remote path:"));
        SetDlgItemTextW(hDlg, IDC_COPY_DESTINATION, ExplorerText(L"copy.destination", L"目标位置", L"Destination"));
        SetDlgItemTextW(hDlg, IDC_COPY_ORIGINAL, ExplorerText(L"copy.original", L"原地生成副本", L"Create a copy in the original folder"));
        SetDlgItemTextW(hDlg, IDC_COPY_SAME_SITE, ExplorerText(L"copy.same_site", L"同站点路径", L"Same site"));
        SetDlgItemTextW(hDlg, IDC_COPY_OTHER_SITE, ExplorerText(L"copy.other_site", L"异站点路径", L"Other site"));
        SetDlgItemTextW(hDlg, IDC_COPY_LOCAL, ExplorerText(L"copy.local", L"本地路径", L"Local folder"));
        SetDlgItemTextW(hDlg, IDC_COPY_SITE_LABEL, ExplorerText(L"copy.target_site", L"目标站点：", L"Target site:"));
        SetDlgItemTextW(hDlg, IDC_COPY_PATH_LABEL, ExplorerText(L"copy.target_path", L"目标路径：", L"Target path:"));
        SetDlgItemTextW(hDlg, IDC_COPY_BROWSE, ExplorerText(L"button.browse", L"浏览...", L"Browse..."));
        SetDlgItemTextW(hDlg, IDOK, ExplorerText(L"button.ok", L"确定", L"OK")); SetDlgItemTextW(hDlg, IDCANCEL, ExplorerText(L"button.cancel", L"取消", L"Cancel"));
        WCHAR source[1200] = {}; if (ctx->count == 1) { WCHAR full[700] = {}; JoinPath(ctx->sourceFolder, ctx->names[0], full, ARRAYSIZE(full)); StringCchPrintf(source, ARRAYSIZE(source), L"%s:%s", ctx->sourceSite, full); }
        else StringCchPrintf(source, ARRAYSIZE(source), ExplorerText(L"copy.multiple_source", L"%s:%s（已选择 %d 项）", L"%s:%s (%d items selected)"), ctx->sourceSite, ctx->sourceFolder, ctx->count);
        SetDlgItemTextW(hDlg, IDC_COPY_SOURCE, source);
        FTPSITE sites[64] = {}; int n = FtpSitesGet(sites, ARRAYSIZE(sites));
        for (int i = 0; i < n; ++i) if (StrCmpI(sites[i].name, ctx->sourceSite) != 0) SendDlgItemMessageW(hDlg, IDC_COPY_SITE, CB_ADDSTRING, 0, (LPARAM)sites[i].name);
        if (SendDlgItemMessageW(hDlg, IDC_COPY_SITE, CB_GETCOUNT, 0, 0) > 0) SendDlgItemMessageW(hDlg, IDC_COPY_SITE, CB_SETCURSEL, 0, 0);
        CopyDialogSetTarget(hDlg, ctx, COPY_ORIGINAL); return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_COPY_ORIGINAL: case IDC_COPY_SAME_SITE: case IDC_COPY_OTHER_SITE: case IDC_COPY_LOCAL:
            if (HIWORD(wp) == BN_CLICKED) { CopyDialogSetTarget(hDlg, ctx, (COPYTARGET)(LOWORD(wp) - IDC_COPY_ORIGINAL)); return TRUE; } break;
        case IDC_COPY_BROWSE: if (HIWORD(wp) == BN_CLICKED) { CopyDialogBrowseLocal(hDlg); return TRUE; } break;
        case IDOK:
            if (IsDlgButtonChecked(hDlg, IDC_COPY_SAME_SITE)) ctx->target = COPY_SAME_SITE;
            else if (IsDlgButtonChecked(hDlg, IDC_COPY_OTHER_SITE)) ctx->target = COPY_OTHER_SITE;
            else if (IsDlgButtonChecked(hDlg, IDC_COPY_LOCAL)) ctx->target = COPY_LOCAL_FOLDER;
            else ctx->target = COPY_ORIGINAL;
            if (ctx->target == COPY_OTHER_SITE) {
                int sel = (int)SendDlgItemMessageW(hDlg, IDC_COPY_SITE, CB_GETCURSEL, 0, 0);
                if (sel == CB_ERR) { MessageBoxW(hDlg, ExplorerText(L"error.copy_other_site_required", L"请选择目标站点。", L"Select a target site."), ExplorerText(L"dialog.copy_to", L"复制到...", L"Copy to..."), MB_OK | MB_ICONWARNING); return TRUE; }
                SendDlgItemMessageW(hDlg, IDC_COPY_SITE, CB_GETLBTEXT, sel, (LPARAM)ctx->targetSite);
            } else StringCchCopy(ctx->targetSite, ARRAYSIZE(ctx->targetSite), ctx->sourceSite);
            if (ctx->target != COPY_ORIGINAL) GetDlgItemTextW(hDlg, IDC_COPY_PATH, ctx->targetPath, ARRAYSIZE(ctx->targetPath));
            if (ctx->target == COPY_SAME_SITE || ctx->target == COPY_OTHER_SITE) {
                if (ctx->targetPath[0] != L'/') { MessageBoxW(hDlg, ExplorerText(L"error.remote_path_required", L"远程目标路径必须以 / 开头。", L"The remote target path must start with /."), ExplorerText(L"dialog.copy_to", L"复制到...", L"Copy to..."), MB_OK | MB_ICONWARNING); return TRUE; }
            }
            if (ctx->target == COPY_LOCAL_FOLDER && !PathIsDirectoryW(ctx->targetPath)) { MessageBoxW(hDlg, ExplorerText(L"error.local_folder_required", L"请选择存在的本地文件夹。", L"Select an existing local folder."), ExplorerText(L"dialog.copy_to", L"复制到...", L"Copy to..."), MB_OK | MB_ICONWARNING); return TRUE; }
            EndDialog(hDlg, IDOK); return TRUE;
        case IDCANCEL: EndDialog(hDlg, IDCANCEL); return TRUE;
        } break;
    }
    return FALSE;
}
static BOOL PromptCopyTarget(HWND hwnd, COPYCTX *ctx)
{
    return DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_COPYBOX), hwnd, CopyDlgProc, (LPARAM)ctx) == IDOK;
}
static BOOL CopyToRemoteFolder(PCWSTR site, PCWSTR sourceFolder, PCWSTR name, PCWSTR targetFolder, BOOL duplicate)
{
    WCHAR src[700] = {}, dst[700] = {}, targetName[MAX_PATH] = {};
    JoinPath(sourceFolder, name, src, ARRAYSIZE(src));
    if (duplicate) StringCchPrintf(targetName, ARRAYSIZE(targetName), ExplorerText(L"name.copy_suffix", L"%s - 副本", L"%s - Copy"), name);
    else StringCchCopy(targetName, ARRAYSIZE(targetName), name);
    if (targetFolder[0] == L'/' && !targetFolder[1]) StringCchPrintf(dst, ARRAYSIZE(dst), L"/%s", targetName);
    else StringCchPrintf(dst, ARRAYSIZE(dst), L"%s/%s", targetFolder, targetName);
    return RunCli(site, L"dup", src, dst, NULL) == 0;
}
static void ServerCopy(HWND hwnd, PCWSTR site, PCWSTR folder, PCWSTR *names, int count, PIDLIST_ABSOLUTE notifyPidl)
{
    COPYCTX ctx = {}; StringCchCopy(ctx.sourceSite, ARRAYSIZE(ctx.sourceSite), site); StringCchCopy(ctx.sourceFolder, ARRAYSIZE(ctx.sourceFolder), folder); ctx.names = names; ctx.count = count;
    if (!PromptCopyTarget(hwnd, &ctx)) return;
    BOOL ok = TRUE;
    for (int i = 0; i < count; ++i) {
        WCHAR src[700] = {}; JoinPath(folder, names[i], src, ARRAYSIZE(src));
        if (ctx.target == COPY_ORIGINAL || ctx.target == COPY_SAME_SITE) {
            if (!CopyToRemoteFolder(site, folder, names[i], ctx.target == COPY_ORIGINAL ? folder : ctx.targetPath, ctx.target == COPY_ORIGINAL || StrCmpI(folder, ctx.targetPath) == 0)) ok = FALSE;
            continue;
        }
        REMOTEMETA meta = {}; if (!ReadRemoteMeta(site, folder, names[i], &meta) || meta.fIsFolder) { ok = FALSE; continue; }
        WCHAR local[MAX_PATH] = {}; if (!TempLocalPath(L"Copy", site, names[i], local, ARRAYSIZE(local)) || RunCli(site, L"get", src, local, NULL) != 0) { ok = FALSE; continue; }
        if (ctx.target == COPY_OTHER_SITE) {
            WCHAR remote[700] = {}; if (ctx.targetPath[0] == L'/' && !ctx.targetPath[1]) StringCchPrintf(remote, ARRAYSIZE(remote), L"/%s", names[i]); else StringCchPrintf(remote, ARRAYSIZE(remote), L"%s/%s", ctx.targetPath, names[i]);
            if (RunCli(ctx.targetSite, L"put", local, remote, NULL) != 0) ok = FALSE;
        } else {
            WCHAR localTarget[MAX_PATH] = {}; StringCchPrintf(localTarget, ARRAYSIZE(localTarget), L"%s\\%s", ctx.targetPath, names[i]);
            if (PathFileExistsW(localTarget) && IDYES != MessageBoxW(hwnd, ExplorerText(L"confirm.overwrite_local", L"目标位置已有同名文件，要覆盖吗？", L"A file with the same name already exists. Replace it?"), ExplorerText(L"dialog.copy_to", L"复制到...", L"Copy to..."), MB_YESNO | MB_ICONWARNING)) { DeleteFileW(local); continue; }
            if (!CopyFileW(local, localTarget, FALSE)) ok = FALSE;
        }
        DeleteFileW(local);
    }
    if (!ok) MessageBoxW(hwnd, ExplorerText(L"error.copy_failed", L"部分项目复制失败。跨站点和本地复制目前仅支持文件。", L"Some items could not be copied. Cross-site and local copies currently support files only."), ExplorerText(L"dialog.remote", L"远程操作", L"Remote"), MB_OK | MB_ICONERROR);
    else AfterRemoteMutation(site, folder, notifyPidl);
}
static void ServerMove(HWND hwnd, PCWSTR site, PCWSTR folder, PCWSTR *names, int count, PIDLIST_ABSOLUTE notifyPidl)
{
    // Move all selected items to a target directory (WinSCP "Move to").
    WCHAR dst[700]; StringCchCopy(dst,ARRAYSIZE(dst),folder && folder[0] ? folder : L"/");
    if(!PromptText(hwnd,ExplorerText(L"dialog.move_to",L"移动到...（目标远程目录）",L"Move to... (target remote directory)"),dst,ARRAYSIZE(dst),dst)) return;
    // strip trailing slash for joining
    if(dst[0] && dst[wcslen(dst)-1]==L'/') dst[wcslen(dst)-1]=0;
    if(!dst[0]) StringCchCopy(dst,ARRAYSIZE(dst),L"/");
    BOOL ok = TRUE;
    for (int i = 0; i < count; i++)
    {
        WCHAR src[700]; JoinPath(folder,names[i],src,ARRAYSIZE(src));
        WCHAR target[700];
        if(dst[0]==L'/' && !dst[1]) StringCchPrintf(target,ARRAYSIZE(target),L"/%s",names[i]);
        else StringCchPrintf(target,ARRAYSIZE(target),L"%s/%s",dst,names[i]);
        if(RunCli(site,L"rename",src,target,NULL)!=0) ok=FALSE;
    }
    if(!ok) MessageBoxW(hwnd,ExplorerText(L"error.move_failed",L"移动失败。",L"Move failed."),ExplorerText(L"dialog.remote",L"远程操作",L"Remote"),MB_OK|MB_ICONERROR);
    else AfterRemoteMutation(site, folder, notifyPidl);
}
static void DoRename(HWND hwnd, PCWSTR site, PCWSTR folder, PCWSTR name, PIDLIST_ABSOLUTE notifyPidl)
{
    WCHAR newName[256]; StringCchCopy(newName,ARRAYSIZE(newName),name);
    if(!PromptText(hwnd,ExplorerText(L"dialog.rename",L"重命名",L"Rename"),newName,ARRAYSIZE(newName),newName)) return;
    if(0==StrCmp(newName,name)) return;
    WCHAR src[700],dst[700]; JoinPath(folder,name,src,ARRAYSIZE(src)); JoinPath(folder,newName,dst,ARRAYSIZE(dst));
    if(RunCli(site,L"rename",src,dst,NULL)!=0) MessageBoxW(hwnd,ExplorerText(L"error.rename_failed",L"重命名失败。",L"Rename failed."),ExplorerText(L"dialog.remote",L"远程操作",L"Remote"),MB_OK|MB_ICONERROR);
    else AfterRemoteMutation(site, folder, notifyPidl);
}

// ---- custom commands ---------------------------------------------------------
// %APPDATA%\ExplorerRemoteFs\custom-commands.json
// [ { "name": "...", "type": "local" | "script", "command": "..." } ]
// placeholders in command: {path} {name} {site} {full}
typedef struct { WCHAR name[128]; WCHAR type[16]; WCHAR command[1200]; } CUSTCMD;
static int LoadCustomCommands(CUSTCMD *out, int max)
{
    int n=0;
    WCHAR file[MAX_PATH];
    if(FAILED(SHGetFolderPathW(NULL,CSIDL_APPDATA,NULL,0,file))) return 0;
    StringCchCatW(file,MAX_PATH,L"\\ExplorerRemoteFs\\custom-commands.json");
    HANDLE h=CreateFileW(file,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
    if(h==INVALID_HANDLE_VALUE) return 0;
    std::string text; char buf[4096]; DWORD got=0;
    while(ReadFile(h,buf,sizeof(buf),&got,NULL)&&got) text.append(buf,got);
    CloseHandle(h);
    size_t pos=0;
    while(n<max && (pos=text.find('{',pos))!=std::string::npos){
        size_t end=text.find('}',pos); if(end==std::string::npos)break;
        std::string obj=text.substr(pos,end-pos); pos=end+1;
        auto getStr=[&](const char*key,WCHAR*out,UINT cch){
            std::string pat=std::string("\"")+key+"\"";
            size_t k=obj.find(pat); if(k==std::string::npos)return;
            k=obj.find(':',k+pat.size()); if(k==std::string::npos)return;
            k=obj.find('"',k); if(k==std::string::npos)return;
            size_t e=obj.find('"',k+1); if(e==std::string::npos)return;
            std::string val=obj.substr(k+1,e-k-1);
            int need=MultiByteToWideChar(CP_UTF8,0,val.c_str(),-1,NULL,0);
            if(need>0) MultiByteToWideChar(CP_UTF8,0,val.c_str(),-1,out,cch);
        };
        CUSTCMD c={};
        getStr("name",&c.name[0],ARRAYSIZE(c.name));
        if(!c.name[0]) continue;
        getStr("type",&c.type[0],ARRAYSIZE(c.type));
        if(!c.type[0]) StringCchCopyW(c.type,ARRAYSIZE(c.type),L"local");
        getStr("command",&c.command[0],ARRAYSIZE(c.command));
        out[n++]=c;
    }
    return n;
}
static void RunCustomCommand(HWND hwnd, PCWSTR site, PCWSTR folder, PCWSTR name, int idx)
{
    CUSTCMD cmds[MAX_CUSTOM]={};
    int n=LoadCustomCommands(cmds,MAX_CUSTOM);
    if(idx<0||idx>=n) return;
    CUSTCMD &c=cmds[idx];
    WCHAR full[700]; JoinPath(folder,name,full,ARRAYSIZE(full));
    // Substitute {path} {name} {site} {full}
    WCHAR cmd[2400]={};
    {
        std::wstring s=c.command;
        auto rep=[&](PCWSTR token,PCWSTR val){
            std::wstring t=token; size_t at=0;
            while((at=s.find(t,at))!=std::wstring::npos){ s.replace(at,t.size(),val); at+=wcslen(val); }
        };
        rep(L"{site}",site); rep(L"{name}",name); rep(L"{path}",folder); rep(L"{full}",full);
        StringCchCopyW(cmd,ARRAYSIZE(cmd),s.c_str());
    }
    if(0==StrCmpIW(c.type,L"script")){
        // Run through winscp.com: open site then run the command line(s)
        WCHAR ws[2600];
        StringCchPrintf(ws,ARRAYSIZE(ws),L"\"%s\" /command \"open \\\"%s\\\"\" \"%s\" \"close\" \"exit\"",GetWinScpPath(),site,cmd);
        STARTUPINFOW si={sizeof(si)}; PROCESS_INFORMATION pi={};
        if(CreateProcessW(NULL,ws,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi)){
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        }
    } else {
        // local command via ShellExecute
        HINSTANCE hr=ShellExecuteW(hwnd,NULL,cmd,NULL,NULL,SW_SHOWNORMAL);
        if((INT_PTR)hr<=32){ MessageBoxW(hwnd,ExplorerText(L"error.custom_command_failed",L"自定义命令执行失败。",L"Custom command failed."),ExplorerText(L"dialog.remote",L"远程操作",L"Remote"),MB_OK|MB_ICONERROR); }
    }
}

// ---- background (folder empty area) menu helpers ----------------------------

static void NewFolderRemote(HWND hwnd, PCWSTR site, PCWSTR folder, PIDLIST_ABSOLUTE notifyPidl)
{
    WCHAR name[256]; StringCchCopy(name, ARRAYSIZE(name), ExplorerText(L"dialog.new_folder_default", L"新建文件夹", L"New folder"));
    if (!PromptText(hwnd, ExplorerText(L"dialog.new_folder", L"新建文件夹", L"New folder"), name, ARRAYSIZE(name), name)) return;
    WCHAR full[700]; JoinPath(folder, name, full, ARRAYSIZE(full));
    if (RunCli(site, L"mkdir", full, NULL, NULL) != 0)
        MessageBoxW(hwnd, ExplorerText(L"error.create_folder_failed", L"创建文件夹失败。", L"Failed to create folder."), ExplorerText(L"dialog.remote", L"远程操作", L"Remote"), MB_OK|MB_ICONERROR);
    else AfterRemoteMutation(site, folder, notifyPidl);
}

// Core upload path — the ONE implementation behind the background-menu paste,
// the folder IDropTarget (Ctrl+V / drag-drop) and any future paste entry.
// The caller owns hdrop's lifetime (clipboard data must not be freed here).
static void PasteHdropToFolder(HWND hwnd, PCWSTR site, PCWSTR folder, HDROP hdrop, PIDLIST_ABSOLUTE notifyPidl)
{
    UINT n = DragQueryFileW(hdrop, 0xFFFFFFFF, NULL, 0);
    BOOL ok = TRUE;
    for (UINT k = 0; k < n; k++)
    {
        WCHAR local[MAX_PATH], name[MAX_PATH];
        if (!DragQueryFileW(hdrop, k, local, ARRAYSIZE(local))) { ok = FALSE; continue; }
        StringCchCopy(name, MAX_PATH, PathFindFileNameW(local));
        WCHAR full[700]; JoinPath(folder, name, full, ARRAYSIZE(full));
        if (RunCli(site, L"put", local, full, NULL) != 0) ok = FALSE;
    }
    AfterRemoteMutation(site, folder, notifyPidl);
    if (!ok) MessageBoxW(hwnd, ExplorerText(L"error.some_uploads_failed", L"部分文件上传失败。", L"Some files could not be uploaded."), ExplorerText(L"dialog.remote", L"远程操作", L"Remote"), MB_OK|MB_ICONERROR);
}

// IDataObject entry used by the folder IDropTarget (Ctrl+V / drag-drop).
// Non-static: called from ExplorerDataProvider.cpp's CFolderDropTarget.
void PasteDataObjectToFolder(HWND hwnd, PCWSTR site, PCWSTR folder, IDataObject *pdo, PIDLIST_ABSOLUTE notifyPidl)
{
    if (!pdo) return;
    FORMATETC fmt = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM st = {};
    if (FAILED(pdo->GetData(&fmt, &st))) return;
    if (st.hGlobal) PasteHdropToFolder(hwnd, site, folder, (HDROP)st.hGlobal, notifyPidl);
    ReleaseStgMedium(&st);
}

static void PasteClipboardToFolder(HWND hwnd, PCWSTR site, PCWSTR folder, PIDLIST_ABSOLUTE notifyPidl)
{
    if (!OpenClipboard(hwnd)) return;
    HANDLE h = GetClipboardData(CF_HDROP);
    if (h) PasteHdropToFolder(hwnd, site, folder, (HDROP)h, notifyPidl);
    CloseClipboard();
}
static void BgCustomCommand(HWND hwnd, PCWSTR site, PCWSTR folder, int idx)
{
    CUSTCMD cmds[MAX_CUSTOM] = {};
    int n = LoadCustomCommands(cmds, MAX_CUSTOM);
    if (idx < 0 || idx >= n) return;
    CUSTCMD &c = cmds[idx];
    WCHAR cmd[2400] = {};
    {
        std::wstring s = c.command;
        auto rep = [&](PCWSTR t, PCWSTR v) {
            std::wstring tt = t; size_t at = 0;
            while ((at = s.find(tt, at)) != std::wstring::npos) { s.replace(at, tt.size(), v); at += wcslen(v); }
        };
        rep(L"{site}", site); rep(L"{path}", folder); rep(L"{name}", L""); rep(L"{full}", L"");
        StringCchCopyW(cmd, ARRAYSIZE(cmd), s.c_str());
    }
    if (0 == StrCmpIW(c.type, L"script"))
    {
        WCHAR ws[2600];
        StringCchPrintf(ws, ARRAYSIZE(ws), L"\"%s\" /command \"open \\\"%s\\\"\" \"%s\" \"close\" \"exit\"", GetWinScpPath(), site, cmd);
        STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi = {};
        if (CreateProcessW(NULL, ws, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    }
    else
    {
        HINSTANCE hr = ShellExecuteW(hwnd, NULL, cmd, NULL, NULL, SW_SHOWNORMAL);
        if ((INT_PTR)hr <= 32) MessageBoxW(hwnd, ExplorerText(L"error.custom_command_failed", L"自定义命令执行失败。", L"Custom command failed."), ExplorerText(L"dialog.remote", L"远程操作", L"Remote"), MB_OK|MB_ICONERROR);
    }
}

class CMenu : public IContextMenu, public IShellExtInit, public IObjectWithSite {
public:
 CMenu():ref(1),data(NULL),site(NULL),m_pidlFolder(NULL){DllAddRef();}
 HRESULT QueryInterface(REFIID r,void**p){static const QITAB q[]={QITABENT(CMenu,IContextMenu),QITABENT(CMenu,IShellExtInit),QITABENT(CMenu,IObjectWithSite),{0}};return QISearch(this,q,r,p);}
 ULONG AddRef(){return InterlockedIncrement(&ref);} ULONG Release(){long n=InterlockedDecrement(&ref);if(!n)delete this;return n;}
 HRESULT QueryContextMenu(HMENU m,UINT i,UINT first,UINT,UINT flags){
    BOOL defaultOnly = (flags&CMF_DEFAULTONLY) != 0;
    SELDATA sel; if(!CollectSelection(data,&sel)) return MAKE_HRESULT(SEVERITY_SUCCESS,0,0);
    // Double-click probe (P0-1): log what Explorer asks for so the default-verb
    // path can be traced if double-click still does nothing.
    ProbeLog(L"[DBLCLK] CMenu::QueryContextMenu flags=0x%08X defaultOnly=%d site='%s' n=%d first='%s'",
             flags,(int)defaultOnly,sel.site,sel.count,sel.count?sel.names[0]:L"");
    // Site-picker items (no site segment in the folder PIDL) get the system
    // default menu (Open/Pin/Rename/Delete/Properties) only — our WinSCP-style
    // commands operate on remote files, not on saved connections.
    if(!sel.site[0]) return MAKE_HRESULT(SEVERITY_SUCCESS,0,0);
    BOOL multi = sel.count>1;
    // Explorer itself owns opening folders.  Advertising our file Open verb for
    // a folder can make the first double-click invoke it instead of navigation.
    if(!sel.firstIsFolder){
        InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_OPEN,ExplorerText(L"menu.open",L"打开",L"Open"));
        SetMenuDefaultItem(m,first+MENU_OPEN,FALSE);
        if(defaultOnly) return MAKE_HRESULT(SEVERITY_SUCCESS,0,1);
        InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_EDIT,ExplorerText(L"menu.edit",L"编辑",L"Edit"));
        InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_DOWNLOAD,ExplorerText(L"menu.download",L"下载",L"Download"));
        InsertMenuW(m,i++,MF_BYPOSITION|MF_SEPARATOR,0,NULL);
    }
    if(defaultOnly) return MAKE_HRESULT(SEVERITY_SUCCESS,0,0);
    InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_COPY_CLIP,ExplorerText(L"menu.copy_clipboard",L"复制到剪贴板",L"Copy to clipboard"));
    InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_COPY_NAME,ExplorerText(L"menu.copy_name",L"复制文件名",L"Copy file name"));
    InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_COPY_NATIVE,ExplorerText(L"menu.copy_remote_path",L"复制远程路径",L"Copy remote path"));
    InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_COPY_FULL,ExplorerText(L"menu.copy_full_path",L"复制完整路径",L"Copy full path"));
    InsertMenuW(m,i++,MF_BYPOSITION|MF_SEPARATOR,0,NULL);
    InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_RCOPY,multi?ExplorerText(L"menu.duplicate_all",L"复制到...（全部）",L"Copy to... (all)"):ExplorerText(L"menu.duplicate",L"复制到...",L"Copy to..."));
    InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_RMOVE,multi?ExplorerText(L"menu.move_all",L"移动到...（全部）",L"Move to... (all)"):ExplorerText(L"menu.move",L"移动到...",L"Move to..."));
    InsertMenuW(m,i++,MF_BYPOSITION|(multi?MF_GRAYED:0),first+MENU_RENAME,ExplorerText(L"menu.rename",L"重命名",L"Rename"));
    InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_DELETE,ExplorerText(L"menu.delete",L"从服务器删除",L"Delete from server"));
    int custom=0; CUSTCMD cmds[MAX_CUSTOM]={};
    if(!multi){ custom=LoadCustomCommands(cmds,MAX_CUSTOM); if(custom>0) InsertMenuW(m,i++,MF_BYPOSITION|MF_SEPARATOR,0,NULL);
        for(int k=0;k<custom;k++) InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_CUSTOM_BASE+k,cmds[k].name); }
    InsertMenuW(m,i++,MF_BYPOSITION|MF_SEPARATOR,0,NULL);
    InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_PROPERTIES,multi?ExplorerText(L"menu.properties_first",L"属性（第一个）",L"Properties (first)"):ExplorerText(L"menu.properties",L"属性",L"Properties"));
    return MAKE_HRESULT(SEVERITY_SUCCESS,0,12+custom);
 }
 HRESULT InvokeCommand(LPCMINVOKECOMMANDINFO ci){
    UINT id=IS_INTRESOURCE(ci->lpVerb)?LOWORD((UINT_PTR)ci->lpVerb):99;
    if(!data)return E_INVALIDARG;
    SELDATA sel; if(!CollectSelection(data,&sel))return E_FAIL;
    ProbeLog(L"[DBLCLK] CMenu::InvokeCommand id=%u site='%s' n=%d",id,sel.site,sel.count);
    PCWSTR pnames[MAX_SEL]; for(int k=0;k<sel.count;k++) pnames[k]=sel.names[k];
    if(id>=MENU_CUSTOM_BASE){ RunCustomCommand(ci->hwnd,sel.site,sel.folder,sel.names[0],id-MENU_CUSTOM_BASE); if(sel.notify)CoTaskMemFree(sel.notify); return S_OK; }
    switch(id){
    case MENU_OPEN: for(int k=0;k<sel.count;k++) OpenRemote(ci->hwnd,sel.site,sel.folder,sel.names[k],FALSE); break;
    case MENU_EDIT: for(int k=0;k<sel.count;k++) OpenRemote(ci->hwnd,sel.site,sel.folder,sel.names[k],TRUE); break;
    case MENU_DOWNLOAD: DownloadFiles(ci->hwnd,sel.site,sel.folder,pnames,sel.count); break;
    case MENU_COPY_CLIP: CopyClipboard(ci->hwnd,sel.site,sel.folder,pnames,sel.count); break;
    case MENU_COPY_NAME:
        { std::wstring t; for(int k=0;k<sel.count;k++){ if(k)t+=L"\r\n"; t+=sel.names[k]; } CopyTextToClipboard(ci->hwnd,t.c_str()); break; }
    case MENU_COPY_NATIVE:
        { std::wstring t; for(int k=0;k<sel.count;k++){ if(k)t+=L"\r\n"; WCHAR full[700]; JoinPath(sel.folder,sel.names[k],full,ARRAYSIZE(full)); t+=full; } CopyTextToClipboard(ci->hwnd,t.c_str()); break; }
    case MENU_COPY_FULL:
        { std::wstring t; for(int k=0;k<sel.count;k++){ if(k)t+=L"\r\n"; WCHAR full[700]; JoinPath(sel.folder,sel.names[k],full,ARRAYSIZE(full)); t+=sel.site; t+=L":"; t+=full; } CopyTextToClipboard(ci->hwnd,t.c_str()); break; }
    case MENU_RCOPY: ServerCopy(ci->hwnd,sel.site,sel.folder,pnames,sel.count,sel.notify); break;
    case MENU_RMOVE: ServerMove(ci->hwnd,sel.site,sel.folder,pnames,sel.count,sel.notify); break;
    case MENU_RENAME: DoRename(ci->hwnd,sel.site,sel.folder,sel.names[0],sel.notify); break;
    case MENU_DELETE:
        if(IDYES==MessageBoxW(ci->hwnd,ExplorerText(L"confirm.delete_remote",L"要从远程服务器删除选中的项目吗？",L"Delete the selected item(s) on the remote server?"),ExplorerText(L"dialog.remote",L"远程操作",L"Remote"),MB_YESNO|MB_ICONWARNING)){
            BOOL ok=TRUE;
            for(int k=0;k<sel.count;k++){
                WCHAR full[700]; JoinPath(sel.folder,sel.names[k],full,ARRAYSIZE(full));
                if(RunCli(sel.site,L"delete",full,NULL,NULL)!=0) ok=FALSE;
            }
            ProbeLog(L"[MUT] delete loop done, ok=%d -> AfterRemoteMutation", ok);
            AfterRemoteMutation(sel.site, sel.folder, sel.notify);
            ProbeLog(L"[MUT] delete case: mutation done, about to return");
            if(!ok) MessageBoxW(ci->hwnd,ExplorerText(L"error.some_deletes_failed",L"部分项目删除失败。",L"Some items could not be deleted."),ExplorerText(L"dialog.remote",L"远程操作",L"Remote"),MB_OK|MB_ICONERROR);
        }
        break;
    case MENU_PROPERTIES:{
        PROPMETA pm={}; StringCchCopy(pm.site,ARRAYSIZE(pm.site),sel.site);
        pm.canSetOwner = SiteCanSetOwner(sel.site);
        JoinPath(sel.folder,sel.names[0],pm.path,ARRAYSIZE(pm.path));
        if(ReadRemoteMeta(sel.site,sel.folder,sel.names[0],&pm.meta))
            DialogBoxParamW(g_hInst,MAKEINTRESOURCEW(IDD_PERMBOX),ci->hwnd,PermDlgProc,(LPARAM)&pm);
        else MessageBoxW(ci->hwnd,ExplorerText(L"info.metadata_unavailable",L"元数据不可用。",L"Metadata unavailable."),sel.firstIsFolder?ExplorerText(L"property.directory_properties",L"目录属性",L"Directory properties"):ExplorerText(L"property.file_properties",L"文件属性",L"File properties"),MB_OK|MB_ICONINFORMATION);
        break; }
    default: if(sel.notify)CoTaskMemFree(sel.notify); return E_INVALIDARG;
    }
    if(sel.notify) CoTaskMemFree(sel.notify);
    return S_OK;
 }
 HRESULT GetCommandString(UINT_PTR id,UINT type,UINT*,LPSTR s,UINT c){
    PCWSTR v=L"";
    switch(id){case MENU_OPEN:v=L"open";break;case MENU_EDIT:v=L"edit";break;case MENU_DOWNLOAD:v=L"download";break;
      case MENU_COPY_CLIP:v=L"copy_to_clipboard";break;case MENU_COPY_NAME:v=L"copy_file_name";break;
      case MENU_COPY_NATIVE:v=L"copy_remote_path";break;case MENU_COPY_FULL:v=L"copy_full_path";break;
      case MENU_RCOPY:v=L"remote_copy";break;case MENU_RMOVE:v=L"remote_move";break;case MENU_RENAME:v=L"rename";break;
      case MENU_DELETE:v=L"delete";break;case MENU_PROPERTIES:v=L"properties";break;default:return E_NOTIMPL;}
    if(type==GCS_VERBW) return StringCchCopyW((PWSTR)s,c,v);
    if(type==GCS_VERBA){ char a[64]; WideCharToMultiByte(CP_ACP,0,v,-1,a,ARRAYSIZE(a),NULL,NULL); return StringCchCopyA(s,c,a); }
    return E_NOTIMPL;
 }
 HRESULT Initialize(PCIDLIST_ABSOLUTE pidlFolder,IDataObject*d,HKEY){
    ProbeLog(L"[MENU] CMenu::Initialize pidlFolder=%p data=%p", pidlFolder, d);
    if(data)data->Release();data=d;if(data)data->AddRef();
    if(m_pidlFolder)ILFree(m_pidlFolder);
    m_pidlFolder=pidlFolder?ILCloneFull(pidlFolder):NULL;
    return S_OK;}
 HRESULT SetSite(IUnknown*s){if(site)site->Release();site=s;if(site)site->AddRef();return S_OK;} HRESULT GetSite(REFIID r,void**p){return site?site->QueryInterface(r,p):E_FAIL;}
private:
 ~CMenu(){if(data)data->Release();if(site)site->Release();if(m_pidlFolder)ILFree(m_pidlFolder);DllRelease();}
 long ref;IDataObject*data;IUnknown*site;PIDLIST_ABSOLUTE m_pidlFolder;
};
HRESULT CFolderViewImplContextMenu_CreateInstance(REFIID riid,void**ppv){*ppv=NULL;CMenu*m=new(std::nothrow)CMenu();if(!m)return E_OUTOFMEMORY;HRESULT hr=m->QueryInterface(riid,ppv);m->Release();return hr;}

// ---- property sheet: Ribbon "Properties" button -> standard Properties
// dialog with our Permissions page (PSN_APPLY writes back via chmod). --------

// Read-only connection info page for a saved site (site-picker level 0).
static INT_PTR CALLBACK SitePageProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        PROPMETA *pm = (PROPMETA*)((LPPROPSHEETPAGE)lp)->lParam;
        if (!pm) return FALSE;
        SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)pm);
        SetWindowTextW(hDlg, ExplorerText(L"property.site_properties", L"站点属性", L"Site properties"));
        LocalizeSiteDialog(hDlg);
        PopulateSiteInfo(hDlg, pm->site);
        return TRUE;
    }
    case WM_NOTIFY:
    {
        NMHDR *nm = (NMHDR*)lp;
        if (nm && nm->code == PSN_APPLY)
        {
            SetWindowLongPtrW(hDlg, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}
static UINT CALLBACK PermPageCallback(HWND /* hwnd */, UINT uMsg, LPPROPSHEETPAGE ppsp)
{
    if (uMsg == PSPCB_RELEASE)
    {
        PROPMETA *pm = (PROPMETA*)ppsp->lParam;
        if (pm)
        {
            if (pm->notify) CoTaskMemFree(pm->notify);
            CoTaskMemFree(pm);
        }
    }
    return 1;
}

static INT_PTR CALLBACK PermPageProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        ProbeLog(L"[DIAG] PermPage WM_INITDIALOG created");
        PROPMETA *pm = (PROPMETA*)((LPPROPSHEETPAGE)lp)->lParam;
        if (!pm) return FALSE;
        SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)pm);
        LocalizePermissionDialog(hDlg);
        REMOTEMETA *m = &pm->meta;
        SetDlgItemTextW(hDlg,3001,m->name); SetDlgItemTextW(hDlg,3002,m->type);
        SetDlgItemTextW(hDlg,3003,m->mode); SetDlgItemTextW(hDlg,3006,m->size); SetDlgItemTextW(hDlg,3007,m->mtime);
        PermInitOwnerGroup(hDlg,m);
        PermSetOwnerChangeVisible(hDlg, pm->canSetOwner);
        PermSetChecks(hDlg,m->bits);
        PermSyncChecksToOctal(hDlg);
        EnableWindow(GetDlgItem(hDlg,3023), m->fIsFolder ? TRUE : FALSE);
        return TRUE;
    }
    case WM_COMMAND:
        if(HIWORD(wp)==BN_CLICKED && LOWORD(wp)>=3011 && LOWORD(wp)<=3019){ PermSyncChecksToOctal(hDlg); return TRUE; }
        if(HIWORD(wp)==EN_CHANGE && LOWORD(wp)==3022){ PermSyncOctalToChecks(hDlg); return TRUE; }
        break;
    case WM_NOTIFY:
    {
        NMHDR *nm = (NMHDR*)lp;
        if (nm && nm->code == PSN_APPLY)
        {
            PROPMETA *pm = (PROPMETA*)GetWindowLongPtrW(hDlg, DWLP_USER);
            if (pm)
            {
                PermSyncOctalToChecks(hDlg);
                DWORD mode = PermCollectChecks(hDlg);
                BOOL recursive = IsDlgButtonChecked(hDlg,3023)!=0;
                if (mode != pm->meta.bits || recursive)
                {
                    WCHAR modeStr[8]; StringCchPrintf(modeStr,ARRAYSIZE(modeStr),L"%03o",mode);
                    if (RunCli(pm->site, recursive?L"chmodr":L"chmod", pm->path, modeStr, NULL)==0)
                    {
                        WCHAR parentDir[512]; PathParent(pm->path, parentDir, ARRAYSIZE(parentDir));
                        AfterRemoteMutation(pm->site, parentDir, pm->notify);
                    }
                    else MessageBoxW(hDlg, ExplorerText(L"error.chmod_failed", L"权限修改失败。", L"Permission update failed."), ExplorerText(L"dialog.remote", L"远程操作", L"Remote"), MB_OK|MB_ICONERROR);
                }
                PermApplyChown(hDlg, pm);
            }
            SetWindowLongPtrW(hDlg, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

class CFolderViewImplPropSheet : public IShellPropSheetExt, public IShellExtInit
{
public:
    CFolderViewImplPropSheet() : ref(1), data(NULL) { DllAddRef(); }
    ~CFolderViewImplPropSheet() { if(data) data->Release(); DllRelease(); }

    HRESULT QueryInterface(REFIID r, void **p)
    {
        static const QITAB q[] = { QITABENT(CFolderViewImplPropSheet, IShellPropSheetExt),
                                   QITABENT(CFolderViewImplPropSheet, IShellExtInit), {0} };
        return QISearch(this, q, r, p);
    }
    ULONG AddRef() { return InterlockedIncrement(&ref); }
    ULONG Release() { long n = InterlockedDecrement(&ref); if(!n) delete this; return n; }

    // IShellExtInit: Explorer gives us the selection (IDataObject).
    HRESULT Initialize(PCIDLIST_ABSOLUTE, IDataObject *d, HKEY)
    {
        if (data) data->Release();
        data = d;
        if (data) data->AddRef();
        return S_OK;
    }

    // IShellPropSheetExt
    HRESULT AddPages(LPFNADDPROPSHEETPAGE pfnAddPage, LPARAM lParam)
    {
        ProbeLog(L"[DIAG] PropSheet AddPages called, data=%p pfnAddPage=%p", (void*)data, (void*)pfnAddPage);
        if (!pfnAddPage || !data) return S_OK;   // no selection -> nothing to add
        SELDATA sel;
        if (!CollectSelection(data, &sel)) return S_OK;
        ProbeLog(L"[DIAG] PropSheet selection ok: site='%s' folder='%s' name='%s' count=%d",
                 sel.site, sel.folder, sel.names[0], sel.count);

        PROPMETA *pm = (PROPMETA*)CoTaskMemAlloc(sizeof(PROPMETA));
        if (!pm) return E_OUTOFMEMORY;
        ZeroMemory(pm, sizeof(*pm));
        StringCchCopy(pm->site, ARRAYSIZE(pm->site), sel.site);
        pm->canSetOwner = SiteCanSetOwner(sel.site);
        JoinPath(sel.folder, sel.names[0], pm->path, ARRAYSIZE(pm->path));
        pm->notify = sel.notify ? ILCloneFull(sel.notify) : NULL;
        if (sel.notify) CoTaskMemFree(sel.notify);
        if (!ReadRemoteMeta(sel.site, sel.folder, sel.names[0], &pm->meta))
        {
            // Site-picker item (level 0): not a remote file — show a read-only
            // connection info page instead of a Permissions page.
            const FTPSITE *s = FtpSiteFind(sel.names[0]);
            if (!s)
            {
                CoTaskMemFree(pm);
                return S_OK;
            }
            StringCchCopy(pm->site, ARRAYSIZE(pm->site), sel.names[0]);
            PROPSHEETPAGE psp = {};
            psp.dwSize = sizeof(psp);
            psp.dwFlags = PSP_USECALLBACK | PSP_USETITLE;
            psp.pszTitle = ExplorerText(L"property.site_properties", L"站点属性", L"Site properties");
            psp.hInstance = g_hInst;
            psp.pszTemplate = MAKEINTRESOURCEW(IDD_SITEPAGE);
            psp.pfnDlgProc = SitePageProc;
            psp.lParam = (LPARAM)pm;
            psp.pfnCallback = PermPageCallback;
            HPROPSHEETPAGE hPage = CreatePropertySheetPage(&psp);
            if (!hPage)
            {
                if (pm->notify) CoTaskMemFree(pm->notify);
                CoTaskMemFree(pm);
                return S_OK;
            }
            if (!pfnAddPage(hPage, lParam))
                DestroyPropertySheetPage(hPage);
            return S_OK;
        }

        PROPSHEETPAGE psp = {};
        psp.dwSize = sizeof(psp);
        psp.dwFlags = PSP_USECALLBACK | PSP_USETITLE;
        psp.pszTitle = PropertyDialogTitle(&pm->meta);
        psp.hInstance = g_hInst;
        psp.pszTemplate = MAKEINTRESOURCEW(IDD_PERMPAGE);
        psp.pfnDlgProc = PermPageProc;
        psp.lParam = (LPARAM)pm;
        psp.pfnCallback = PermPageCallback;

        HPROPSHEETPAGE hPage = CreatePropertySheetPage(&psp);
        ProbeLog(L"[DIAG] PropSheet CreatePropertySheetPage hPage=%p lastErr=%u", (void*)hPage, GetLastError());
        if (!hPage)
        {
            if (pm->notify) CoTaskMemFree(pm->notify);
            CoTaskMemFree(pm);
            return S_OK;
        }
        BOOL added = pfnAddPage(hPage, lParam);     // pass Explorer's lParam back verbatim
        ProbeLog(L"[DIAG] PropSheet pfnAddPage returned %d", added);
        if (!added)
        {
            DestroyPropertySheetPage(hPage);
        }
        return S_OK;
    }
    HRESULT ReplacePage(EXPPS, LPFNSVADDPROPSHEETPAGE, LPARAM) { return E_NOTIMPL; }

private:
    long ref;
    IDataObject *data;
};

HRESULT CFolderViewImplPropSheet_CreateInstance(REFIID riid, void **ppv)
{
    *ppv = NULL;
    CFolderViewImplPropSheet *m = new (std::nothrow) CFolderViewImplPropSheet();
    if (!m) return E_OUTOFMEMORY;
    HRESULT hr = m->QueryInterface(riid, ppv);
    m->Release();
    return hr;
}

// ---- background menu wrapper: system default menu + our folder commands -----
static CUSTCMD g_cmds[MAX_CUSTOM];
class CFolderViewImplBgMenu : public IContextMenu, public IObjectWithSite
{
public:
    CFolderViewImplBgMenu(IContextMenu *pDef, PCIDLIST_ABSOLUTE pidlFolder, int level)
        : ref(1), m_pDefault(pDef), m_site(NULL), m_lastFirst(0), m_defaultCount(0), m_nLevel(level)
    {
        if (m_pDefault) m_pDefault->AddRef();   // keep the default menu alive
        m_pidl = pidlFolder ? ILCloneFull(pidlFolder) : NULL;
        DllAddRef();
    }
    ~CFolderViewImplBgMenu()
    {
        if (m_pDefault) m_pDefault->Release();
        if (m_site) m_site->Release();
        if (m_pidl) ILFree(m_pidl);
        DllRelease();
    }
    HRESULT QueryInterface(REFIID r, void **p)
    {
        static const QITAB q[] = { QITABENT(CFolderViewImplBgMenu, IContextMenu),
                                   QITABENT(CFolderViewImplBgMenu, IObjectWithSite), {0} };
        return QISearch(this, q, r, p);
    }
    ULONG AddRef() { return InterlockedIncrement(&ref); }
    ULONG Release() { long n = InterlockedDecrement(&ref); if (!n) delete this; return n; }

    HRESULT QueryContextMenu(HMENU m, UINT i, UINT first, UINT maxid, UINT flags)
    {
        UINT n = 0;
        if (m_pDefault)
        {
            HRESULT hr = m_pDefault->QueryContextMenu(m, i, first, maxid, flags);
            if (SUCCEEDED(hr)) n = LOWORD(hr);
        }
        m_lastFirst = first;
        m_defaultCount = n;
        ProbeLog(L"[BG] QueryContextMenu first=%u maxid=%u flags=0x%X defaultCount=%u", first, maxid, flags, n);

        UINT pos = i + n;
        UINT our = first + n;
        UINT added = 0;
#define BG_INSERT(text) do { if (our < maxid) { InsertMenuW(m, pos++, MF_BYPOSITION, our++, (text)); added++; } } while(0)
        if (m_nLevel == 0)
        {
            // Site picker (connection manager): the only useful action here is
            // creating a new site, which launches the GUI site manager.
            BG_INSERT(ExplorerText(L"menu.new_site", L"新建站点...", L"New site..."));
        }
        else
        {
            BG_INSERT(ExplorerText(L"menu.copy_current_path", L"复制当前路径", L"Copy current path"));
            BG_INSERT(ExplorerText(L"menu.new_folder", L"新建文件夹...", L"New folder..."));
            BG_INSERT(ExplorerText(L"menu.paste_files", L"在此粘贴文件", L"Paste files here"));
            // level 1 is a site's remote root; every level from there has a
            // current-site configuration. Directory metadata is shown when available.
            BG_INSERT(ExplorerText(L"menu.current_directory_properties", L"显示当前目录属性", L"Current directory properties"));
            BG_INSERT(ExplorerText(L"menu.current_site_information", L"显示当前站点信息", L"Current site information"));
        }
#undef BG_INSERT
        int custom = 0;
        if (added > 0 && our < maxid)
        {
            custom = LoadCustomCommands(g_cmds, MAX_CUSTOM);
            if (custom > 0)
            {
                InsertMenuW(m, pos++, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
                for (int k = 0; k < custom && our < maxid; k++)
                    InsertMenuW(m, pos++, MF_BYPOSITION, our++, g_cmds[k].name);
            }
        }
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, n + added + custom);
    }

    HRESULT InvokeCommand(LPCMINVOKECOMMANDINFO ci)
    {
        UINT id = IS_INTRESOURCE(ci->lpVerb) ? LOWORD((UINT_PTR)ci->lpVerb) : 99;
        // Explorer may pass either the absolute menu id (first+k) or the
        // relative index (k). Normalize to relative.
        UINT rel = id;
        if (m_lastFirst > 0 && id >= m_lastFirst) rel = id - m_lastFirst;
        ProbeLog(L"[BG] InvokeCommand id=%u rel=%u defaultCount=%u", id, rel, m_defaultCount);
        if (m_pDefault && rel < m_defaultCount)
            return m_pDefault->InvokeCommand(ci);           // system item

        WCHAR site[64] = {}, folder[512] = {};
        if (m_pidl) {
            PidlSite(m_pidl, site, ARRAYSIZE(site));
            PidlPath(m_pidl, folder, ARRAYSIZE(folder));
            ApplySiteStartPath(site, folder, ARRAYSIZE(folder));
        }
        UINT k = rel - m_defaultCount;
        if (m_nLevel == 0)
        {
            // Site picker: new site launches the GUI client's site manager.
            if (k == 0 && GetClientPath()[0])
                ShellExecuteW(ci->hwnd, NULL, GetClientPath(), L"--show", NULL, SW_SHOWNORMAL);
            return S_OK;
        }
        switch (k)
        {
        case 0: { std::wstring t = site; t += L":"; t += folder; CopyTextToClipboard(ci->hwnd, t.c_str()); break; }
        case 1: NewFolderRemote(ci->hwnd, site, folder, m_pidl); break;
        case 2: PasteClipboardToFolder(ci->hwnd, site, folder, m_pidl); break;
        case 3: ShowCurrentFolderProperties(ci->hwnd, site, folder); break;
        case 4: ShowCurrentSiteInfo(ci->hwnd, site); break;
        default: BgCustomCommand(ci->hwnd, site, folder, k - 5); break;
        }
        return S_OK;
    }

    HRESULT GetCommandString(UINT_PTR id, UINT type, UINT *r, LPSTR s, UINT c)
    {
        if (m_pDefault && id < m_lastFirst + m_defaultCount)
            return m_pDefault->GetCommandString(id, type, r, s, c);
        return E_NOTIMPL;
    }

    HRESULT SetSite(IUnknown *s)
    {
        if (m_site) m_site->Release();
        m_site = s;
        if (m_site) m_site->AddRef();
        return S_OK;
    }
    HRESULT GetSite(REFIID r, void **p)
    {
        if (m_pDefault)
        {
            IObjectWithSite *ows = NULL;
            if (SUCCEEDED(m_pDefault->QueryInterface(IID_PPV_ARGS(&ows))))
            {
                HRESULT hr = ows->GetSite(r, p);
                ows->Release();
                return hr;
            }
        }
        return m_site ? m_site->QueryInterface(r, p) : E_FAIL;
    }

private:
    long ref;
    IContextMenu *m_pDefault;
    IUnknown *m_site;
    PIDLIST_ABSOLUTE m_pidl;
    UINT m_lastFirst;
    UINT m_defaultCount;
    int m_nLevel;
};

HRESULT CFolderViewImplBgMenu_Create(IContextMenu *pDef, PCIDLIST_ABSOLUTE pidlFolder, int level, REFIID riid, void **ppv)
{
    *ppv = NULL;
    CFolderViewImplBgMenu *bg = new (std::nothrow) CFolderViewImplBgMenu(pDef, pidlFolder, level);
    if (!bg) return E_OUTOFMEMORY;
    HRESULT hr = bg->QueryInterface(riid, ppv);
    bg->Release();
    return hr;
}
