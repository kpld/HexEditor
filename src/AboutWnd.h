//
// CAboutWnd - 关于窗口
//
// 独立 SHostWnd（owner = 主窗口），WS_POPUP + SOUI 自绘 caption。
// 为什么不用原生 MessageBoxIndirect：Windows 的深色 MessageBox 只认系统全局
// 深色设置，无法 per-app 跟随应用主题（实测 uxtheme AllowDarkModeForWindow /
// SetWindowTheme 对 #32770 无效），所以关于窗口也走 SOUI 自绘，才能适配深/浅色。
// 头文件内联实现，避免新增 .cpp 触发 CMake 的 file(GLOB) 重新配置。
//
#ifndef ABOUT_WND_H
#define ABOUT_WND_H

#include <souistd.h>
#include <control/SouiCtrls.h>
#include <core/SHostWnd.h>

#include "I18n.h"
#include "Theme.h"

using namespace SOUI;

class CAboutWnd : public SHostWnd
{
  public:
    CAboutWnd()
        : SHostWnd(L"LAYOUT:XML_ABOUT")
    {
    }

    // 按当前语言填充文本（创建后 / 切换语言后调用）。
    // 正文复用 lang_*.xml 里已有的三条 bullet 翻译，按行拼起来。
    void RefreshText()
    {
        if (SWindow *p = FindChildByName(L"txt_name"))
            p->SetWindowText(L"HexEditor 1.0");
        if (SWindow *p = FindChildByName(L"txt_sub"))
            p->SetWindowText(I18n::T(L"基于 SOUI4 的十六进制编辑器"));
        if (SWindow *p = FindChildByName(L"txt_body"))
        {
            SStringT body = I18n::T(L"· 只读 mmap + 页覆盖层，超大文件秒开\n") +
                            I18n::T(L"· Piece 表描述逻辑流，插入/删除无需搬移数据\n") +
                            I18n::T(L"· 多级撤销/重做、后台查找、数据检查器");
            p->SetWindowText(body);
        }
        if (SWindow *p = FindChildByName(L"btn_ok"))
            p->SetWindowText(I18n::T(L"确定"));
    }

    // 应用浅色 / 深色配色（与数据检查器 / 查找浮窗同一套做法）。
    void ApplyTheme(const STheme &th)
    {
        const SChrome &c = th.chrome;
        const bool     bDark = th.bDark;

        const SStringW strCaption = Clr(c.crCaption);
        const SStringW strContent = Clr(bDark ? RGB(0x25, 0x25, 0x26) : RGB(0xFF, 0xFF, 0xFF));
        const SStringW strBorder = Clr(c.crBorder);
        const SStringW strTitle = Clr(c.crTitle);
        const SStringW strGlyph = Clr(c.crGlyph);
        const SStringW strText = Clr(c.crText);
        const SStringW strDim = Clr(c.crTextDim);
        const SStringW strBtnSkin =
            bDark ? SStringW(L"skin_find_btn_dark") : SStringW(L"skin_find_btn");

        if (SWindow *pRoot = GetRoot())
            pRoot->SetAttribute(L"colorBkgnd", strContent);

        if (SWindow *p = FindChildByName(L"caption"))
            p->SetAttribute(L"colorBkgnd", strCaption);
        if (SWindow *p = FindChildByName(L"txt_title"))
            p->SetAttribute(L"colorText", strTitle);
        if (SWindow *p = FindChildByName(L"btn_close"))
            p->SetAttribute(L"colorText", strGlyph);

        const wchar_t *arrHr[] = {L"hr0", L"hr1"};
        for (int i = 0; i < _countof(arrHr); i++)
        {
            if (SWindow *p = FindChildByName(arrHr[i]))
                p->SetAttribute(L"colorLine", strBorder);
        }

        if (SWindow *p = FindChildByName(L"content"))
            p->SetAttribute(L"colorBkgnd", strContent);

        if (SWindow *p = FindChildByName(L"txt_name"))
            p->SetAttribute(L"colorText", strTitle);
        if (SWindow *p = FindChildByName(L"txt_sub"))
            p->SetAttribute(L"colorText", strDim);
        if (SWindow *p = FindChildByName(L"txt_body"))
            p->SetAttribute(L"colorText", strText);

        if (SWindow *p = FindChildByName(L"btn_ok"))
        {
            p->SetAttribute(L"skin", strBtnSkin);
            p->SetAttribute(L"colorText", strText);
            p->Invalidate();
        }

        if (SWindow *pRoot = GetRoot())
            pRoot->Invalidate();
    }

    // 居中显示在主窗口上
    void ShowCentered(HWND hParent)
    {
        RECT rcP, rc;
        ::GetWindowRect(hParent, &rcP);
        ::GetWindowRect(m_hWnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        int x = rcP.left + ((rcP.right - rcP.left) - w) / 2;
        int y = rcP.top + ((rcP.bottom - rcP.top) - h) / 2;
        ::SetWindowPos(m_hWnd, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
        ::SetForegroundWindow(m_hWnd);
        if (SWindow *p = FindChildByName(L"btn_ok"))
            p->SetFocus();
    }

  protected:
    void OnBtnClose()
    {
        ShowWindow(SW_HIDE);
    }
    void OnClose()
    {
        ShowWindow(SW_HIDE);
    }
    void OnOk()
    {
        ShowWindow(SW_HIDE);
    }

    BEGIN_MSG_MAP_EX(CAboutWnd)
    MSG_WM_CLOSE(OnClose)
    CHAIN_MSG_MAP(SHostWnd)
    END_MSG_MAP()

    EVENT_MAP_BEGIN()
    EVENT_NAME_COMMAND(L"btn_close", OnBtnClose)
    EVENT_NAME_COMMAND(L"btn_ok", OnOk)
    EVENT_MAP_END2(SHostWnd)

  private:
    static SStringW Clr(COLORREF cr)
    {
        SStringW s;
        s.Format(L"#%02X%02X%02X", GetRValue(cr), GetGValue(cr), GetBValue(cr));
        return s;
    }
};

#endif // ABOUT_WND_H
