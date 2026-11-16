//
// CMainFrame - 主窗口：标题栏 / 菜单栏 / 工具栏 / 查找栏 / 十六进制视图 / 数据检查器 / 状态栏
//
#ifndef MAIN_FRAME_H
#define MAIN_FRAME_H

#include <souistd.h>
#include <control/SouiCtrls.h>
#include <control/STabCtrl.h>

#include "HexFileModel.h"
#include "HexSearch.h"
#include "HexView.h"
#include "InspectorWnd.h"
#include "FindWnd.h"
#include "AboutWnd.h"
#include "Theme.h"

using namespace SOUI;

class CInspectorWnd;

/** STabCtrl 派生：把 protected 的测量/矩形接口提升为 public。
    标签滚动按钮的显隐判定与自动滚动需要标签的【自然宽度】（未裁剪）——
    GetItemRect 会把越出标签条的部分 IntersectRect 裁掉，溢出时返回截断矩形，
    不能用于"内容是否溢出"的判定；必须用未裁剪的 MeasureTabWidth 累加。 */
class STabCtrlEx : public STabCtrl
{
    DEF_SOBJECT(STabCtrl, L"tabctrl_ex")
  public:
    BOOL GetItemRectPublic(int nIndex, CRect &rcItem) const
    {
        return GetItemRect(nIndex, rcItem);
    }
    /** 标签的自然宽度（未裁剪，随 DPI/字体真实测量） */
    int MeasureTabWidthPublic(int nIndex) const
    {
        SAutoRefPtr<IRenderTarget> pRT;
        GETRENDERFACTORY->CreateRenderTarget(&pRT);
        BeforePaintEx(pRT);
        return MeasureTabWidth(pRT, nIndex);
    }
};

class CMainFrame : public SHostWnd, public IHexViewListener, public IFindWndListener
{
  public:
    CMainFrame();
    ~CMainFrame();

    // IHexViewListener
    void OnHexViewCaretChanged(CHexView *pView) override;
    void OnHexViewModifiedChanged(CHexView *pView) override;
    void OnHexViewCommand(CHexView *pView, int nCmd) override;

    // IFindWndListener（查找/替换/转到 浮窗）
    void OnFindWndFindNext() override;
    void OnFindWndFindPrev() override;
    void OnFindWndReplace() override;
    void OnFindWndReplaceAll() override;
    void OnFindWndHexToggle() override;
    void OnFindWndGoto() override;
    void OnFindWndClose() override;

    BOOL OpenFile(const TCHAR *pszPath);
    /** 追加一个命令行文件（构造后、Create 前调用；只写固定缓冲，见 cpp 实现注释）。
     *  显式宽字符：m_arCmdFiles 是 wchar_t[][512]、模型层路径也是 wchar_t，
     *  与 TCHAR 宏解耦，Windows/非 Win 一致。 */
    void AddCmdFile(const wchar_t *pszPath);
    void UpdateTitle();

  protected:
    BOOL OnInitDialog(HWND hWnd, LPARAM lParam);
    void OnClose();
    void OnSize(UINT nType, CSize size);
    void OnCommand(UINT uNotifyCode, int nID, HWND wndCtl);

    // ---- 滚轮：宿主层兜底分发（绕过 SOUI 内部滚轮路由的不可达问题） ----
    BOOL OnHostMouseWheel(UINT nFlags, short zDelta, CPoint pt);

    // ---- 大 bpr 延迟刷新（CHexView::SetBytesPerRow 投递） ----
    LRESULT OnBprDeferredRefresh(UINT uMsg, WPARAM wParam, LPARAM lParam);
    // ---- 大 bpr 延迟 UI 同步（ApplyBprValue 投递：按钮文本/状态栏） ----
    LRESULT OnBprDeferredSyncUi(UINT uMsg, WPARAM wParam, LPARAM lParam);
    // ---- 标签条延迟按钮重算（容器显示递归激活子窗口后，布局收敛再校正显隐） ----
    LRESULT OnTabDeferredBtnUpdate(UINT uMsg, WPARAM wParam, LPARAM lParam);
    // ---- 窗口缩放后的延迟重排（标签条子项 + 停靠检查器；须等 SOUI 布局收敛） ----
    LRESULT OnDeferredInspectorLayout(UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT OnDeferredLangRefresh(UINT uMsg, WPARAM wParam, LPARAM lParam);

    // ---- 数据检查器独立悬浮窗口（可停靠：dock/floating 双态） ----
    void EnsureInspectorWnd();  // 惰性创建 SHostWnd（XML_INSPECTOR 布局）
    // ---- 查找/替换/转到 独立非模态浮窗（XML_FIND） ----
    void EnsureFindWnd();
    void ShowFindWnd(int nMode); // 0=查找 1=替换 2=转到
    void EnsureAboutWnd();       // 惰性创建 关于窗口（XML_ABOUT）
    void ShowInspectorWnd(BOOL bShow); // 显示/隐藏（显示时按当前 dock 状态定位）
    void DockInspectorWnd();           // 悬浮 → 停靠：SetParent(主窗) + 定位右缘
    void UndockInspectorWnd();         // 停靠 → 悬浮：SetParent(NULL) + WS_POPUP
    void SyncInspRowFocus();   // 检查器值行焦点 → hex 列框选联动（宽度映射）
    void OnInspRowSetFocus(int nWidth); // 值行获得焦点：hex 列框选 nWidth 字节
    void OnInspRowKillFocus();          // 值行失焦：hex 列恢复单点
    struct InspRowBinding
    {
        SWindow *pEdit;
        int nWidth; // 该行对应的数据宽度（字节）：1/2/4/8
    };
    std::vector<InspRowBinding> m_arInspRows;
    bool IsInspectorDocked() const
    {
        // popup 的 GetParent 返回 owner（也是主窗口）——必须按 WS_CHILD 样式判定
        if (!m_pInspWnd || !::IsWindow(m_pInspWnd->m_hWnd))
            return false;
        LONG_PTR dwStyle = ::GetWindowLongPtrW(m_pInspWnd->m_hWnd, GWL_STYLE);
        return (dwStyle & WS_CHILD) != 0 && ::GetParent(m_pInspWnd->m_hWnd) == m_hWnd;
    }
    void LayoutInspectorDock(); // docked 态下贴编辑区指定边缘
    void OnInspWndMove();       // 检查器窗口移动结束：检测是否贴近编辑区边缘 → dock
    void OnInspWndResized();    // 用户拉伸结束：记住尺寸（dock 布局沿用用户尺寸）
    int rcMain2Right();         // 主窗口 mainarea 右缘（宿主客户区系）
    int m_nInspDockEdge;         // 停靠边：0=左 1=右 2=上 3=下（默认右）
    int m_cxInspUser;             // 用户拉伸后的宽（左右停靠用）；默认 270
    int m_cyInspUser;             // 用户拉伸后的高（上下停靠用）；默认 430
    RECT m_rcInspFloat;          // 上次悬浮位置（dock 时记忆，拖出恢复）
    bool m_bInspFloatSaved;      // 是否记忆了悬浮位置
    CPoint m_ptInspDragOffset;   // 拖出瞬间的光标-窗口偏移（保持标题栏跟随光标）
    bool m_bInspDragOffsetValid;
    bool m_bInspJustUndocked;    // 刚从停靠拖出：下一次松手跳过 dock 检测
                                  // （防止原地松手立即被吸回）

    // ---- 文件拖放（IDropTarget，OLE 方式） ----
    class CFileDropTarget;
    friend class CFileDropTarget;
    void OnDropFile(const TCHAR *pszPath);
    BOOL RegisterFileDrop();

    // ---- 标题栏按钮 ----
    void OnMinimize();
    void OnMaximize();
    void OnRestore();
    // 拦截 WM_SYSCOMMAND 的 SC_MAXIMIZE/SC_RESTORE，改走手动最大化（不进原生 zoom 态），
    // 避免无边框客户区被 ±frame 推到任务栏后；其余（SC_MINIMIZE/SC_CLOSE 等）交基类。
    // 双击标题栏（SCaption 控件）也走这里：它发的是 SC_MAXIMIZE|HTCAPTION，
    // 且靠 WS_MAXIMIZE 样式位判断状态（我们手动最大化不设该位），故恒发"最大化"。
    // 这里按 m_bMaximized 做 toggle，使双击与"最大化/还原"按钮完全等价。
    void OnSysCommand(UINT nID, CPoint lParam);
    void OnCloseBtn();

    // ---- 工具栏 / 菜单动作 ----
    void OnActOpen();
    void OnActSave();
    void OnActSaveAs();
    void OnActNew();
    void OnActFind();
    void OnActFindNext();
    void OnActFindPrev();
    void OnActGoto();
    void OnActUndo();
    void OnActRedo();
    void OnActCut();
    void OnActCopy();
    void OnActPaste();
    void OnActDelete();
    void OnActSelectAll();
    void OnActToggleInsertMode();
    void OnActToggleInspector();
    void OnActCloseInspector(); // 检查器右上角 × 按钮
    void OnActToggleStatusBar();
    void OnActToggleToolbar();
    void OnActSetBpr(int nBpr);
    void OnActAbout();
    void OnActKeys();
    void OnActNotImplemented(const TCHAR *pszName);
    // ---- 新增：文件属性 / 校验和 / 只读 / 插入字节 / 填充 / 字符集 / 替换 ----
    void OnActFileProperties();
    void OnActChecksum();
    void OnActInsertByte();
    void OnActFillSelection();
    void OnActToggleReadOnly();
    void OnActToggleFastSave();
    void OnActCharset();
    // ---- 主题（浅色 / 深色）----
    void OnActSetTheme(int nTheme);
    void OnActLanguage(int nLang);
    void ApplyTheme();        // 十六进制区 + 窗口外框
    void ApplyChromeTheme();  // 只刷窗口外框
    void ApplyMenuTheme(SMenuEx *pMenu); // 把当前配色下发给一个下拉菜单（含子菜单）
    void ApplyMenuThemeAll(); // 所有主菜单下拉
    void LoadTheme();
    void SaveTheme();
    void OnActReplace();
    void OnBtnReplace();
    void OnBtnReplaceAll();
    void OnCharsetSelChanged(IEvtArgs *pEvt);
    void OnBprSelChanged(IEvtArgs *pEvt);      // 下拉列表选择
    void OnBprEditChanged(IEvtArgs *pEvt);      // 可编辑：输入变化
    void OnBprEditKillFocus(IEvtArgs *pEvt);   // 可编辑：失焦提交
    void SyncBprCombo(int nBpr);               // 组合框文本与视图值同步
    void OnTabSelChanged(IEvtArgs *pEvt);      // 文件标签切换
    void OnTabCtxMenu(IEvtArgs *pEvt);         // 文件标签右键菜单
    void OnTabScrollLeft();                    // 标签条向左滚动
    void OnTabScrollRight();                   // 标签条向右滚动
    void OnTabListFiles();                      // ▾ 按钮：弹出打开文件菜单（点击切换）
    void UpdateTabScrollButtons();             // 标签集合变化后维持 tabPos 合法
    void EnsureTabVisible(int nIndex);         // 自动滚动标签条使指定标签完整可见
    int TabStripContentWidth() const;           // 标签条内容自然总宽（未裁剪测量）
    void LayoutTabStrip();                      // 手动布局标签条四个子窗口
    void SnapStatusBar();                        // 状态栏钉回主窗口最底部
    bool ApplyBprText(const SStringT &str, bool bCommit); // 解析并应用每行字节数

    // ---- 查找 / 转到栏 ----
    void OnBtnFindNext();
    void OnBtnFindPrev();
    void OnBtnFindHex();
    void OnBtnFindClose();
    void OnBtnGotoOk();
    void OnBtnGotoClose();

    void OnTimer(UINT_PTR nIDEvent);

    void OnPopMenu(IEvtArgs *pEvt);   // EventPopMenu：菜单即将弹出
    void OnMenuSelected(IEvtArgs *pEvt); // EventSelectMenu：菜单项被选中

    void ShowFindBar(BOOL bShow);
    void ShowGotoBar(BOOL bShow);
    void RunSearch(bool bBackward);
    /** 同步查找（替换用）：阻塞等待后台检索结果 */
    bool FindSync(const std::vector<hexmodel::u8> &pat, hexmodel::u64 from, bool bBackward,
                  hexmodel::u64 &pos);
    void FinishSearch();
    bool ParsePattern(std::vector<u8> &out);
    void UpdateStatus();
    void UpdateInspector();
    void UpdateUndoButtons();
    void UpdateCaptionButtons();
    void UpdateMenuStates(); // 菜单弹出前刷新勾选/禁用状态
    void SyncModeUi();
    void Notify(const TCHAR *pszMsg);

    // 适配 HexFileModel 到 IHexSource
    class ModelSource : public hexsearch::IHexSource
    {
      public:
        ModelSource(hexmodel::HexFileModel *p)
            : m_p(p)
        {
        }
        void Rebind(hexmodel::HexFileModel *p)
        {
            m_p = p;
        }
        u64 GetSize() const override
        {
            return m_p->Size();
        }
        u64 Read(u64 pos, u8 *buf, u64 len) const override
        {
            return m_p->Read(pos, buf, len);
        }

      private:
        hexmodel::HexFileModel *m_p;
    };

    EVENT_MAP_BEGIN()
    // 标题栏
    EVENT_NAME_COMMAND(L"btn_min", OnMinimize)
    EVENT_NAME_COMMAND(L"btn_max", OnMaximize)
    EVENT_NAME_COMMAND(L"btn_restore", OnRestore)
    EVENT_NAME_COMMAND(L"btn_close", OnCloseBtn)
    // 菜单弹出前刷新勾选/禁用
    EVENT_NAME_HANDLER(L"menubar", EventPopMenu::EventID, OnPopMenu)
    // 菜单选择走 EventSelectMenu → OnCommand
    EVENT_NAME_HANDLER(L"menubar", EventSelectMenu::EventID, OnMenuSelected)
    // 工具栏
    EVENT_NAME_COMMAND(L"tb_new", OnActNew)
    EVENT_NAME_COMMAND(L"tb_open", OnActOpen)
    EVENT_NAME_COMMAND(L"tb_save", OnActSave)
    EVENT_NAME_COMMAND(L"tb_undo", OnActUndo)
    EVENT_NAME_COMMAND(L"tb_redo", OnActRedo)
    EVENT_NAME_COMMAND(L"tb_cut", OnActCut)
    EVENT_NAME_COMMAND(L"tb_copy", OnActCopy)
    EVENT_NAME_COMMAND(L"tb_paste", OnActPaste)
    EVENT_NAME_COMMAND(L"tb_delete", OnActDelete)
    EVENT_NAME_COMMAND(L"tb_find", OnActFind)
    EVENT_NAME_COMMAND(L"tb_goto", OnActGoto)
    EVENT_NAME_COMMAND(L"tb_mode", OnActToggleInsertMode)
    // 查找 / 转到栏
    EVENT_NAME_COMMAND(L"btn_find_next", OnBtnFindNext)
    EVENT_NAME_COMMAND(L"btn_find_prev", OnBtnFindPrev)
    EVENT_NAME_COMMAND(L"btn_find_hex", OnBtnFindHex)
    EVENT_NAME_COMMAND(L"btn_find_close", OnBtnFindClose)
    EVENT_NAME_COMMAND(L"btn_goto_ok", OnBtnGotoOk)
    EVENT_NAME_COMMAND(L"btn_goto_close", OnBtnGotoClose)
    // 替换栏
    EVENT_NAME_COMMAND(L"btn_replace", OnBtnReplace)
    EVENT_NAME_COMMAND(L"btn_replace_all", OnBtnReplaceAll)
    // 每行字节数：可编辑组合框（1..4096；大值走延迟刷新避免重入）
    EVENT_NAME_HANDLER(L"cbx_bpr", EventCBSelChange::EventID, OnBprSelChanged)
    EVENT_NAME_HANDLER(L"cbx_bpr", EventRENotify::EventID, OnBprEditChanged)
    EVENT_NAME_HANDLER(L"cbx_bpr", EventKillFocus::EventID, OnBprEditKillFocus)
    // 字符集下拉
    EVENT_NAME_HANDLER(L"cbx_charset", EventCBSelChange::EventID, OnCharsetSelChanged)
    // 文件标签页：切换 / 右键菜单 / 滚动按钮
    EVENT_NAME_HANDLER(L"tab_files", EventTabSelChanged::EventID, OnTabSelChanged)
    EVENT_NAME_HANDLER(L"tab_files", EventCtxMenu::EventID, OnTabCtxMenu)
    EVENT_NAME_COMMAND(L"tab_scroll_left", OnTabScrollLeft)
    EVENT_NAME_COMMAND(L"tab_scroll_right", OnTabScrollRight)
    EVENT_NAME_COMMAND(L"tab_list_files", OnTabListFiles)
    // 数据检查器关闭按钮（重新打开走 查看 菜单的 IDM_VIEW_INSPECTOR）
    EVENT_NAME_COMMAND(L"insp_close", OnActCloseInspector)
    EVENT_MAP_END2(SHostWnd)

    BEGIN_MSG_MAP_EX(CMainFrame)
    MSG_WM_INITDIALOG(OnInitDialog)
    MSG_WM_CLOSE(OnClose)
    MSG_WM_SIZE(OnSize)
    MSG_WM_TIMER(OnTimer)
    MSG_WM_COMMAND(OnCommand)
    MSG_WM_MOUSEWHEEL(OnHostMouseWheel)
    MSG_WM_SYSCOMMAND(OnSysCommand)
    MESSAGE_HANDLER_EX(WM_APP + 0x31, OnBprDeferredRefresh)
    MESSAGE_HANDLER_EX(WM_APP + 0x32, OnBprDeferredSyncUi)
    MESSAGE_HANDLER_EX(WM_APP + 0x33, OnTabDeferredBtnUpdate)
    MESSAGE_HANDLER_EX(WM_APP + 0x34, OnDeferredInspectorLayout)
    MESSAGE_HANDLER_EX(WM_APP + 0x35, OnDeferredLangRefresh)
    MESSAGE_HANDLER_EX(WM_APP + 0x36, OnDeferredFixMaximize)
    CHAIN_MSG_MAP(SHostWnd)
    REFLECT_NOTIFICATIONS_EX()
    END_MSG_MAP()

  private:
    // ---- 多文件标签页（HxD 风格） ----
    struct DocTab
    {
        hexmodel::HexFileModel *pModel; // 该标签的文件模型
        int nBpr;                      // 每行字节数（每标签独立记忆）
        u64 caret;                    // 光标位置
        u64 selStart, selEnd;         // 选区（半开区间）
        SStringT title;               // 标签标题（文件名）
        DocTab()
            : pModel(NULL)
            , nBpr(16)
            , caret(0)
            , selStart(0)
            , selEnd(0)
        {
        }
    };
    std::vector<DocTab> m_tabs; // 标签页数组（与 tabctrl 的页一一对应）
    int m_nCurTab;              // 当前激活标签索引（-1 无）
    int m_nTheme;               // 当前配色（Theme::LIGHT / Theme::DARK）
    ModelSource m_source;        // 查找用源适配（重绑到当前模型）
    hexsearch::HexSearch m_search;

    // 自管"最大化"状态：本窗口无边框（客户区==窗口矩形），若走原生最大化
    // （ShowWindow(SW_MAXIMIZE)/WM_SYSCOMMAND SC_MAXIMIZE），Windows 会把
    // 带 WS_THICKFRAME 的窗口放到"工作区 ±frame"，使客户区溢出任务栏。因此
    // 最大化改为手动：直接把窗口矩形设为工作区，并不进入原生 zoom 态。
    bool m_bMaximized = false;  // 是否处于（手动）最大化（Create 期间 WM_SIZE 早于 OnInitDialog，必须就地初始化）
    CRect m_rcRestore;          // 最大化前的矩形，用于还原
    void MaximizeToWorkArea();  // 把窗口矩形设为当前显示器工作区
    LRESULT OnDeferredFixMaximize(UINT, WPARAM, LPARAM); // 延迟把"原生最大化"纠正为手动最大化

    /** 当前标签的模型；无标签时返回一个静态空模型（IsOpen=false），
        使既有 m_model 引用点（约 50 处）无需判空 */
    hexmodel::HexFileModel *CurModel();
    hexmodel::HexFileModel *m_pEmptyModel; // 无标签时的占位模型

    DocTab *CurTab()
    {
        return (m_nCurTab >= 0 && m_nCurTab < (int)m_tabs.size()) ? &m_tabs[m_nCurTab] : NULL;
    }
    /** 把视图绑定到指定标签的模型并恢复其 UI 状态 */
    void BindViewToTab(int nIndex);
    /** 有 2+ 标签时显示标签条；1 个隐藏（回到无标签外观） */
    void ShowTabStrip();
    void ActivateTab(int nIndex);   // 切换到指定标签（保存/恢复视图状态）
    void CloseTab(int nIndex);     // 关闭标签（含保存询问）
    void SyncTabTitle(int nIndex);  // 同步标签标题（修改标记等）
    /** 每行字节数变化时写入当前标签记忆 */
    void RememberBpr(int nBpr);

    CHexView *m_pHex;
    SWindow *m_pFindBar;
    SWindow *m_pGotoBar;
    SWindow *m_pToolbar;
    SWindow *m_pStatusBar;
    CInspectorWnd *m_pInspWnd; // 数据检查器独立悬浮窗口（取代内嵌面板）
    CFindWnd *m_pFindWnd;      // 查找/替换/转到 独立浮窗
    CAboutWnd *m_pAboutWnd;    // 关于窗口 独立浮窗
    SWindow *m_pEditFind;
    SEdit *m_pEditGoto;
    SStatic *m_pTxtTitle;
    SStatic *m_pTxtSize;
    SStatic *m_pTxtOffset;
    SStatic *m_pTxtSel;
    SStatic *m_pTxtMode;
    SStatic *m_pTxtFile;
    SWindow *m_pTxtFileInsp; // 检查器悬浮窗口内的文件名控件（类型无关）
    SComboBox *m_pCbxBpr;  // 每行字节数可编辑组合框
    SComboBox *m_pCbxCharset; // 字符集下拉（ANSI/UTF-8/UTF-16LE）
    SEdit *m_pEditReplace; // 替换栏输入框
    STabCtrl *m_pTabFiles; // 文件标签控件（实际为 STabCtrlEx，XML tagctrl_ex）
    STabCtrlEx *m_pTabFilesEx; // 同对象的派生接口（访问 GetItemRectPublic）
    SWindow *m_pTabBarWrap;   // 标签条容器（含滚动按钮）
    SWindow *m_pTabScrollL;   // 左滚动按钮
    SWindow *m_pTabScrollR;   // 右滚动按钮
    int m_nTabPos;             // 标签条当前偏移（px，0=最左，负=向左滚出）
    // 检查器值行控件：XML 里是 SEdit（可选可复制），SetWindowText 是
    // SWindow 通用方法——用基类指针避免类型查找失败（历史 bug：改成 edit 后
    // FindChildByName2<SStatic> 全部返回 NULL，检查器值不再更新）
    SWindow *m_pValByte, *m_pValWord, *m_pValDword, *m_pValQword, *m_pValInt8, *m_pValInt16,
        *m_pValInt32, *m_pValInt64, *m_pValFloat, *m_pValDouble, *m_pValChar;

    bool m_bSearchHex;   // true=十六进制查找
    bool m_bHasLastFind; // 是否已有一次查找（用于 F3）
    std::vector<u8> m_lastPattern;
    bool m_bClosing;
    bool m_bInBprSync;   // 每行字节数组合框同步防重入
    bool m_bInCharsetSync; // 字符集组合框同步防重入
    bool m_bFastSaveLarge; // 大文件快速保存（只写脏页）开关，全局设置
    bool m_bInTabSync;   // 标签事件处理防重入（程序化 SetCurSel 不再触发切换逻辑）
    // 命令行文件（构造后、Create 前只能安全写普通缓冲；支持多个）
    static const int kMaxCmdFiles = 16;
    wchar_t m_arCmdFiles[kMaxCmdFiles][512];
    int m_nCmdFileCount;
};

#endif // MAIN_FRAME_H
