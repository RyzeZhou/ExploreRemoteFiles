#pragma once
// 列模型：把"显示列号"与"语义属性号"分开。
//
// 语义属性号 = 用户看到的**哪一列**，不随显示顺序变化：
//   0=名称 1=类型 2=大小 3=修改时间 4=权限 5=所有者 6=所有者ID 7=组 8=组ID
//
// 站点设置 ColumnOrder 是一串 9 个数字（0..8 的一个排列），第 i 位表示
// "显示列号 i 显示哪个属性"；默认 "012345678"（名称/类型/大小/时间/权限/
// 所有者/所有者ID/组/组ID）。
//
// 为什么需要这层翻译：第一版直接拿显示列号当属性用，列序一变就错位 ——
// 实测"UID 列怎么拖都缩不窄、而 GID 列正常"，根因就是宽度表按错位的下标取。
// 现在列宽 / 表头 / 取值 / 排序 / MapColumnToSCID 共用本文件的同一个入口。
//
// 本文件刻意不依赖 windows.h：纯逻辑可以脱离 shell 单独编译测试
// （见 research/tools/colorder-test/）。

#define ERF_COLUMN_COUNT 9

// 默认列顺序（= 恒等映射）。
static const char *ErfDefaultColumnOrder = "012345678";

// 解析站点设置里的 ColumnOrder。
// 非法（长度不为 9 / 出现非 0..8 的字符 / 数字重复）**整体**回退默认：
// 半截自定义比默认更难排查，而且列序错乱会连带宽度和排序一起错。
inline void ErfParseColumnOrder(const char *spec, int out[ERF_COLUMN_COUNT])
{
    int parsed[ERF_COLUMN_COUNT] = {};
    bool seen[ERF_COLUMN_COUNT] = {};
    int n = 0;
    bool ok = true;
    for (const char *p = spec; p && *p; ++p)
    {
        if (*p < '0' || *p > '0' + (ERF_COLUMN_COUNT - 1) || n >= ERF_COLUMN_COUNT) { ok = false; break; }
        int v = *p - '0';
        if (seen[v]) { ok = false; break; }
        seen[v] = true;
        parsed[n++] = v;
    }
    if (ok && n == ERF_COLUMN_COUNT)
    {
        for (int i = 0; i < ERF_COLUMN_COUNT; ++i) out[i] = parsed[i];
        return;
    }
    for (int i = 0; i < ERF_COLUMN_COUNT; ++i) out[i] = i;
}

// 每列的**建议宽度（单位：字符）**，按语义属性号给。
// 宽度按"这一列实际会显示什么"给：名称最长、ID 最短（4~5 位数字）。
// 注意 Explorer 的硬下限是**表头文字宽度**：表头不改短就压不到更窄。
inline int ErfColumnWidthChars(int sem, bool sitePicker)
{
    if (sitePicker)
    {
        switch (sem)
        {
        case 0: return 26;   // 名称
        case 1: return 18;   // 主机
        case 2: return 8;    // 协议
        case 3: return 7;    // 端口
        case 4: return 12;   // 用户
        case 5: return 24;   // 起始路径
        default: return 12;
        }
    }
    switch (sem)
    {
    case 0: return 32;   // 名称
    case 1: return 12;   // 类型
    case 2: return 10;   // 大小
    case 3: return 18;   // 修改时间
    case 4: return 12;   // 权限
    case 5: return 12;   // 所有者
    case 6: return 6;    // 所有者 ID（数字，尽量窄）
    case 7: return 12;   // 组
    case 8: return 6;    // 组 ID
    default: return 12;
    }
}

// 语义属性号 -> 诊断用英文名（只进日志，不进 UI）。
inline const char *ErfColumnDebugName(int sem)
{
    switch (sem)
    {
    case 0: return "name";
    case 1: return "type";
    case 2: return "size";
    case 3: return "modified";
    case 4: return "permissions";
    case 5: return "owner";
    case 6: return "ownerId";
    case 7: return "group";
    case 8: return "groupId";
    default: return "?";
    }
}
