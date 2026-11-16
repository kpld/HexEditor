//
// CInspectorWnd - 数据检查器窗口（实现）
//
#include "InspectorWnd.h"

CInspectorWnd::CInspectorWnd()
    : SHostWnd(L"LAYOUT:XML_INSPECTOR")
    , m_pfnOnMoveEnd(NULL)
    , m_pfnOnResizeEnd(NULL)
    , m_pMoveEndCtx(NULL)
    , m_pfnOnCaptionDrag(NULL)
    , m_bDragOffsetValid(false)
    , m_bEnterPosValid(false)
    , m_bPendingUndock(false)
    , m_nDockEdge(-1)
{
    m_ptDragOffset.x = m_ptDragOffset.y = 0;
    m_ptEnterPos.x = m_ptEnterPos.y = 0;
    m_szEnterSize.cx = m_szEnterSize.cy = 0;
}

namespace
{
SStringW InsColor(COLORREF cr)
{
    SStringW s;
    s.Format(L"#%02X%02X%02X", GetRValue(cr), GetGValue(cr), GetBValue(cr));
    return s;
}
} // namespace

// 应用浅色 / 深色配色。
// 注意：SRichEdit（值行）不画自己的背景，是把父窗口背景 BitBlt 过来的，
// 所以只需要给内容容器设 colorBkgnd，值行改 colorText 即可。
void CInspectorWnd::ApplyTheme(const STheme &th)
{
    const SChrome &c = th.chrome;
    const bool     bDark = th.bDark;

    const SStringW strCaption = InsColor(c.crCaption);
    const SStringW strContent = InsColor(bDark ? RGB(0x25, 0x25, 0x26) : RGB(0xFA, 0xFB, 0xFC));
    const SStringW strBorder = InsColor(c.crBorder);
    const SStringW strTitle = InsColor(c.crTitle);
    const SStringW strGlyph = InsColor(c.crGlyph);
    const SStringW strValue = InsColor(th.hex.crHex);
    const SStringW strDim = InsColor(c.crTextDim);
    const SStringW strAccent = InsColor(bDark ? RGB(0x56, 0x9C, 0xD6) : RGB(0x2F, 0x55, 0x97));

    // 根窗口：vbox interval / inset 产生的间隙由它填充。不设的话会一直透出
    // 浅色的 root skin（_skin.sys.wnd.bkgnd），深色模式下就在标题栏下方形成
    // 一条刺眼的白条（浅色模式因为底色本来就浅，看不出来）。
    if (SWindow *pRoot = GetRoot())
        pRoot->SetAttribute(L"colorBkgnd", strContent);

    if (SWindow *p = FindChildByName(L"caption"))
        p->SetAttribute(L"colorBkgnd", strCaption);
    if (SWindow *p = FindChildByName(L"txt_title"))
        p->SetAttribute(L"colorText", strTitle);
    if (SWindow *p = FindChildByName(L"btn_close"))
        p->SetAttribute(L"colorText", strGlyph);

    // 分隔线（SHrLine 识别的是 colorLine，不是 colorBorder）
    const wchar_t *arrHr[] = {L"hr0", L"hr1", L"hr2", L"hr3"};
    for (int i = 0; i < _countof(arrHr); i++)
    {
        if (SWindow *p = FindChildByName(arrHr[i]))
            p->SetAttribute(L"colorLine", strBorder);
    }

    // 内容区
    SWindow *pContent = FindChildByName(L"content");
    if (pContent)
    {
        pContent->SetAttribute(L"colorBkgnd", strContent);
        // 值行统一文字色（11 行 + 文件名）
        for (SWindow *p = pContent->GetWindow(GSW_FIRSTCHILD); p;
             p = p->GetWindow(GSW_NEXTSIBLING))
        {
            p->SetAttribute(L"colorText", strValue);
        }
        pContent->Invalidate();
    }
    // 覆盖两处特殊色：小标题用强调色、文件名用次要色
    if (SWindow *p = FindChildByName(L"lbl_file"))
        p->SetAttribute(L"colorText", strAccent);
    if (SWindow *p = FindChildByName(L"txt_file"))
        p->SetAttribute(L"colorText", strDim);

    // caption（标题文字）不在 content 子树里，上面只刷了 content 区域。
    // 不整窗重绘的话，深色模式下标题仍是浅色主题的深灰字，在深底上等于隐形。
    Invalidate();
}

void CInspectorWnd::OnBtnClose()
{
    // 关闭 = 隐藏（不销毁控件树；查看菜单 IDM_VIEW_INSPECTOR 重新显示）
    ShowWindow(SW_HIDE);
}

void CInspectorWnd::OnClose()
{
    ShowWindow(SW_HIDE);
}

void CInspectorWnd::OnEnterSizeMove()
{
    SetMsgHandled(FALSE);
    // 记录进入循环时的窗口矩形：用【右下角是否变化】区分拖动与拉伸——
    // 拖动只平移（右下角跟随移动，但相对左上角距离不变）；
    // 拉伸必然改变窗口尺寸（右下角相对左上角的位置变化）。
    // 不能只看 left/top：拉伸左/上边缘时位置也变，会被误判成拖动而触发
    // dock 吸附（用户只想调整大小）。
    RECT rc;
    ::GetWindowRect(m_hWnd, &rc);
    m_ptEnterPos.x = rc.left;
    m_ptEnterPos.y = rc.top;
    m_szEnterSize.cx = rc.right - rc.left;
    m_szEnterSize.cy = rc.bottom - rc.top;
    m_bEnterPosValid = true;
}

void CInspectorWnd::OnExitSizeMove()
{
    m_bDragOffsetValid = false; // 拖动结束，偏移失效（下次拖出重新记录）
    m_bPendingUndock = false;   // 单击（未拖动）未消费的 undock 请求取消
    SetMsgHandled(FALSE);
    if (!m_pfnOnMoveEnd)
        return;
    // 尺寸未变 = 纯拖动（做 dock 检测）；
    // 尺寸变了 = 拉伸（哪怕 left/top 也移动了）——只记录尺寸，不做 dock 检测。
    // 浮窗悬停在编辑区上调整尺寸不应被吸附——只有拖到编辑区里松手才 dock。
    RECT rc;
    ::GetWindowRect(m_hWnd, &rc);
    int cxNow = rc.right - rc.left;
    int cyNow = rc.bottom - rc.top;
    bool bResized = !m_bEnterPosValid
                    || cxNow != m_szEnterSize.cx || cyNow != m_szEnterSize.cy;
    bool bMoved = !m_bEnterPosValid
                  || rc.left != m_ptEnterPos.x || rc.top != m_ptEnterPos.y;
    m_bEnterPosValid = false;
    if (bResized)
    {
        // 拉伸（含拉伸左/上边导致的位移）——只报告尺寸
        if (m_pfnOnResizeEnd)
            m_pfnOnResizeEnd(m_pMoveEndCtx);
    }
    else if (bMoved)
        m_pfnOnMoveEnd(m_pMoveEndCtx);
}

void CInspectorWnd::OnLButtonDown(UINT nFlags, CPoint pt)
{
    // 标题栏（y<28）按下时记录【光标 - 窗口左上角】偏移——SC_MOVE 系统拖动
    // 会把窗口中心吸到光标处，WM_MOVING 里按此偏移把窗口钉回原相对位置
    // （标题栏跟光标走）。dock/float 两种状态都需要：悬浮态第二次抓取拖动
    // 同样要求跟手。
    LONG_PTR dwStyle = ::GetWindowLongPtrW(m_hWnd, GWL_STYLE);
    if (pt.y < 28)
    {
        POINT ptCur;
        ::GetCursorPos(&ptCur);
        RECT rc;
        ::GetWindowRect(m_hWnd, &rc);
        m_ptDragOffset.x = ptCur.x - rc.left;
        m_ptDragOffset.y = ptCur.y - rc.top;
        m_bDragOffsetValid = true;
        // dock 态：只挂起 undock 请求——延迟到真实拖动开始（WM_MOVING 首次
        // 触发）才转换悬浮。单击标题栏（按下即松开）窗口保持停靠。
        if ((dwStyle & WS_CHILD) && m_pfnOnCaptionDrag)
            m_bPendingUndock = true;
    }
    SetMsgHandled(FALSE); // 继续走 SCaption（发起拖动）
}

void CInspectorWnd::OnLButtonDblClk(UINT nFlags, CPoint pt)
{
    // 吞掉标题栏双击：SCaption 默认行为是最大化/恢复（SC_MAXIMIZE/SC_RESTORE），
    // 检查器窗口不需要。SetMsgHandled(TRUE) 阻断消息继续传给 SCaption。
    SetMsgHandled(TRUE);
}

void CInspectorWnd::OnMoving(UINT nSide, LPRECT pRect)
{
    // 真实拖动开始：停靠态下先执行挂起的 undock（转 WS_POPUP），窗口才能移动
    if (m_bPendingUndock && m_pfnOnCaptionDrag)
    {
        m_bPendingUndock = false;
        m_pfnOnCaptionDrag(m_pMoveEndCtx);
        // undock 后窗口已定位在【光标 - 偏移】——本次 WM_MOVING 的系统建议
        // 矩形作废，直接钉回同一位置（等下一帧 WM_MOVING 再跟随）
        int cx = pRect->right - pRect->left;
        int cy = pRect->bottom - pRect->top;
        POINT ptCur;
        ::GetCursorPos(&ptCur);
        pRect->left = ptCur.x - m_ptDragOffset.x;
        pRect->top = ptCur.y - m_ptDragOffset.y;
        pRect->right = pRect->left + cx;
        pRect->bottom = pRect->top + cy;
        return;
    }
    // 系统拖动中：把窗口钉在【光标 - 记录偏移】的位置，标题栏跟随光标
    if (!m_bDragOffsetValid)
        return;
    POINT ptCur;
    ::GetCursorPos(&ptCur);
    int cx = pRect->right - pRect->left;
    int cy = pRect->bottom - pRect->top;
    pRect->left = ptCur.x - m_ptDragOffset.x;
    pRect->top = ptCur.y - m_ptDragOffset.y;
    pRect->right = pRect->left + cx;
    pRect->bottom = pRect->top + cy;
}

LRESULT CInspectorWnd::OnNcHitTest(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    // 停靠到主窗口左/右边缘时，检查器窗口的贴边侧正好盖住主窗口的 6px
    // 自绘缩放边框（主窗口是 WS_POPUP，边框在客户区内，不是系统 NC 区）。
    // 若不处理，鼠标会先命中检查器自己的 4px 边框（SNcPainter 返回
    // HTRIGHT/HTLEFT），于是拖出来的是检查器，主窗口无法从边缘拉伸。
    //
    // 解决：贴边侧的 8px 直接返回 HTTRANSPARENT，让 WM_NCHITTEST 穿透到
    // 父窗口（同一线程），由主窗口的 SNcPainter 判定为 HTRIGHT/HTLEFT，
    // 完成主窗口缩放。这样不产生视觉缝隙，也不需要硬编码主窗口边框宽度。
    // 只处理左右停靠：上/下停靠的贴边侧不在主窗口边框上，保留检查器自身缩放。
    if (m_nDockEdge == 0 || m_nDockEdge == 1)
    {
        int x = (int)(short)LOWORD(lParam); // 屏幕坐标
        RECT wr;
        ::GetWindowRect(m_hWnd, &wr);
        const int kGrip = 12; // 覆盖主窗边框（6px@100%，150% 约 9px）
        if (m_nDockEdge == 0 && x <= wr.left + kGrip)
            return HTTRANSPARENT;
        if (m_nDockEdge == 1 && x >= wr.right - kGrip)
            return HTTRANSPARENT;
    }
    SetMsgHandled(FALSE); // 其余情况走 SHostWnd 默认（SNcPainter）命中测试
    return 0;
}
