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
    if (p2 && p2[0])
        StringCchPrintf(cmd,ARRAYSIZE(cmd),L"\"D:\\tools\\explorer-remote-fs\\dist\\cli\\ExplorerRemoteFs.Cli.exe\" %s \"%s\" \"%s\" \"%s\"",verb,site,p1,p2);
    else
        StringCchPrintf(cmd,ARRAYSIZE(cmd),L"\"D:\\tools\\explorer-remote-fs\\dist\\cli\\ExplorerRemoteFs.Cli.exe\" %s \"%s\" \"%s\"",verb,site,p1);
    SECURITY_ATTRIBUTES sa={sizeof(sa),NULL,TRUE}; HANDLE rd=NULL,wr=NULL;
    if(captured && !CreatePipe(&rd,&wr,&sa,0)) return -1;
    if(captured) SetHandleInformation(rd,HANDLE_FLAG_INHERIT,0);
    STARTUPINFOW si={sizeof(si)}; if(captured){si.dwFlags=STARTF_USESTDHANDLES;si.hStdOutput=wr;si.hStdError=wr;si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);}
    PROCESS_INFORMATION pi={}; BOOL ok=CreateProcessW(NULL,cmd,NULL,NULL,captured?TRUE:FALSE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi);
    if(captured) CloseHandle(wr); if(!ok){if(rd)CloseHandle(rd);return -1;}
    if(captured){char b[4096];DWORD n=0;while(ReadFile(rd,b,sizeof(b),&n,NULL)&&n)captured->append(b,n);CloseHandle(rd);}
    WaitForSingleObject(pi.hProcess,30000);DWORD code=1;GetExitCodeProcess(pi.hProcess,&code);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return code==0?0:-1;
}

#define BIT(v) ((v)?BST_CHECKED:BST_UNCHECKED)
typedef struct RemoteMeta {
    WCHAR name[MAX_PATH]; WCHAR type[24]; WCHAR mode[16]; WCHAR owner[40]; WCHAR group[40];
    WCHAR size[32]; WCHAR mtime[32]; DWORD bits; DWORD dwSize; DWORD dwMtime; BOOL fIsFolder; BOOL fIsSymlink;
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
static void FormatSizeString(DWORD size, BOOL folder, PWSTR out, UINT cch)
{
    if (folder) { StringCchCopy(out, cch, L"-"); return; }
    if (size < 1024) StringCchPrintf(out, cch, L"%u B", size);
    else if (size < 1024*1024) StringCchPrintf(out, cch, L"%.1f KB", size/1024.0);
    else if (size < 1024*1024*1024) StringCchPrintf(out, cch, L"%.1f MB", size/(1024.0*1024.0));
    else StringCchPrintf(out, cch, L"%.2f GB", size/(1024.0*1024.0*1024.0));
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
    FTPENTRY entries[MAX_OBJS] = {};
    int n = FtpListCached(site, folder, entries, ARRAYSIZE(entries));
    for (int i = 0; i < n; i++)
    {
        if (0 != StrCmp(entries[i].szName, name)) continue;
        ZeroMemory(meta, sizeof(*meta));
        StringCchCopy(meta->name, ARRAYSIZE(meta->name), entries[i].szName);
        StringCchCopy(meta->owner, ARRAYSIZE(meta->owner), entries[i].szOwner[0] ? entries[i].szOwner : L"?");
        StringCchCopy(meta->group, ARRAYSIZE(meta->group), entries[i].szGroup[0] ? entries[i].szGroup : L"?");
        meta->bits = entries[i].dwMode;
        meta->fIsFolder = entries[i].fIsFolder;
        meta->fIsSymlink = entries[i].fIsSymlink;
        WCHAR mode[16];
        FormatModeString(entries[i].dwMode, entries[i].fIsFolder, entries[i].fIsSymlink, mode, ARRAYSIZE(mode));
        StringCchCopy(meta->mode, ARRAYSIZE(meta->mode), mode);
        meta->dwSize = entries[i].dwSize;
        meta->dwMtime = entries[i].dwMtime;
        StringCchCopy(meta->type, ARRAYSIZE(meta->type),
            entries[i].fIsSymlink ? L"Symbolic Link" : (entries[i].fIsFolder ? L"Folder" : L"File"));
        FormatSizeString(entries[i].dwSize, entries[i].fIsFolder, meta->size, ARRAYSIZE(meta->size));
        FormatMtimeString(entries[i].dwMtime, meta->mtime, ARRAYSIZE(meta->mtime));
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
} PROPMETA;

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

static INT_PTR CALLBACK PermDlgProc(HWND hDlg,UINT msg,WPARAM wp,LPARAM lp)
{
    switch(msg){
    case WM_INITDIALOG:{
        PROPMETA *pm=(PROPMETA*)lp; if(!pm)return TRUE;
        SetWindowLongPtrW(hDlg,DWLP_USER,(LONG_PTR)pm);
        REMOTEMETA *m=&pm->meta;
        SetDlgItemTextW(hDlg,3001,m->name); SetDlgItemTextW(hDlg,3002,m->type);
        SetDlgItemTextW(hDlg,3003,m->mode); SetDlgItemTextW(hDlg,3004,m->owner);
        SetDlgItemTextW(hDlg,3005,m->group); SetDlgItemTextW(hDlg,3006,m->size); SetDlgItemTextW(hDlg,3007,m->mtime);
        PermSetChecks(hDlg,m->bits);
        PermSyncChecksToOctal(hDlg);
        // Recursive apply is only meaningful for directories.
        EnableWindow(GetDlgItem(hDlg,3023), m->fIsFolder ? TRUE : FALSE);
        return TRUE;}
    case WM_COMMAND:
        if(HIWORD(wp)==BN_CLICKED && LOWORD(wp)>=3011 && LOWORD(wp)<=3019){ PermSyncChecksToOctal(hDlg); return TRUE; }
        if(HIWORD(wp)==EN_CHANGE && LOWORD(wp)==3022){ PermSyncOctalToChecks(hDlg); return TRUE; }
        if(LOWORD(wp)==IDCANCEL){EndDialog(hDlg,IDCANCEL);return TRUE;}
        if(LOWORD(wp)==IDOK){
            PROPMETA *pm=(PROPMETA*)GetWindowLongPtrW(hDlg,DWLP_USER);
            if(pm){
                PermSyncOctalToChecks(hDlg);           // octal field wins if edited
                DWORD mode = PermCollectChecks(hDlg);
                BOOL recursive = IsDlgButtonChecked(hDlg,3023)!=0;
                if(mode != pm->meta.bits || recursive){
                    WCHAR modeStr[8]; StringCchPrintf(modeStr,ARRAYSIZE(modeStr),L"%03o",mode);
                    if(RunCli(pm->site, recursive?L"chmodr":L"chmod", pm->path, modeStr, NULL)==0){
                        FtpCacheClear();
                        if(pm->notify) SHChangeNotify(SHCNE_UPDATEDIR,SHCNF_IDLIST,pm->notify,NULL);
                    } else MessageBoxW(hDlg,L"chmod failed.",L"Remote",MB_OK|MB_ICONERROR);
                }
            }
            EndDialog(hDlg,IDOK); return TRUE;}
        break;
    }
    return FALSE;
}

typedef struct { WCHAR* buf; UINT cch; PCWSTR caption; PCWSTR initial; } PROMPTCTX;
static INT_PTR CALLBACK NameDlgProc(HWND h,UINT m,WPARAM w,LPARAM l)
{
    if(m==WM_INITDIALOG){PROMPTCTX*c=(PROMPTCTX*)l;SetWindowLongPtrW(h,DWLP_USER,(LONG_PTR)c);SetWindowTextW(h,c->caption);SetDlgItemTextW(h,3101,c->initial);return TRUE;}
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
    PIDLIST_ABSOLUTE notify;
} SELDATA;

static BOOL CollectSelection(IDataObject *data, SELDATA *out)
{
    out->count=0; out->site[0]=0; out->folder[0]=0; out->notify=NULL;
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
        out->notify=ILCloneFull(parent);
        for(UINT i=1;i<=cida->cidl && out->count<MAX_SEL;i++){
            PCUIDLIST_RELATIVE child=(PCUIDLIST_RELATIVE)((BYTE*)cida+cida->aoffset[i]);
            if(IsOurs(child)){ CopyName((const COMPACTITEM*)child,out->names[out->count],ARRAYSIZE(out->names[0])); out->count++; }
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

static BOOL TempDir(PCWSTR sub, PCWSTR site, PWSTR out, UINT cch)
{
    WCHAR tmp[MAX_PATH];
    if(!GetTempPathW(ARRAYSIZE(tmp),tmp)) return FALSE;
    StringCchPrintf(out,cch,L"%sRemoteFs%s\\%s\\",tmp,sub,site);
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
    if(RunCli(site,L"get",full,local,NULL)!=0){ MessageBoxW(hwnd,L"Download failed.",L"Remote",MB_OK|MB_ICONERROR); return; }
    HINSTANCE hr=ShellExecuteW(hwnd,edit?L"open":L"open",local,NULL,NULL,SW_SHOWNORMAL);
    if((INT_PTR)hr<=32){ MessageBoxW(hwnd,L"Open failed.",L"Remote",MB_OK|MB_ICONERROR); DeleteFileW(local); return; }
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
        OPENFILENAMEW ofn={sizeof(ofn)}; ofn.hwndOwner=hwnd; ofn.lpstrFilter=L"All Files\0*.*\0";
        ofn.lpstrFile=local; ofn.nMaxFile=ARRAYSIZE(local); ofn.Flags=OFN_OVERWRITEPROMPT; ofn.lpstrTitle=L"Download to";
        if(!GetSaveFileNameW(&ofn)) return;
        WCHAR full[700]; JoinPath(folder,names[0],full,ARRAYSIZE(full));
        if(RunCli(site,L"get",full,local,NULL)!=0) MessageBoxW(hwnd,L"Download failed.",L"Remote",MB_OK|MB_ICONERROR);
        return;
    }
    // Multiple: save into the temp folder (already keyed by site).
    for(int i=0;i<count;i++){
        WCHAR full[700]; JoinPath(folder,names[i],full,ARRAYSIZE(full));
        WCHAR local[MAX_PATH]; StringCchPrintf(local,ARRAYSIZE(local),L"%s%s",dir,names[i]);
        RunCli(site,L"get",full,local,NULL);
    }
    WCHAR msg[512]; StringCchPrintf(msg,ARRAYSIZE(msg),L"Downloaded %d files to:\n%s",count,dir);
    MessageBoxW(hwnd,msg,L"Remote",MB_OK|MB_ICONINFORMATION);
}
static void CopyClipboard(HWND hwnd, PCWSTR site, PCWSTR folder, PCWSTR *names, int count)
{
    WCHAR dir[MAX_PATH];
    if(!TempDir(L"Clip",site,dir,ARRAYSIZE(dir))) return;
    CleanupDir(dir);
    // Download each file into temp dir
    PCWSTR *paths=(PCWSTR*)CoTaskMemAlloc(sizeof(PCWSTR)*count);
    if(!paths) return;
    WCHAR *buf=(WCHAR*)CoTaskMemAlloc(sizeof(WCHAR)*MAX_PATH*count);
    for(int i=0;i<count;i++){
        StringCchPrintf(&buf[i*MAX_PATH],MAX_PATH,L"%s%s",dir,names[i]);
        WCHAR full[700]; JoinPath(folder,names[i],full,ARRAYSIZE(full));
        if(RunCli(site,L"get",full,&buf[i*MAX_PATH],NULL)!=0) continue;
        paths[i]=&buf[i*MAX_PATH];
    }
    // Build CF_HDROP
    SIZE_T sz=sizeof(DROPFILES)+2;
    for(int i=0;i<count;i++) sz+=(wcslen(paths[i])+1)*sizeof(WCHAR);
    HGLOBAL h=GlobalAlloc(GMEM_MOVEABLE,sz);
    if(h){
        DROPFILES *df=(DROPFILES*)GlobalLock(h);
        df->pFiles=sizeof(DROPFILES); df->fWide=TRUE; df->pt.x=0; df->pt.y=0;
        WCHAR *p=(WCHAR*)((BYTE*)df+sizeof(DROPFILES));
        for(int i=0;i<count;i++){ StringCchCopy(p,(sz-((BYTE*)p-(BYTE*)df))/2,paths[i]); p+=wcslen(paths[i])+1; }
        *p=0;
        GlobalUnlock(h);
        if(OpenClipboard(hwnd)){ EmptyClipboard(); SetClipboardData(CF_HDROP,h); CloseClipboard(); }
        else GlobalFree(h);
    }
    CoTaskMemFree(paths); CoTaskMemFree(buf);
}
static void ServerCopy(HWND hwnd, PCWSTR site, PCWSTR folder, PCWSTR *names, int count)
{
    // WinSCP Duplicate: every selected item gets a "<name> - Copy" copy on the server.
    BOOL ok = TRUE;
    for (int i = 0; i < count; i++)
    {
        WCHAR newName[256];
        StringCchPrintf(newName,ARRAYSIZE(newName),L"%s - Copy",names[i]);
        WCHAR src[700],dst[700]; JoinPath(folder,names[i],src,ARRAYSIZE(src)); JoinPath(folder,newName,dst,ARRAYSIZE(dst));
        if(RunCli(site,L"dup",src,dst,NULL)!=0) ok=FALSE;
    }
    if(!ok) MessageBoxW(hwnd,L"Remote copy failed.",L"Remote",MB_OK|MB_ICONERROR);
    else FtpCacheClear();
}
static void ServerMove(HWND hwnd, PCWSTR site, PCWSTR folder, PCWSTR *names, int count)
{
    // Move all selected items to a target directory (WinSCP "Move to").
    WCHAR dst[700]; StringCchCopy(dst,ARRAYSIZE(dst),L"/");
    if(!PromptText(hwnd,L"Move to... (target remote directory)",dst,ARRAYSIZE(dst),dst)) return;
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
    if(!ok) MessageBoxW(hwnd,L"Move failed.",L"Remote",MB_OK|MB_ICONERROR);
    else FtpCacheClear();
}
static void DoRename(HWND hwnd, PCWSTR site, PCWSTR folder, PCWSTR name)
{
    WCHAR newName[256]; StringCchCopy(newName,ARRAYSIZE(newName),name);
    if(!PromptText(hwnd,L"Rename",newName,ARRAYSIZE(newName),newName)) return;
    if(0==StrCmp(newName,name)) return;
    WCHAR src[700],dst[700]; JoinPath(folder,name,src,ARRAYSIZE(src)); JoinPath(folder,newName,dst,ARRAYSIZE(dst));
    if(RunCli(site,L"rename",src,dst,NULL)!=0) MessageBoxW(hwnd,L"Rename failed.",L"Remote",MB_OK|MB_ICONERROR);
    else FtpCacheClear();
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
        StringCchPrintf(ws,ARRAYSIZE(ws),L"\"C:\\Program Files (x86)\\WinSCP\\WinSCP.com\" /command \"open \\\"%s\\\"\" \"%s\" \"close\" \"exit\"",site,cmd);
        STARTUPINFOW si={sizeof(si)}; PROCESS_INFORMATION pi={};
        if(CreateProcessW(NULL,ws,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi)){
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        }
    } else {
        // local command via ShellExecute
        HINSTANCE hr=ShellExecuteW(hwnd,NULL,cmd,NULL,NULL,SW_SHOWNORMAL);
        if((INT_PTR)hr<=32){ MessageBoxW(hwnd,L"Custom command failed.",L"Remote",MB_OK|MB_ICONERROR); }
    }
}

// ---- background (folder empty area) menu helpers ----------------------------

static void NewFolderRemote(HWND hwnd, PCWSTR site, PCWSTR folder)
{
    WCHAR name[256] = L"New folder";
    if (!PromptText(hwnd, L"New folder", name, ARRAYSIZE(name), name)) return;
    WCHAR full[700]; JoinPath(folder, name, full, ARRAYSIZE(full));
    if (RunCli(site, L"mkdir", full, NULL, NULL) != 0)
        MessageBoxW(hwnd, L"Failed to create folder.", L"Remote", MB_OK|MB_ICONERROR);
    else FtpCacheClear();
}
static void PasteClipboardToFolder(HWND hwnd, PCWSTR site, PCWSTR folder)
{
    if (!OpenClipboard(hwnd)) return;
    HANDLE h = GetClipboardData(CF_HDROP);
    if (h)
    {
        DROPFILES *df = (DROPFILES*)GlobalLock(h);
        if (df)
        {
            BOOL wide = df->fWide;
            PCWSTR pw = (PCWSTR)((BYTE*)df + df->pFiles);
            PCSTR  pa = (PCSTR)((BYTE*)df + df->pFiles);
            BOOL ok = TRUE;
            while (wide ? *pw : *pa)
            {
                WCHAR local[MAX_PATH], name[MAX_PATH];
                if (wide) { StringCchCopy(local, MAX_PATH, pw); pw += wcslen(pw) + 1; }
                else      { MultiByteToWideChar(CP_ACP, 0, pa, -1, local, MAX_PATH); pa += strlen(pa) + 1; }
                StringCchCopy(name, MAX_PATH, PathFindFileNameW(local));
                WCHAR full[700]; JoinPath(folder, name, full, ARRAYSIZE(full));
                if (RunCli(site, L"put", local, full, NULL) != 0) ok = FALSE;
            }
            GlobalUnlock(h);
            FtpCacheClear();
            if (!ok) MessageBoxW(hwnd, L"Some files could not be uploaded.", L"Remote", MB_OK|MB_ICONERROR);
        }
    }
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
        StringCchPrintf(ws, ARRAYSIZE(ws), L"\"C:\\Program Files (x86)\\WinSCP\\WinSCP.com\" /command \"open \\\"%s\\\"\" \"%s\" \"close\" \"exit\"", site, cmd);
        STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi = {};
        if (CreateProcessW(NULL, ws, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    }
    else
    {
        HINSTANCE hr = ShellExecuteW(hwnd, NULL, cmd, NULL, NULL, SW_SHOWNORMAL);
        if ((INT_PTR)hr <= 32) MessageBoxW(hwnd, L"Custom command failed.", L"Remote", MB_OK|MB_ICONERROR);
    }
}

class CMenu : public IContextMenu, public IShellExtInit, public IObjectWithSite {
public:
 CMenu():ref(1),data(NULL),site(NULL),m_pidlFolder(NULL){DllAddRef();}
 HRESULT QueryInterface(REFIID r,void**p){static const QITAB q[]={QITABENT(CMenu,IContextMenu),QITABENT(CMenu,IShellExtInit),QITABENT(CMenu,IObjectWithSite),{0}};return QISearch(this,q,r,p);}
 ULONG AddRef(){return InterlockedIncrement(&ref);} ULONG Release(){long n=InterlockedDecrement(&ref);if(!n)delete this;return n;}
 HRESULT QueryContextMenu(HMENU m,UINT i,UINT first,UINT,UINT flags){
    if(flags&CMF_DEFAULTONLY)return MAKE_HRESULT(SEVERITY_SUCCESS,0,0);
    SELDATA sel; if(!CollectSelection(data,&sel)) return MAKE_HRESULT(SEVERITY_SUCCESS,0,0);
    // Site-picker items (no site segment in the folder PIDL) get the system
    // default menu (Open/Pin/Rename/Delete/Properties) only — our WinSCP-style
    // commands operate on remote files, not on saved connections.
    if(!sel.site[0]) return MAKE_HRESULT(SEVERITY_SUCCESS,0,0);
    BOOL multi = sel.count>1;
    InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_OPEN,L"Open");
    InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_EDIT,L"Edit");
    InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_DOWNLOAD,L"Download");
    InsertMenuW(m,i++,MF_BYPOSITION|MF_SEPARATOR,0,NULL);
    InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_COPY_CLIP,L"Copy to clipboard");
    InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_COPY_NAME,L"Copy file name");
    InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_COPY_NATIVE,L"Copy remote path");
    InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_COPY_FULL,L"Copy full path");
    InsertMenuW(m,i++,MF_BYPOSITION|MF_SEPARATOR,0,NULL);
    InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_RCOPY,multi?L"Duplicate (all)":L"Duplicate");
    InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_RMOVE,multi?L"Move to... (all)":L"Move to...");
    InsertMenuW(m,i++,MF_BYPOSITION|(multi?MF_GRAYED:0),first+MENU_RENAME,L"Rename");
    InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_DELETE,L"Delete from server");
    int custom=0; CUSTCMD cmds[MAX_CUSTOM]={};
    if(!multi){ custom=LoadCustomCommands(cmds,MAX_CUSTOM); if(custom>0) InsertMenuW(m,i++,MF_BYPOSITION|MF_SEPARATOR,0,NULL);
        for(int k=0;k<custom;k++) InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_CUSTOM_BASE+k,cmds[k].name); }
    InsertMenuW(m,i++,MF_BYPOSITION|MF_SEPARATOR,0,NULL);
    InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_PROPERTIES,multi?L"Properties (first)":L"Remote properties");
    return MAKE_HRESULT(SEVERITY_SUCCESS,0,12+custom);
 }
 HRESULT InvokeCommand(LPCMINVOKECOMMANDINFO ci){
    UINT id=IS_INTRESOURCE(ci->lpVerb)?LOWORD((UINT_PTR)ci->lpVerb):99;
    if(!data)return E_INVALIDARG;
    SELDATA sel; if(!CollectSelection(data,&sel))return E_FAIL;
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
    case MENU_RCOPY: ServerCopy(ci->hwnd,sel.site,sel.folder,pnames,sel.count); break;
    case MENU_RMOVE: ServerMove(ci->hwnd,sel.site,sel.folder,pnames,sel.count); break;
    case MENU_RENAME: DoRename(ci->hwnd,sel.site,sel.folder,sel.names[0]); break;
    case MENU_DELETE:
        if(IDYES==MessageBoxW(ci->hwnd,L"Delete the selected item(s) on the remote server?",L"Remote",MB_YESNO|MB_ICONWARNING)){
            BOOL ok=TRUE;
            for(int k=0;k<sel.count;k++){
                WCHAR full[700]; JoinPath(sel.folder,sel.names[k],full,ARRAYSIZE(full));
                if(RunCli(sel.site,L"delete",full,NULL,NULL)!=0) ok=FALSE;
            }
            FtpCacheClear();
            if(sel.notify) SHChangeNotify(SHCNE_UPDATEDIR,SHCNF_IDLIST,sel.notify,NULL);
            if(!ok) MessageBoxW(ci->hwnd,L"Some items could not be deleted.",L"Remote",MB_OK|MB_ICONERROR);
        }
        break;
    case MENU_PROPERTIES:{
        PROPMETA pm={}; StringCchCopy(pm.site,ARRAYSIZE(pm.site),sel.site);
        JoinPath(sel.folder,sel.names[0],pm.path,ARRAYSIZE(pm.path));
        if(ReadRemoteMeta(sel.site,sel.folder,sel.names[0],&pm.meta))
            DialogBoxParamW(g_hInst,MAKEINTRESOURCEW(IDD_PERMBOX),ci->hwnd,PermDlgProc,(LPARAM)&pm);
        else MessageBoxW(ci->hwnd,L"Metadata unavailable.",L"Remote properties",MB_OK|MB_ICONINFORMATION);
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
        const FTPSITE *s = FtpSiteFind(pm->site);
        WCHAR buf[64] = {};
        SetDlgItemTextW(hDlg, 4001, (s && s->name[0]) ? s->name : pm->site);
        SetDlgItemTextW(hDlg, 4002, s ? s->host : L"");
        SetDlgItemTextW(hDlg, 4003, s ? s->type : L"");
        if (s) StringCchPrintf(buf, ARRAYSIZE(buf), L"%d", s->port);
        SetDlgItemTextW(hDlg, 4004, buf);
        SetDlgItemTextW(hDlg, 4005, s ? s->user : L"");
        SetDlgItemTextW(hDlg, 4006, s ? s->startPath : L"");
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
        REMOTEMETA *m = &pm->meta;
        SetDlgItemTextW(hDlg,3001,m->name); SetDlgItemTextW(hDlg,3002,m->type);
        SetDlgItemTextW(hDlg,3003,m->mode); SetDlgItemTextW(hDlg,3004,m->owner);
        SetDlgItemTextW(hDlg,3005,m->group); SetDlgItemTextW(hDlg,3006,m->size); SetDlgItemTextW(hDlg,3007,m->mtime);
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
                        FtpCacheClear();
                        if (pm->notify) SHChangeNotify(SHCNE_UPDATEDIR,SHCNF_IDLIST,pm->notify,NULL);
                    }
                    else MessageBoxW(hDlg,L"chmod failed.",L"Remote",MB_OK|MB_ICONERROR);
                }
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
            psp.dwFlags = PSP_USECALLBACK;
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
        psp.dwFlags = PSP_USECALLBACK;              // title comes from template CAPTION
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
            BG_INSERT(L"New site...");
        }
        else
        {
            BG_INSERT(L"Copy current path");
            BG_INSERT(L"New folder...");
            BG_INSERT(L"Paste files here");
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
        if (m_pidl) { PidlSite(m_pidl, site, ARRAYSIZE(site)); PidlPath(m_pidl, folder, ARRAYSIZE(folder)); }
        UINT k = rel - m_defaultCount;
        if (m_nLevel == 0)
        {
            // Site picker: new site launches the GUI client's site manager.
            if (k == 0)
                ShellExecuteW(ci->hwnd, NULL,
                    L"D:\\tools\\explorer-remote-fs\\src-client\\RemoteFsClient\\bin\\Release\\net8.0-windows\\RemoteFsClient.exe",
                    NULL, NULL, SW_SHOWNORMAL);
            return S_OK;
        }
        switch (k)
        {
        case 0: { std::wstring t = site; t += L":"; t += folder; CopyTextToClipboard(ci->hwnd, t.c_str()); break; }
        case 1: NewFolderRemote(ci->hwnd, site, folder); break;
        case 2: PasteClipboardToFolder(ci->hwnd, site, folder); break;
        default: BgCustomCommand(ci->hwnd, site, folder, k - 3); break;
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
