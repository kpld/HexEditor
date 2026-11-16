// HexEditor 资源标识与菜单命令 ID
#ifndef HEX_RESOURCE_H
#define HEX_RESOURCE_H

#define XML_MAINWND L"XML_MAINWND"

// ---- 图标资源 ----
// 与 HexEditor.rc 里的 `#define IDI_APPICON 101` 保持一致（rc 不 include 本文件）。
// 关于对话框用 MessageBoxIndirect + MB_USERICON 指定它。
#define IDI_APPICON 101

// ---- 文件菜单 ----
#define IDM_FILE_NEW        1001
#define IDM_FILE_OPEN       1002
#define IDM_FILE_SAVE       1003
#define IDM_FILE_SAVEAS     1004
#define IDM_FILE_EXIT       1005
#define IDM_FILE_READONLY   1006
#define IDM_FILE_PROPERTIES 1007
#define IDM_FILE_FASTSAVE   1008

// ---- 编辑菜单 ----
#define IDM_EDIT_UNDO       2001
#define IDM_EDIT_REDO       2002
#define IDM_EDIT_CUT        2003
#define IDM_EDIT_COPY       2004
#define IDM_EDIT_PASTE      2005
#define IDM_EDIT_DELETE     2006
#define IDM_EDIT_INSERT     2007
#define IDM_EDIT_SELECTALL  2008
#define IDM_EDIT_INSMODE    2009
#define IDM_EDIT_FILL       2010

// ---- 查看菜单 ----
#define IDM_VIEW_BPR16      3001
#define IDM_VIEW_BPR24      3002
#define IDM_VIEW_BPR32      3003
#define IDM_VIEW_BPR48      3004
#define IDM_VIEW_INSPECTOR  3005
#define IDM_VIEW_STATUSBAR  3006
#define IDM_VIEW_TOOLBAR    3007
#define IDM_VIEW_CHARSET    3008
#define IDM_VIEW_THEME_LIGHT 3009
#define IDM_VIEW_THEME_DARK  3010
#define IDM_VIEW_LANG_ZH     3011
#define IDM_VIEW_LANG_EN     3012

// ---- 搜索菜单 ----
#define IDM_SEARCH_FIND     4001
#define IDM_SEARCH_NEXT     4002
#define IDM_SEARCH_PREV     4003
#define IDM_SEARCH_GOTO     4004
#define IDM_SEARCH_REPLACE  4005
#define IDM_SEARCH_CHECKSUM 4006

// ---- 窗口菜单 ----
#define IDM_WIN_MIN         5001
#define IDM_WIN_MAXRESTORE  5002
#define IDM_WIN_CLOSE       5003

// ---- 帮助菜单 ----
#define IDM_HELP_KEYS       6002
#define IDM_HELP_ABOUT      6001

#endif // HEX_RESOURCE_H
