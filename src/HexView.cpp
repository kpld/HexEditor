#include "HexView.h"
#include "I18n.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <souistd.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace hexview
{
static const int DEFAULT_BPR = 16;
// 最大每行字节数：与工具栏组合框最大预置值、m_rowCache[4096] 一致。
// 稳定性规避仍保留：4 列制横向 nMax、大 bpr 延迟刷新（见 UpdateScrollBar /
// SetBytesPerRow）；横向浮层滚动条对所有 bpr 正常显示，取值不再钳到 256。
static const int MAX_BPR = 4096;
// 大 bpr 延迟刷新消息（SetBytesPerRow 投递到宿主，空闲时刷新滚动条与视图）
static const UINT WM_APP_BPR_REFRESH = WM_APP + 0x31;
}

using namespace hexview;

// 避免与 Windows 的 min/max 宏冲突
static inline u64 umin64(u64 a, u64 b)
{
    return a < b ? a : b;
}
static inline u64 umax64(u64 a, u64 b)
{
    return a > b ? a : b;
}
static inline int imin(int a, int b)
{
    return a < b ? a : b;
}

CHexView::CHexView()
{
    m_pModel = nullptr;
    m_pListener = nullptr;
    m_topRow = 0;
    m_vUnit = 1;
    m_caret = 0;
    m_selAnchor = 0;
    m_selStart = 0;
    m_selEnd = 0;
    m_hitStart = 0;
    m_hitEnd = 0;
    m_nFocusCol = 0;
    m_nNibble = 0;
    m_u8Pending = 0;
    m_bPendingValid = false;
    m_bInsertMode = true;
    m_bCapture = false;
    m_ptLButtonDown = CPoint();
    m_leftCol = 0;
    m_bTypingActive = false;
    m_bShowCaret = true;
    m_timerAutoScroll = 0;
    m_bSbHoverV = false;
    m_bSbHoverH = false;
    m_bSbDragV = false;
    m_bSbDragH = false;
    m_nSbGrabV = 0;
    m_nSbGrabH = 0;
    m_cxChar = 8;
    m_cyRow = 20;
    m_cyFont = 16;
    m_cySmall = 11;
    m_bytesPerRow = DEFAULT_BPR;
    m_charset = 0;
    m_xOff = 4;
    m_xHex = 4;
    m_xAscii = 4;
    m_cxOff = 4;
    m_cxHex = 4;
    m_cxAscii = 4;
    m_offDigits = 8;
    m_rowCacheRow = (u64)-1;
    m_bRowCacheOk = false;
    m_bMetricsDirty = true;
    m_dwLastNotify = 0;

    m_crBg = RGBA(255, 255, 255, 255);
    m_crBgAlt = RGBA(245, 247, 250, 255);
    m_crOffText = RGBA(140, 150, 160, 255);
    m_crHexText = RGBA(25, 28, 33, 255);
    m_crAsciiText = RGBA(30, 90, 150, 255);
    m_crSelBg = RGBA(51, 133, 255, 255);
    m_crSelText = RGBA(255, 255, 255, 255);
    m_crCaret = RGBA(0, 0, 0, 255);
    m_crGrid = RGBA(228, 232, 238, 255);
    m_crHitBg = RGBA(255, 224, 130, 255);
    m_crHitText = RGBA(60, 45, 0, 255);
    m_crHeader = RGBA(90, 100, 112, 255);
}

CHexView::~CHexView()
{
    if (m_timerAutoScroll)
    {
        KillTimer(2);
        m_timerAutoScroll = 0;
    }
}

void CHexView::BindModel(hexmodel::HexFileModel *pModel)
{
    m_pModel = pModel;
    m_topRow = 0;
    m_vUnit = 1;
    m_leftCol = 0;
    m_caret = 0;
    m_selAnchor = 0;
    m_selStart = 0;
    m_selEnd = 0;
    m_hitStart = m_hitEnd = 0;
    m_bTypingActive = false;
    m_bRowCacheOk = false;
    m_bMetricsDirty = true; // offDigits 依赖文件大小，需重算
    UpdateScrollBar();
    Invalidate();
}

// ---------------------------------------------------------------------------
// 度量
// ---------------------------------------------------------------------------
void CHexView::EnsureFont(IRenderTarget *pRT)
{
    if (m_fontText)
    {
        pRT->SelectObject((IRenderObj *)(IFontS *)m_fontText, NULL);
        return;
    }
    IRenderFactory *pFac = GETRENDERFACTORY;
    if (!pFac)
        return;

    static const wchar_t *kFaces[] = { L"Consolas", L"Courier New", L"DejaVu Sans Mono",
                                       L"Liberation Mono", L"monospace" };
    for (int i = 0; i < (int)(sizeof(kFaces) / sizeof(kFaces[0])); i++)
    {
        LOGFONT lf;
        memset(&lf, 0, sizeof(lf));
        lf.lfHeight = 18; // 像素高度
        lf.lfWeight = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        _tcscpy(lf.lfFaceName, kFaces[i]);
        IFontS *pFont = NULL;
        if (pFac->CreateFont(&pFont, &lf) && pFont)
        {
            m_fontText = pFont;
            m_bMetricsDirty = true;
            pFont->Release();
            lf.lfWeight = FW_BOLD;
            pFont = NULL;
            if (pFac->CreateFont(&pFont, &lf) && pFont)
            {
                m_fontBold = pFont;
                pFont->Release();
            }
            // 列偏移表头的高位小标字体：约主字体 60% 高、加粗（小字更清晰）。
            // 每行 >256 字节时用于标出 0x100 以上溢出低 8 位的高 4 位（1..F）。
            LOGFONT lfSmall = lf;
            lfSmall.lfHeight = 11;
            lfSmall.lfWeight = FW_BOLD;
            pFont = NULL;
            if (pFac->CreateFont(&pFont, &lfSmall) && pFont)
            {
                m_fontSmall = pFont;
                m_cySmall = pFont->TextSize();
                if (m_cySmall < 6 || m_cySmall > 16)
                    m_cySmall = 11;
                pFont->Release();
            }
            pRT->SelectObject((IRenderObj *)(IFontS *)m_fontText, NULL);
            return;
        }
    }
}

void CHexView::ComputeMetrics(IRenderTarget *pRT)
{
    if (!m_fontText)
        return;
    pRT->SelectObject((IRenderObj *)(IFontS *)m_fontText, NULL);
    // 字体度量与布局只在字体 / bpr / 模型 / 尺寸变化时重算。旧实现每帧
    // 都做 2 次 MeasureText + GetFontMetrics，鼠标拖动时是纯浪费。
    if (!m_bMetricsDirty)
        return;
    m_bMetricsDirty = false;
    SIZE sz = { 0, 0 };
    pRT->MeasureText(L"0123456789ABCDEF", 16, &sz);
    if (sz.cx > 0)
    {
        // 取【单字符】测量宽度并向上取整：整行 TextOut 时字体按自身 advance 逐字符
        // 推进（本字体实测 advance≈8.6，16 字符总宽 147≈9.19/字符的平均值），
        // 若用平均值 9 做网格，绘制位置与命中网格随字节累积错位（选中重绘错开、
        // 空隙被邻字节文本覆盖）。改为测量【单个】字符宽度作为统一网格步进，
        // 所有绘制（整行与选中重绘）与 CellRect/命中测试全部用同一 m_cxChar。
        SIZE szOne = { 0, 0 };
        pRT->MeasureText(L"0", 1, &szOne);
        int cx = szOne.cx > 0 ? szOne.cx : (sz.cx + 15) / 16;
        if (cx < 4)
            cx = 8;
        m_cxChar = cx;
    }
    else
    {
        TEXTMETRIC tm;
        memset(&tm, 0, sizeof(tm));
        pRT->GetFontMetrics(&tm);
        m_cxChar = tm.tmAveCharWidth;
    }
    if (m_cxChar < 4)
        m_cxChar = 8;
    TEXTMETRIC tm;
    memset(&tm, 0, sizeof(tm));
    if (SUCCEEDED(pRT->GetFontMetrics(&tm)) && tm.tmHeight > 0)
        m_cyFont = tm.tmHeight;
    else
        m_cyFont = m_fontText->TextSize();
    if (m_cyFont < 12)
        m_cyFont = 12;

    const int PAD = 6;
    const int GAP = 10;

    // 偏移列位数：8 位十六进制最多只能寻址 4GB（0xFFFFFFFF）。因此按文件大小取
    // 「能表示最大偏移所需的最少位数」，下限 8（与 HxD 在小文件下的 8 位一致），
    // 上限 16（64 位偏移）。这样 ≤4GB 文件是 8 位、4GB~64GB 是 9~10 位……既不会像
    // 固定 8 位那样在 >4GB 文件上截断偏移（错位），也不会像旧逻辑那样一到 >4GB 就
    // 跳到 16 位浪费宽度。
    int offDigits = 8;
    if (m_pModel && m_pModel->Size() > 0)
    {
        u64 v = m_pModel->Size() - 1;
        int need = 0;
        while (v) { v >>= 4; ++need; }
        if (need < 8) need = 8;
        if (need > 16) need = 16;
        offDigits = need;
    }
    m_offDigits = offDigits;
    m_xOff = PAD;
    m_cxOff = m_cxChar * m_offDigits;
    m_xHex = m_xOff + m_cxOff + GAP;
    // 16 * 2 字符 + 15 个间隔 + 第 8 字节后额外分组间隔
    m_cxHex = m_cxChar * (m_bytesPerRow * 3 - 1) + m_cxChar * 2;
    m_xAscii = m_xHex + m_cxHex + GAP;
    m_cxAscii = m_cxChar * m_bytesPerRow;
    m_cyRow = m_cyFont + 6;
    if (m_cyRow < 16)
        m_cyRow = 16;
}

u64 CHexView::GetTotalRows() const
{
    if (!m_pModel || m_pModel->Size() == 0)
        return 1;
    return (m_pModel->Size() + m_bytesPerRow - 1) / m_bytesPerRow;
}

int CHexView::GetVisibleRows() const
{
    CRect rc;
    GetClientRect(&rc);
    int h = rc.Height() - GetHeaderHeight(); // 表头占一行
    // 用 floor：只统计【完整】可见的行。此前用 ceil 会把只露出几个像素、
    // 实际装不下的行也计入 page，导致 nMax-nPage+1 偏小、拖到底时最后一行
    // 恰好显示不出来（bpr=48 + 横向条扣高时必现）。
    int rows = h / m_cyRow;
    if (rows < 1)
        rows = 1;
    return rows;
}

// ---------------------------------------------------------------------------
// 滚动条（SPanel 存状态、CHexView 自绘浮层）：同步滚动信息
// 纵向以「行」为单位；横向以「4 字符列」为单位（见 UpdateScrollBar）。
// ---------------------------------------------------------------------------
int CHexView::GetContentWidth() const
{
    // 直接按 m_bytesPerRow 现算（不依赖 m_xAscii/m_cxAscii 缓存）：
    // SetBytesPerRow 里 UpdateScrollBar 先于 OnPaint 的 ComputeMetrics 执行，
    // 若用旧缓存值会误判横向滚动条需求（bpr=64 时用 16 的布局算出内容不超宽）。
    int cxChar = m_cxChar > 0 ? m_cxChar : 8;
    int xHex = m_xOff + m_cxOff + 10;
    int cxHex = cxChar * (m_bytesPerRow * 3 - 1) + cxChar * 2;
    int xAscii = xHex + cxHex + 10;
    int cxAscii = cxChar * m_bytesPerRow;
    // 右侧留 2 字符宽边距：滚到最右时最右字符与视口右缘保持距离
    return xAscii + cxAscii + 2 * cxChar;
}

// 偏移列（冻结列）占据的宽度：偏移文本 + 右侧分隔
int CHexView::GetFrozenWidth() const
{
    return m_xOff + m_cxOff + 8;
}

// 表头行高：与数据行同高；无模型时 0（不占空间）
int CHexView::GetHeaderHeight() const
{
    return (m_pModel && m_pModel->IsOpen() && m_pModel->Size() > 0) ? m_cyRow : 0;
}

void CHexView::UpdateScrollBar()
{
    if (!m_pModel)
        return;

    // ---- 第一遍：先确定两向滚动条的显示需求 ----
    // 注意顺序：纵向的 visible 依赖客户区高度，而客户区高度又依赖横向条是否显示；
    // 横向条的 viewW 依赖客户区宽度（受纵向条影响）。两向都要先声明，再统一测量。
    u64 total = GetTotalRows();

    // 横向需求（只依赖宽度，不依赖高度）：
    int contentW = GetContentWidth();
    int frozen = GetFrozenWidth();
    int scrollableW = contentW - frozen; // 冻结列右侧真正可滚动的内容宽

    // 用「假设两向都可能显示」的极限值预判：
    CRect rcCur = GetClientRect();
    int viewW = rcCur.Width() - frozen - GetSbWidth(); // 冻结列+纵向条让位
    bool bNeedH = scrollableW > viewW && viewW > 20;
    // 【所有 bpr 均显示横向滚动条】：早期为规避 soui4.dll 0xc000041d 曾强制
    // bpr>300 时关掉横向条，结果 512/1024/2048/4096 下右侧字节完全没法横向浏览。
    // 该崩溃的实际规避与「是否显示横向条」无关，由以下几点共同承担：
    //   ① 横向 SCROLLINFO 走 4 列制（unit=cxChar*4），nMax 与 bpr 同量级
    //      （bpr=4096 时约 4 千），而非像素制的十几万；
    //   ② sbWid="0"：SPanel::ShowScrollBar 只置 m_wBarVisible 位 + 发
    //      WM_NCCALCSIZE，原生条不布局不绘制，滚动条由 OnPaint 自绘为浮层；
    //   ③ 大 bpr 的 UpdateScrollBar 经 WM_APP_BPR_REFRESH 延迟到消息栈退净后
    //      执行，避免栈内 ShowScrollBar→WM_NCCALCSIZE→Invalidate 重入；
    //   ④ SPanel::m_dwUpdateTime 已初始化为 0（原未初始化读影响 ScrollUpdate
    //      节流判断）。
    // 纵向条在大文件下一直走同一条 ShowScrollBar 路径且从未崩溃，也印证了
    // 显示条本身不是崩溃源。
    if (bNeedH != HasScrollBar(FALSE))
        ShowScrollBar(SSB_HORZ, bNeedH);

    // 横向条显示状态确定后，客户区高度已扣除横向条（NCCALCSIZE 同步生效）。
    // 此时再算纵向 visible / page，保证滚到最底时最后一行完整可见。
    CRect rcClient = GetClientRect();
    int visible = GetVisibleRows();
    bool bNeedV = total > (u64)visible;
    if (bNeedV != HasScrollBar(TRUE))
        ShowScrollBar(SSB_VERT, bNeedV);
    if (bNeedV)
    {
        // 纵向「行组」单位：让 nMax（int32）永远不超过 SAFE_NMAX，避免总行数超过
        // INT_MAX（约 21 亿）时 nMax 被 (int) 截断成负数（典型：小 bpr + 超大文件，
        // 如 8B/行 × 29GB ≈ 39 亿行）。仅依赖 total，窗口缩放（visible 变化）不会改变
        // 单位，保证滚动位置稳定。m_topRow 始终是「真实行号」，只在与 SPanel 滚动信息
        // 互转时乘/除 m_vUnit（横向早已用 m_cxChar*4 的同款思路）。
        const i64 SAFE_NMAX = 1000000000LL; // 远小于 INT_MAX(2147483647)
        m_vUnit = 1;
        if ((i64)total > SAFE_NMAX)
            m_vUnit = (int)(((i64)total + SAFE_NMAX - 1) / SAFE_NMAX);

        // bpr 变大使总行数减少时，旧 m_topRow 可能越界；先夹到 [0, total-visible]。
        if (total > (u64)visible)
        {
            u64 maxTop = total - visible;
            if (m_topRow > maxTop)
                m_topRow = maxTop;
        }
        else
            m_topRow = 0;

        // 行组制下：nMaxV = ceil(最大行索引 / vUnit)，保证最后一行可达；
        // nPageV = max(1, visible/vUnit)，thumb 占比 = nPageV/nRangeV = visible/total
        // （vUnit 在分子分母抵消，比例仍精确）；nPosV = m_topRow/vUnit。
        i64 maxRow = (i64)total - 1;
        i64 nMaxV = (maxRow + m_vUnit - 1) / m_vUnit;
        if (nMaxV > 2147483646LL)
            nMaxV = 2147483646LL; // 双保险，绝不溢出 int32
        int nPageV = (int)((i64)visible / m_vUnit);
        if (nPageV < 1)
            nPageV = 1;
        int nPosV = (int)((i64)m_topRow / m_vUnit);
        SCROLLINFO si = { sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE | SIF_POS };
        si.nMin = 0;
        si.nMax = (int)nMaxV;
        si.nPage = (UINT)nPageV;
        si.nPos = nPosV;
        SetScrollInfo(si, TRUE);
    }
    else
    {
        m_topRow = 0;
    }

    // ---- 第二遍：横向 SCROLLINFO（用扣除纵向条后的最终客户区） ----
    // 【横向滚动按「4 字符列」为单位】而非像素：早期实验里 SOUI 滚动条处理
    // 超大 nMax（像素制下大 bpr 的 nMax 到十几万）在特定消息时序下会崩溃
    // （soui4.dll 固定偏移 0xc000041d/0xc0000005，调试器附加即消失的时序缺陷）。
    // 4 列制（unit=cxChar*4 ≈ 32px）把 nMax 压到与 bpr 同量级：bpr=4096、
    // cxChar=8 时内容宽约 13 万像素，nMax 仅约 4 千，比像素制小一个数量级。
    // 这是大 bpr 能安全显示横向滚动条的关键（配合 sbWid=0 浮层自绘与
    // SetBytesPerRow 的延迟刷新）。
    if (bNeedH)
    {
        int viewW2 = rcClient.Width() - frozen; // rcClient 已扣纵向条
        int maxLeft = scrollableW - viewW2;    // 实际可滚动距离（像素）
        int unit = m_cxChar * 4;               // 滚动单位：4 字符列 = 32px
        // 上取整对齐单位边界：若向下对齐（floor），maxLeft 会比几何最右少
        // 最多 unit-1 px，滚到最右时最右字符反而被推过视口右缘裁掉、
        // 视觉上"贴边"；ceil 保证右边距完整保留（2 字符宽）。
        int maxUnit = (maxLeft + unit - 1) / unit;
        maxLeft = maxUnit * unit;
        SCROLLINFO si = { sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE | SIF_POS };
        si.nMin = 0;
        si.nMax = maxUnit + (int)viewW2 / unit - 1; // Win32 语义：内容宽(单位)-1
        si.nPage = (UINT)(viewW2 / unit);
        si.nPos = m_leftCol / unit;
        SetScrollInfo(si, FALSE);
        if (m_leftCol > maxLeft)
            m_leftCol = maxLeft;
        if (m_leftCol < 0)
            m_leftCol = 0;
        // 对齐到单位边界，避免半列残显
        m_leftCol = (m_leftCol / unit) * unit;
        SetScrollPos(FALSE, m_leftCol / unit, FALSE);
    }
    else
    {
        m_leftCol = 0;
    }
}

// SPanel 滚动回调：更新视图状态
BOOL CHexView::OnScroll(BOOL bVertical, UINT uCode, int nPos)
{
    const SCROLLINFO *psi = GetScrollBarInfo(bVertical);
    int nOldPos = psi->nPos;
    BOOL bRet = SPanel::OnScroll(bVertical, uCode, nPos);
    if (bRet)
    {
        int nNewPos = GetScrollBarInfo(bVertical)->nPos;
        if (bVertical)
        {
            // 纵向行组制：位置×vUnit 还原成真实行号，再夹到 [0, total-visible]，
            // 并对齐到网格（避免与 nPos=m_topRow/vUnit 反复互转时漂移）。
            m_topRow = (u64)nNewPos * (u64)m_vUnit;
            u64 t = GetTotalRows();
            int vis = GetVisibleRows();
            u64 maxTop = (t > (u64)vis) ? t - vis : 0;
            if (m_topRow > maxTop)
                m_topRow = maxTop;
            m_topRow = (m_topRow / m_vUnit) * m_vUnit;
        }
        else
            m_leftCol = nNewPos * (m_cxChar * 4); // 横向按「4 列」滚动：位置×32px
        Invalidate();
    }
    return bRet;
}

int CHexView::GetScrollLineSize(BOOL bVertical)
{
    return bVertical ? 1 : 1; // 纵向按行；横向按「4 列」单位（32px）
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------
static void Hex16(SStringW &str, u64 value, int digits)
{
    wchar_t buf[20];
    const wchar_t *hex = L"0123456789ABCDEF";
    int n = 0;
    for (int i = digits - 1; i >= 0; i--)
    {
        buf[n++] = hex[(value >> (i * 4)) & 0xF];
    }
    buf[n] = 0;
    str = buf;
}

void CHexView::DrawStatusText(IRenderTarget *pRT, CRect rcClient, LPCTSTR pszText)
{
    if (!m_fontText)
        return;
    pRT->SetTextColor(m_crOffText);
    pRT->SelectObject((IRenderObj *)(IFontS *)m_fontText, NULL);
    RECT rc = { m_xOff + 4, rcClient.top + 4, rcClient.right - 8, rcClient.bottom - 4 };
    pRT->DrawText(pszText, -1, &rc, DT_LEFT | DT_TOP | DT_SINGLELINE);
}

// ---------------------------------------------------------------------------
// HxD 风格列偏移表头：顶部固定行，标识每个字节在行内的偏移（00 01 02 ...）。
// 左缘偏移列表头格留空；hex/ASCII 部分与内容列共享横向滚动；
// 垂直滚动时表头钉在顶端不动（绘制发生在内容区下移之前）。
//
// 每行 >256 字节时低 8 位（00..FF）会回绕，0x000 与 0x100 无法区分。
// 仿 HxD 在【本列】偏移值的左下角紧贴着补一个缩小的高 4 位（col>>8，1..F），
// 合起来就是完整 12 位偏移：bpr=4096 的最后一列显示 F + FF = FFF。
// col<256 的高位为 0 不画，保持原表头外观。注意小标不能借前一列的间隙，
// 那样会紧贴前一列值的末尾、被读成前一列的下标。
// ---------------------------------------------------------------------------
void CHexView::DrawColumnHeader(IRenderTarget *pRT, CRect rcClient)
{
    const int frozen = GetFrozenWidth();
    const int headerH = GetHeaderHeight();

    // 表头背景：与内容底色区分（斑马纹浅色），底部一条分隔线
    CRect rcHead(0, 0, rcClient.right, headerH);
    pRT->FillSolidRect(&rcHead, m_crBgAlt);
    CRect rcLine(0, headerH - 1, rcClient.right, headerH);
    pRT->FillSolidRect(&rcLine, m_crGrid);

    pRT->SetTextColor(m_crOffText);

    // hex 部分：每字节偏移两位十六进制（00..FF），按字节网格画在 hex 列位置
    const wchar_t *hex = L"0123456789ABCDEF";
    int stride = 3 * m_cxChar;
    int half = m_bytesPerRow / 2;
    // 先在未平移坐标画 hex 头（与内容同随 m_leftCol 平移）：进滚动区坐标
    CRect rcScroll(frozen, 0, rcClient.right, headerH);
    pRT->PushClipRect(&rcScroll);
    pRT->OffsetViewportOrg(-m_leftCol, 0);
    wchar_t buf[3];
    buf[2] = 0;
    int yText = (headerH - m_cyFont) / 2;
    // bpr>256 时表头要同时容纳低 8 位主数值与左下角的高位小标，两者墨迹
    // 原本有约 4px 垂直重叠。主数值上移 3px 让出底部空间，小标贴到本列
    // 起点后就不会与主数值的下半部笔画糊在一起。
    if (m_bytesPerRow > 256)
        yText -= 3;
    if (yText < 0)
        yText = 0;
    // 可视区裁剪（大 bpr 时表头同样只画可见列）
    int viewL = m_leftCol - m_cxChar * 2;
    int viewR = m_leftCol + rcClient.right + m_cxChar * 2;
    for (int col = 0; col < m_bytesPerRow; col++)
    {
        int x = m_xHex + col * stride + ((col >= half && half > 0) ? m_cxChar : 0);
        if (x + 2 * m_cxChar < viewL || x > viewR)
            continue;
        buf[0] = hex[(col >> 4) & 0xF];
        buf[1] = hex[col & 0xF];
        pRT->TextOut(x, yText, buf, 1);
        pRT->TextOut(x + m_cxChar, yText, buf + 1, 1);
    }

    // ---- 高位小标（仅 bpr>256）----
    // 单独第二遍绘制：整遍只切一次字体，避免逐列 SelectObject/SetTextColor。
    if (m_bytesPerRow > 256 && m_fontSmall)
    {
        pRT->SelectObject((IRenderObj *)(IFontS *)m_fontSmall, NULL);
        pRT->SetTextColor(m_crHeader);
        int ySmall = headerH - m_cySmall - 1;
        if (ySmall < 0)
            ySmall = 0;
        // 估算首个可能可见的列（x 随 col 单调增；half 处有 +1 字符跳变，回退 2 列兜底）
        int startCol = (viewL - m_xHex) / stride - 2;
        if (startCol < 256)
            startCol = 256;
        for (int col = startCol; col < m_bytesPerRow; col++)
        {
            int x = m_xHex + col * stride + ((col >= half && half > 0) ? m_cxChar : 0);
            if (x > viewR)
                break; // x 单调递增，之后全在屏外
            if (x + 2 * m_cxChar < viewL)
                continue;
            wchar_t hi = hex[(col >> 8) & 0xF];
            // 贴着本列值的左下角画，让高位小标在视觉上明确属于【本列】的
            // 偏移量（此前放在前一列的 1 字符间隙里，紧贴前一列值的末尾，
            // 看起来像前一列的下标）。配合上面主数值上移 3px，水平虽有小幅
            // 交叠但垂直已错开，不会糊字；也无需加高表头行。
            pRT->TextOut(x - 2, ySmall, &hi, 1);
        }
        // 恢复主字体/颜色：OnPaint 后续的偏移列与内容绘制依赖这个字体状态
        pRT->SelectObject((IRenderObj *)(IFontS *)m_fontText, NULL);
        pRT->SetTextColor(m_crOffText);
    }
    // ASCII 列上方不画列偏移（用户要求：该处不需要；且表头在 ascii 列
    // 与数据重叠渲染会有混色——只保留 hex 部分的列偏移）
    pRT->OffsetViewportOrg(m_leftCol, 0);
    pRT->PopClip();
}

void CHexView::DrawByteRangeBands(IRenderTarget *pRT, CRect rcClient,
                                  u64 start, u64 end, COLORREF cr)
{
    if (start >= end)
        return;
    const int stride = 3 * m_cxChar;
    const int half = m_bytesPerRow / 2;
    // 内容坐标下的可视范围（滚动区已 OffsetViewportOrg(-m_leftCol, 0)）
    const int viewL = m_leftCol - m_cxChar;
    const int viewR = m_leftCol + rcClient.right + m_cxChar;

    // 直接从首个可见行开始，避免选区起点远在屏幕上方时遍历几百万行
    u64 firstRow = start / m_bytesPerRow;
    if (firstRow < m_topRow)
        firstRow = m_topRow;
    for (u64 row = firstRow;; row++)
    {
        u64 rowBase = row * m_bytesPerRow;
        if (rowBase >= end)
            break;
        i64 dy = (i64)row - (i64)m_topRow;
        int y = int(dy * m_cyRow);
        if (y >= rcClient.bottom)
            break; // 行号递增，之后全在屏外
        if (y + m_cyRow <= 0)
            continue;

        u64 a = (start > rowBase) ? (start - rowBase) : 0;
        u64 rem = end - rowBase;
        u64 b = (rem < (u64)m_bytesPerRow) ? rem : (u64)m_bytesPerRow;
        if (b <= a)
            continue;
        u64 last = b - 1;

        // hex 列：整段合并成一个矩形（分组空格一并涂色，与 HxD 观感一致）。
        // 旧实现逐字节 FillSolidRect，bpr=256 时一行最多 256 次调用。
        int xL = m_xHex + int(a * stride) + ((int)a >= half && half > 0 ? m_cxChar : 0);
        int xR = m_xHex + int(last * stride) +
                 ((int)last >= half && half > 0 ? m_cxChar : 0) + 2 * m_cxChar;
        if (xR > viewL && xL < viewR)
        {
            CRect rc(xL < viewL ? viewL : xL, y, xR > viewR ? viewR : xR, y + m_cyRow);
            pRT->FillSolidRect(&rc, cr);
        }
        // ASCII 列：整段一个矩形
        int aL = m_xAscii + int(a * m_cxChar);
        int aR = m_xAscii + int(b * m_cxChar);
        if (aR > viewL && aL < viewR)
        {
            CRect rc(aL < viewL ? viewL : aL, y, aR > viewR ? viewR : aR, y + m_cyRow);
            pRT->FillSolidRect(&rc, cr);
        }
    }
}

void CHexView::DrawSelection(IRenderTarget *pRT, CRect rcClient)
{
    if (m_selStart == m_selEnd)
        return;
    DrawByteRangeBands(pRT, rcClient, m_selStart, m_selEnd, m_crSelBg);
}

// ---------------------------------------------------------------------------
// 浮层滚动条（Win11 风格）
//   sbWid=0 → SPanel 不再为滚动条预留布局空间，也不自绘；
//   这里在内容之上自绘：静止 6px 细条、悬停 14px 并显示轨道与箭头，横竖通用。
//   注意：SOUI 的 FillSolidRect 走 SColor(cr)，高位字节是 alpha，
//   所以颜色必须带 0xFF（本工程主题色来自 XML，已经是 ARGB，安全）。
// ---------------------------------------------------------------------------
static const int kSbRestW = 6;
static const int kSbHoverW = 14;
// 滑块最小长度（绝对保底，也用于 SbThumbTrack 的轨道过短退化阈值）：40px，
// 保证普通内容下也能抓住。但在超大范围（如 4096 字节/行）下按比例算出的滑块
// 只有几像素、几乎成一个"点"，极难拖动；因此 SbThumb 里再叠加一个随轨道宽度
// 缩放的最小长度（至少 trackLen/16），让滑块始终是易抓的"宽手柄"。
// 注意：滚动逻辑仍由 nPage/nMax 决定（点轨道右侧=翻一屏、拖动范围=完整内容），
// 把滑块画宽只是手柄视觉，不会撑破语义。
static const int kSbMinThumb = 40;

// 自动化验证用开关：本机前台常被别的进程占住，真实光标进不到本窗口，
// 系统会立刻补发 WM_MOUSELEAVE 把悬停态清掉。设 HEX_FORCE_HOVER=1 可锁定悬停态。
// 对照实验开关：HEX_OV_OFF=1 时完全关闭浮层滚动条（用于隔离 sbWid=0 本身是否有问题）
static bool OvEnabled()
{
    static int s = -1;
    if (s < 0)
    {
        char b[8] = {0};
        s = (GetEnvironmentVariableA("HEX_OV_OFF", b, sizeof(b)) > 0 && b[0] == '1') ? 0 : 1;
    }
    return s == 1;
}

static bool HexForceHover()
{
    static int s = -1;
    if (s < 0)
    {
        char b[8] = {0};
        s = (GetEnvironmentVariableA("HEX_FORCE_HOVER", b, sizeof(b)) > 0 && b[0] == '1') ? 1 : 0;
    }
    return s == 1;
}

static COLORREF SbBlend(COLORREF fg, COLORREF bg, int a)
{
    int r = (GetRValue(fg) * a + GetRValue(bg) * (255 - a)) / 255;
    int g = (GetGValue(fg) * a + GetGValue(bg) * (255 - a)) / 255;
    int b = (GetBValue(fg) * a + GetBValue(bg) * (255 - a)) / 255;
    return 0xFF000000u | RGB(r, g, b);
}

// 画一个三角形箭头。
//   bVert=true 画上/下三角，false 画左/右三角；
//   bToStart=true 表示尖端朝「起点侧」（竖直=上、水平=左），即窄端在起点、宽端在终点。
static void SbDrawArrow(IRenderTarget *pRT, const CRect &rcCell, bool bVert, bool bToStart,
                        COLORREF cr)
{
    int h = bVert ? rcCell.Height() : rcCell.Width();
    int rows = (h - 4) / 2;
    if (rows < 2)
        rows = 2;
    if (bVert)
    {
        int cx = rcCell.CenterPoint().x;
        int top = rcCell.top + (h - rows) / 2;
        for (int i = 0; i < rows; i++)
        {
            int w = 1 + 2 * i;
            int y = bToStart ? (top + i) : (top + rows - 1 - i);
            CRect r(cx - w / 2, y, cx - w / 2 + w, y + 1);
            pRT->FillSolidRect(&r, cr);
        }
    }
    else
    {
        int cy = rcCell.CenterPoint().y;
        int left = rcCell.left + (h - rows) / 2;
        for (int i = 0; i < rows; i++)
        {
            int w = 1 + 2 * i;
            int x = bToStart ? (left + i) : (left + rows - 1 - i);
            CRect r(x, cy - w / 2, x + 1, cy - w / 2 + w);
            pRT->FillSolidRect(&r, cr);
        }
    }
}

bool CHexView::SbNeedV()
{
    return HasScrollBar(TRUE) != FALSE;
}

bool CHexView::SbNeedH()
{
    return HasScrollBar(FALSE) != FALSE;
}

bool CHexView::SbHoverActive() const
{
    return m_bSbHoverV || m_bSbHoverH || HexForceHover();
}

int CHexView::SbThick(bool bVert) const
{
    bool hv = (bVert ? m_bSbHoverV : m_bSbHoverH) || HexForceHover();
    return hv ? kSbHoverW : kSbRestW;
}

CRect CHexView::SbTrack(bool bVert) const
{
    CRect rc;
    GetClientRect(&rc);
    int w = SbThick(bVert);
    if (bVert)
        return CRect(rc.Width() - w, 0, rc.Width(), rc.Height());
    return CRect(0, rc.Height() - w, rc.Width(), rc.Height());
}

// 滑块可移动区间：轨道两端是箭头按钮的位置，滑块只能在两者之间滑动。
// 注意这里**不管是否悬停都要内缩**：静止态虽然不画箭头，但也要把箭头的位置空出来，
// 否则悬停时箭头一出现，滑块就会被挤下去 14px（看起来像"跳了一下"）。
CRect CHexView::SbThumbTrack(bool bVert) const
{
    CRect tr = SbTrack(bVert);
    CRect inner = tr;
    if (bVert)
    {
        inner.top += kSbHoverW;
        inner.bottom -= kSbHoverW;
        if (inner.Height() < kSbMinThumb)
            return tr; // 轨道太短，退化为整条
    }
    else
    {
        inner.left += kSbHoverW;
        inner.right -= kSbHoverW;
        if (inner.Width() < kSbMinThumb)
            return tr;
    }
    return inner;
}

CRect CHexView::SbThumb(bool bVert)
{
    if (!(bVert ? SbNeedV() : SbNeedH()))
        return CRect(0, 0, 0, 0);
    const SCROLLINFO *psi = GetScrollBarInfo(bVert);
    if (!psi || psi->nPage <= 0)
        return CRect(0, 0, 0, 0);
    CRect tr = SbThumbTrack(bVert);
    int trackLen = bVert ? tr.Height() : tr.Width();
    if (trackLen <= 0)
        return CRect(0, 0, 0, 0);

    int nRange = psi->nMax + 1; // nMax 是最大位置，总范围要 +1
    if (nRange <= 0)
        return CRect(0, 0, 0, 0);
    int thumbLen = (int)((long long)trackLen * psi->nPage / nRange);
    // 宽手柄：超大范围（如 4096 字节/行）下按比例算出的滑块只有几像素、几乎成
    // 一个"点"，极难拖动。这里叠加一个随轨道宽度缩放的最小长度：取 kSbMinThumb
    // 与 trackLen/12 的较大者，让滑块始终是易抓的"宽手柄"（接近 HxD 的观感）。
    // 注意这只影响手柄视觉，不撑破"点轨道右侧翻一屏、拖动覆盖全部内容"的滚动语义
    // （那些仍由 nPage/nMax 决定；off=(trackLen-thumbLen)*pos/maxScroll 仍把整条
    // 轨道映射成完整内容范围，滑块再大也不会让"拖到底=看到最右列"失效）。
    int minLen = kSbMinThumb;
    int scaled = trackLen / 12;
    if (scaled > minLen)
        minLen = scaled;
    if (thumbLen < minLen)
        thumbLen = minLen;
    if (thumbLen > trackLen)
        thumbLen = trackLen;

    int maxScroll = nRange - psi->nPage;
    int pos = psi->nPos;
    if (pos < 0)
        pos = 0;
    if (pos > maxScroll)
        pos = maxScroll;
    int off = 0;
    if (maxScroll > 0)
        off = (int)((long long)(trackLen - thumbLen) * pos / maxScroll);
    if (bVert)
        return CRect(tr.left, tr.top + off, tr.right, tr.top + off + thumbLen);
    return CRect(tr.left + off, tr.top, tr.left + off + thumbLen, tr.bottom);
}

void CHexView::DrawOverlayScroll(IRenderTarget *pRT)
{
    if (!OvEnabled())
        return;
    if (!SbNeedV() && !SbNeedH())
        return;
    int lum = (GetRValue(m_crBg) * 299 + GetGValue(m_crBg) * 587 + GetBValue(m_crBg) * 114) / 1000;
    bool bDark = lum < 128;
    COLORREF crThumb = bDark ? RGB(0xB4, 0xB4, 0xB4) : RGB(0x66, 0x66, 0x66);
    COLORREF crThumbC = SbBlend(crThumb, m_crBg, bDark ? 150 : 140);
    COLORREF crTrackC = SbBlend(crThumb, m_crBg, bDark ? 42 : 34);

    for (int pass = 0; pass < 2; pass++)
    {
        bool bVert = (pass == 0);
        if (!(bVert ? SbNeedV() : SbNeedH()))
            continue;
        CRect tr = SbTrack(bVert);
        CRect th = SbThumb(bVert);
        bool hv = ((bVert ? m_bSbHoverV : m_bSbHoverH) || HexForceHover());

        if (hv)
        {
            pRT->FillSolidRect(&tr, crTrackC);
            if (bVert)
            {
                // 顶端 = 上三角（尖端朝上），底端 = 下三角（尖端朝下）
                CRect a1(tr.left, tr.top, tr.right, tr.top + kSbHoverW);
                CRect a2(tr.left, tr.bottom - kSbHoverW, tr.right, tr.bottom);
                SbDrawArrow(pRT, a1, true, true, crThumbC);
                SbDrawArrow(pRT, a2, true, false, crThumbC);
            }
            else
            {
                // 左端 = 左三角（尖端朝左），右端 = 右三角（尖端朝右）
                CRect a1(tr.left, tr.top, tr.left + kSbHoverW, tr.bottom);
                CRect a2(tr.right - kSbHoverW, tr.top, tr.right, tr.bottom);
                SbDrawArrow(pRT, a1, false, true, crThumbC);
                SbDrawArrow(pRT, a2, false, false, crThumbC);
            }
        }
        if (th.Height() > 0 && th.Width() > 0)
        {
            // 圆角胶囊：FillSolidRoundRect 内部内缩 0.5px 且带抗锯齿，
            // 窄条上再留 inset 会细到看不清，所以静止态不内缩、悬停态才内缩 3px。
            int inset = hv ? 3 : 0;
            CRect ti = th;
            if (bVert)
            {
                ti.left += inset;
                ti.right -= inset;
                ti.top += 1;
                ti.bottom -= 1;
            }
            else
            {
                ti.top += inset;
                ti.bottom -= inset;
                ti.left += 1;
                ti.right -= 1;
            }
            if (ti.Width() > 0 && ti.Height() > 0)
            {
                int rad = (bVert ? ti.Width() : ti.Height()) / 2;
                pRT->FillSolidRoundRect(&ti, CPoint(rad, rad), crThumbC);
            }
        }
    }
}

void CHexView::UpdateSbHover(CPoint pt)
{
    if (!OvEnabled())
        return;
    CRect rc;
    GetClientRect(&rc);
    int hitW = kSbRestW + 6;
    bool hvV = SbNeedV() && pt.x >= rc.Width() - hitW;
    bool hvH = SbNeedH() && pt.y >= rc.Height() - hitW;
    if (HexForceHover())
    {
        hvV = SbNeedV();
        hvH = SbNeedH();
    }
    if (hvV != m_bSbHoverV || hvH != m_bSbHoverH)
    {
        m_bSbHoverV = hvV;
        m_bSbHoverH = hvH;
        Invalidate();
    }
}

bool CHexView::SbHandleDown(CPoint pt)
{
    if (!OvEnabled())
        return false;
    CRect rc;
    GetClientRect(&rc);
    int hitW = kSbRestW + 6;

    for (int pass = 0; pass < 2; pass++)
    {
        bool bVert = (pass == 0);
        if (!(bVert ? SbNeedV() : SbNeedH()))
            continue;
        if (bVert ? (pt.x < rc.Width() - hitW) : (pt.y < rc.Height() - hitW))
            continue;

        CRect th = SbThumb(bVert);
        if ((bVert ? th.Height() : th.Width()) > 0 &&
            (bVert ? (pt.y >= th.top && pt.y < th.bottom) : (pt.x >= th.left && pt.x < th.right)))
        {
            if (bVert)
            {
                m_bSbDragV = true;
                m_nSbGrabV = pt.y - th.top;
            }
            else
            {
                m_bSbDragH = true;
                m_nSbGrabH = pt.x - th.left;
            }
            return true;
        }
        // 点击滑块以外：整页翻动
        const SCROLLINFO *psi = GetScrollBarInfo(bVert);
        if (!psi)
            return true;
        int page = psi->nPage > 1 ? psi->nPage - 1 : 1;
        bool bBefore = bVert ? (pt.y < th.top) : (pt.x < th.left);
        int np = psi->nPos + (bBefore ? -page : page);
        if (np < 0)
            np = 0;
        if (np > psi->nMax)
            np = psi->nMax;
        // SetScrollPos 内部（OnScroll 返回 TRUE 时）已经 Invalidate()，勿重复。
        SetScrollPos(bVert, np, TRUE);
        return true;
    }
    return false;
}

void CHexView::SbUpdateDrag(CPoint pt)
{
    bool bVert = m_bSbDragV;
    if (!bVert && !m_bSbDragH)
        return;
    const SCROLLINFO *psi = GetScrollBarInfo(bVert);
    if (!psi)
        return;
    // 与 SbThumb 用同一个「可移动区间」，否则悬停时箭头按钮占位会让拖动与显示不一致
    CRect tr = SbThumbTrack(bVert);
    CRect th = SbThumb(bVert);
    int trackLen = bVert ? tr.Height() : tr.Width();
    int thumbLen = bVert ? th.Height() : th.Width();
    if (trackLen <= thumbLen || thumbLen <= 0)
        return;
    int nRange = psi->nMax + 1;
    int maxScroll = nRange - psi->nPage;
    if (maxScroll <= 0)
        return;
    int off = bVert ? (pt.y - m_nSbGrabV - tr.top) : (pt.x - m_nSbGrabH - tr.left);
    int pos = (int)((long long)off * maxScroll / (trackLen - thumbLen));
    if (pos < 0)
        pos = 0;
    if (pos > maxScroll)
        pos = maxScroll;
    // 同一滚动位置的高频 mousemove 直接跳过：SetScrollPos 会走
    // OnScroll→Invalidate→整帧重绘，没必要重复触发。
    if (pos == psi->nPos)
        return;
    // bRedraw=FALSE：浮层滚动条由 OnPaint 自绘，不需要 SPanel 额外
    // invalidate 滚动条矩形。SPanel::SetScrollPos 在 OnScroll 返回 TRUE 时
    // 内部已经 Invalidate()，这里不要再重复整窗失效一次。
    SetScrollPos(bVert, pos, FALSE);
}

void CHexView::OnPaint(IRenderTarget *pRT)
{
    // SPanel 的 GetClientRect 返回已扣除滚动条区域的矩形（父窗口坐标系）。
    // 渲染原点平移到客户区左上角，内容用本地坐标绘制。
    CRect rcWin = GetClientRect();
    CRect rcClient(0, 0, rcWin.Width(), rcWin.Height());
    pRT->OffsetViewportOrg(rcWin.left, rcWin.top);

    pRT->FillSolidRect(&rcClient, m_crBg);

    EnsureFont(pRT);
    if (!m_fontText)
    {
        DrawOverlayScroll(pRT);
        pRT->OffsetViewportOrg(-rcWin.left, -rcWin.top);
        return;
    }
    ComputeMetrics(pRT);
    pRT->SelectObject((IRenderObj *)(IFontS *)m_fontText, NULL);

    if (!m_pModel || !m_pModel->IsOpen() || m_pModel->Size() == 0)
    {
        DrawStatusText(pRT, rcClient, I18n::T(L"未打开文件  使用 Ctrl+O 打开，或把文件拖到窗口"));
        DrawOverlayScroll(pRT);
        pRT->OffsetViewportOrg(-rcWin.left, -rcWin.top);
        return;
    }

    // 注：UpdateScrollBar 曾在每帧调用；它内部 ShowScrollBar/SetScrollInfo
    // 会同步发送 WM_NCCALCSIZE 并 Invalidate——在 WM_PAINT 栈内做这些构成
    // 消息重入，是大 bpr 场景 soui4.dll 崩溃的触发路径之一。现已改为只在
    // OnSize / SetBytesPerRow(小值) / RefreshAfterBprChange 等入口调用。

    const int rows = GetVisibleRows();
    const int textRight = rcClient.right - 4;
    const int frozen = GetFrozenWidth(); // 偏移列（冻结列）右边界
    const int headerH = GetHeaderHeight();

    // ---- 固定表头：字节列偏移（HxD 风格），垂直滚动时钉在顶端 ----
    if (headerH > 0)
        DrawColumnHeader(pRT, rcClient);

    // ---- 内容区整体下移一行（表头高度）----
    if (headerH > 0)
        pRT->OffsetViewportOrg(0, headerH);
    rcClient.bottom -= headerH;

    // ---- 冻结列：偏移列固定在左缘，不参与横向滚动 ----
    {
        CRect rcFrozen(0, 0, frozen, rcClient.bottom);
        pRT->PushClipRect(&rcFrozen);
        for (int r = 0; r < rows; r++)
        {
            u64 offset = (m_topRow + r) * m_bytesPerRow;
            if (offset >= m_pModel->Size())
                break;
            int y = r * m_cyRow;
            if (y >= rcClient.bottom)
                break;
            SStringW strOff;
            Hex16(strOff, offset, m_offDigits);
            pRT->SetTextColor(m_crOffText);
            pRT->TextOut(m_xOff, y + 3, strOff, m_offDigits);
        }
        pRT->PopClip();
    }

    // ---- 滚动区：hex/ASCII 列随 m_leftCol 平移 ----
    // 内容绘制一律裁剪在客户区内：滚动条位于非客户区（半透明皮肤），
    // 不裁剪时文本会画进滚动条区域并从皮肤下透出（显示为"半透明"残影）。
    CRect rcScroll(frozen, 0, rcClient.right, rcClient.bottom);
    pRT->PushClipRect(&rcScroll);
    pRT->OffsetViewportOrg(-m_leftCol, 0);

    // ---- 背景斑马纹（滚动区部分）----
    for (int r = 0; r < rows; r++)
    {
        if ((r & 1) == 0)
            continue;
        CRect rc(frozen, r * m_cyRow, textRight, (r + 1) * m_cyRow);
        if (rc.top >= rcClient.bottom)
            break;
        if (rc.bottom > rcClient.bottom)
            rc.bottom = rcClient.bottom;
        if (rc.right > frozen)
            pRT->FillSolidRect(&rc, m_crBgAlt);
    }

    // ---- 命中高亮：与选中底共用按行合并矩形，不再逐字节 CellRect + 填充 ----
    if (m_hitEnd > m_hitStart)
        DrawByteRangeBands(pRT, rcClient, m_hitStart, m_hitEnd, m_crHitBg);

    // ---- 选择区 ----
    DrawSelection(pRT, rcClient);

    // ---- 文本（hex / ASCII 列，滚动区）----
    for (int r = 0; r < rows; r++)
    {
        u64 offset = (m_topRow + r) * m_bytesPerRow;
        if (offset >= m_pModel->Size())
            break;
        int y = r * m_cyRow;
        if (y >= rcClient.bottom)
            break;
        u64 n = m_pModel->Read(offset, m_rowCache, m_bytesPerRow);
        if (n == 0)
            break;
        // 最后一行不足 bpr 时只画实际字节数（不补零，与 HxD 行为一致）

        // 本行落在选区内的列区间 [selFrom, selTo)。旧实现在文本画完后又遍历
        // 【整个选区】用白字重画一遍：每字节 2 次 u64 除法、2~6 次 At()/
        // DecodeCellAt()，且选中字节被重复 TextOut 两次。现在直接在本遍
        // 绘制里按列切换文字颜色，一次画完，数据也复用 m_rowCache。
        u64 selFrom = 0, selTo = 0;
        if (m_selEnd > m_selStart && m_selStart < offset + n && m_selEnd > offset)
        {
            u64 a = m_selStart > offset ? m_selStart - offset : 0;
            u64 d = m_selEnd - offset;
            u64 b = d < n ? d : n;
            if (b > a)
            {
                selFrom = a;
                selTo = b;
            }
        }

        // 十六进制列：n 个字节（跨过半行后的分组间隔）。
        // 【逐字符绘制】：整串 TextOut 会按字体 subpixel advance（实测 8.6px/字符）
        // 推进，与 CellRect 的整数网格必然累积错位；选中重绘（逐字符、整数位置）
        // 与底层字符错开 ≤1px 就会在边缘混出杂色——用户看到的"重影"。
        // 改为每个字符都画在自己的整数网格位置上，与 CellRect/命中测试完全同源。
        // 【可视区裁剪】：大 bpr（如 4096）时一行上万个字符，绝大多数在屏幕外；
        // 逐字符绘制前按可视范围跳过屏外列，保证大 bpr 流畅不卡不崩。
        int half = m_bytesPerRow / 2;
        pRT->SetTextColor(m_crHexText);
        // 视口内容坐标范围（滚动区已平移 -m_leftCol，视口=[m_leftCol, m_leftCol+宽]）
        int viewL = m_leftCol - m_cxChar;                // 略放宽一格
        int viewR = m_leftCol + rcClient.right + m_cxChar;
        int iFrom = 0;
        if (m_xHex + 3 * m_cxChar > viewL)
            iFrom = 0;
        else
            iFrom = (viewL - m_xHex) / (3 * m_cxChar) - 1;
        if (iFrom < 0)
            iFrom = 0;
        u64 iEnd = n;
        {
            // 首个完全越过 viewR 的列
            i64 tEnd = (i64)(viewR - m_xHex) / (3 * m_cxChar) + 2;
            if (tEnd < 0)
                tEnd = 0;
            if ((u64)tEnd < iEnd)
                iEnd = (u64)tEnd;
        }
        bool bInSelHex = false;
        for (u64 i = (u64)iFrom; i < iEnd; i++)
        {
            bool bSel = (selTo > selFrom && i >= selFrom && i < selTo);
            if (bSel != bInSelHex)
            {
                pRT->SetTextColor(bSel ? m_crSelText : m_crHexText);
                bInSelHex = bSel;
            }
            int cxCell = m_xHex + int(i * 3 * m_cxChar) + ((int)i >= half && half > 0 ? m_cxChar : 0);
            wchar_t c0[2] = { L"0123456789ABCDEF"[m_rowCache[i] >> 4], 0 };
            wchar_t c1[2] = { L"0123456789ABCDEF"[m_rowCache[i] & 0xF], 0 };
            pRT->TextOut(cxCell, y + 3, c0, 1);
            pRT->TextOut(cxCell + m_cxChar, y + 3, c1, 1);
        }

        // ASCII 列：n 个字节，同样逐字符画在整数网格上（含可视区裁剪）
        pRT->SetTextColor(m_crAsciiText);
        u64 aFrom = 0;
        if (m_xAscii + m_cxChar <= viewL)
        {
            i64 t = (i64)(viewL - m_xAscii) / m_cxChar - 1;
            aFrom = t > 0 ? (u64)t : 0;
        }
        u64 aEnd = n;
        {
            i64 tEnd = (i64)(viewR - m_xAscii) / m_cxChar + 2;
            if (tEnd < 0)
                tEnd = 0;
            if ((u64)tEnd < aEnd)
                aEnd = (u64)tEnd;
        }
        bool bInSelAsc = false;
        for (u64 i = aFrom; i < aEnd; i++)
        {
            bool bSel = (selTo > selFrom && i >= selFrom && i < selTo);
            if (bSel != bInSelAsc)
            {
                pRT->SetTextColor(bSel ? m_crSelText : m_crAsciiText);
                bInSelAsc = bSel;
            }
            wchar_t ch[2] = { DecodeCellBuf(m_rowCache, (int)n, (int)i), 0 };
            pRT->TextOut(m_xAscii + int(i * m_cxChar), y + 3, ch, 1);
        }
    }

    // 选中文字已在主文本循环里用对比色一次画完，不再二次重绘。

    // ---- 光标 ----
    if (m_caret < m_pModel->Size() || m_caret == 0)
    {
        CRect rc = CellRect(m_caret);
        if (rc.bottom > 0 && rc.top < rcClient.bottom && m_bShowCaret)
        {
            if (!m_bCapture)
            {
                // 覆盖/插入模式统一绘制为【竖线】（应用户要求——此前覆盖模式
                // 为反色实心块）。竖线 2px 宽、上下各收 1px。
                CRect rcLine(rc.left, rc.top + 1, rc.left + 2, rc.bottom - 1);
                pRT->FillSolidRect(&rcLine, m_crCaret);
            }
        }
    }

    // 恢复水平滚动平移并解除客户区裁剪（滚动条由 SPanel 绘制在非客户区，不经此路径）
    pRT->OffsetViewportOrg(m_leftCol, 0);
    pRT->PopClip();

    // 恢复内容区下移（表头高度）
    if (headerH > 0)
        pRT->OffsetViewportOrg(0, -headerH);

    // 浮层滚动条画在内容之上（此时视口原点已回到客户区左上角）
    DrawOverlayScroll(pRT);

    pRT->OffsetViewportOrg(-rcWin.left, -rcWin.top);
}

CRect CHexView::CellRect(u64 pos) const
{
    CRect rc(0, 0, 0, 0);
    int row = int(pos / m_bytesPerRow);
    int col = int(pos % m_bytesPerRow);
    rc.top = int((u64(row) - m_topRow) * m_cyRow);
    rc.bottom = rc.top + m_cyRow;
    if (m_nFocusCol == 1)
    {
        rc.left = m_xAscii + col * m_cxChar;
        rc.right = rc.left + m_cxChar;
    }
    else
    {
        // 与文本绘制布局精确对齐：每字节 = 2字符 + 1空格（3 字符步进），
        // 第 half 字节前有一个额外分组空格（1 字符）。
        // 文本里字节 i (i>=half) 的起点 = (i*3+1)*cxChar + m_xHex，
        // 因此 extra = cxChar（不是 2*cxChar——那会把单元格右移一个字符宽，
        // 选中重绘的字节便与底层文本错位 9px，空格位置被邻字节文本覆盖）。
        int stride = 3 * m_cxChar;
        int half = m_bytesPerRow / 2;
        int extra = (col >= half && half > 0) ? m_cxChar : 0;
        rc.left = m_xHex + col * stride + extra;
        rc.right = rc.left + 2 * m_cxChar;
    }
    return rc;
}

void CHexView::OnSize(UINT nType, CSize size)
{
    m_bMetricsDirty = true; // DPI / 字体缩放可能随尺寸变化
    UpdateScrollBar();
    Invalidate();
}

void CHexView::OnSetFocus(SWND wndOld)
{
    static char id = 1;
    SetTimer(id, 500);
    Invalidate();
}

void CHexView::OnKillFocus(SWND wndFocus)
{
    KillTimer(1);
    Invalidate();
}

void CHexView::OnTimer(char cTimerID)
{
    if (cTimerID == 1)
    {
        m_bShowCaret = !m_bShowCaret;
        Invalidate();
    }
    else if (cTimerID == 2)
    {
        AutoScrollTick();
    }
}

void CHexView::SetCaretBlinkEnabled(bool bEnable)
{
    // 暂停/恢复光标闪烁定时器。菜单模式循环期间大 bpr 的重绘帧
    // 会触发 soui4.dll 崩溃，因此弹菜单前必须停掉一切周期性重绘源。
    if (bEnable)
    {
        static char id = 1;
        SetTimer(id, 500); // 恢复（SetTimer 重复调用无害，重置周期）
    }
    else
    {
        KillTimer(1);
    }
}

// ---------------------------------------------------------------------------
// 鼠标
// ---------------------------------------------------------------------------
int CHexView::ColumnHitTest(CPoint pt, int *pNibble) const
{
    int x = pt.x;
    if (pNibble)
        *pNibble = 0;
    if (x < m_xOff + m_cxOff)
        return -1; // 偏移列
    if (x >= m_xAscii)
        return 1; // ascii
    // 十六进制列
    int rel = x - m_xHex;
    int stride = 3 * m_cxChar;
    int idx = rel / stride;
    if (idx < 0)
        idx = 0;
    if (idx >= m_bytesPerRow)
        idx = m_bytesPerRow - 1;
    if (pNibble)
    {
        int half = m_bytesPerRow / 2; // 与 CellRect 的分组间隔一致（1 字符宽）
        int inCell = rel - idx * stride - ((idx >= half && half > 0) ? m_cxChar : 0);
        *pNibble = (inCell < m_cxChar) ? 0 : 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// SOUI 派发鼠标消息时坐标是宿主窗口客户区坐标（未经控件本地化），
// 而命中测试/单元格矩形全部按控件本地坐标（原点=本控件左上角）计算。
// 统一在鼠标消息入口做一次宿主→本地转换。
// ---------------------------------------------------------------------------
CPoint CHexView::ToLocalPoint(CPoint pt) const
{
    // 宿主客户区坐标 → 本控件客户区坐标（SPanel 客户区已扣除滚动条，
    // 滚动条上的点击走 NC 消息，不会进入这里的客户区消息）
    CRect rc = GetClientRect();
    pt.x -= rc.left;
    pt.y -= rc.top;
    return pt;
}

void CHexView::OnLButtonDown(UINT nFlags, CPoint pt)
{
    SetFocus();
    pt = ToLocalPoint(pt);
    // 浮层滚动条优先：命中就消费掉，不再走内容选择逻辑
    if (SbHandleDown(pt))
        return;
    // 不用 SetCapture：SOUI 的控件级捕获在释放时机上不可靠，
    // 一旦漏放会吞掉后续所有点击（工具栏按钮点不动）。
    // 拖选依靠鼠标移动消息 + 边缘自动滚动计时器完成。
    // 滚动条由 SPanel 处理（NC 区），不经过本函数。
    m_bCapture = true;
    m_bShowCaret = true;

    int nibble = 0;
    // 命中测试进入内容坐标系（内容绘制平移了 -m_leftCol）
    CPoint ptContent = pt;
    ptContent.x += m_leftCol;
    int col = ColumnHitTest(ptContent, &nibble);
    if (col < 0)
        return;
    m_nFocusCol = col;
    m_nNibble = nibble;

    CRect rc;
    GetClientRect(&rc);
    int y = (pt.y - GetHeaderHeight()) / m_cyRow; // 扣除顶部固定表头
    u64 row = m_topRow + (y < 0 ? 0 : y);
    u64 maxRow = m_pModel ? (m_pModel->Size() + m_bytesPerRow - 1) / m_bytesPerRow : 0;
    if (row >= maxRow && maxRow > 0)
        row = maxRow - 1;

    int idx;
    if (col == 1)
    {
        int rel = (ptContent.x - m_xAscii) / m_cxChar;
        idx = rel < 0 ? 0 : (rel >= m_bytesPerRow ? m_bytesPerRow - 1 : rel);
    }
    else
    {
        idx = 0;
        int stride = 3 * m_cxChar;
        int rel = ptContent.x - m_xHex;
        int half = m_bytesPerRow / 2; // 分组空格位置（与 CellRect 一致）
        if (rel >= half * stride + m_cxChar)
            rel -= m_cxChar; // 跨过半行处的分组空格（1 字符宽）
        int guess = rel / stride;
        if (guess < 0)
            guess = 0;
        if (guess > m_bytesPerRow - 1)
            guess = m_bytesPerRow - 1;
        idx = guess;
    }

    u64 pos = row * m_bytesPerRow + idx;
    if (m_pModel && pos > m_pModel->Size())
        pos = m_pModel->Size();
    if (m_pModel && pos == m_pModel->Size() && pos > 0)
        pos--;

    m_caret = pos;
    m_selAnchor = pos;
    m_selStart = m_selEnd = pos;
    m_ptLButtonDown = pt;
    EndTypingGroup();
    Invalidate();
    NotifyCaretChanged();
}

void CHexView::OnLButtonDbClick(UINT nFlags, CPoint pt)
{
    pt = ToLocalPoint(pt);
    // 双击选中连续可打印字符（或连续 0x00）
    if (!m_pModel || m_pModel->Size() == 0)
        return;
    CPoint ptContent = pt;
    ptContent.x += m_leftCol; // 内容坐标系
    int col = ColumnHitTest(ptContent, nullptr);
    if (col < 0)
        return;
    int y = (pt.y - GetHeaderHeight()) / m_cyRow; // 扣除顶部固定表头
    u64 row = m_topRow + (y < 0 ? 0 : y);
    int idx = 0;
    if (col == 1)
    {
        int rel = (ptContent.x - m_xAscii) / m_cxChar;
        idx = rel < 0 ? 0 : (rel >= m_bytesPerRow ? m_bytesPerRow - 1 : rel);
    }
    else
    {
        int stride = 3 * m_cxChar;
        int rel = ptContent.x - m_xHex;
        int half = m_bytesPerRow / 2;
        if (rel >= half * stride + m_cxChar)
            rel -= m_cxChar; // 跨过半行处的分组空格（与 CellRect 一致）
        int guess = rel / stride;
        if (guess < 0)
            guess = 0;
        if (guess > m_bytesPerRow - 1)
            guess = m_bytesPerRow - 1;
        idx = guess;
    }
    u64 pos = row * m_bytesPerRow + idx;
    if (pos >= m_pModel->Size())
        return;
    u8 v = m_pModel->At(pos);
    bool bPrintable = (v >= 0x20 && v < 0x7F);
    u64 start = pos, end = pos + 1;
    while (start > 0)
    {
        u8 c = m_pModel->At(start - 1);
        bool p = (c >= 0x20 && c < 0x7F);
        if (p == bPrintable && !(c == 0))
            start--;
        else
            break;
    }
    while (end < m_pModel->Size())
    {
        u8 c = m_pModel->At(end);
        bool p = (c >= 0x20 && c < 0x7F);
        if (p == bPrintable && !(c == 0))
            end++;
        else
            break;
    }
    m_selStart = start;
    m_selEnd = end;
    m_selAnchor = start;
    m_caret = end;
    NotifyCaretChanged();
    Invalidate();
}

void CHexView::OnLButtonUp(UINT nFlags, CPoint pt)
{
    bool bWasCapture = m_bCapture;
    m_bSbDragV = false;
    m_bSbDragH = false;
    m_bCapture = false;
    KillTimer(2);
    m_timerAutoScroll = 0;
    // 拖选期间状态栏/检查器通知被节流，松手时补发一次，保证显示最终值
    if (bWasCapture)
    {
        m_dwLastNotify = 0;
        NotifyCaretChanged();
    }
}

void CHexView::OnMouseLeave()
{
    if (m_bSbHoverV && !HexForceHover())
    {
        m_bSbHoverV = false;
        Invalidate();
    }
}

BOOL CHexView::OnSetCursor(const CPoint &pt)
{
    // 光标形状随位置切换：hex 列与 ascii 列（可编辑文本区）→ I-beam 插入符；
    // 偏移列/表头/ascii 右侧超出内容区/滚动条区域 → 箭头。
    // pt 是宿主客户区坐标（OnFrameSetCursor 直接透传 WM_SETCURSOR 的
    // lParam）→ 转本地后按列命中测试。
    CPoint ptLocal = ToLocalPoint(pt);
    CRect rc;
    GetClientRect(&rc);

    // 滚动条区域、以及「正在拖动滚动条」期间，光标始终是箭头：
    // 拖动时指针很容易滑到 hex/ascii 内容上，此时不该切成 I-beam。
    const int kSbHit = kSbRestW + 6;
    if (m_bSbDragV || m_bSbDragH || (SbNeedV() && ptLocal.x >= rc.Width() - kSbHit) ||
        (SbNeedH() && ptLocal.y >= rc.Height() - kSbHit))
    {
        HCURSOR hArrow = ::LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        if (hArrow)
        {
            ::SetCursor(hArrow);
            return TRUE;
        }
        return SWindow::OnSetCursor(pt);
    }

    // 垂直方向：表头以下的内容行区域
    int nHeader = GetHeaderHeight();
    if (ptLocal.y >= nHeader && ptLocal.y < rc.Height()
        && ptLocal.x >= 0 && ptLocal.x < rc.Width())
    {
        // 内容坐标系（绘制平移 -m_leftCol）
        int xContent = ptLocal.x + m_leftCol;
        // 内容右界 = ascii 列尾（ColumnHitTest 对超界 x 做钳制会误报 ascii，
        // 先显式排除超出内容宽度的位置）
        int xContentEnd = m_xAscii + m_cxChar * m_bytesPerRow;
        if (xContent < xContentEnd)
        {
            int col = ColumnHitTest(CPoint(xContent, ptLocal.y), nullptr);
            if (col == 0 || col == 1)
            {
                HCURSOR hCur = ::LoadCursorW(NULL, (LPCWSTR)IDC_IBEAM);
                if (hCur)
                {
                    ::SetCursor(hCur);
                    return TRUE;
                }
            }
        }
    }
    // 其余区域走 SOUI 默认（SWindow::OnSetCursor → style cursor → 箭头）
    return SWindow::OnSetCursor(pt);
}

void CHexView::OnMouseMove(UINT nFlags, CPoint pt)
{
    pt = ToLocalPoint(pt);
    m_ptLastMouse = pt;

    // 浮层滚动条：拖拽中优先处理，否则只更新悬停态
    if (m_bSbDragV || m_bSbDragH)
    {
        // 防御：在窗口外松开左键时收不到 WM_LBUTTONUP，拖动标志会残留
        // （光标一直是箭头、指针移动还会继续滚动）。这里按按键状态兜底结束拖动。
        if (!(nFlags & MK_LBUTTON))
        {
            m_bSbDragV = false;
            m_bSbDragH = false;
        }
        else
        {
            SbUpdateDrag(pt);
            return;
        }
    }
    UpdateSbHover(pt);

    // 防御：左键已经抬起却还持有捕获时立刻释放，
    // 否则后续点击会被本控件吞掉（工具栏按钮点不动）
    if (m_bCapture && !(nFlags & MK_LBUTTON))
    {
        m_bCapture = false;
        KillTimer(2);
        m_timerAutoScroll = 0;
        return;
    }
    if (!m_bCapture)
        return;
    CRect rc;
    GetClientRect(&rc);

    if (pt.y < 0 || pt.y > rc.Height() || pt.x < 0 || pt.x > rc.Width())
    {
        if (!m_timerAutoScroll)
        {
            m_timerAutoScroll = 1;
            SetTimer(2, 30);
        }
    }

    int col = ColumnHitTest(CPoint(pt.x + m_leftCol, pt.y), nullptr);
    if (col < 0)
        col = m_nFocusCol;
    int y = (pt.y - GetHeaderHeight()) / m_cyRow; // 扣除顶部固定表头
    u64 row = m_topRow + (y < 0 ? 0 : y);
    int idx = 0;
    if (col == 1)
    {
        int rel = (pt.x + m_leftCol - m_xAscii) / m_cxChar;
        idx = rel < 0 ? 0 : (rel >= m_bytesPerRow ? m_bytesPerRow - 1 : rel);
    }
    else
    {
        int stride = 3 * m_cxChar;
        int rel = pt.x + m_leftCol - m_xHex;
        int half = m_bytesPerRow / 2;
        if (rel >= half * stride + m_cxChar)
            rel -= m_cxChar; // 跨过半行处的分组空格（与 CellRect 一致）
        int guess = rel / stride;
        if (guess < 0)
            guess = 0;
        if (guess > m_bytesPerRow - 1)
            guess = m_bytesPerRow - 1;
        idx = guess;
    }
    u64 pos = row * m_bytesPerRow + idx;
    if (m_pModel)
    {
        if (pos > m_pModel->Size())
            pos = m_pModel->Size();
        // 一个字节格子约 24px，100Hz 的鼠标消息绝大多数落在同一格内。
        // 旧实现此时仍无条件全窗 Invalidate + 刷新十几个控件，是拖动迟滞的主因。
        if (pos == m_caret)
            return;
        m_caret = pos;
        m_selStart = umin64(m_selAnchor, m_caret);
        m_selEnd = umax64(m_selAnchor, m_caret);
        EndTypingGroup();
        Invalidate();
        NotifyCaretChangedThrottled();
    }
}

void CHexView::AutoScrollTick()
{
    CPoint pt = m_ptLastMouse;
    CRect rc;
    GetClientRect(&rc);
    int before = 0;
    if (pt.y < 0)
    {
        ScrollByRows(-1);
        before = 1;
    }
    else if (pt.y > rc.Height())
    {
        ScrollByRows(1);
        before = 2;
    }
    if (before && m_bCapture)
    {
        // 同步光标到视图边缘，保持拖选
        CPoint ptEdge = pt;
        if (ptEdge.y < 0)
            ptEdge.y = 0;
        if (ptEdge.y > rc.Height() - 1)
            ptEdge.y = rc.Height() - 1;
        ptEdge.x += m_leftCol; // 内容坐标系
        int col = ColumnHitTest(ptEdge, nullptr);
        if (col < 0)
            col = m_nFocusCol;
        int y = (ptEdge.y - GetHeaderHeight()) / m_cyRow; // 扣除顶部固定表头
        u64 row = m_topRow + (y < 0 ? 0 : y);
        int idx = 0;
        if (col == 1)
        {
            int rel = (ptEdge.x - m_xAscii) / m_cxChar;
            idx = rel < 0 ? 0 : (rel >= m_bytesPerRow ? m_bytesPerRow - 1 : rel);
        }
        else
        {
            int stride = 3 * m_cxChar;
            int rel = ptEdge.x - m_xHex;
            int half = m_bytesPerRow / 2;
            if (rel >= half * stride + m_cxChar)
                rel -= m_cxChar; // 跨过半行处的分组空格（与 CellRect 一致）
            int guess = rel / stride;
            if (guess < 0)
                guess = 0;
            if (guess > m_bytesPerRow - 1)
                guess = m_bytesPerRow - 1;
            idx = guess;
        }
        u64 pos = row * m_bytesPerRow + idx;
        if (m_pModel)
        {
            if (pos > m_pModel->Size())
                pos = m_pModel->Size();
            m_caret = pos;
            m_selStart = umin64(m_selAnchor, m_caret);
            m_selEnd = umax64(m_selAnchor, m_caret);
            Invalidate();
            NotifyCaretChanged();
        }
    }
}

// 重写 SPanel::OnMouseWheel 虚函数（SPanel 消息映射中 OnMouseWheel 经虚表解析到这里）
BOOL CHexView::OnMouseWheel(UINT nFlags, short zDelta, CPoint pt)
{
    if (GetKeyState(VK_CONTROL) < 0)
    {
        SetMsgHandled(FALSE);
        return FALSE;
    }
    // Shift+滚轮：横向滚动（SOUI 无内置转换，由本控件驱动水平方向）
    if ((nFlags & MK_SHIFT) || GetKeyState(VK_SHIFT) < 0)
    {
        if (HasScrollBar(FALSE))
        {
            // 每次 3 个字符步进（与 GetScrollLineSize(FALSE) 一致）
            for (int i = 0; i < 3; i++)
                OnScroll(FALSE, zDelta > 0 ? SB_LINEUP : SB_LINEDOWN, 0);
        }
        return TRUE;
    }
    // 普通滚轮：交给 SPanel 默认纵向处理
    return SPanel::OnMouseWheel(nFlags, zDelta, pt);
}

void CHexView::ScrollByRows(int n)
{
    if (n == 0 || !m_pModel)
        return;
    // 直接驱动 SPanel 的纵向滚动（会走 OnScroll 回调更新 m_topRow）
    if (HasScrollBar(TRUE))
    {
        OnScroll(TRUE, n > 0 ? SB_LINEDOWN : SB_LINEUP, 0);
        // 多行滚动
        for (int i = 1; i < (n > 0 ? n : -n); i++)
            OnScroll(TRUE, n > 0 ? SB_LINEDOWN : SB_LINEUP, 0);
    }
    else
    {
        m_topRow = 0;
    }
    Invalidate();
}

void CHexView::ScrollToOffset(u64 pos, bool bCenter)
{
    if (!m_pModel)
        return;
    int visible = GetVisibleRows();
    u64 total = GetTotalRows();
    u64 row = pos / m_bytesPerRow;
    i64 t;
    if (bCenter)
    {
        t = (i64)row - visible / 2;
    }
    else
    {
        if (row < m_topRow)
            t = (i64)row;
        else if (row >= m_topRow + (u64)visible)
            t = (i64)row - visible / 2;
        else
            t = (i64)m_topRow;
    }
    if (t < 0)
        t = 0;
    if (HasScrollBar(TRUE))
    {
        // 行组制：把目标行 t 换算成滚动单位位置交给 SPanel 校验上限，再乘回真实行号。
        SetScrollPos(TRUE, (int)((u64)t / m_vUnit), FALSE);
        m_topRow = (u64)GetScrollPos(TRUE) * (u64)m_vUnit;
        u64 tt = GetTotalRows();
        int vis = GetVisibleRows();
        u64 maxTop = (tt > (u64)vis) ? tt - vis : 0;
        if (m_topRow > maxTop)
            m_topRow = maxTop;
    }
    else
    {
        m_topRow = 0;
    }
    Invalidate();
}

// ---------------------------------------------------------------------------
// 宿主层滚轮兜底（CMainFrame::OnHostMouseWheel 调用）
// ---------------------------------------------------------------------------
void CHexView::OnHostWheelV(int nLines)
{
    if (!m_pModel)
        return;
    if (HasScrollBar(TRUE))
    {
        // 经 SPanel 校验上限，滚动条与视图同步。nLines 是「行」增量，按 vUnit 折算成
        // 滚动单位步长（至少 1 单位），用 SPanel 的单位位置做累加避免卡顿/粘滞。
        int nPos = GetScrollPos(TRUE);
        int step = nLines / m_vUnit;
        if (step == 0 && nLines != 0)
            step = (nLines > 0) ? 1 : -1;
        SetScrollPos(TRUE, nPos + step, FALSE);
        m_topRow = (u64)GetScrollPos(TRUE) * (u64)m_vUnit;
        u64 tt = GetTotalRows();
        int vis = GetVisibleRows();
        u64 maxTop = (tt > (u64)vis) ? tt - vis : 0;
        if (m_topRow > maxTop)
            m_topRow = maxTop;
        Invalidate();
    }
}

void CHexView::OnHostWheelH(int zDelta)
{
    if (!m_pModel)
        return;
    // 所有 bpr（含大 bpr 显示横向条）都走这里：直接改 m_leftCol，避免
    // SetScrollPos/OnScroll 链在大内容宽下的消息重入。浮层滑块位置取自
    // m_siHoz.nPos，所以下面要顺手同步它——UpdateScrollBar 早已不在 OnPaint
    // 里每帧调用，不能再指望"下一帧自动同步"。
    const SCROLLINFO *psi = GetScrollBarInfo(FALSE);
    int unit = m_cxChar * 4;
    int maxLeft = HasScrollBar(FALSE) ? ((int)psi->nMax - (int)psi->nPage + 1) * unit
                                      : GetContentWidth() - GetFrozenWidth()
                                            - (GetClientRect().Width() - GetFrozenWidth());
    if (maxLeft < 0)
        maxLeft = 0;
    int nSteps = zDelta > 0 ? -3 : 3;
    int nl = m_leftCol + nSteps * unit;
    if (nl < 0)
        nl = 0;
    if (nl > maxLeft)
        nl = maxLeft;
    if (nl != m_leftCol)
    {
        m_leftCol = nl;
        // 同步 SPanel 的横向滚动位置（浮层滑块由 m_siHoz.nPos 决定）。
        // 用 SetScrollInfo 纯内存赋值，不发 WM_NCCALCSIZE、不走 OnScroll 重入链。
        // 注意 SetScrollInfo 内部用「传入的 si.nPage」判空，只带 SIF_POS 会让
        // nPage=0 把 nPos 清零，所以 range/page 必须原样回填。
        if (HasScrollBar(FALSE) && psi->nPage > 0)
        {
            SCROLLINFO si = { sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE | SIF_POS };
            si.nMin = psi->nMin;
            si.nMax = psi->nMax;
            si.nPage = psi->nPage;
            si.nPos = m_leftCol / unit;
            SetScrollInfo(si, FALSE);
        }
        Invalidate();
    }
}

// ---------------------------------------------------------------------------
// 键盘与编辑
// ---------------------------------------------------------------------------
void CHexView::BeginTypingGroup()
{
    if (!m_bTypingActive)
    {
        if (m_pModel)
            m_pModel->BeginUndo();
        m_bTypingActive = true;
    }
}

void CHexView::EndTypingGroup()
{
    if (m_bTypingActive)
    {
        if (m_pModel)
            m_pModel->EndUndo();
        m_bTypingActive = false;
        if (m_pListener)
            m_pListener->OnHexViewModifiedChanged(this);
    }
}

void CHexView::SetCaretPos(u64 pos, bool bExtend)
{
    m_bPendingValid = false;
    if (m_pModel)
    {
        if (pos > m_pModel->Size())
            pos = m_pModel->Size();
    }
    m_caret = pos;
    m_nNibble = 0;
    if (!bExtend)
    {
        m_selAnchor = pos;
        m_selStart = m_selEnd = pos;
    }
    else
    {
        m_selStart = umin64(m_selAnchor, m_caret);
        m_selEnd = umax64(m_selAnchor, m_caret);
    }
    m_bShowCaret = true;
    Invalidate();
    NotifyCaretChanged();
}

void CHexView::MoveCaret(i64 delta, bool bExtend)
{
    i64 p = (i64)m_caret + delta;
    if (p < 0)
        p = 0;
    if (m_pModel && (u64)p > m_pModel->Size())
        p = (i64)m_pModel->Size();
    EnsureCaretVisible();
    SetCaretPos((u64)p, bExtend);
}

void CHexView::PageMove(i64 lines, bool bExtend)
{
    ScrollByRows((int)lines);
    i64 p = (i64)m_caret + lines * m_bytesPerRow;
    if (p < 0)
        p = 0;
    if (m_pModel && (u64)p > m_pModel->Size())
        p = (i64)m_pModel->Size();
    SetCaretPos((u64)p, bExtend);
}

void CHexView::EnsureCaretVisible()
{
    if (!m_pModel)
        return;
    u64 row = m_caret / m_bytesPerRow;
    int visible = GetVisibleRows();
    if (row < m_topRow)
    {
        ScrollByRows((int)(row - m_topRow));
    }
    else if (row >= m_topRow + (u64)visible - 1)
    {
        ScrollByRows((int)(row - m_topRow - (u64)visible / 2));
    }

    // 横向可见性：光标单元格移入可视范围（SPanel 横向滚动，按「4 列」单位）
    if (HasScrollBar(FALSE))
    {
        CRect rcCell = CellRect(m_caret); // 内容坐标系
        CRect rc = GetClientRect();
        int viewW = rc.Width();
        int unit = m_cxChar * 4;
        if (rcCell.right < m_leftCol)
        {
            int nl = rcCell.left - m_cxChar * 2;
            nl = nl < 0 ? 0 : nl;
            nl = (nl / unit) * unit; // 对齐单位边界
            if (nl != m_leftCol)
            {
                m_leftCol = nl;
                SetScrollPos(FALSE, nl / unit, TRUE);
                Invalidate();
            }
        }
        else if (rcCell.left > m_leftCol + viewW)
        {
            int nl = rcCell.right - viewW + m_cxChar * 2;
            const SCROLLINFO *psi = GetScrollBarInfo(FALSE);
            int maxLeft = ((int)psi->nMax - (int)psi->nPage + 1) * unit; // 单位制→像素
            if (maxLeft < 0)
                maxLeft = 0;
            if (nl > maxLeft)
                nl = maxLeft;
            nl = (nl / unit) * unit; // 对齐单位边界
            if (nl != m_leftCol)
            {
                m_leftCol = nl;
                SetScrollPos(FALSE, nl / unit, TRUE);
                Invalidate();
            }
        }
    }
}

void CHexView::NotifyCaretChanged()
{
    if (m_pListener)
        m_pListener->OnHexViewCaretChanged(this);
}

void CHexView::NotifyCaretChangedThrottled()
{
    DWORD dwNow = GetTickCount();
    // 33ms ≈ 30Hz。无符号减法在 GetTickCount 回绕时仍然正确。
    // 状态栏/检查器一次刷新要写十几个控件，没必要跟着 100Hz 的鼠标消息跑。
    if (m_dwLastNotify && (dwNow - m_dwLastNotify) < 33)
        return;
    m_dwLastNotify = dwNow;
    NotifyCaretChanged();
}

void CHexView::WriteByteAtCaret(u8 byNew)
{
    if (!m_pModel || m_pModel->Size() == 0 || m_pModel->IsReadOnly())
        return;
    BeginTypingGroup();
    if (m_bInsertMode)
        m_pModel->Insert(m_caret, &byNew, 1);
    else
    {
        if (m_caret >= m_pModel->Size())
        {
            // 覆盖模式下越过文件尾：先追加一个字节
            u8 zero = 0;
            m_pModel->Insert(m_caret, &zero, 1);
        }
        m_pModel->Write(m_caret, &byNew, 1);
    }
    MoveCaret(1, false);
}

void CHexView::Cut()
{
    Copy();
    DeleteSelection();
}

void CHexView::DeleteSelection()
{
    if (!m_pModel || m_selStart == m_selEnd || m_pModel->IsReadOnly())
        return;
    BeginTypingGroup();
    u64 len = m_selEnd - m_selStart;
    if (m_bInsertMode)
    {
        m_pModel->Delete(m_selStart, len);
    }
    else
    {
        std::vector<u8> zeros((size_t)len, 0);
        m_pModel->Write(m_selStart, &zeros[0], len);
    }
    EndTypingGroup();
    SetCaretPos(m_selStart, false);
}

void CHexView::SetSelection(u64 start, u64 end)
{
    m_selAnchor = start;
    m_selStart = start;
    m_selEnd = end;
    m_caret = end;
    Invalidate();
    NotifyCaretChanged();
}

void CHexView::SetSelectionKeepCaret(u64 start, u64 end)
{
    // 设置选区但【不移动光标】：检查器联动框选用——光标必须留在原字节，
    // 否则 (1) 框选 1 字节时光标跳到下一字节闪烁；
    //       (2) caret 被改写后，下次联动按新 caret 计算 → 选区逐次漂移
    //           （点字节→选[0,1) caret=1；点字→按 caret=1 选[1,3) 而非 [0,2)）。
    m_selAnchor = start;
    m_selStart = start;
    m_selEnd = end;
    Invalidate();
}

void CHexView::SelectAll()
{
    if (!m_pModel)
        return;
    u64 size = m_pModel->Size();
    SetSelection(0, size);
    m_caret = 0;
}

bool CHexView::GetSelection(u64 &start, u64 &end) const
{
    if (m_selStart == m_selEnd)
        return false;
    start = m_selStart;
    end = m_selEnd;
    return true;
}

void CHexView::SetBytesPerRow(int n)
{
    if (n < 1)
        n = DEFAULT_BPR;
    if (n > MAX_BPR)
        n = MAX_BPR;
    if (n == m_bytesPerRow)
        return;
    m_bytesPerRow = n;
    m_leftCol = 0; // 重置水平滚动
    m_bRowCacheOk = false;
    m_bMetricsDirty = true; // m_cxHex / m_xAscii 依赖 bpr
    // 256 只是「同步刷新 / 延迟刷新」的分界，不是取值上限（上限是 MAX_BPR=4096）。
    if (n <= 256)
    {
        UpdateScrollBar();
        Invalidate();
    }
    else
    {
        // 大 bpr：从编辑框 KillFocus 的消息栈内同步驱动
        // ShowScrollBar→WM_NCCALCSIZE→Invalidate 的重入链会触发 soui4.dll
        // 内部时序缺陷（0xc000041d，调试器一附加即消失）。改为投递到
        // 宿主，等当前消息栈完全退净后在空闲回调里刷新。
        HWND hHost = GetContainer()->GetHostHwnd();
        if (hHost)
            ::PostMessageW(hHost, WM_APP_BPR_REFRESH, 0, 0);
    }
}

void CHexView::SetCharset(int n)
{
    if (n < 0 || n > 2)
        n = 0;
    if (n == m_charset)
        return;
    m_charset = n;
    m_bRowCacheOk = false;
    Invalidate();
}

// 行内缓冲解码：i 为列索引（buf 为该行起始的 n 个字节）
wchar_t CHexView::DecodeCellBuf(const u8 *buf, int n, int i) const
{
    if (!buf || i < 0 || i >= n)
        return L'.';
    u8 b = buf[i];
    if (m_charset == 2) // UTF-16LE：偶数列读 2 字节，奇数列为高字节
    {
        if ((i & 1) != 0 || i + 1 >= n)
            return L'.';
        wchar_t w = (wchar_t)(b | ((unsigned)buf[i + 1] << 8));
        if (w < 0x20 || w == 0x7F || w == 0xFFFF)
            return L'.';
        return w;
    }
    if (m_charset == 1) // UTF-8：首字节显示字符，续字节显示 '.'
    {
        if ((b & 0xC0) == 0x80)
            return L'.';
        int len = 0;
        wchar_t w = 0;
        if (b < 0x80)
        {
            len = 1;
            w = b;
        }
        else if ((b & 0xE0) == 0xC0)
        {
            len = 2;
            w = b & 0x1F;
        }
        else if ((b & 0xF0) == 0xE0)
        {
            len = 3;
            w = b & 0x0F;
        }
        else if ((b & 0xF8) == 0xF0)
        {
            len = 4;
            w = b & 0x07;
        }
        else
            return L'.';
        if (i + len > n)
            return L'.';
        for (int k = 1; k < len; k++)
        {
            u8 c = buf[i + k];
            if ((c & 0xC0) != 0x80)
                return L'.';
            w = (wchar_t)((w << 6) | (c & 0x3F));
        }
        if (w < 0x20 || w == 0x7F)
            return L'.';
        return w;
    }
    // ANSI：按 Latin-1 直映射（0x20-0x7E 可见 ASCII，0xA0-0xFF 扩展字符）。
    // 不用 CP_ACP 解码：中文系统 CP936 会把单字节解成 CJK 字符，而 ASCII 列
    // 字体无 CJK 字形，反而渲染成 '?'（HxD 的 ANSI 列同样是 Latin-1 语义）。
    if (b < 0x80)
        return (b >= 0x20 && b != 0x7F) ? (wchar_t)b : L'.';
    if (b >= 0xA0)
        return (wchar_t)b;
    return L'.'; // 0x80-0x9F：C1 控制区
}

wchar_t CHexView::DecodeCellAt(u64 pos) const
{
    if (!m_pModel)
        return L'.';
    u64 sz = m_pModel->Size();
    if (pos >= sz)
        return L'.';
    u64 rowBase = pos - (pos % (u64)m_bytesPerRow);
    int col = (int)(pos - rowBase);
    if (m_charset == 2 && (col & 1) != 0)
        return L'.'; // UTF-16LE 高字节
    u8 buf[4] = {0, 0, 0, 0};
    int n = 0;
    for (; n < 4 && col + n < m_bytesPerRow && rowBase + col + n < sz; n++)
        buf[n] = m_pModel->At(rowBase + col + n);
    return DecodeCellBuf(buf, n, 0);
}

void CHexView::RefreshAfterBprChange()
{
    UpdateScrollBar();
    Invalidate();
}

void CHexView::SetInsertMode(bool bInsert)
{
    m_bInsertMode = bInsert;
    Invalidate();
}

void CHexView::OnKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags)
{
    bool bCtrl = (GetKeyState(VK_CONTROL) < 0);
    bool bShift = (GetKeyState(VK_SHIFT) < 0);

    switch (nChar)
    {
    case VK_TAB:
        m_nFocusCol = (m_nFocusCol == 0) ? 1 : 0;
        Invalidate();
        return;
    case VK_LEFT:
        EndTypingGroup();
        MoveCaret(-1, bShift);
        return;
    case VK_RIGHT:
        EndTypingGroup();
        if (bCtrl)
        {
            // 跳到下一个非零变化点
            MoveCaret(16, bShift);
        }
        else
            MoveCaret(1, bShift);
        return;
    case VK_UP:
        EndTypingGroup();
        MoveCaret(-m_bytesPerRow, bShift);
        return;
    case VK_DOWN:
        EndTypingGroup();
        MoveCaret(m_bytesPerRow, bShift);
        return;
    case VK_HOME:
        EndTypingGroup();
        if (bCtrl)
            SetCaretPos(0, bShift);
        else
            SetCaretPos(m_caret / m_bytesPerRow * m_bytesPerRow, bShift);
        EnsureCaretVisible();
        return;
    case VK_END:
        EndTypingGroup();
        if (bCtrl)
        {
            if (m_pModel)
                SetCaretPos(m_pModel->Size(), bShift);
        }
        else
            SetCaretPos(m_caret / m_bytesPerRow * m_bytesPerRow + (m_bytesPerRow - 1), bShift);
        EnsureCaretVisible();
        return;
    case VK_PRIOR:
        EndTypingGroup();
        PageMove(-(GetVisibleRows() - 1), bShift);
        return;
    case VK_NEXT:
        EndTypingGroup();
        PageMove(GetVisibleRows() - 1, bShift);
        return;
    case VK_DELETE:
        DeleteSelection();
        return;
    case VK_BACK: {
        if (!m_pModel)
            return;
        if (m_selStart != m_selEnd)
        {
            DeleteSelection();
            return;
        }
        if (m_bInsertMode)
        {
            BeginTypingGroup();
            if (m_caret > 0)
                m_pModel->Delete(m_caret - 1, 1);
            EndTypingGroup();
            MoveCaret(-1, false);
        }
        else
        {
            BeginTypingGroup();
            if (m_caret > 0)
            {
                u8 zero = 0;
                m_pModel->Write(m_caret - 1, &zero, 1);
            }
            EndTypingGroup();
            MoveCaret(-1, false);
        }
        return;
    }
    case VK_ESCAPE:
        m_selStart = m_selEnd = m_caret;
        m_selAnchor = m_caret;
        Invalidate();
        return;
    case 'A':
        if (bCtrl)
        {
            SelectAll();
            return;
        }
        break;
    case 'C':
        if (bCtrl)
        {
            Copy();
            return;
        }
        break;
    case 'V':
        if (bCtrl)
        {
            Paste();
            return;
        }
        break;
    case 'X':
        if (bCtrl)
        {
            Copy();
            DeleteSelection();
            return;
        }
        break;
    case 'Z':
        if (bCtrl && m_pModel)
        {
            EndTypingGroup();
            if (m_pModel->Undo())
            {
                RefreshFromModel();
            }
            return;
        }
        break;
    case 'Y':
        if (bCtrl && m_pModel)
        {
            if (m_pModel->Redo())
                RefreshFromModel();
            return;
        }
        break;
    case 'F':
        if (bCtrl && m_pListener)
        {
            m_pListener->OnHexViewCommand(this, 1);
            return;
        }
        break;
    case 'G':
        if (bCtrl && m_pListener)
        {
            m_pListener->OnHexViewCommand(this, 2);
            return;
        }
        break;
    case 'S':
        if (bCtrl && m_pListener)
        {
            m_pListener->OnHexViewCommand(this, 3);
            return;
        }
        break;
    case VK_F3:
        if (m_pListener)
        {
            m_pListener->OnHexViewCommand(this, 4);
            return;
        }
        break;
    default:
        break;
    }
    SetMsgHandled(FALSE);
}

void CHexView::OnChar(UINT nChar, UINT nFlagsCnt, UINT nFlags)
{
    if (!m_pModel || m_pModel->Size() == 0 || m_pModel->IsReadOnly())
        return;

    if (m_nFocusCol == 0)
    {
        // 十六进制列：输入 nibble
        int v = -1;
        if (nChar >= '0' && nChar <= '9')
            v = nChar - '0';
        else if (nChar >= 'a' && nChar <= 'f')
            v = 10 + nChar - 'a';
        else if (nChar >= 'A' && nChar <= 'F')
            v = 10 + nChar - 'A';
        if (v < 0)
            return;

        if (m_nNibble == 0)
        {
            if (m_bInsertMode)
            {
                // 插入模式：记下待写入字节的高 4 位
                m_u8Pending = (u8)(v << 4);
                m_bPendingValid = true;
            }
            else
            {
                // 覆盖模式：直接改写当前字节高 4 位
                if (m_caret < m_pModel->Size())
                {
                    u8 cur = m_pModel->At(m_caret);
                    u8 byNew = (u8)((cur & 0x0F) | (v << 4));
                    BeginTypingGroup();
                    m_pModel->Write(m_caret, &byNew, 1);
                }
            }
            m_nNibble = 1;
            m_bRowCacheOk = false;
            Invalidate();
            return;
        }

        // 第二个 nibble
        u8 byNew;
        if (m_bInsertMode)
        {
            byNew = (u8)(m_u8Pending | v);
            m_bPendingValid = false;
            BeginTypingGroup();
            m_pModel->Insert(m_caret, &byNew, 1);
        }
        else
        {
            if (m_caret < m_pModel->Size())
            {
                u8 cur = m_pModel->At(m_caret);
                byNew = (u8)((cur & 0xF0) | v);
            }
            else
            {
                byNew = (u8)v;
            }
            BeginTypingGroup();
            if (m_caret < m_pModel->Size())
                m_pModel->Write(m_caret, &byNew, 1);
        }
        m_nNibble = 0;
        m_bRowCacheOk = false;
        MoveCaret(1, false);
        Invalidate();
        return;
    }

    // ASCII 列：直接写入字符值
    if (nChar >= 0x20 && nChar < 0x7F)
    {
        WriteByteAtCaret((u8)nChar);
        return;
    }
}

void CHexView::RefreshFromModel()
{
    if (!m_pModel)
        return;
    if (m_caret > m_pModel->Size())
        m_caret = m_pModel->Size();
    if (m_selEnd > m_pModel->Size() || m_selStart > m_pModel->Size())
    {
        m_selStart = m_selEnd = m_caret;
        m_selAnchor = m_caret;
    }
    m_bRowCacheOk = false;
    UpdateScrollBar();
    Invalidate();
    NotifyCaretChanged();
    if (m_pListener)
        m_pListener->OnHexViewModifiedChanged(this);
}


// ---------------------------------------------------------------------------
// 剪贴板
// ---------------------------------------------------------------------------
#ifndef _WIN32
static std::vector<u8> s_clipboard; // 非 Windows 平台：进程内剪贴板
#endif

void CHexView::Copy()
{
    if (!m_pModel || m_selStart == m_selEnd)
        return;
    u64 len = m_selEnd - m_selStart;
    if (len == 0)
        return;
    std::vector<u8> data((size_t)len);
    m_pModel->Read(m_selStart, &data[0], len);

    // ASCII 视图文本
    std::wstring text;
    text.reserve(data.size() + 1);
    for (size_t i = 0; i < data.size(); i++)
    {
        u8 c = data[i];
        text.push_back((c >= 0x20 && c < 0x7F) ? (wchar_t)c : L'.');
    }

#ifdef _WIN32
    if (!OpenClipboard(NULL))
        return;
    EmptyClipboard();
    HGLOBAL hText = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (hText)
    {
        wchar_t *p = (wchar_t *)GlobalLock(hText);
        if (p)
        {
            memcpy(p, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(hText);
            SetClipboardData(CF_UNICODETEXT, hText);
        }
    }
    UINT uFmt = RegisterClipboardFormatW(L"HexEditor.Bytes");
    if (uFmt)
    {
        HGLOBAL hBin = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)len);
        if (hBin)
        {
            u8 *p = (u8 *)GlobalLock(hBin);
            if (p)
            {
                memcpy(p, &data[0], (size_t)len);
                GlobalUnlock(hBin);
                SetClipboardData(uFmt, hBin);
            }
        }
    }
    CloseClipboard();
#else
    s_clipboard = data;
#endif
}

void CHexView::Paste()
{
    if (!m_pModel || m_pModel->IsReadOnly())
        return;
    std::vector<u8> data;

#ifdef _WIN32
    if (OpenClipboard(NULL))
    {
        UINT uFmt = RegisterClipboardFormatW(L"HexEditor.Bytes");
        HANDLE h = uFmt ? GetClipboardData(uFmt) : NULL;
        if (h)
        {
            u8 *p = (u8 *)GlobalLock(h);
            SIZE_T n = GlobalSize(h);
            if (p && n)
                data.assign(p, p + n);
            GlobalUnlock(h);
        }
        else
        {
            HANDLE t = GetClipboardData(CF_UNICODETEXT);
            if (t)
            {
                const wchar_t *p = (const wchar_t *)GlobalLock(t);
                if (p)
                {
                    std::wstring ws(p);
                    for (size_t i = 0; i < ws.size(); i++)
                        data.push_back((u8)(ws[i] & 0xFF));
                }
                GlobalUnlock(t);
            }
        }
        CloseClipboard();
    }
#else
    data = s_clipboard;
#endif

    if (data.empty())
        return;

    BeginTypingGroup();
    if (m_selStart != m_selEnd)
    {
        u64 len = m_selEnd - m_selStart;
        if (m_bInsertMode)
            m_pModel->Delete(m_selStart, len);
        else
        {
            std::vector<u8> zeros((size_t)len, 0);
            m_pModel->Write(m_selStart, &zeros[0], len);
        }
    }

    u64 caret = m_selStart ? m_selStart : m_caret;
    if (m_bInsertMode)
        m_pModel->Insert(caret, &data[0], data.size());
    else
        m_pModel->Write(caret, &data[0], data.size());
    EndTypingGroup();

    m_selStart = m_selEnd = caret + data.size();
    m_selAnchor = m_selEnd;
    m_caret = m_selEnd;
    EnsureCaretVisible();
    Invalidate();
    NotifyCaretChanged();
    if (m_pListener)
        m_pListener->OnHexViewModifiedChanged(this);
}
