/**************************************************************************
    RemoteFsShell - real FTP/SFTP data source bridge.
    Phase 2-A: launches ExplorerRemoteFs.Cli for Provider-backed directory
    listings and file operations.  The shell process performs no network I/O.

    The bridge path and connection name are hardcoded for the local test
    server; Phase 2-B will replace per-call spawn with a persistent service.
**************************************************************************/
#pragma once

#include "ItemData.h"

// Enumerate an FTP directory. Returns the number of items filled, or -1 on error.
int FtpListDirectory(const wchar_t *path, ITEMDATA *out, int maxItems);

// File operations (delegated to the C# bridge).  Return 0 on success, nonzero on failure.
int FsOpDelete(const wchar_t *path);
int FsOpRename(const wchar_t *from, const wchar_t *to);
int FsOpMkdir(const wchar_t *path);

// Remember the most recently enumerated directory's remote path so the
// context menu handler can recover the current folder even though explorer
// hands it a truncated PIDL (missing the directory segment).  TTL-limited to
// avoid cross-window confusion.  Also remembers the FULL absolute PIDL of
// that directory so SHChangeNotify can refresh the correct folder.
void RememberEnumPath(int nLevel, const wchar_t *pszPath, PCIDLIST_ABSOLUTE pidlFull);
const wchar_t *GetLastEnumPath();
PCIDLIST_ABSOLUTE GetLastEnumPidl();
