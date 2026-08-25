#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <string>
#include <time.h>
#include "Utils.h"
#include "resource.h"
#include <new>

#define MENU_DELETE 0
#define MENU_PROPERTIES 1
#define MYOBJID 0x1234
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
    out[0]=0; PCUIDLIST_RELATIVE p=(PCUIDLIST_RELATIVE)abs;
    while(p && p->mkid.cb) {
        if(IsOurs(p)) { WCHAR name[256]; CopyName((const COMPACTITEM*)p,name,ARRAYSIZE(name));
            if(name[0]) { StringCchCat(out,cch,L"/"); StringCchCat(out,cch,name); }
        }
        p=ILNext(p);
    }
    if(!out[0]) StringCchCopy(out,cch,L"/");
}
static void JoinPath(PCWSTR folder, PCWSTR name, PWSTR out, UINT cch)
{
    if(folder[0]==L'/' && !folder[1]) StringCchPrintf(out,cch,L"/%s",name);
    else StringCchPrintf(out,cch,L"%s/%s",folder,name);
}
static int RunCli(PCWSTR verb, PCWSTR p1, std::string *captured)
{
    WCHAR cmd[1400]; StringCchPrintf(cmd,ARRAYSIZE(cmd),L"\"D:\\tools\\explorer-remote-fs\\dist\\cli\\ExplorerRemoteFs.Cli.exe\" %s \"local-ftp\" \"%s\"",verb,p1);
    SECURITY_ATTRIBUTES sa={sizeof(sa),NULL,TRUE}; HANDLE rd=NULL,wr=NULL;
    if(captured && !CreatePipe(&rd,&wr,&sa,0)) return -1;
    if(captured) SetHandleInformation(rd,HANDLE_FLAG_INHERIT,0);
    STARTUPINFOW si={sizeof(si)}; if(captured){si.dwFlags=STARTF_USESTDHANDLES;si.hStdOutput=wr;si.hStdError=wr;si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);}
    PROCESS_INFORMATION pi={}; BOOL ok=CreateProcessW(NULL,cmd,NULL,NULL,captured?TRUE:FALSE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi);
    if(captured) CloseHandle(wr); if(!ok){if(rd)CloseHandle(rd);return -1;}
    if(captured){char b[4096];DWORD n=0;while(ReadFile(rd,b,sizeof(b),&n,NULL)&&n)captured->append(b,n);CloseHandle(rd);}
    WaitForSingleObject(pi.hProcess,10000);DWORD code=1;GetExitCodeProcess(pi.hProcess,&code);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return code==0?0:-1;
}

#define BIT(v) ((v)?BST_CHECKED:BST_UNCHECKED)
typedef struct RemoteMeta {
    WCHAR name[MAX_PATH]; WCHAR type[24]; WCHAR mode[16]; WCHAR owner[40]; WCHAR group[40];
    WCHAR size[32]; WCHAR mtime[32]; DWORD bits;
} REMOTEMETA;
static DWORD ParseModeFromLine(const WCHAR *fields[])
{
    DWORD mode = 0;
    if (!fields || !fields[1]) return 0;
    const WCHAR *m = fields[1];
    for (int i = 0; i < 9; i++) { if (m[i+1] != L'-') mode |= (0400 >> i); }
    return mode;
}
static void FillMetaFromFields(REMOTEMETA *meta, WCHAR *fields[])
{
    ZeroMemory(meta, sizeof(*meta));
    StringCchCopy(meta->name, ARRAYSIZE(meta->name), fields[9]);
    StringCchCopy(meta->mode, ARRAYSIZE(meta->mode), fields[1]);
    StringCchCopy(meta->owner, ARRAYSIZE(meta->owner), fields[4]);
    StringCchCopy(meta->group, ARRAYSIZE(meta->group), fields[5]);
    meta->bits = ParseModeFromLine((const WCHAR**)fields);
    BOOL folder = _wtoi(fields[6]) != 0;
    BOOL symlink = _wtoi(fields[7]) != 0;
    StringCchCopy(meta->type, ARRAYSIZE(meta->type), symlink ? L"Symbolic Link" : (folder ? L"Folder" : L"File"));
    DWORD size = (DWORD)_wtoi64(fields[3]);
    if (size < 1024) StringCchPrintf(meta->size, ARRAYSIZE(meta->size), L"%u B", size);
    else if (size < 1024*1024) StringCchPrintf(meta->size, ARRAYSIZE(meta->size), L"%.1f KB", size/1024.0);
    else if (size < 1024*1024*1024) StringCchPrintf(meta->size, ARRAYSIZE(meta->size), L"%.1f MB", size/(1024.0*1024.0));
    else StringCchPrintf(meta->size, ARRAYSIZE(meta->size), L"%.2f GB", size/(1024.0*1024.0*1024.0));
    __time64_t t = (__time64_t)_wtoi64(fields[2]); struct tm tmLocal;
    if (_localtime64_s(&tmLocal, &t)==0) StringCchPrintf(meta->mtime, ARRAYSIZE(meta->mtime), L"%04d-%02d-%02d %02d:%02d",
        tmLocal.tm_year+1900, tmLocal.tm_mon+1, tmLocal.tm_mday, tmLocal.tm_hour, tmLocal.tm_min);
    else StringCchCopy(meta->mtime, ARRAYSIZE(meta->mtime), L"-");
}
static BOOL ReadRemoteMeta(PCWSTR folder, PCWSTR name, REMOTEMETA *meta)
{
    std::string text; RunCli(L"pipe", folder, &text);
    std::string key="\t"; int need=WideCharToMultiByte(CP_UTF8,0,name,-1,NULL,0,NULL,NULL);
    std::string n(need?need:1,'\0'); if(need>1){ WideCharToMultiByte(CP_UTF8,0,name,-1,n.data(),need,NULL,NULL); n.resize(need-1);} else n.clear(); key+=n;
    size_t pos=text.find(key); if(pos==std::string::npos) return FALSE;
    size_t begin=text.rfind('\n',pos); begin=begin==std::string::npos?0:begin+1;
    size_t end=text.find('\n',pos); std::string line=text.substr(begin,end==std::string::npos?text.size()-begin:end-begin);
    WCHAR wide[2048]={}; if(!MultiByteToWideChar(CP_UTF8,0,line.c_str(),-1,wide,ARRAYSIZE(wide))) return FALSE;
    WCHAR *fields[10]={}; int nf=0; WCHAR *ctx=NULL; WCHAR *tok=wcstok_s(wide,L"\t",&ctx);
    while(tok&&nf<10){fields[nf++]=tok;tok=wcstok_s(NULL,L"\t",&ctx);} if(nf<10) return FALSE;
    FillMetaFromFields(meta,fields); return TRUE;
}
static INT_PTR CALLBACK PermDlgProc(HWND hDlg,UINT msg,WPARAM wp,LPARAM lp)
{
    switch(msg){
    case WM_INITDIALOG:{
        REMOTEMETA *m=(REMOTEMETA*)lp; if(!m)return TRUE;
        SetDlgItemTextW(hDlg,3001,m->name); SetDlgItemTextW(hDlg,3002,m->type);
        SetDlgItemTextW(hDlg,3003,m->mode); SetDlgItemTextW(hDlg,3004,m->owner);
        SetDlgItemTextW(hDlg,3005,m->group); SetDlgItemTextW(hDlg,3006,m->size); SetDlgItemTextW(hDlg,3007,m->mtime);
        CheckDlgButton(hDlg,3011,BIT(m->bits&0400)); CheckDlgButton(hDlg,3012,BIT(m->bits&0200)); CheckDlgButton(hDlg,3013,BIT(m->bits&0100));
        CheckDlgButton(hDlg,3014,BIT(m->bits&0040)); CheckDlgButton(hDlg,3015,BIT(m->bits&0020)); CheckDlgButton(hDlg,3016,BIT(m->bits&0010));
        CheckDlgButton(hDlg,3017,BIT(m->bits&0004)); CheckDlgButton(hDlg,3018,BIT(m->bits&0002)); CheckDlgButton(hDlg,3019,BIT(m->bits&0001));
        SetWindowLongPtrW(hDlg,DWLP_USER,(LONG_PTR)m); return TRUE;}
    case WM_COMMAND:
        if(LOWORD(wp)==IDOK||LOWORD(wp)==IDCANCEL){EndDialog(hDlg,LOWORD(wp));return TRUE;} break;
    }
    return FALSE;
}

class CMenu : public IContextMenu, public IShellExtInit, public IObjectWithSite {
public:
 CMenu():ref(1),data(NULL),site(NULL){DllAddRef();}
 HRESULT QueryInterface(REFIID r,void**p){static const QITAB q[]={QITABENT(CMenu,IContextMenu),QITABENT(CMenu,IShellExtInit),QITABENT(CMenu,IObjectWithSite),{0}};return QISearch(this,q,r,p);}
 ULONG AddRef(){return InterlockedIncrement(&ref);} ULONG Release(){long n=InterlockedDecrement(&ref);if(!n)delete this;return n;}
 HRESULT QueryContextMenu(HMENU m,UINT i,UINT first,UINT,UINT flags){if(flags&CMF_DEFAULTONLY)return MAKE_HRESULT(SEVERITY_SUCCESS,0,0);InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_DELETE,L"Delete from server");InsertMenuW(m,i,MF_BYPOSITION,first+MENU_PROPERTIES,L"Remote properties");return MAKE_HRESULT(SEVERITY_SUCCESS,0,2);}
 HRESULT InvokeCommand(LPCMINVOKECOMMANDINFO ci){UINT id=IS_INTRESOURCE(ci->lpVerb)?LOWORD((UINT_PTR)ci->lpVerb):99;if(id>1||!data)return E_INVALIDARG;WCHAR folder[512],name[256],full[700];PIDLIST_ABSOLUTE notify=NULL;if(!Selection(folder,ARRAYSIZE(folder),name,ARRAYSIZE(name),&notify))return E_FAIL;JoinPath(folder,name,full,ARRAYSIZE(full));
   if(id==MENU_DELETE){if(IDYES==MessageBoxW(ci->hwnd,L"Delete the selected item on the remote server?",L"Remote",MB_YESNO|MB_ICONWARNING)){if(RunCli(L"delete",full,NULL)==0){SHChangeNotify(SHCNE_UPDATEDIR,SHCNF_IDLIST,notify,NULL);}else MessageBoxW(ci->hwnd,L"Delete failed.",L"Remote",MB_OK|MB_ICONERROR);}}
   else {REMOTEMETA meta;if(ReadRemoteMeta(folder,name,&meta)){DialogBoxParamW(g_hInst,MAKEINTRESOURCEW(IDD_PERMBOX),ci->hwnd,PermDlgProc,(LPARAM)&meta);}else MessageBoxW(ci->hwnd,L"Metadata unavailable.",L"Remote properties",MB_OK|MB_ICONINFORMATION);}
   CoTaskMemFree(notify);return S_OK;}
 HRESULT GetCommandString(UINT_PTR id,UINT type,UINT*,LPSTR s,UINT c){if(type==GCS_VERBW) return StringCchCopyW((PWSTR)s,c,id==0?L"delete":L"properties");if(type==GCS_VERBA)return StringCchCopyA(s,c,id==0?"delete":"properties");return E_NOTIMPL;}
 HRESULT Initialize(PCIDLIST_ABSOLUTE,IDataObject*d,HKEY){if(data)data->Release();data=d;if(data)data->AddRef();return S_OK;}
 HRESULT SetSite(IUnknown*s){if(site)site->Release();site=s;if(site)site->AddRef();return S_OK;} HRESULT GetSite(REFIID r,void**p){return site?site->QueryInterface(r,p):E_FAIL;}
private:
 ~CMenu(){if(data)data->Release();if(site)site->Release();DllRelease();}
 BOOL Selection(PWSTR folder,UINT cf,PWSTR name,UINT cn,PIDLIST_ABSOLUTE *notify){FORMATETC f={(CLIPFORMAT)RegisterClipboardFormatW(CFSTR_SHELLIDLIST),NULL,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};STGMEDIUM st={};if(FAILED(data->GetData(&f,&st)))return FALSE;CIDA*cida=(CIDA*)GlobalLock(st.hGlobal);BOOL ok=FALSE;if(cida&&cida->cidl){PCIDLIST_ABSOLUTE parent=(PCIDLIST_ABSOLUTE)((BYTE*)cida+cida->aoffset[0]);PCUIDLIST_RELATIVE child=(PCUIDLIST_RELATIVE)((BYTE*)cida+cida->aoffset[1]);if(IsOurs(child)){PidlPath(parent,folder,cf);CopyName((const COMPACTITEM*)child,name,cn);*notify=ILCloneFull(parent);ok=TRUE;}}if(cida)GlobalUnlock(st.hGlobal);ReleaseStgMedium(&st);return ok;}
 long ref;IDataObject*data;IUnknown*site;
};
HRESULT CFolderViewImplContextMenu_CreateInstance(REFIID riid,void**ppv){*ppv=NULL;CMenu*m=new(std::nothrow)CMenu();if(!m)return E_OUTOFMEMORY;HRESULT hr=m->QueryInterface(riid,ppv);m->Release();return hr;}
