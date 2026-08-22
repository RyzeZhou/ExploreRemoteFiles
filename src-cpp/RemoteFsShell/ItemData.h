/**************************************************************************
    RemoteFsShell - shared PIDL item data layout and formatting helpers.
    Used by RemoteFsShell.cpp (folder) and ContextMenu.cpp (context menu).
**************************************************************************/
#pragma once

#include <windows.h>
#include <strsafe.h>
#include <time.h>
#include <stdio.h>

// Debug logging helper (append to C:\temp\remotefs-debug.log)
inline void DebugLog(const wchar_t *fmt, ...)
{
    FILE *f = NULL;
    if (_wfopen_s(&f, L"C:\\temp\\remotefs-debug.log", L"a") == 0 && f)
    {
        va_list args;
        va_start(args, fmt);
        vfwprintf(f, fmt, args);
        va_end(args);
        fwprintf(f, L"\n");
        fclose(f);
    }
}

#define MYOBJID 0x1234

// FVITEMID is allocated with a variable size, szName is the beginning
// of a NULL-terminated string buffer.
#pragma pack(1)
typedef struct tagObject
{
    USHORT  cb;
    WORD    MyObjID;
    BYTE    nLevel;      // 0=connections, 1=remote root, 2=directory contents
    DWORD   dwMode;      // unix permission bits, e.g. 0755
    DWORD   dwSize;      // file size in bytes (0 for directories)
    DWORD   dwMtime;     // modified time (unix epoch)
    BYTE    nOwner;      // index into c_rgOwners
    BYTE    nGroup;      // index into c_rgGroups
    BOOL    fIsFolder;
    BOOL    fIsSymlink;
    BYTE    cchName;
    WCHAR   szName[1];
} FVITEMID;
#pragma pack()

typedef UNALIGNED FVITEMID *PFVITEMID;
typedef const UNALIGNED FVITEMID *PCFVITEMID;

static const PCWSTR c_rgOwners[] = { L"root", L"deploy", L"www-data", L"ftpuser", L"alex" };
static const PCWSTR c_rgGroups[] = { L"root", L"dev", L"www-data", L"ftp", L"staff" };

inline BOOL IsValidRemoteItem(PCUIDLIST_RELATIVE pidl, PCFVITEMID *ppItem)
{
    *ppItem = NULL;
    if (!pidl)
    {
        return FALSE;
    }
    PCFVITEMID pidmine = (PCFVITEMID)pidl;
    if (pidmine->cb && MYOBJID == pidmine->MyObjID && pidmine->nLevel <= 3)
    {
        *ppItem = pidmine;
        return TRUE;
    }
    return FALSE;
}

inline void FormatMode(DWORD dwMode, BOOL fIsFolder, BOOL fIsSymlink, PWSTR psz, UINT cch)
{
    WCHAR chType = fIsSymlink ? L'l' : (fIsFolder ? L'd' : L'-');
    StringCchPrintf(psz, cch, L"%c%c%c%c%c%c%c%c%c%c", chType,
        (dwMode & 0400) ? L'r' : L'-', (dwMode & 0200) ? L'w' : L'-', (dwMode & 0100) ? L'x' : L'-',
        (dwMode & 0040) ? L'r' : L'-', (dwMode & 0020) ? L'w' : L'-', (dwMode & 0010) ? L'x' : L'-',
        (dwMode & 0004) ? L'r' : L'-', (dwMode & 0002) ? L'w' : L'-', (dwMode & 0001) ? L'x' : L'-');
}

inline void FormatSize(DWORD dwSize, BOOL fIsFolder, PWSTR psz, UINT cch)
{
    if (fIsFolder)
    {
        StringCchCopy(psz, cch, L"-");
        return;
    }
    if (dwSize < 1024)
    {
        StringCchPrintf(psz, cch, L"%u B", dwSize);
    }
    else if (dwSize < 1024 * 1024)
    {
        StringCchPrintf(psz, cch, L"%.1f KB", dwSize / 1024.0);
    }
    else if (dwSize < 1024 * 1024 * 1024)
    {
        StringCchPrintf(psz, cch, L"%.1f MB", dwSize / (1024.0 * 1024.0));
    }
    else
    {
        StringCchPrintf(psz, cch, L"%.2f GB", dwSize / (1024.0 * 1024.0 * 1024.0));
    }
}

inline void FormatMtime(DWORD dwMtime, PWSTR psz, UINT cch)
{
    __time64_t t = (__time64_t)dwMtime;
    struct tm tmLocal;
    if (_localtime64_s(&tmLocal, &t) == 0)
    {
        StringCchPrintf(psz, cch, L"%04d-%02d-%02d %02d:%02d",
            tmLocal.tm_year + 1900, tmLocal.tm_mon + 1, tmLocal.tm_mday,
            tmLocal.tm_hour, tmLocal.tm_min);
    }
    else
    {
        StringCchCopy(psz, cch, L"-");
    }
}
