#pragma once

#include "stdafx.h"
#include "CrtCompat.h" // 补齐 _countof（非 Win）等 MSVC 专有 CRT；作为窗口类公共上游一处覆盖

#include <string>

// 十六进制视图配色（HexView 自绘用）
struct SHexPalette
{
    COLORREF crBg;      // 普通行背景
    COLORREF crBgAlt;   // 交替行背景
    COLORREF crOffset;  // 偏移列文字
    COLORREF crHex;     // 十六进制列文字
    COLORREF crAscii;   // ASCII 列文字
    COLORREF crSelBg;   // 选中背景
    COLORREF crSelText; // 选中文字
    COLORREF crCaret;   // 光标
    COLORREF crGrid;    // 行分隔线
};

// 窗口外框配色（标题栏 / 菜单栏 / 工具栏 / 标签栏 / 状态栏）
struct SChrome
{
    COLORREF crCaption; // 标题栏底色
    COLORREF crBar;     // 菜单栏、工具栏底色
    COLORREF crStatus;  // 状态栏、标签条底色
    COLORREF crContent; // 内容区底色
    COLORREF crBorder;  // 边框 / 分隔线
    COLORREF crTitle;   // 标题文字
    COLORREF crText;    // 菜单、工具栏文字
    COLORREF crTextDim; // 状态栏文字
    COLORREF crGlyph;   // 窗口按钮、工具条字形
};

// 一套完整配色（浅色 / 深色）
struct STheme
{
    const wchar_t *pszName;
    bool           bDark;
    SHexPalette    hex;
    SChrome        chrome;
};

namespace Theme
{
enum
{
    LIGHT = 0,
    DARK  = 1,
};

const STheme &Get(int nIndex);
int           GetCount();

// 配置持久化（exe 目录 HexEditor.ini 的 [View] Theme）
int  LoadSavedIndex();
void SaveIndex(int nIndex);
/** 配置文件路径（exe 目录 HexEditor.ini） */
std::wstring IniPath();

// 原生 Win32 菜单（TrackPopupMenu）的暗色开关。
// Win10 1903+ 的 uxtheme 未公开导出序号：135 = SetPreferredAppMode，136 = FlushMenuThemes。
// **必须在进程创建任何窗口之前调用**，否则不生效。
void ApplyNativeMenuTheme(bool bDark);

// 允许指定窗口使用暗色主题（uxtheme 序号 133 = AllowDarkModeForWindow）。
// 原生菜单是否画成暗色，取决于【所有者窗口】是否被标记为暗色。
void AllowDarkWindow(HWND hWnd, bool bDark);
} // namespace Theme
