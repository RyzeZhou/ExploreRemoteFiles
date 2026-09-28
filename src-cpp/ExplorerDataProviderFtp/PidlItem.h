// ERF PIDL 项结构 —— **唯一权威定义**。
//
// 为什么必须共享：ExplorerDataProvider.cpp 创建 PIDL，ContextMenu.cpp 在右键菜单里读它。
// 这两处曾经各写一份结构体（tagObject / COMPACTITEM），2026-09-27 给 tagObject 插入
// fAbsRoot 时漏改了另一份，szName 的偏移从 12 变成 16 —— 于是右键菜单读到的"文件名"
// 其实是 fAbsRoot 那 4 个字节：菜单项照旧能构建（IsOurs 的 cb 检查恰好还能通过），
// 但名字全空，属性 / 新建文件 / 新建文件夹 / 刷新等依赖路径解析的功能一律报错。
//
// 结论：布局只允许在这里改一次。下面两条 static_assert 就是防止再犯的哨兵。
#pragma once

#include <windows.h>
#include <shlobj.h>

#define MYOBJID 0x1234

// FVITEMID 按变长分配，szName 是 NUL 结尾字符串的起始处。
#pragma pack(1)
typedef struct tagObject
{
    USHORT  cb;
    WORD    MyObjID;
    BYTE    nLevel;
    BYTE    nSize;
    BYTE    nSides;
    BYTE    cchName;
    BOOL    fIsFolder;
    // 本段的路径基准：FALSE = 站点配置的 StartPath（默认，站点根语义）；
    // TRUE = 服务器绝对根 "/" —— 地址栏输入"起始路径之外"的路径时用（见 ParseDisplayName）。
    // 只有 level 1 的站点段会带它；后续段照常相对拼接，读方据此决定要不要补 StartPath。
    BOOL    fAbsRoot;
    WCHAR   szName[1];
} FVITEMID;
#pragma pack()

typedef UNALIGNED FVITEMID *PFVITEMID;
typedef const UNALIGNED FVITEMID *PCFVITEMID;

// 名字偏移的编译期哨兵：任何字段增删都会在这里编译失败，而不是等到运行时读错名字。
static_assert(FIELD_OFFSET(FVITEMID, szName) == 16,
              "FVITEMID layout changed: every PIDL reader must be updated together");
static_assert(sizeof(FVITEMID) == 18, "FVITEMID must stay packed (pragma pack(1))");

// 这一段 PIDL 是不是我们的项。
inline BOOL ErfPidlIsOurs(PCUIDLIST_RELATIVE p)
{
    return p && p->mkid.cb >= FIELD_OFFSET(FVITEMID, szName) + sizeof(WCHAR) &&
           ((const FVITEMID *)p)->MyObjID == MYOBJID;
}
