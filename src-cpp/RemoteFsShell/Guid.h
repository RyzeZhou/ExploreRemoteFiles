/**************************************************************************
    RemoteFsShell - Explorer Remote Filesystem Shell Namespace Extension
    GUID definitions for the RemoteFS namespace extension.
**************************************************************************/

// This file contains the CLSID and Property Keys used in the extension.
#define INITGUID
#include <guiddef.h>
#include <propkeydef.h>

// Command Line,
// explorer ::{20D04FE0-3AEA-1069-A2D8-08002B30309D}\::{BB7CB9B5-4CD1-4C2E-B585-BF95B141CD15}

DEFINE_GUID(CLSID_RemoteFsShell, 0xbb7cb9b5, 0x4cd1, 0x4c2e, 0xb5, 0x85, 0xbf, 0x95, 0xb1, 0x41, 0xcd, 0x15);

DEFINE_GUID(CLSID_RemoteFsShellContextMenu, 0xd3fd7c50, 0xbf7e, 0x4a0c, 0xba, 0x6f, 0xe3, 0x69, 0x4b, 0x91, 0xab, 0xc8);

DEFINE_GUID(CLSID_RemoteFsShellPropSheet, 0x1a733758, 0x8795, 0x437d, 0xb7, 0xe5, 0xf1, 0x4c, 0x39, 0xdc, 0xec, 0x3f);

// Col 1
// name="Remote.Permissions"  -> "drwxr-xr-x" 风格
// {FBF7453E-DC5F-4A6F-9302-526228C77955}
DEFINE_PROPERTYKEY(PKEY_Remote_Permissions, 0xfbf7453e, 0xdc5f, 0x4a6f, 0x93, 0x02, 0x52, 0x62, 0x28, 0xc7, 0x79, 0x55, 2);

// Col 2
// name="Remote.Owner"
// {EB18C18D-CF33-4AF1-8DCB-34880C260AC5}
DEFINE_PROPERTYKEY(PKEY_Remote_Owner, 0xeb18c18d, 0xcf33, 0x4af1, 0x8d, 0xcb, 0x34, 0x88, 0x0c, 0x26, 0x0a, 0xc5, 2);

// Col 3
// name="Remote.Group"
// {D4149AAF-7D8D-49EE-B436-BC24A4B575C5}
DEFINE_PROPERTYKEY(PKEY_Remote_Group, 0xd4149aaf, 0x7d8d, 0x49ee, 0xb4, 0x36, 0xbc, 0x24, 0xa4, 0xb5, 0x75, 0xc5, 2);

// Col 4
// name="Remote.Size"
// {1E83002B-7181-461E-AD20-9070048FCC58}
DEFINE_PROPERTYKEY(PKEY_Remote_Size, 0x1e83002b, 0x7181, 0x461e, 0xad, 0x20, 0x90, 0x70, 0x04, 0x8f, 0xcc, 0x58, 2);

// Col 5
// name="Remote.Modified"
// {3D0E1BF8-FA3A-4F1F-BDBC-EB808CCF1FD6}
DEFINE_PROPERTYKEY(PKEY_Remote_Modified, 0x3d0e1bf8, 0xfa3a, 0x4f1f, 0xbd, 0xbc, 0xeb, 0x80, 0x8c, 0xcf, 0x1f, 0xd6, 2);
