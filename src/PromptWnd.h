//
// CPromptWnd - 跨平台单行文本输入框（SOUI 自绘 + SHostDialog 模态）
//
// 替代 MainFrame.cpp 里 Win32 原生 PromptText（CreateWindowExW 建 STATIC/EDIT/BUTTON）。
// swinx 在 Linux/macOS 不支持原生控件的系统绘制、也缺 IsDialogMessage 等 W 后缀消息 API，
// 所以改用 SOUI 自绘，两平台统一。Deep/浅色主题由 ApplyTheme 下发，文案走 I18n::T。
// 头文件内联实现，避免新增 .cpp 触发 CMake 的 file(GLOB) 重新配置。
//
#ifndef PROMPT_WND_H
#define PROMPT_WND_H

#include <souistd.h>
#include <control/SouiCtrls.h>
#include <core/SHostDialog.h>

#include "I18n.h"
#include "Theme.h"

using namespace SOUI;

class CPromptWnd : public SHostDialog
{
  public:
    CPromptWnd(const wchar_t *title, const wchar_t *label, const wchar_t *def)
        : SHostDialog(L"LAYOUT:XML_PROMPT")
        , m_title(title ? title : L"")
        , m_label(label ? label : L"")
        , m_def(def ? def : L"")
    {
    }

    // DoModal 返回 IDOK 时才有效
    const std::wstring &Value() const
    {
        return m_out;
    }

    // 与 CAboutWnd / CFindWnd 同一套浅/深配色下发。
    void ApplyTheme(const STheme &th)
    {
        const SChrome &c = th.chrome;
        const bool bDark = th.bDark;
        const SStringW strCaption = Clr(c.crCaption);
        const SStringW strContent = Clr(bDark ? RGB(0x25, 0x25, 0x26) : RGB(0xFF, 0xFF, 0xFF));
        const SStringW strBorder = Clr(c.crBorder);
        const SStringW strTitle = Clr(c.crTitle);
        const SStringW strGlyph = Clr(c.crGlyph);
        const SStringW strText = Clr(c.crText);
        const SStringW strBtnSkin = bDark ? SStringW(L"skin_find_btn_dark") : SStringW(L"skin_find_btn");

        if (SWindow *p = GetRoot())
            p->SetAttribute(L"colorBkgnd", strContent);
        if (SWindow *p = FindChildByName(L"caption"))
            p->SetAttribute(L"colorBkgnd", strCaption);
        if (SWindow *p = FindChildByName(L"txt_title"))
            p->SetAttribute(L"colorText", strTitle);
        if (SWindow *p = FindChildByName(L"btn_close"))
            p->SetAttribute(L"colorText", strGlyph);
        if (SWindow *p = FindChildByName(L"hr0"))
            p->SetAttribute(L"colorLine", strBorder);
        if (SWindow *p = FindChildByName(L"content"))
            p->SetAttribute(L"colorBkgnd", strContent);
        if (SWindow *p = FindChildByName(L"txt_label"))
            p->SetAttribute(L"colorText", strText);
        const wchar_t *arrBtn[] = {L"btn_ok", L"btn_cancel"};
        for (int i = 0; i < _countof(arrBtn); i++)
        {
            if (SWindow *p = FindChildByName(arrBtn[i]))
            {
                p->SetAttribute(L"skin", strBtnSkin);
                p->SetAttribute(L"colorText", strText);
                p->Invalidate();
            }
        }
        if (SWindow *p = GetRoot())
            p->Invalidate();
    }

  protected:
    BOOL OnInitDialog(HWND, LPARAM)
    {
        if (SWindow *p = FindChildByName(L"txt_title"))
            p->SetWindowText(m_title.c_str());
        if (SWindow *p = FindChildByName(L"txt_label"))
            p->SetWindowText(m_label.c_str());
        if (SWindow *p = FindChildByName(L"btn_ok"))
            p->SetWindowText(I18n::T(L"确定"));
        if (SWindow *p = FindChildByName(L"btn_cancel"))
            p->SetWindowText(I18n::T(L"取消"));
        if (SEdit *p = FindChildByName2<SEdit>(L"edit_value"))
        {
            p->SetWindowText(m_def.c_str());
            p->SetFocus();
            p->SSendMessage(EM_SETSEL, 0, -1); // 默认值全选，直接输入即可覆盖
        }
        // 跟随应用当前主题（与主窗口/关于窗口一致），调用方无需感知。
        ApplyTheme(Theme::Get(Theme::LoadSavedIndex()));
        return TRUE;
    }

    void OnOk()
    {
        if (SEdit *p = FindChildByName2<SEdit>(L"edit_value"))
        {
            SStringT s = p->GetWindowText();
            m_out = s.c_str();
        }
        EndDialog(IDOK);
    }
    void OnCancel()
    {
        EndDialog(IDCANCEL);
    }

    // 回车/Esc 显式处理：实测发现基类 SHostDialog::OnKeyDown 的
    // FindChildByID(IDOK)->FireCommand() 在本自绘布局下未生效（按钮默认不吞键但
    // 命令也没触发），因此这里直接拦截 WM_KEYDOWN 调用本类的 OnOk/OnCancel，
    // 保证「回车=确定(且读取输入值)、Esc=取消」在两平台都稳定工作。
    void OnKeyDown(UINT nChar, UINT, UINT)
    {
        if (nChar == VK_RETURN)
            OnOk();
        else if (nChar == VK_ESCAPE)
            OnCancel();
        // 其余键不吞：交还基类/焦点控件继续处理
        else
            SetMsgHandled(FALSE);
    }

    BEGIN_MSG_MAP_EX(CPromptWnd)
        MSG_WM_INITDIALOG(OnInitDialog)
        MSG_WM_CLOSE(OnCancel)
        MSG_WM_KEYDOWN(OnKeyDown)
        CHAIN_MSG_MAP(SHostDialog)
        REFLECT_NOTIFICATIONS_EX()
    END_MSG_MAP()

    EVENT_MAP_BEGIN()
        EVENT_ID_COMMAND(IDOK, OnOk)
        EVENT_ID_COMMAND(IDCANCEL, OnCancel)
        EVENT_NAME_COMMAND(L"btn_ok", OnOk)
        EVENT_NAME_COMMAND(L"btn_cancel", OnCancel)
        EVENT_NAME_COMMAND(L"btn_close", OnCancel)
    EVENT_MAP_END2(SHostDialog)

  private:
    static SStringW Clr(COLORREF cr)
    {
        SStringW s;
        s.Format(L"#%02X%02X%02X", GetRValue(cr), GetGValue(cr), GetBValue(cr));
        return s;
    }

    std::wstring m_title, m_label, m_def, m_out;
};

#endif // PROMPT_WND_H
