#pragma once
#include <cstdint>

#pragma pack(push, 1)

// ==============================================================================
// LCLD: Localizer Compiled Language Dictionary Binary Format
// 纯 C 二进制字典布局：彻底消除跨 Qt 版本 C++ ABI 结构体与虚函数调用依赖
// ==============================================================================

struct LcldHeader {
    char magic[4];             // "LCLD"
    uint32_t version;          // 1
    uint32_t totalSections;    // 作用域子块总数
    uint32_t totalEntries;     // 键值对总数
    uint32_t sectionsOffset;   // LcldSection 数组相对于文件开头的字节偏移
    uint32_t stringTableOffset;// 字符串表相对于文件开头的字节偏移
    uint32_t stringTableSize;  // 字符串表总字节数
};

struct LcldSection {
    uint32_t nameOffset;       // 作用域名称（如 "common", "hammer"）在字符串表中的偏移
    uint32_t entryCount;       // 该子块中的条目数量
    uint32_t entriesOffset;    // 该子块 LcldEntry 数组相对于文件开头的字节偏移
};

struct LcldEntry {
    uint32_t keyOffset;        // UTF-8 原文字符串在字符串表中的偏移
    uint32_t valOffset;        // UTF-8 译文字符串在字符串表中的偏移
};

#pragma pack(pop)

