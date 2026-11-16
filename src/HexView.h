//
// CHexView - SOUI 十六进制显示控件
//
// HxD 风格的三栏布局：偏移 | 十六进制 | ASCII。支持鼠标拖选、键盘编辑、
// 撤销重做、插入/覆盖两种写模式。
// 继承 SPanel：滚动条（纵/横）由 SOUI 框架绘制与交互，使用系统皮肤，
// 控件只负责内容绘制 + SetScrollInfo/OnScroll。
//
#ifndef HEX_VIEW_H
#define HEX_VIEW_H

#include <souistd.h>
#include <core/SPanel.h>
#include <interface/SRender-i.h>
#include <vector>

#include "HexFileModel.h"
#include "HexSearch.h"

using namespace SOUI;

using hexmodel::u8;
using hexmodel::i64;
using hexmodel::u64;

class CHexView;

/** 控件回调：宿主窗口用它更新状态栏与数据检查器 */
class IHexViewListener
{
  public:
    virtual void OnHexViewCaretChanged(CHexView *pView) = 0;
    virtual void OnHexViewModifiedChanged(CHexView *pView) = 0;
    /** nCmd: 1=查找 2=转到 3=保存 4=查找下一个 */
    virtual void OnHexViewCommand(CHexView *pView, int nCmd) = 0;
};

class CHexView : public SPanel
{
    DEF_SOBJECT(SPanel, L"hexview")
  public:
    CHexView();
    ~CHexView();

    // ---------------- 数据 ----------------
    void BindModel(hexmodel::HexFileModel *pModel);
    hexmodel::HexFileModel *GetModel() const
    {
        return m_pModel;
    }

    // ---------------- 状态 ----------------
    u64 GetCaret() const
    {
        return m_caret;
    }
    /** 取选择区间 [start, end)，无选择时返回 false */
    bool GetSelection(u64 &start, u64 &end) const;
    void SetSelection(u64 start, u64 end);
    void SetSelectionKeepCaret(u64 start, u64 end); // 设选区不移动光标（检查器联动框选）
    void SelectAll();
    bool HasSelection() const
    {
        return m_selStart != m_selEnd;
    }
    void ScrollToOffset(u64 pos, bool bCenter = true);
    /** 宿主层滚轮兜底：直接驱动滚动（绕开 SOUI 内部滚轮路由） */
    void OnHostWheelV(int nLines);   // 纵向：正数向下
    void OnHostWheelH(int zDelta);   // 横向：delta>0 向左，<0 向右
    bool HasVScrollBar() const
    {
        return HasScrollBar(TRUE);
    }

    // ---------------- 编辑 ----------------
    void Copy();
    void Cut();
    void Paste();
    void DeleteSelection();
    void SetInsertMode(bool bInsert);
    /** 每行字节数（16/24/32/48） */
    void SetBytesPerRow(int n);
    /** 大 bpr 变更后的延迟刷新入口（宿主 WM_APP 回调里调用；栈内直接刷新
        会触发 soui4.dll 的消息重入时序缺陷） */
    void RefreshAfterBprChange();
    /** 光标闪烁定时器开关（弹出菜单前暂停——菜单模式循环期间的大 bpr
        重绘帧会触发 soui4.dll _PaintChildren 悬空崩溃） */
    void SetCaretBlinkEnabled(bool bEnable);
    int GetBytesPerRow() const
    {
        return m_bytesPerRow;
    }
    bool IsInsertMode() const
    {
        return m_bInsertMode;
    }
    void Undo()
    {
        if (m_pModel && m_pModel->Undo())
            RefreshFromModel();
    }
    void Redo()
    {
        if (m_pModel && m_pModel->Redo())
            RefreshFromModel();
    }

    // ---------------- 检索 ----------------
    /** 设置检索模式高亮；start==end 清除 */
    void SetHitRange(u64 start, u64 end)
    {
        m_hitStart = start;
        m_hitEnd = end;
    }

    void SetListener(IHexViewListener *pListener)
    {
        m_pListener = pListener;
    }

    /** 字符集：0=ANSI(CP_ACP) 1=UTF-8 2=UTF-16LE（仅影响 ASCII 列解码显示） */
    void SetCharset(int n);
    int GetCharset() const
    {
        return m_charset;
    }

    /** 模型被宿主直接修改后（插入/填充/替换等）刷新滚动与重绘 */
    void RefreshView()
    {
        RefreshFromModel();
    }

  protected:
    // ---------- 绘制 ----------
    void OnPaint(IRenderTarget *pRT);
    void OnSize(UINT nType, CSize size);

    // ---------- 浮层滚动条（Win11 风格：静止细条 / 悬停加宽显箭头 / 不占布局） ----------
    bool  SbNeedV();
    bool  SbNeedH();
    bool  SbHoverActive() const;              // 任一方向处于悬停
    int   SbThick(bool bVert) const;          // 滚动条厚度（6 静止 / 14 悬停）
    CRect SbTrack(bool bVert) const;
    CRect SbThumbTrack(bool bVert) const;     // 滑块可移动区间（悬停时扣掉两端箭头按钮）
    CRect SbThumb(bool bVert);
    void  DrawOverlayScroll(IRenderTarget *pRT);
    void  UpdateSbHover(CPoint pt);           // pt 为控件本地坐标
    bool  SbHandleDown(CPoint pt);            // 返回 true 表示已消费
    void  SbUpdateDrag(CPoint pt);

    // ---------- 滚动（SPanel 回调） ----------
    BOOL OnScroll(BOOL bVertical, UINT uCode, int nPos) override;
    int GetScrollLineSize(BOOL bVertical) override;
    // 滚轮：经 CHexView 消息映射进入（Shift=横向滚动，普通=回落 SPanel 纵向处理）
    BOOL OnMouseWheel(UINT nFlags, short zDelta, CPoint pt);
    void UpdateScrollBar(); // 同步滚动信息到 SPanel

    // ---------- 鼠标（内容区） ----------
    void OnLButtonDown(UINT nFlags, CPoint pt);
    void OnLButtonDbClick(UINT nFlags, CPoint pt);
    void OnLButtonUp(UINT nFlags, CPoint pt);
    void OnMouseMove(UINT nFlags, CPoint pt);
    void OnMouseLeave();
    BOOL OnSetCursor(const CPoint &pt) override; // hex/ascii 列 I-beam，其余箭头

    void OnSetFocus(SWND wndOld);
    void OnKillFocus(SWND wndFocus);

    // ---------- 键盘 ----------
    void OnKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags);
    void OnChar(UINT nChar, UINT nFlagsCnt, UINT nFlags);

    // ---------- 定时器 ----------
    void OnTimer(char cTimerID);

    SOUI_MSG_MAP_BEGIN()
    MSG_WM_PAINT_EX(OnPaint)
    MSG_WM_SIZE(OnSize)
    MSG_WM_LBUTTONDOWN(OnLButtonDown)
    MSG_WM_LBUTTONDBLCLK(OnLButtonDbClick)
    MSG_WM_LBUTTONUP(OnLButtonUp)
    MSG_WM_MOUSEMOVE(OnMouseMove)
    MSG_WM_MOUSELEAVE(OnMouseLeave)
    MSG_WM_MOUSEWHEEL(OnMouseWheel)
    MSG_WM_SETFOCUS_EX(OnSetFocus)
    MSG_WM_KILLFOCUS_EX(OnKillFocus)
    MSG_WM_KEYDOWN(OnKeyDown)
    MSG_WM_CHAR(OnChar)
    MSG_WM_TIMER_EX(OnTimer)
    SOUI_MSG_MAP_END()

  protected:
    void EnsureFont(IRenderTarget *pRT);
    void ComputeMetrics(IRenderTarget *pRT);
    u64 GetTotalRows() const;
    int GetVisibleRows() const;
    void UpdateCaretRect();
    int ColumnHitTest(CPoint pt, int *pNibble) const;
    CPoint ToLocalPoint(CPoint pt) const; // 宿主客户区坐标 → 控件本地坐标
    CRect CellRect(u64 pos) const;
    void DrawSelection(IRenderTarget *pRT, CRect rcClient);
    /** 把 [start,end) 的选中/命中底按行合并成矩形（每行最多 2 次填充），
        并裁掉视口左右两侧的屏外列 */
    void DrawByteRangeBands(IRenderTarget *pRT, CRect rcClient, u64 start, u64 end,
                            COLORREF cr);
    void DrawStatusText(IRenderTarget *pRT, CRect rcClient, LPCTSTR pszText);
    void DrawColumnHeader(IRenderTarget *pRT, CRect rcClient); // HxD 风格列偏移表头（固定顶端）
    int GetContentWidth() const; // 行内容总宽（像素）
    int GetFrozenWidth() const;  // 偏移列（冻结列）宽度：不参与横向滚动
    int GetHeaderHeight() const; // 表头行高（与数据行一致）；无模型时为 0

    // 编辑辅助
    void BeginTypingGroup();
    void EndTypingGroup();
    void WriteByteAtCaret(u8 byNew);
    void MoveCaret(i64 delta, bool bExtend);
    void SetCaretPos(u64 pos, bool bExtend);
    void EnsureCaretVisible();
    void NotifyCaretChanged();
    /** 鼠标拖选专用：把状态栏/检查器刷新节流到约 30Hz，松手时补发最终值 */
    void NotifyCaretChangedThrottled();
    void PageMove(i64 lines, bool bExtend);
    void ScrollByRows(int n);
    void AutoScrollTick();
    void RefreshFromModel();
    /** 把一行字节按当前字符集解码为单元格字符（'.' 表示不可显示） */
    wchar_t DecodeCellBuf(const u8 *buf, int n, int i) const;
    /** 同上，但直接从模型按偏移读取（用于选区重绘） */
    wchar_t DecodeCellAt(u64 pos) const;

    // 数据
    hexmodel::HexFileModel *m_pModel;
    IHexViewListener *m_pListener;

    // 视图状态
    u64 m_topRow;  // 首行偏移（以行计）＝纵向滚动位置
    int m_vUnit;   // 纵向滚动「行组」单位：nMax/nPage/nPos 都按此缩放，
                   // 避免总行数超过 INT_MAX（约 21 亿）时 SCROLLINFO.nMax(int32)
                   // 溢出成负数（典型：小 bpr + 超大文件，如 8B/行 × 29GB ≈ 39 亿行）。
                   // m_topRow 始终是「真实行号」，只在与 SPanel 滚动信息互转时乘/除 m_vUnit。
    int m_leftCol; // 水平滚动偏移（像素）
    u64 m_caret;   // 光标字节偏移
    u64 m_selAnchor;
    u64 m_selStart, m_selEnd; // 半开区间
    u64 m_hitStart, m_hitEnd; // 最近一次检索命中
    int m_nFocusCol;          // 0=hex 1=ascii
    int m_nNibble;            // 0=高4位 1=低4位
    u8 m_u8Pending;           // 插入模式下待写入的字节
    bool m_bPendingValid;
    bool m_bInsertMode;
    int m_bytesPerRow;
    int m_charset;
    bool m_bCapture;
    CPoint m_ptLButtonDown;
    CPoint m_ptLastMouse;
    bool m_bTypingActive;
    UINT m_uCaretBlink; // 光标闪烁状态
    bool m_bShowCaret;
    char m_timerAutoScroll;
    // 浮层滚动条状态
    bool m_bSbHoverV;
    bool m_bSbHoverH;
    bool m_bSbDragV;
    bool m_bSbDragH;
    int  m_nSbGrabV;
    int  m_nSbGrabH;
    // 绘制度量
    SAutoRefPtr<IFontS> m_fontText;
    SAutoRefPtr<IFontS> m_fontBold;
    // 列偏移表头高位小标字体（约主字体 60% 高）：bpr>256 时标 0x100 以上的高 4 位
    SAutoRefPtr<IFontS> m_fontSmall;
    int m_cxChar;
    int m_cyRow;
    int m_cyFont;
    int m_cySmall;
    int m_xOff; // 偏移列起始
    int m_xHex; // 十六进制列起始
    int m_xAscii;
    int m_cxOff;
    int m_cxHex;
    int m_cxAscii;
    int m_offDigits;

    // 颜色
    COLORREF m_crBg, m_crBgAlt, m_crOffText, m_crHexText, m_crAsciiText, m_crSelBg, m_crSelText,
        m_crCaret, m_crGrid, m_crHitBg, m_crHitText, m_crHeader;

    SOUI_ATTRS_BEGIN()
    ATTR_COLOR(L"colorBg", m_crBg, FALSE)
    ATTR_COLOR(L"colorBgAlt", m_crBgAlt, FALSE)
    ATTR_COLOR(L"colorOffset", m_crOffText, FALSE)
    ATTR_COLOR(L"colorHeader", m_crHeader, FALSE) // 表头列偏移的高位小标
    ATTR_COLOR(L"colorHex", m_crHexText, FALSE)
    ATTR_COLOR(L"colorAscii", m_crAsciiText, FALSE)
    ATTR_COLOR(L"colorSelBg", m_crSelBg, FALSE)
    ATTR_COLOR(L"colorSelText", m_crSelText, FALSE)
    ATTR_COLOR(L"colorCaret", m_crCaret, FALSE)
    ATTR_COLOR(L"colorHitBg", m_crHitBg, FALSE)
    ATTR_COLOR(L"colorGrid", m_crGrid, FALSE)
    ATTR_BOOL(L"insertMode", m_bInsertMode, FALSE)
    SOUI_ATTRS_END()

  private:
    // 行缓存，避免逐字节调用模型（上限 MAX_BPR=4096，与组合框最大选项一致）
    u8 m_rowCache[4096];
    u64 m_rowCacheRow;
    bool m_bRowCacheOk;
    // 绘制度量缓存：字体度量与 bpr 只在字体/布局/模型变化时重算，不再每帧 MeasureText
    bool m_bMetricsDirty;
    // 状态栏/检查器通知节流的时间戳（GetTickCount），避免拖动时 100Hz 刷新十几个控件
    DWORD m_dwLastNotify;
};

#endif // HEX_VIEW_H
