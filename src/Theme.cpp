#include "stdafx.h"
#include "Theme.h"

#include "CrtCompat.h" // 非 Win 平台补齐 _stprintf_s 等 MSVC 专有 CRT（Win 下为空）

#include <string>

namespace
{
// 浅色：沿用 HexEditor 原来的配色（十六进制区 + 浅灰工具条）
const STheme g_lightTheme = {
    L"浅色",
    false,
    // ---- 十六进制区 ----
    {
        RGBA(255, 255, 255, 255), // crBg
        RGBA(245, 247, 250, 255), // crBgAlt
        RGBA(140, 150, 160, 255), // crOffset
        RGBA(25, 28, 33, 255),    // crHex
        RGBA(30, 90, 150, 255),   // crAscii
        RGBA(51, 133, 255, 255),  // crSelBg
        RGBA(255, 255, 255, 255), // crSelText
        RGBA(0, 0, 0, 255),       // crCaret
        RGBA(228, 232, 238, 255), // crGrid
    },
    // ---- 窗口外框 ----
    // 标题栏 / 菜单栏+工具栏 / 标签栏+状态栏 三处统一成同一底色（仿照 CodeEditor 的 crWindow），
    // 避免几块深浅不一的灰拼在一起。
    {
        RGBA(240, 240, 240, 255), // crCaption
        RGBA(240, 240, 240, 255), // crBar
        RGBA(240, 240, 240, 255), // crStatus
        RGBA(255, 255, 255, 255), // crContent
        RGBA(215, 219, 225, 255), // crBorder
        RGBA(31, 41, 55, 255),    // crTitle
        RGBA(75, 85, 99, 255),    // crText
        RGBA(64, 64, 64, 255),    // crTextDim
        RGBA(58, 63, 69, 255),    // crGlyph
    }};

// 深色：VS Code 暗色系
const STheme g_darkTheme = {
    L"深色",
    true,
    // ---- 十六进制区 ----
    {
        RGBA(30, 30, 30, 255),    // crBg
        RGBA(37, 37, 38, 255),    // crBgAlt
        RGBA(110, 118, 129, 255), // crOffset
        RGBA(212, 212, 212, 255), // crHex
        RGBA(78, 201, 176, 255),  // crAscii
        RGBA(38, 79, 120, 255),   // crSelBg
        RGBA(255, 255, 255, 255), // crSelText
        RGBA(174, 175, 173, 255), // crCaret
        RGBA(44, 44, 46, 255),    // crGrid
    },
    // ---- 窗口外框 ----
    // 与浅色同理：三处外框底色统一（#252526，同 CodeEditor 深色 crWindow）。
    {
        RGBA(37, 37, 38, 255),    // crCaption
        RGBA(37, 37, 38, 255),    // crBar
        RGBA(37, 37, 38, 255),    // crStatus
        RGBA(30, 30, 30, 255),    // crContent
        RGBA(60, 60, 60, 255),    // crBorder
        RGBA(230, 230, 230, 255), // crTitle
        RGBA(204, 204, 204, 255), // crText
        RGBA(154, 154, 154, 255), // crTextDim
        RGBA(204, 204, 204, 255), // crGlyph
    }};

const STheme *g_themes[] = {&g_lightTheme, &g_darkTheme};
} // namespace

namespace Theme
{
const STheme &Get(int nIndex)
{
    if (nIndex < 0 || nIndex >= GetCount())
        nIndex = LIGHT;
    return *g_themes[nIndex];
}

int GetCount()
{
    return (int)(sizeof(g_themes) / sizeof(g_themes[0]));
}

namespace
{
std::wstring IniPathImpl()
{
    TCHAR szPath[MAX_PATH] = {0};
    ::GetModuleFileName(NULL, szPath, MAX_PATH);
    std::wstring str = szPath;
    size_t nPos = str.find_last_of(L'\\');
    if (nPos != std::wstring::npos)
        str = str.substr(0, nPos);
    return str + L"\\HexEditor.ini";
}
} // namespace

std::wstring IniPath()
{
    return IniPathImpl();
}

int LoadSavedIndex()
{
    int n = ::GetPrivateProfileInt(_T("View"), _T("Theme"), LIGHT, IniPathImpl().c_str());
    if (n < 0 || n >= GetCount())
        n = LIGHT;
    return n;
}

void SaveIndex(int nIndex)
{
    TCHAR szBuf[16] = {0};
    _stprintf_s(szBuf, _T("%d"), nIndex);
    ::WritePrivateProfileString(_T("View"), _T("Theme"), szBuf, IniPathImpl().c_str());
}

void ApplyNativeMenuTheme(bool bDark)
{
#ifdef _WIN32
    // 原生 Win32 菜单（主菜单下拉 / 标签栏 ▾ / 右键菜单）的暗色开关。
    // 必须在创建任何窗口之前调用（见 main.cpp）。非 Windows 平台没有 uxtheme，
    // 暗色由 SOUI 自带主题处理，这里直接 no-op。
    HMODULE hUx = ::LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!hUx)
        return;
    typedef int(WINAPI * fnSetPreferredAppMode)(int);
    typedef void(WINAPI * fnFlushMenuThemes)(void);
    fnSetPreferredAppMode pSetMode =
        (fnSetPreferredAppMode)::GetProcAddress(hUx, MAKEINTRESOURCEA(135));
    fnFlushMenuThemes pFlush =
        (fnFlushMenuThemes)::GetProcAddress(hUx, MAKEINTRESOURCEA(136));
    if (pSetMode)
        pSetMode(bDark ? 2 : 0); // 2 = ForceDark, 0 = Default
    if (pFlush)
        pFlush();
    ::FreeLibrary(hUx);
#else
    (void)bDark;
#endif
}

void AllowDarkWindow(HWND hWnd, bool bDark)
{
#ifdef _WIN32
    if (!hWnd)
        return;
    HMODULE hUx = ::LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!hUx)
        return;
    // 133 = AllowDarkModeForWindow
    typedef BOOL(WINAPI * fnAllowDarkModeForWindow)(HWND, BOOL);
    fnAllowDarkModeForWindow pAllow =
        (fnAllowDarkModeForWindow)::GetProcAddress(hUx, MAKEINTRESOURCEA(133));
    if (pAllow)
        pAllow(hWnd, bDark ? TRUE : FALSE);
    // 让窗口按暗色主题解析视觉样式（菜单、滚动条等）
    typedef HRESULT(WINAPI * fnSetWindowTheme)(HWND, LPCWSTR, LPCWSTR);
    fnSetWindowTheme pSetTheme = (fnSetWindowTheme)::GetProcAddress(hUx, "SetWindowTheme");
    if (pSetTheme)
        pSetTheme(hWnd, bDark ? L"DarkMode_Explorer" : L"Explorer", NULL);
    ::FreeLibrary(hUx);
#else
    (void)hWnd;
    (void)bDark;
#endif
}
} // namespace Theme
