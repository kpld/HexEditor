//
// CFindWnd - 查找 / 替换 / 转到 非模态浮窗
//
// 独立 SHostWnd（owner = 主窗口），WS_POPUP + SOUI 自绘 caption，可拖动、可关闭。
// 只负责输入与按钮事件；检索/替换逻辑仍由 CMainFrame 处理（通过 IFindWndListener
// 回调）。头文件内联实现，避免新增 .cpp 触发 CMake 的 file(GLOB) 重新配置。
//
#ifndef FIND_WND_H
#define FIND_WND_H

#include <souistd.h>
#include <control/SouiCtrls.h>
#include <core/SHostWnd.h>

#include "I18n.h"
#include "Theme.h"

using namespace SOUI;

class IFindWndListener
{
  public:
    virtual void OnFindWndFindNext() = 0;
    virtual void OnFindWndFindPrev() = 0;
    virtual void OnFindWndReplace() = 0;
    virtual void OnFindWndReplaceAll() = 0;
    virtual void OnFindWndHexToggle() = 0;
    virtual void OnFindWndGoto() = 0;
    virtual void OnFindWndClose() = 0;

  protected:
    virtual ~IFindWndListener()
    {
    }
};

class CFindWnd : public SHostWnd
{
  public:
    CFindWnd()
        : SHostWnd(L"LAYOUT:XML_FIND")
        , m_pListener(NULL)
    {
    }

    void SetListener(IFindWndListener *p)
    {
        m_pListener = p;
    }

    SStringT GetFindText()
    {
        SEdit *p = FindChildByName2<SEdit>(L"edit_find");
        return p ? p->GetWindowText() : SStringT();
    }
    SStringT GetReplaceText()
    {
        SEdit *p = FindChildByName2<SEdit>(L"edit_replace");
        return p ? p->GetWindowText() : SStringT();
    }
    SStringT GetGotoText()
    {
        SEdit *p = FindChildByName2<SEdit>(L"edit_goto");
        return p ? p->GetWindowText() : SStringT();
    }
    void SetFindText(const SStringT &s)
    {
        SEdit *p = FindChildByName2<SEdit>(L"edit_find");
        if (p)
            p->SetWindowText(s);
    }
    void SetStatus(const SStringT &s)
    {
        SStatic *p = FindChildByName2<SStatic>(L"txt_status");
        if (p)
            p->SetWindowText(s);
    }
    /** HEX 模式下按钮高亮提示 */
    void SetHexMode(bool bHex)
    {
        SWindow *p = FindChildByName2<SWindow>(L"btn_hex");
        if (p)
            p->SetWindowText(bHex ? L"HEX" : I18n::T(L"文本"));
    }

    // 应用浅色 / 深色配色（与数据检查器同一套做法）。
    // 根窗口、标题栏、分隔线、标签、输入框、按钮、状态文字全部按主题下发；
    // 按钮皮肤在浅/深两套 colorrect 之间切换（默认系统按钮皮肤是浅色的）。
    void ApplyTheme(const STheme &th)
    {
        const SChrome &c = th.chrome;
        const bool     bDark = th.bDark;

        const SStringW strCaption = FindColor(c.crCaption);
        const SStringW strContent = FindColor(bDark ? RGB(0x25, 0x25, 0x26) : RGB(0xFF, 0xFF, 0xFF));
        const SStringW strBorder = FindColor(c.crBorder);
        const SStringW strTitle = FindColor(c.crTitle);
        const SStringW strGlyph = FindColor(c.crGlyph);
        const SStringW strText = FindColor(c.crText);
        const SStringW strDim = FindColor(c.crTextDim);
        const SStringW strCue = FindColor(bDark ? RGB(0x86, 0x86, 0x86) : RGB(0x9A, 0x9A, 0x9A));
        const SStringW strBtnSkin = bDark ? SStringW(L"skin_find_btn_dark") : SStringW(L"skin_find_btn");

        if (SWindow *pRoot = GetRoot())
            pRoot->SetAttribute(L"colorBkgnd", strContent);

        if (SWindow *p = FindChildByName(L"caption"))
            p->SetAttribute(L"colorBkgnd", strCaption);
        if (SWindow *p = FindChildByName(L"txt_title"))
            p->SetAttribute(L"colorText", strTitle);
        if (SWindow *p = FindChildByName(L"btn_close"))
            p->SetAttribute(L"colorText", strGlyph);
        // 分隔线（SHrLine 识别的是 colorLine，不是 colorBorder）
        if (SWindow *p = FindChildByName(L"hr0"))
            p->SetAttribute(L"colorLine", strBorder);

        // 内容容器：输入框（SRichEdit 透明）靠它透出的底色
        if (SWindow *p = FindChildByName(L"content"))
            p->SetAttribute(L"colorBkgnd", strContent);

        // 三个标签
        const wchar_t *arrLbl[] = {L"lbl_find", L"lbl_replace", L"lbl_goto"};
        for (int i = 0; i < _countof(arrLbl); i++)
        {
            if (SWindow *p = FindChildByName(arrLbl[i]))
                p->SetAttribute(L"colorText", strText);
        }

        // 三个输入框：文字色 + 提示文字色（背景透明，透出内容区底色）
        const wchar_t *arrEdit[] = {L"edit_find", L"edit_replace", L"edit_goto"};
        for (int i = 0; i < _countof(arrEdit); i++)
        {
            if (SWindow *p = FindChildByName(arrEdit[i]))
            {
                p->SetAttribute(L"colorText", strText);
                p->SetAttribute(L"cueColor", strCue);
                p->Invalidate();
            }
        }

        // 普通按钮：换皮肤 + 文字色
        const wchar_t *arrBtn[] = {L"btn_hex",         L"btn_goto",       L"btn_find_prev",
                                   L"btn_find_next",   L"btn_replace",    L"btn_replace_all",
                                   L"btn_close2"};
        for (int i = 0; i < _countof(arrBtn); i++)
        {
            if (SWindow *p = FindChildByName(arrBtn[i]))
            {
                p->SetAttribute(L"skin", strBtnSkin);
                p->SetAttribute(L"colorText", strText);
                p->Invalidate();
            }
        }

        if (SWindow *p = FindChildByName(L"txt_status"))
            p->SetAttribute(L"colorText", strDim);

        if (SWindow *pRoot = GetRoot())
            pRoot->Invalidate();
    }

  private:
    // COLORREF -> "#RRGGBB"（SOUI 属性用）
    static SStringW FindColor(COLORREF cr)
    {
        SStringW s;
        s.Format(L"#%02X%02X%02X", GetRValue(cr), GetGValue(cr), GetBValue(cr));
        return s;
    }

  public:

    void FocusFind()
    {
        SEdit *p = FindChildByName2<SEdit>(L"edit_find");
        if (p)
            p->SetFocus();
    }
    void FocusReplace()
    {
        SEdit *p = FindChildByName2<SEdit>(L"edit_replace");
        if (p)
            p->SetFocus();
    }
    void FocusGoto()
    {
        SEdit *p = FindChildByName2<SEdit>(L"edit_goto");
        if (p)
            p->SetFocus();
    }

    void ShowAsFind()
    {
        ShowWindow(SW_SHOWNOACTIVATE);
        ::SetForegroundWindow(m_hWnd);
        FocusFind();
    }
    void ShowAsReplace()
    {
        ShowWindow(SW_SHOWNOACTIVATE);
        ::SetForegroundWindow(m_hWnd);
        FocusReplace();
    }
    void ShowAsGoto()
    {
        ShowWindow(SW_SHOWNOACTIVATE);
        ::SetForegroundWindow(m_hWnd);
        FocusGoto();
    }

  protected:
    void OnBtnClose()
    {
        ShowWindow(SW_HIDE);
        if (m_pListener)
            m_pListener->OnFindWndClose();
    }
    void OnClose()
    {
        OnBtnClose();
    }
    void OnFindNext()
    {
        if (m_pListener)
            m_pListener->OnFindWndFindNext();
    }
    void OnFindPrev()
    {
        if (m_pListener)
            m_pListener->OnFindWndFindPrev();
    }
    void OnReplace()
    {
        if (m_pListener)
            m_pListener->OnFindWndReplace();
    }
    void OnReplaceAll()
    {
        if (m_pListener)
            m_pListener->OnFindWndReplaceAll();
    }
    void OnHexToggle()
    {
        if (m_pListener)
            m_pListener->OnFindWndHexToggle();
    }
    void OnGoto()
    {
        if (m_pListener)
            m_pListener->OnFindWndGoto();
    }

    BEGIN_MSG_MAP_EX(CFindWnd)
    MSG_WM_CLOSE(OnClose)
    CHAIN_MSG_MAP(SHostWnd)
    END_MSG_MAP()

    EVENT_MAP_BEGIN()
    EVENT_NAME_COMMAND(L"btn_close", OnBtnClose)
    EVENT_NAME_COMMAND(L"btn_close2", OnBtnClose)
    EVENT_NAME_COMMAND(L"btn_find_next", OnFindNext)
    EVENT_NAME_COMMAND(L"btn_find_prev", OnFindPrev)
    EVENT_NAME_COMMAND(L"btn_replace", OnReplace)
    EVENT_NAME_COMMAND(L"btn_replace_all", OnReplaceAll)
    EVENT_NAME_COMMAND(L"btn_hex", OnHexToggle)
    EVENT_NAME_COMMAND(L"btn_goto", OnGoto)
    EVENT_MAP_END2(SHostWnd)

  private:
    IFindWndListener *m_pListener;
};

#endif // FIND_WND_H
