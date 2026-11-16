//
// CInspectorWnd - 数据检查器窗口（可停靠悬浮）
//
// SHostWnd 派生：纯 WS_POPUP（无系统边框）+ SOUI 自绘 caption。
// 两种状态（由 CMainFrame 管理 HWND 父子关系）：
//   docked  —— 作为主窗口子窗口贴在主窗口客户区右缘
//   float   —— WS_POPUP 悬浮，拖标题栏自由移动
// 拖动/缩放结束（WM_EXITSIZEMOVE）时通知宿主（GetParent 或 owner）
// 做贴近检测：靠近主窗口右缘 → 自动 re-dock。
//
#ifndef INSPECTOR_WND_H
#define INSPECTOR_WND_H

#include <souistd.h>
#include <control/SouiCtrls.h>
#include <core/SHostWnd.h>

#include "Theme.h"

using namespace SOUI;

class CInspectorWnd : public SHostWnd
{
  public:
    CInspectorWnd();

    /** 应用浅色 / 深色配色（由 CMainFrame 在切换主题时调用） */
    void ApplyTheme(const STheme &th);

    /** 停靠贴边方向：0=左 1=右 2=上 3=下，-1=悬浮。
        用于在贴边侧让出主窗口的缩放热区（见 OnNcHitTest）。 */
    void SetDockEdge(int nEdge)
    {
        m_nDockEdge = nEdge;
    }

    /** 移动/缩放结束回调（宿主设置；用于停靠判定） */
    void (*m_pfnOnMoveEnd)(void *ctx);
    /** 纯拉伸结束回调（位置未变：只记录尺寸，不做 dock 检测） */
    void (*m_pfnOnResizeEnd)(void *ctx);
    void *m_pMoveEndCtx;
    /** dock 态下标题栏被按下：宿主先 undock（dock 态判定看 GWL_STYLE 的
        WS_CHILD——单一事实源，不用冗余标志位） */
    void (*m_pfnOnCaptionDrag)(void *ctx);
    /** 标题栏跟随光标的偏移（宿主在 undock 前记录：光标位置 - 窗口位置） */
    POINT m_ptDragOffset;
    bool m_bDragOffsetValid;
    /** 进入移动/拉伸循环时的窗口矩形：用【尺寸是否变化】区分拖动与拉伸——
        拉伸左/上边缘时位置也变，只看位置会误判成拖动而触发 dock 吸附 */
    POINT m_ptEnterPos;
    SIZE m_szEnterSize;
    bool m_bEnterPosValid;
    /** 停靠态按下标题栏：延迟到真实拖动（WM_MOVING 首次触发）才 undock——
        单击标题栏（按下即松开）窗口保持停靠 */
    bool m_bPendingUndock;

  protected:
    void OnBtnClose();         // ×：隐藏（查看菜单重新显示）
    void OnClose();            // WM_CLOSE 兜底：同样只隐藏
    void OnEnterSizeMove();    // 进入移动/拉伸循环：记录起点
    void OnExitSizeMove();     // 拖动/缩放结束：通知宿主做 dock 检测
    void OnLButtonDown(UINT nFlags, CPoint pt); // dock 态标题栏按下 → undock + 继续拖
    void OnLButtonDblClk(UINT nFlags, CPoint pt); // 吞掉标题栏双击（SCaption 默认最大化/恢复——不需要）
    void OnMoving(UINT nSide, LPRECT pRect); // 系统拖动中：按偏移钉住窗口（标题栏跟光标走）
    /** 停靠到主窗左右边缘时，贴边侧返回 HTTRANSPARENT，把缩放命中交还主窗口 */
    LRESULT OnNcHitTest(UINT uMsg, WPARAM wParam, LPARAM lParam);

    BEGIN_MSG_MAP_EX(CInspectorWnd)
    MSG_WM_CLOSE(OnClose)
    MSG_WM_ENTERSIZEMOVE(OnEnterSizeMove)
    MSG_WM_EXITSIZEMOVE(OnExitSizeMove)
    MSG_WM_LBUTTONDOWN(OnLButtonDown)
    MSG_WM_LBUTTONDBLCLK(OnLButtonDblClk)
    MSG_WM_MOVING(OnMoving)
    MESSAGE_HANDLER_EX(WM_NCHITTEST, OnNcHitTest)
    CHAIN_MSG_MAP(SHostWnd)
    END_MSG_MAP()

    EVENT_MAP_BEGIN()
    EVENT_NAME_COMMAND(L"btn_close", OnBtnClose)
    EVENT_MAP_END2(SHostWnd)

  private:
    int m_nDockEdge; // 0/1/2/3 = 停靠方向，-1 = 悬浮
};

#endif // INSPECTOR_WND_H
