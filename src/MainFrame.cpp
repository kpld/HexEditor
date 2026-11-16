//
// CMainFrame - 主窗口实现
//

#include "MainFrame.h"
#include "resource.h"
#include "I18n.h"
#include "CrtCompat.h" // 非 Win 平台补齐 _wcstoi64 等 MSVC 专有 CRT（Win 下为空）
#ifndef _WIN32
#include "PromptWnd.h" // 非 Win 用 SOUI 自绘输入框（替代原生 CreateWindowExW 输入框）
#endif

#include <algorithm> // std::min/std::max（Windows 下由 windows.h 提供 min/max 宏，非 Win 需显式）
using std::max;
using std::min;

#include <control/SMenuBar.h>
#include <helper/SMenu.h>
#include <helper/SMenuEx.h>

#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdint.h>
// shobjidl.h（IFileDialog / COM 文件对话框）只有 Windows 的 PickFile 实现用到，
// swinx 不提供该头；非 Win 的 PickFile 走 commdlg 的 GetOpenFileNameW/GetSaveFileNameW。
#ifdef _WIN32
#include <shobjidl.h>
#endif

namespace
{
// COLORREF -> SOUI 属性用的 "#RRGGBB"
SStringW ColorToAttr(COLORREF cr)
{
    SStringW str;
    str.Format(L"#%02X%02X%02X", GetRValue(cr), GetGValue(cr), GetBValue(cr));
    return str;
}

// 每行字节数取值范围：与 CHexView::MAX_BPR、m_rowCache[4096] 保持一致。
const int kMinBpr = 1;
const int kMaxBpr = 4096;

// 解析每行字节数输入。必须用 64 位读取：swscanf(L"%d") 在超出 int 范围时
// 会回绕（实测 UCRT：2147483648 -> INT_MIN、4294967296 -> 0、
// 99999999999999999999 -> -1），再按 1..4096 钳制会误变成 1 而不是 4096。
// _wcstoi64 对超 long long 的输入饱和到 LLONG_MAX/MIN，钳制结果稳定。
// 返回 false 表示一个数字都没解析到（空串或纯非法字符）。
bool ParseBprRaw(const wchar_t *s, long long &n64)
{
    n64 = 0;
    if (!s || !*s)
        return false;
    wchar_t *end = NULL;
    n64 = _wcstoi64(s, &end, 10);
    return end != s; // 尾部多余字符按宽容处理，与原 %d 行为一致
}

// 任何 >4096 的输入（含超过 int 范围的超大数）统一变成 4096；<1 变成 1。
int ClampBpr(long long n)
{
    if (n < kMinBpr)
        return kMinBpr;
    if (n > kMaxBpr)
        return kMaxBpr;
    return (int)n;
}
} // namespace

#include <stdio.h>
#include <stdlib.h> // _wcstoi64

#ifdef _WIN32
#include <bcrypt.h> // Windows CNG：MD5/SHA-1/SHA-256（HashRange 的 Windows 实现）
#else
#include <openssl/evp.h> // 非 Windows 用 OpenSSL EVP 计算同样的三种哈希
#endif

#include <souistd.h>

#ifdef _WIN32
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "bcrypt.lib")
#endif

#define TIMER_SEARCH 5000

// ---------------------------------------------------------------------------
// 通用小工具：单行文本输入框（Win32 模态弹窗，无 .rc 资源依赖）
// 用于「插入字节数量」「填充值」「替换为」等需要输入的菜单动作。
// ---------------------------------------------------------------------------
namespace
{
// 该输入框用 CreateWindowExW 建原生 STATIC/EDIT/BUTTON 控件。swinx 在 Linux/macOS
// 明确不支持原生控件的系统绘制、也缺 IsDialogMessage 等 W 后缀消息 API，所以这段
// 仅在 Windows 编译；非 Windows 用下面的 PromptText 降级桩（返回 false）。
#ifdef _WIN32
struct PromptCtx
{
    std::wstring label;
    std::wstring text;
    bool ok;
    HWND hEdit;
};

LRESULT CALLBACK PromptProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    PromptCtx *ctx = (PromptCtx *)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    switch (msg)
    {
    case WM_CREATE:
    {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lParam;
        ctx = (PromptCtx *)cs->lpCreateParams;
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)ctx);
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HWND hLabel = CreateWindowExW(0, L"STATIC", ctx->label.c_str(),
                                      WS_CHILD | WS_VISIBLE | SS_LEFT, 12, 12, 390, 20, hWnd,
                                      NULL, NULL, NULL);
        ctx->hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", ctx->text.c_str(),
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 12, 36,
                                     390, 24, hWnd, (HMENU)1001, NULL, NULL);
        HWND hOk = CreateWindowExW(0, L"BUTTON", I18n::T(L"确定"),
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 252, 72,
                                   72, 26, hWnd, (HMENU)IDOK, NULL, NULL);
        HWND hCancel = CreateWindowExW(0, L"BUTTON", I18n::T(L"取消"),
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP, 330, 72, 72, 26, hWnd,
                                       (HMENU)IDCANCEL, NULL, NULL);
        SendMessageW(hLabel, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(ctx->hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hOk, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hCancel, WM_SETFONT, (WPARAM)hFont, TRUE);
        SetFocus(ctx->hEdit);
        SendMessageW(ctx->hEdit, EM_SETSEL, 0, -1);
        return 0;
    }
    case WM_COMMAND:
        if (ctx)
        {
            if (LOWORD(wParam) == IDOK)
            {
                wchar_t buf[512] = {0};
                GetWindowTextW(ctx->hEdit, buf, 511);
                ctx->text = buf;
                ctx->ok = true;
                DestroyWindow(hWnd);
                return 0;
            }
            if (LOWORD(wParam) == IDCANCEL)
            {
                ctx->ok = false;
                DestroyWindow(hWnd);
                return 0;
            }
        }
        break;
    case WM_CLOSE:
        if (ctx)
            ctx->ok = false;
        DestroyWindow(hWnd);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

bool PromptText(HWND hParent, const wchar_t *title, const wchar_t *label, const wchar_t *def,
                std::wstring &out)
{
    static bool s_reg = false;
    if (!s_reg)
    {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = PromptProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"HexEditorPromptWnd";
        RegisterClassExW(&wc);
        s_reg = true;
    }
    PromptCtx ctx;
    ctx.label = label ? label : L"";
    ctx.text = def ? def : L"";
    ctx.ok = false;
    ctx.hEdit = NULL;
    const int w = 420, h = 140;
    RECT rc = {0, 0, 0, 0};
    if (hParent)
        GetWindowRect(hParent, &rc);
    int x = rc.left + ((rc.right - rc.left) - w) / 2;
    int y = rc.top + ((rc.bottom - rc.top) - h) / 2;
    HWND hWnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"HexEditorPromptWnd",
                                title ? title : L"", WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w,
                                h, hParent, NULL, GetModuleHandleW(NULL), &ctx);
    if (!hWnd)
        return false;
    if (hParent)
        EnableWindow(hParent, FALSE);
    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);
    MSG msg;
    while (IsWindow(hWnd) && GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        if (!IsDialogMessageW(hWnd, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (hParent)
        EnableWindow(hParent, TRUE);
    if (!ctx.ok)
        return false;
    out = ctx.text;
    return true;
}
#else
// 非 Windows：swinx 不支持原生控件系统绘制，改用 SOUI 自绘模态输入框
//（CPromptWnd : SHostDialog），恢复「插入字节数 / 填充选区」两个功能。
bool PromptText(HWND hParent, const wchar_t *title, const wchar_t *label, const wchar_t *def,
                std::wstring &out)
{
    CPromptWnd dlg(title, label, def);
    if (dlg.DoModal(hParent) != IDOK)
        return false;
    out = dlg.Value();
    return true;
}
#endif // _WIN32

// ---- CRC32 ----
uint32_t g_crcTable[256];
bool g_crcReady = false;
void EnsureCrcTable()
{
    if (g_crcReady)
        return;
    for (uint32_t i = 0; i < 256; i++)
    {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crcTable[i] = c;
    }
    g_crcReady = true;
}
uint32_t Crc32Update(uint32_t crc, const hexmodel::u8 *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        crc = g_crcTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

SStringT ToHex(const std::vector<hexmodel::u8> &v)
{
    SStringT s;
    wchar_t b[4];
    for (size_t i = 0; i < v.size(); i++)
    {
        swprintf(b, 3, L"%02X", v[i]);
        s += b;
    }
    return s;
}

// 解析文本/十六进制字节串（与 CMainFrame::ParsePattern 同规则）
bool ParseByteText(const SStringT &str, bool bHex, std::vector<hexmodel::u8> &out)
{
    out.clear();
    if (bHex)
    {
        SStringA a = S_CT2A(str, CP_UTF8);
        int byte = -1;
        for (int i = 0; i < a.GetLength(); i++)
        {
            char ch = a[i];
            int v = -1;
            if (ch >= '0' && ch <= '9')
                v = ch - '0';
            else if (ch >= 'a' && ch <= 'f')
                v = 10 + ch - 'a';
            else if (ch >= 'A' && ch <= 'F')
                v = 10 + ch - 'A';
            if (v < 0)
            {
                if (ch == ' ' || ch == '\t')
                    continue;
                return false;
            }
            if (byte < 0)
                byte = v;
            else
            {
                out.push_back((hexmodel::u8)((byte << 4) | v));
                byte = -1;
            }
        }
        if (byte >= 0)
            return false;
    }
    else
    {
        SStringA a = S_CT2A(str, CP_UTF8);
        if (a.IsEmpty())
            return false;
        out.assign((const hexmodel::u8 *)a.c_str(),
                   (const hexmodel::u8 *)a.c_str() + a.GetLength());
    }
    return !out.empty();
}

// 单遍读取计算 CRC32 / MD5 / SHA-1 / SHA-256
#ifdef _WIN32
// Windows：用系统 CNG(bcrypt) 流式哈希。
bool HashRange(hexmodel::HexFileModel *pM, u64 start, u64 len, uint32_t &crc,
               std::vector<hexmodel::u8> &md5, std::vector<hexmodel::u8> &sha1,
               std::vector<hexmodel::u8> &sha256)
{
    EnsureCrcTable();
    BCRYPT_ALG_HANDLE hAlg[3] = {NULL, NULL, NULL};
    BCRYPT_HASH_HANDLE hHash[3] = {NULL, NULL, NULL};
    std::vector<hexmodel::u8> obj[3];
    DWORD outLen[3] = {0, 0, 0};
    LPCWSTR algId[3] = {BCRYPT_MD5_ALGORITHM, BCRYPT_SHA1_ALGORITHM, BCRYPT_SHA256_ALGORITHM};

    bool ok = true;
    for (int i = 0; i < 3 && ok; i++)
    {
        DWORD cb = 0, objLen = 0;
        if (BCryptOpenAlgorithmProvider(&hAlg[i], algId[i], NULL, 0) < 0)
        {
            ok = false;
            break;
        }
        if (BCryptGetProperty(hAlg[i], BCRYPT_OBJECT_LENGTH, (PUCHAR)&objLen, sizeof(objLen), &cb,
                              0) < 0
            || BCryptGetProperty(hAlg[i], BCRYPT_HASH_LENGTH, (PUCHAR)&outLen[i],
                                 sizeof(outLen[i]), &cb, 0) < 0)
        {
            ok = false;
            break;
        }
        obj[i].resize(objLen);
        if (BCryptCreateHash(hAlg[i], &hHash[i], obj[i].empty() ? NULL : &obj[i][0], objLen, NULL,
                             0, 0) < 0)
        {
            ok = false;
            break;
        }
    }

    uint32_t c = 0xFFFFFFFFu;
    if (ok)
    {
        const size_t CHUNK = 1 << 20;
        std::vector<hexmodel::u8> buf(CHUNK);
        u64 pos = start, end = start + len;
        while (pos < end)
        {
            u64 want = end - pos;
            if (want > CHUNK)
                want = CHUNK;
            u64 got = pM->Read(pos, &buf[0], want);
            if (got == 0)
                break;
            c = Crc32Update(c, &buf[0], (size_t)got);
            for (int i = 0; i < 3; i++)
                BCryptHashData(hHash[i], &buf[0], (ULONG)got, 0);
            pos += got;
        }
    }
    if (ok)
    {
        md5.resize(outLen[0]);
        sha1.resize(outLen[1]);
        sha256.resize(outLen[2]);
        for (int i = 0; i < 3; i++)
        {
            std::vector<hexmodel::u8> &dst = (i == 0) ? md5 : (i == 1) ? sha1 : sha256;
            if (BCryptFinishHash(hHash[i], &dst[0], (ULONG)dst.size(), 0) < 0)
                ok = false;
        }
    }
    for (int i = 0; i < 3; i++)
    {
        if (hHash[i])
            BCryptDestroyHash(hHash[i]);
        if (hAlg[i])
            BCryptCloseAlgorithmProvider(hAlg[i], 0);
    }
    crc = c ^ 0xFFFFFFFFu;
    return ok;
}
#else
// 非 Windows：用 OpenSSL EVP 做同样的三种流式哈希；CRC32 仍用上面的自实现。
// 与 Windows 分支共享「单遍读取 + 1MB 分块」逻辑，结果逐字节一致。
bool HashRange(hexmodel::HexFileModel *pM, u64 start, u64 len, uint32_t &crc,
               std::vector<hexmodel::u8> &md5, std::vector<hexmodel::u8> &sha1,
               std::vector<hexmodel::u8> &sha256)
{
    EnsureCrcTable();
    const EVP_MD *algs[3] = {EVP_md5(), EVP_sha1(), EVP_sha256()};
    EVP_MD_CTX *ctx[3] = {NULL, NULL, NULL};
    bool ok = true;
    for (int i = 0; i < 3; i++)
    {
        ctx[i] = EVP_MD_CTX_new();
        if (!ctx[i] || EVP_DigestInit_ex(ctx[i], algs[i], NULL) != 1)
        {
            ok = false;
            break;
        }
    }

    uint32_t c = 0xFFFFFFFFu;
    if (ok)
    {
        const size_t CHUNK = 1 << 20;
        std::vector<hexmodel::u8> buf(CHUNK);
        u64 pos = start, end = start + len;
        while (pos < end)
        {
            u64 want = end - pos;
            if (want > CHUNK)
                want = CHUNK;
            u64 got = pM->Read(pos, &buf[0], want);
            if (got == 0)
                break;
            c = Crc32Update(c, &buf[0], (size_t)got);
            for (int i = 0; i < 3; i++)
            {
                if (EVP_DigestUpdate(ctx[i], &buf[0], (size_t)got) != 1)
                    ok = false;
            }
            if (!ok)
                break;
            pos += got;
        }
    }
    if (ok)
    {
        std::vector<hexmodel::u8> *dst[3] = {&md5, &sha1, &sha256};
        for (int i = 0; i < 3; i++)
        {
            unsigned int n = 0;
            hexmodel::u8 digest[EVP_MAX_MD_SIZE];
            if (EVP_DigestFinal_ex(ctx[i], digest, &n) != 1)
            {
                ok = false;
                break;
            }
            dst[i]->assign(digest, digest + n);
        }
    }
    for (int i = 0; i < 3; i++)
        if (ctx[i])
            EVP_MD_CTX_free(ctx[i]);
    crc = c ^ 0xFFFFFFFFu;
    return ok;
}
#endif // _WIN32
} // namespace

// ---------------------------------------------------------------------------
// 文件拖放（OLE IDropTarget，经 SOUI 的 SDropTargetDispatcher 派发）
// ---------------------------------------------------------------------------
class CMainFrame::CFileDropTarget : public IDropTarget
{
  public:
    explicit CFileDropTarget(CMainFrame *pFrame)
        : m_pFrame(pFrame)
        , m_nRef(1) // new 出来即持 1 个引用
        , m_bHasFile(false)
    {
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override
    {
        HRESULT hr = S_FALSE;
        if (riid == __uuidof(IUnknown))
            *ppvObject = (IUnknown *)this, hr = S_OK;
        else if (riid == __uuidof(IDropTarget))
            *ppvObject = (IDropTarget *)this, hr = S_OK;
        if (SUCCEEDED(hr))
            AddRef();
        return hr;
    }
    ULONG STDMETHODCALLTYPE AddRef(void) override
    {
        return ++m_nRef;
    }
    ULONG STDMETHODCALLTYPE Release(void) override
    {
        ULONG uRet = --m_nRef;
        if (uRet == 0)
            delete this; // 引用归零即销毁（各注册处 AddRef 持有）
        return uRet;
    }

    // IDropTarget
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *pDataObj, DWORD grfKeyState, POINTL pt,
                                       DWORD *pdwEffect) override
    {
        m_bHasFile = HasFileFormat(pDataObj);
        *pdwEffect = m_bHasFile ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD grfKeyState, POINTL pt, DWORD *pdwEffect) override
    {
        *pdwEffect = m_bHasFile ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave(void) override
    {
        m_bHasFile = false;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject *pDataObj, DWORD grfKeyState, POINTL pt,
                                  DWORD *pdwEffect) override
    {
        *pdwEffect = m_bHasFile ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        m_bHasFile = false;

        FORMATETC format = { CF_HDROP, 0, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM medium;
        if (FAILED(pDataObj->GetData(&format, &medium)))
            return S_FALSE;

        // OLE STGMEDIUM 的内存只由 ReleaseStgMedium 统一释放；
        // 这里不能调 DragFinish（那是 WM_DROPFILES 路径的 HDROP 语义，会释放
        // hGlobal 本体，随后 ReleaseStgMedium 再释放 → 双重释放 → 堆损坏崩溃）
        HDROP hdrop = (HDROP)GlobalLock(medium.hGlobal);
        if (!hdrop)
        {
            ReleaseStgMedium(&medium);
            return S_FALSE;
        }

        int nFiles = DragQueryFile(hdrop, -1, NULL, 0);
        if (nFiles > 0 && m_pFrame)
        {
            TCHAR filename[MAX_PATH * 2];
            // 打开第一个文件（多文件时忽略其余，与 HxD 单实例行为一致）
            if (DragQueryFile(hdrop, 0, filename, MAX_PATH * 2) > 0)
            {
                GlobalUnlock(medium.hGlobal);
                ReleaseStgMedium(&medium);
                m_pFrame->OnDropFile(filename); // 可能弹保存询问/重建视图，先释放 OLE 资源
                return S_OK;
            }
        }
        GlobalUnlock(medium.hGlobal);
        ReleaseStgMedium(&medium);
        return S_OK;
    }

  private:
    static bool HasFileFormat(IDataObject *pDataObj)
    {
        FORMATETC format = { CF_HDROP, 0, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        return pDataObj && SUCCEEDED(pDataObj->QueryGetData(&format));
    }

    CMainFrame *m_pFrame;
    ULONG m_nRef;
    bool m_bHasFile;
};

BOOL CMainFrame::RegisterFileDrop()
{
#ifdef _WIN32
    // 1) 宿主 HWND 注册 OLE 拖放（SDropTargetDispatcher 接收并按 SOUI 窗口树派发）
    EnableDragDrop();
    // 2) 派发器按「SwndFromPoint 命中的最深控件」查注册表，
    //    因此必须把目标注册到实际会接住鼠标的控件上，而不是注册到 root（root 几乎不会命中）。
    //    给主区域相关控件与 root 全部挂上同一目标，实现"任意位置可放"。
    IDropTarget *pDT = new CFileDropTarget(this);
    BOOL bOk = FALSE;
    if (m_pHex)
        bOk = m_pHex->RegisterDragDrop(pDT); // 十六进制主区域
    SWindow *pInspector = FindChildByName(L"inspector");
    if (pInspector)
    {
        pDT->AddRef();
        pInspector->RegisterDragDrop(pDT); // 数据检查器
    }
    {
        pDT->AddRef();
        GetRoot()->RegisterDragDrop(pDT); // 其余空白区域（命中 root 时）
    }
    pDT->Release(); // 各注册处已 AddRef，释放本地引用
    return bOk;
#else
    return FALSE; // 非 Windows 平台暂不支持拖放
#endif
}

void CMainFrame::OnDropFile(const TCHAR *pszPath)
{
    // 拖放打开与菜单打开同一入口（含"是否保存"询问）
    OpenFile(pszPath);
}

// ---------------------------------------------------------------------------
// Windows 文件对话框
// ---------------------------------------------------------------------------
#ifdef _WIN32
static BOOL PickFile(HWND hWnd, BOOL bSave, LPCTSTR pszDef, SStringT *pOut)
{
    IFileDialog *pDlg = NULL;
    HRESULT hr;
    if (bSave)
        hr = CoCreateInstance(CLSID_FileSaveDialog, NULL, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&pDlg));
    else
        hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&pDlg));
    if (FAILED(hr) || !pDlg)
        return FALSE;

    DWORD dwOpts = 0;
    pDlg->GetOptions(&dwOpts);
    pDlg->SetOptions(dwOpts | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
                     (bSave ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST));

    COMDLG_FILTERSPEC spec[] = {
        { I18n::T(L"所有文件"), L"*.*" },
    };
    pDlg->SetFileTypes(1, spec);
    if (pszDef && pszDef[0])
    {
        // 默认文件名只取文件名部分
        LPCTSTR p = _tcsrchr(pszDef, '\\');
        p = p ? p + 1 : pszDef;
        pDlg->SetFileName(p);
    }
    pDlg->SetDefaultExtension(L"bin");

    BOOL bOk = FALSE;
    if (SUCCEEDED(pDlg->Show(hWnd)))
    {
        IShellItem *pItem = NULL;
        if (SUCCEEDED(pDlg->GetResult(&pItem)))
        {
            LPWSTR pszPath = NULL;
            if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath)) && pszPath)
            {
                *pOut = pszPath;
                CoTaskMemFree(pszPath);
                bOk = TRUE;
            }
            pItem->Release();
        }
    }
    pDlg->Release();
    return bOk;
}
#else
// ---------------------------------------------------------------------------
// 非 Windows 文件对话框：走 swinx 提供的 GetOpenFileNameW / GetSaveFileNameW。
// Linux 下它依次尝试 xdg-desktop-portal（DBus）→ KDE/GNOME 门户 → zenity 命令行，
// 所以系统里至少要有一个可用的文件选择器：
//     sudo apt install zenity
// 三者都没有时对话框不会出现（函数返回 FALSE），打开/保存菜单表现为“点了没反应”。
// ---------------------------------------------------------------------------
static BOOL PickFile(HWND hWnd, BOOL bSave, LPCTSTR pszDef, SStringT *pOut)
{
    wchar_t szFile[1024] = { 0 };
    if (pszDef && pszDef[0])
    {
        // 只把文件名部分作为初始值：swinx 的 W 版包装不会把 lpstrFile 传给选择器，
        // 塞整条路径反而可能超过缓冲；目录交给 lpstrInitialDir。
        SStringT strDef = pszDef;
        int nSep = strDef.ReverseFind(L'/');
        int nSep2 = strDef.ReverseFind(L'\\');
        if (nSep2 > nSep)
            nSep = nSep2;
        SStringT strName = (nSep >= 0) ? strDef.Mid(nSep + 1) : strDef;
        for (int i = 0; i < strName.GetLength() && i < 1023; i++)
            szFile[i] = strName[i];
    }

    // lpstrFilter 要求 "显示名\0通配符\0\0" 这种双零结尾形式，不能用字符串字面量
    // 直接拼（C++ 字面量里的 \0 会被 wcslen 截断）。
    wchar_t szFilter[128] = { 0 };
    {
        SStringT strName = I18n::T(L"所有文件");
        int i = 0;
        for (; i < strName.GetLength() && i < 100; i++)
            szFilter[i] = strName[i];
        const wchar_t *pszPattern = L"*.*";
        for (int k = 0; pszPattern[k] && i + 1 + k < 126; k++)
            szFilter[i + 1 + k] = pszPattern[k];
    }

    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFilter = szFilter;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = 1024;
    // 起始位置：直接把当前文件的完整路径交给 lpstrInitialDir。
    // Linux 下 swinx 会把它原样传给 zenity 的 --filename（语义是"默认选中哪个文件或
    // 目录"），传完整文件路径正好让对话框定位到所在目录并把该文件预选上；
    // 传目录则只会定位。Windows 分支不用这段代码。
    ofn.lpstrInitialDir = (pszDef && pszDef[0]) ? pszDef : NULL;
    ofn.lpstrDefExt = L"bin";
    ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST |
                (bSave ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);

    BOOL bOk = bSave ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    if (!bOk || !szFile[0])
        return FALSE;
    *pOut = szFile;
    return TRUE;
}
#endif

CMainFrame::CMainFrame()
    : SHostWnd(L"LAYOUT:XML_MAINWND")
    , m_source(NULL)
{
    m_pHex = NULL;
    m_pFindBar = NULL;
    m_pGotoBar = NULL;
    m_pToolbar = NULL;
    m_pStatusBar = NULL;
    m_pInspWnd = NULL;
    m_pFindWnd = NULL;
    m_pAboutWnd = NULL;
    m_pTxtTitle = NULL;
    m_pCbxBpr = NULL;
    m_pCbxCharset = NULL;
    m_pEditReplace = NULL;
    m_pEditFind = NULL;
    m_pEditGoto = NULL;
    m_pTxtSize = NULL;
    m_pTxtOffset = NULL;
    m_pTxtSel = NULL;
    m_pTxtMode = NULL;
    m_pTxtFile = NULL;
    m_pValByte = m_pValWord = m_pValDword = m_pValQword = NULL;
    m_pValInt8 = m_pValInt16 = m_pValInt32 = m_pValInt64 = NULL;
    m_pValFloat = m_pValDouble = m_pValChar = NULL;
    m_bSearchHex = false;
    m_bHasLastFind = false;
    m_bClosing = false;
    m_bInBprSync = false;
    m_bInCharsetSync = false;
    m_bFastSaveLarge = true;
    m_bInTabSync = false;
    m_nCurTab = -1;
    m_nTheme = Theme::LIGHT;
    m_pTabFiles = NULL;
    m_pTabFilesEx = NULL;
    m_pTabBarWrap = NULL;
    m_pTabScrollL = NULL;
    m_pTabScrollR = NULL;
    m_nTabPos = 0;
    m_nCmdFileCount = 0;
    m_pTxtFileInsp = NULL;
    m_bInspFloatSaved = false;
    m_nInspDockEdge = 1; // 默认停靠右缘
    m_cxInspUser = 300;  // 默认厚度（用户拉伸后更新）。300 而非 270：
                         // Linux 的 DejaVu Sans Mono 比 Consolas 宽约 10%，
                         // 270 下 Float32/Double64 两行会被右边缘裁掉一位。
    m_cyInspUser = 430;
    m_bInspJustUndocked = false;
    memset(&m_rcInspFloat, 0, sizeof(m_rcInspFloat));
    m_bInspDragOffsetValid = false;
    m_pEmptyModel = NULL;
}

CMainFrame::~CMainFrame()
{
    // 清理所有标签模型（正常关闭路径已在 OnClose 清空；防御双重清理）
    for (size_t i = 0; i < m_tabs.size(); i++)
    {
        delete m_tabs[i].pModel;
        m_tabs[i].pModel = NULL;
    }
    delete m_pEmptyModel;
    m_pEmptyModel = NULL;
}

BOOL CMainFrame::OnInitDialog(HWND hWnd, LPARAM lParam)
{
    m_bMaximized = false;
    m_rcRestore.SetRectEmpty();
    m_pHex = FindChildByName2<CHexView>(L"hexview");
    m_pFindBar = FindChildByName2<SWindow>(L"findbar");
    m_pGotoBar = FindChildByName2<SWindow>(L"gotobar");
    m_pToolbar = FindChildByName2<SWindow>(L"toolbar");
    m_pStatusBar = FindChildByName2<SWindow>(L"statusbar");
    m_pTxtTitle = FindChildByName2<SStatic>(L"txt_title");
    m_pCbxBpr = FindChildByName2<SComboBox>(L"cbx_bpr");
    m_pCbxCharset = FindChildByName2<SComboBox>(L"cbx_charset");
    m_pEditReplace = FindChildByName2<SEdit>(L"edit_replace");
    m_pTabFilesEx = FindChildByName2<STabCtrlEx>(L"tab_files");
    m_pTabFiles = m_pTabFilesEx; // 基类指针复用（既有代码不区分）
    m_pTabBarWrap = FindChildByName2<SWindow>(L"tabbar_wrap");
    m_pTabScrollL = FindChildByName2<SWindow>(L"tab_scroll_left");
    m_pTabScrollR = FindChildByName2<SWindow>(L"tab_scroll_right");
    m_pEditFind = FindChildByName2<SEdit>(L"edit_find");
    m_pEditGoto = FindChildByName2<SEdit>(L"edit_goto");
    m_pTxtSize = FindChildByName2<SStatic>(L"txt_size");
    m_pTxtOffset = FindChildByName2<SStatic>(L"txt_offset");
    m_pTxtSel = FindChildByName2<SStatic>(L"txt_sel");
    m_pTxtMode = FindChildByName2<SStatic>(L"txt_mode");
    m_pTxtFile = FindChildByName2<SStatic>(L"txt_file");

    // 数据检查器窗口：创建并【初始停靠】在主窗口内（右缘）；
    // 拖标题栏拖出 → 悬浮；靠近主窗口边缘 → 自动 re-dock
    EnsureInspectorWnd();
    if (m_pInspWnd && ::IsWindow(m_pInspWnd->m_hWnd))
    {
        // 类型无关查找（SWindow 基类）：XML 值行已改为 SEdit（可选可复制），
        // SetWindowText 为 SWindow 通用方法，无需具体类型
        m_pValByte = m_pInspWnd->FindChildByName2<SWindow>(L"val_byte");
        m_pValWord = m_pInspWnd->FindChildByName2<SWindow>(L"val_word");
        m_pValDword = m_pInspWnd->FindChildByName2<SWindow>(L"val_dword");
        m_pValQword = m_pInspWnd->FindChildByName2<SWindow>(L"val_qword");
        m_pValInt8 = m_pInspWnd->FindChildByName2<SWindow>(L"val_int8");
        m_pValInt16 = m_pInspWnd->FindChildByName2<SWindow>(L"val_int16");
        m_pValInt32 = m_pInspWnd->FindChildByName2<SWindow>(L"val_int32");
        m_pValInt64 = m_pInspWnd->FindChildByName2<SWindow>(L"val_int64");
        m_pValFloat = m_pInspWnd->FindChildByName2<SWindow>(L"val_float");
        m_pValDouble = m_pInspWnd->FindChildByName2<SWindow>(L"val_double");
        m_pValChar = m_pInspWnd->FindChildByName2<SWindow>(L"val_char");
        m_pTxtFileInsp = m_pInspWnd->FindChildByName2<SWindow>(L"txt_file");
        DockInspectorWnd(); // 初始：嵌入主窗口右缘

        // 值行焦点 → hex 列框选联动：每行记录对应数据宽度，
        // 获得焦点时把 hexview 选区设为 [caret, caret+宽度)——
        // hex/ascii 列自动框选对应 1/2/4/8 个字节；失焦恢复单点。
        struct RowDef
        {
            LPCWSTR pszName;
            int nWidth;
        };
        static const RowDef rows[] = {
            { L"val_byte", 1 },   { L"val_word", 2 },   { L"val_dword", 4 },
            { L"val_qword", 8 },  { L"val_int8", 1 },   { L"val_int16", 2 },
            { L"val_int32", 4 },  { L"val_int64", 8 },  { L"val_float", 4 },
            { L"val_double", 8 }, { L"val_char", 1 },
        };
        for (int i = 0; i < _countof(rows); i++)
        {
            SWindow *pRow = m_pInspWnd->FindChildByName2<SWindow>(rows[i].pszName);
            if (!pRow)
                continue;
            InspRowBinding b;
            b.pEdit = pRow;
            b.nWidth = rows[i].nWidth;
            m_arInspRows.push_back(b);
            pRow->GetEventSet()->subscribeEvent(
                EventSetFocus::EventID,
                [this, nWidth = rows[i].nWidth](IEvtArgs *e) {
                    OnInspRowSetFocus(nWidth);
                    return true;
                });
            pRow->GetEventSet()->subscribeEvent(
                EventKillFocus::EventID,
                [this](IEvtArgs *e) {
                    OnInspRowKillFocus();
                    return true;
                });
        }
    }
    else
    {
        m_pValByte = m_pValWord = m_pValDword = m_pValQword = NULL;
        m_pValInt8 = m_pValInt16 = m_pValInt32 = m_pValInt64 = NULL;
        m_pValFloat = m_pValDouble = m_pValChar = NULL;
        m_pTxtFileInsp = NULL;
    }

    if (m_pHex)
    {
        m_pHex->SetListener(this);
        m_pHex->SetInsertMode(false);
    }
    m_pEmptyModel = new hexmodel::HexFileModel(); // 占位：无标签时 CurModel() 返回它
    m_source.Rebind(m_pEmptyModel);
    if (m_pHex)
        m_pHex->BindModel(m_pEmptyModel);

    RegisterFileDrop(); // 支持从资源管理器拖放文件进来打开

    UpdateStatus();
    UpdateInspector();
    UpdateTitle();
    UpdateCaptionButtons();
    SyncModeUi();
    SyncBprCombo(16);
    // 恢复上次的配色（浅色 / 深色），并立即应用到十六进制区和窗口外框
    LoadTheme();
    ApplyTheme();
    // 原生菜单是否画成暗色取决于「所有者窗口」是否被标记为暗色主题
    Theme::AllowDarkWindow(GetHwnd(), Theme::Get(m_nTheme).bDark);

    // 下拉菜单的配色已由 ApplyTheme -> ApplyChromeTheme -> ApplyMenuThemeAll 下发。

    PostMessage(WM_APP + 0x33, 0, 0); // 标签滚动按钮延迟重算（布局收敛后）

    // 命令行传入的文件：逐个打开（支持多个）
    for (int i = 0; i < m_nCmdFileCount; i++)
        OpenFile(m_arCmdFiles[i]);
    m_nCmdFileCount = 0;

    // 测试辅助：环境变量 HEX_BPR 设置初始每行字节数（绕开菜单时序做稳定性测试）
    {
        TCHAR buf[32] = { 0 };
        if (GetEnvironmentVariable(L"HEX_BPR", buf, 31) > 0 && m_pHex)
        {
            long long n64 = 0;
            if (ParseBprRaw(buf, n64))
            {
                m_pHex->SetBytesPerRow(ClampBpr(n64));
                // 显示视图实际采用的值：超限时组合框显示钳制后的 4096，
                // 不能显示输入的假值
                SyncBprCombo(m_pHex->GetBytesPerRow());
            }
        }
    }
    return TRUE;
}

void CMainFrame::OnClose()
{
    // 关闭前逐个处理未保存的标签（从后往前，任一取消则中止关闭）
    m_bClosing = true;
    while (!m_tabs.empty())
    {
        int nLast = (int)m_tabs.size() - 1;
        DocTab &t = m_tabs[nLast];
        if (t.pModel && t.pModel->IsOpen() && t.pModel->IsModified())
        {
            m_bClosing = false; // CloseTab 内可能弹询问框，需要窗口活着
            // 直接走 CloseTab（含保存询问）；用户取消时它返回但标签仍在
            int nBefore = (int)m_tabs.size();
            CloseTab(nLast);
            if ((int)m_tabs.size() == nBefore)
            {
                return; // 用户取消保存 → 中止关闭程序
            }
            m_bClosing = true;
        }
        else
        {
            CloseTab(nLast);
        }
    }
    m_bClosing = true;
    // 检查器悬浮窗口随主窗口销毁
    if (m_pInspWnd)
    {
        if (::IsWindow(m_pInspWnd->m_hWnd))
            m_pInspWnd->DestroyWindow();
        delete m_pInspWnd;
        m_pInspWnd = NULL;
    }
    delete m_pEmptyModel;
    m_pEmptyModel = NULL;
    SHostWnd::DestroyWindow();
}

void CMainFrame::OnSize(UINT nType, CSize size)
{
    // 防御：若窗口被"原生最大化"（外部 ShowWindow(SW_MAXIMIZE) / 系统手势 /
    // 漏网的双击），WS_THICKFRAME 会把客户区推到任务栏后。不要在 WM_SIZE 回调里同步
    // ShowWindow/SetWindowPos（重入易触发异常），改为投递延迟消息，回到消息循环后再
    // 捕获还原矩形、退出 zoom 态并转成手动最大化。
    // 注意：不能加 !m_bMaximized 条件——即使已处于手动最大化，系统仍可能把它强行
    // 拉进原生 zoom 态，必须照样纠正。OnDeferredFixMaximize 内部用 IsZoomed() 判定，
    // 已纠正（非 zoom）时直接返回，不会循环。
    if (nType == SIZE_MAXIMIZED)
        PostMessage(WM_APP + 0x36);
    UpdateCaptionButtons();
    // 注意：此刻 SOUI 还没按新尺寸重排（默认处理在 SetMsgHandled(FALSE) 之后才跑），
    // 所有窗口矩形都还是【旧】的。标签条子项和停靠检查器都依赖实际矩形：
    //   - 检查器按旧 mainarea 底边贴边 → 窗口变矮后压到状态栏上；
    //   - 检查器按旧右缘贴边 → 窗口变窄后跑到窗口外；
    //   - 标签条三个按钮按旧宽度定位 → 缩放后错位。
    // 所以这里只投递一条消息，等布局收敛后再统一重排。
    SetMsgHandled(FALSE);
    PostMessage(WM_APP + 0x34);
}

// 拦截 WM_SYSCOMMAND 的 SC_MAXIMIZE / SC_RESTORE，改走手动最大化（不进入原生
// zoom 态）。本窗口是无边框 SOUI 窗口，客户区 == 窗口矩形；若走原生最大化，Windows
// 会把带 WS_THICKFRAME 的窗口放到"工作区 ±frame"，使客户区（含底部状态栏）被任务栏
// 遮挡。改为手动把窗口矩形设为工作区即可彻底规避。SC_MINIMIZE / SC_CLOSE 等仍交基类。
void CMainFrame::OnSysCommand(UINT nID, CPoint lParam)
{
    // WM_SYSCOMMAND 的 wParam 低 4 位由系统保留：SCaption::OnLButtonDblClk 发的是
    // SC_MAXIMIZE|HTCAPTION / SC_RESTORE|HTCAPTION，必须用 0xFFF0 取命令码，
    // 否则精确比较会漏掉，命令落到基类的 DefWindowProc 走原生最大化。
    UINT nCmd = nID & 0xFFF0;

    if (nCmd == SC_MAXIMIZE)
    {
        // 标题栏是客户区里的 <caption> 控件（无边框窗口，WM_NCHITTEST 全为 HTCLIENT），
        // 双击由 SCaption 处理。它靠 WS_MAXIMIZE 样式位判断当前状态，而我们手动最大化
        // 刻意不进原生 zoom 态、不设该位 → 它每次都发 SC_MAXIMIZE，从不发 SC_RESTORE。
        // 因此这里按自管状态 toggle：已最大化则还原，使"双击标题栏"与"最大化/还原
        // 按钮"行为完全一致（按钮本身也是按 m_bMaximized 切换显示与动作）。
        if (m_bMaximized)
            OnRestore();
        else
            OnMaximize();
        return;
    }
    // 只在"手动最大化且非最小化"时接管还原。窗口处于最小化（IsIconic）时
    // SC_RESTORE 必须交回基类 DefWindowProc 原生还原——OnRestore() 只做
    // SetWindowPos，不会解除最小化状态，否则窗口无法从任务栏恢复。
    if (nCmd == SC_RESTORE && m_bMaximized && !::IsIconic(m_hWnd))
    {
        OnRestore();
        return;
    }
    SHostWnd::OnSysCommand(nID, lParam);
}

// 延迟纠正"原生最大化"：此时已回到消息循环，可安全 ShowWindow/SetWindowPos。
LRESULT CMainFrame::OnDeferredFixMaximize(UINT, WPARAM, LPARAM)
{
    // 只有确实处于原生 zoom 态才需要纠正。手动最大化状态下窗口不是 zoomed，
    // 这里直接返回即可终止循环（OnSize 无条件投递本消息，靠这个判定收敛）。
    if (!::IsZoomed(m_hWnd))
        return 0;

    // 仅在首次（还原态）进入时保存真正的还原矩形。若已处于手动最大化又被系统
    // 强行拉进 zoom 态，rcNormalPosition 会是工作区矩形，不能覆盖 m_rcRestore，
    // 否则之后"还原"会还原成满屏。
    if (!m_bMaximized)
    {
        WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
        if (::GetWindowPlacement(m_hWnd, &wp))
            m_rcRestore = wp.rcNormalPosition;
    }
    m_bMaximized = true;
    ::ShowWindow(m_hWnd, SW_RESTORE); // 退出原生 zoom 态（否则 SetWindowPos 被系统忽略）
    MaximizeToWorkArea();             // 此时非 zoom，定位到工作区才会生效
    UpdateCaptionButtons();
    return 0;
}

// 把窗口矩形设为当前显示器的工作区（手动"最大化"）。不走原生 zoom，因此不会有
// ±frame 把客户区推到任务栏后。
void CMainFrame::MaximizeToWorkArea()
{
    HMONITOR hMon = ::MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
    if (!hMon)
        return;
    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (!::GetMonitorInfo(hMon, &mi))
        return;
    ::SetWindowPos(m_hWnd, NULL, mi.rcWork.left, mi.rcWork.top,
                   mi.rcWork.right - mi.rcWork.left, mi.rcWork.bottom - mi.rcWork.top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
}

// 延迟重排：此时 SOUI 已按新尺寸完成布局，各种窗口矩形都是新的。
LRESULT CMainFrame::OnDeferredInspectorLayout(UINT, WPARAM, LPARAM)
{
    // 缩放后 SOUI 的 vbox 不会正确给 mainarea 让出状态栏（实测：mainarea 被
    // 撑到窗口底部 739，statusbar 仍停在旧宽度 1100，两者重叠）。这里手工
    // 把状态栏贴到底部全宽，mainarea 截止到状态栏上沿，再交给下面的
    // LayoutTabStrip / LayoutInspectorDock。
    CRect rcCli = GetClientRect();
    SWindow *pSB = FindChildByName2<SWindow>(L"statusbar");
    if (pSB && m_pHex)
    {
        CRect rsb = pSB->GetWindowRect();
        int sbH = rsb.Height();
        if (sbH <= 0)
            sbH = 24;
        CRect rsbNew(0, rcCli.bottom - sbH, rcCli.right, rcCli.bottom);
        pSB->Move(&rsbNew);

        SWindow *pArea = m_pHex->GetParent();
        CRect ra = pArea->GetWindowRect();
        ra.right = rcCli.right - 1;
        ra.bottom = rsbNew.top;
        pArea->Move(&ra);
    }
    // 标签条子项（含 mainarea 让位；内部也会重贴检查器）
    LayoutTabStrip();
    // 标签条未创建时 LayoutTabStrip 会提前返回，这里兜底保证检查器仍会重贴
    if (IsInspectorDocked())
        LayoutInspectorDock();
    return 0;
}

// ---------------------------------------------------------------------------
// 标签条手动布局：SOUI 布局引擎对"容器 display 运行时切换"的子项重排不可靠
// （多轮实测：hbox/绝对定位/恒显示按钮全部无法让 tabctrl 让位），改为代码
// 直接 Move 三个子窗口到精确矩形。tabctrl 占左侧至倒数 44px，按钮各占 22px。
// ---------------------------------------------------------------------------
void CMainFrame::LayoutTabStrip()
{
    if (!m_pTabFilesEx || !m_pTabBarWrap)
        return;
    CRect rcWrap = m_pTabBarWrap->GetWindowRect(); // 宿主客户区系
    int cx = rcWrap.Width();
    if (cx < 100)
        return; // 容器矩形未收敛
    // 标签条显示后 vbox 不会自动给 mainarea 让位（运行时 display 切换的
    // 老问题）——手动把 mainarea（hexview 的父容器）下移 26px 并压缩高度，
    // 否则 dock 的检查器仍按旧 mainarea 顶停靠、盖住标签条。
    SWindow *pArea = m_pHex ? m_pHex->GetParent() : NULL;
    if (pArea)
    {
        CRect rcArea = pArea->GetWindowRect();
        if (rcArea.top < rcWrap.bottom) // mainarea 顶尚未让出标签条空间
        {
            CRect rcNew(rcArea.left, rcWrap.bottom, rcArea.right, rcArea.bottom);
            pArea->Move(&rcNew);
            rcArea = rcNew;
            if (m_pHex)
            {
                // hexview 跟随 mainarea（若 dock 检查器在右缘则保持让位）
                CRect rcHex;
                if (IsInspectorDocked())
                {
                    int nInspW = max(180, min(m_cxInspUser, rcNew.Width() * 4 / 5));
                    rcHex = CRect(rcNew.left, rcNew.top, rcNew.right - nInspW, rcNew.bottom);
                }
                else
                    rcHex = rcNew;
                m_pHex->Move(&rcHex);
            }
        }
        // dock 检查器无条件按当前 mainarea 重贴——启动时它按"标签条还隐藏"
        // 的旧 mainarea（top=97）贴过位；标签条显示后 mainarea 下移，检查器
        // 若不重贴会盖住标签条区域。
        LayoutInspectorDock();
    }
    // 三个按钮各占 22px：‹ › 滚动 + ▾ 文件列表
    CRect rcTab(rcWrap.left, rcWrap.top, rcWrap.left + cx - 66, rcWrap.bottom);
    m_pTabFilesEx->Move(&rcTab);
    if (m_pTabScrollL)
    {
        CRect rcL(rcWrap.left + cx - 66, rcWrap.top, rcWrap.left + cx - 44, rcWrap.bottom);
        m_pTabScrollL->Move(&rcL);
    }
    if (m_pTabScrollR)
    {
        CRect rcR(rcWrap.left + cx - 44, rcWrap.top, rcWrap.left + cx - 22, rcWrap.bottom);
        m_pTabScrollR->Move(&rcR);
    }
    SWindow *pListBtn = FindChildByName2<SWindow>(L"tab_list_files");
    if (pListBtn)
    {
        CRect rcB(rcWrap.left + cx - 22, rcWrap.top, rcWrap.left + cx, rcWrap.bottom);
        pListBtn->Move(&rcB);
    }
}

// ---------------------------------------------------------------------------
// 标题栏按钮
// ---------------------------------------------------------------------------
void CMainFrame::UpdateCaptionButtons()
{
    SWindow *pMax = FindChildByName2<SWindow>(L"btn_max");
    SWindow *pRestore = FindChildByName2<SWindow>(L"btn_restore");
    if (!pMax || !pRestore)
        return;
    pMax->SetVisible(!m_bMaximized, TRUE);
    pRestore->SetVisible(m_bMaximized, TRUE);
}

void CMainFrame::OnMinimize()
{
    ::ShowWindow(m_hWnd, SW_MINIMIZE);
}

void CMainFrame::OnMaximize()
{
    if (m_bMaximized)
        return;
    // 用 WINDOWPLACEMENT.rcNormalPosition 取"还原矩形"：若此刻窗口恰好处于原生
    // zoom 态，GetWindowRect() 拿到的是被 ±frame 扩大的矩形（如 -8 -8 1544 824），
    // 存下来会导致之后还原成满屏。rcNormalPosition 始终是未最大化前的正常矩形。
    WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
    if (::GetWindowPlacement(m_hWnd, &wp))
        m_rcRestore = wp.rcNormalPosition;
    else
    {
        CRect rc;
        ::GetWindowRect(m_hWnd, &rc);
        m_rcRestore = rc;
    }
    m_bMaximized = true;
    MaximizeToWorkArea();
    UpdateCaptionButtons();
}

void CMainFrame::OnRestore()
{
    if (!m_bMaximized)
        return;
    m_bMaximized = false;
    ::SetWindowPos(m_hWnd, NULL, m_rcRestore.left, m_rcRestore.top,
                   m_rcRestore.Width(), m_rcRestore.Height(),
                   SWP_NOZORDER | SWP_NOACTIVATE);
    UpdateCaptionButtons();
}

void CMainFrame::OnCloseBtn()
{
    OnClose();
}

// ---------------------------------------------------------------------------
// 菜单弹出前刷新勾选 / 禁用状态
// ---------------------------------------------------------------------------
static void MenuCheck(SMenuBar *pBar, int iMenu, UINT idItem, BOOL bCheck)
{
    SMenu *p = pBar ? pBar->GetMenu(iMenu) : NULL;
    if (p)
        p->CheckMenuItem(idItem, MF_BYCOMMAND | (bCheck ? MF_CHECKED : MF_UNCHECKED));
}

static void MenuEnable(SMenuBar *pBar, int iMenu, UINT idItem, BOOL bEnable)
{
    SMenu *p = pBar ? pBar->GetMenu(iMenu) : NULL;
    if (p)
        p->EnableMenuItem(idItem, MF_BYCOMMAND | (bEnable ? MF_ENABLED : MF_GRAYED));
}

void CMainFrame::UpdateMenuStates()
{
    SMenuBar *pBar = FindChildByName2<SMenuBar>(L"menubar");
    if (!pBar)
        return;
    BOOL bOpen = CurModel()->IsOpen();

    // 文件菜单(0)
    MenuEnable(pBar, 0, IDM_FILE_SAVE, bOpen && CurModel()->IsModified());
    MenuEnable(pBar, 0, IDM_FILE_SAVEAS, bOpen);
    MenuEnable(pBar, 0, IDM_FILE_READONLY, bOpen);
    MenuCheck(pBar, 0, IDM_FILE_READONLY, CurModel()->IsReadOnly());
    MenuCheck(pBar, 0, IDM_FILE_FASTSAVE, m_bFastSaveLarge);
    MenuEnable(pBar, 0, IDM_FILE_PROPERTIES, bOpen);

    // 编辑菜单(1)
    MenuEnable(pBar, 1, IDM_EDIT_UNDO, CurModel()->CanUndo());
    MenuEnable(pBar, 1, IDM_EDIT_REDO, CurModel()->CanRedo());
    MenuEnable(pBar, 1, IDM_EDIT_CUT, m_pHex && m_pHex->HasSelection());
    MenuEnable(pBar, 1, IDM_EDIT_COPY, m_pHex && m_pHex->HasSelection());
    MenuEnable(pBar, 1, IDM_EDIT_DELETE, m_pHex && m_pHex->HasSelection());
    MenuEnable(pBar, 1, IDM_EDIT_SELECTALL, bOpen);
    MenuCheck(pBar, 1, IDM_EDIT_INSMODE, m_pHex && m_pHex->IsInsertMode());
    MenuEnable(pBar, 1, IDM_EDIT_INSERT, bOpen && !CurModel()->IsReadOnly());
    MenuEnable(pBar, 1, IDM_EDIT_FILL,
               bOpen && !CurModel()->IsReadOnly() && m_pHex && m_pHex->HasSelection());

    // 查看菜单(2)
    int bpr = m_pHex ? m_pHex->GetBytesPerRow() : 16;
    MenuCheck(pBar, 2, IDM_VIEW_BPR16, bpr == 16);
    MenuCheck(pBar, 2, IDM_VIEW_BPR24, bpr == 24);
    MenuCheck(pBar, 2, IDM_VIEW_BPR32, bpr == 32);
    MenuCheck(pBar, 2, IDM_VIEW_BPR48, bpr == 48);
    MenuCheck(pBar, 2, IDM_VIEW_INSPECTOR,
              m_pInspWnd && ::IsWindow(m_pInspWnd->m_hWnd) && ::IsWindowVisible(m_pInspWnd->m_hWnd));
    MenuCheck(pBar, 2, IDM_VIEW_STATUSBAR, m_pStatusBar && m_pStatusBar->IsVisible(TRUE));
    MenuCheck(pBar, 2, IDM_VIEW_TOOLBAR, m_pToolbar && m_pToolbar->IsVisible(TRUE));
    MenuCheck(pBar, 2, IDM_VIEW_LANG_ZH, I18n::Current() == I18n::ZH);
    MenuCheck(pBar, 2, IDM_VIEW_LANG_EN, I18n::Current() == I18n::EN);

    // 搜索菜单(3)
    MenuEnable(pBar, 3, IDM_SEARCH_FIND, bOpen);
    MenuEnable(pBar, 3, IDM_SEARCH_NEXT, bOpen && m_bHasLastFind);
    MenuEnable(pBar, 3, IDM_SEARCH_PREV, bOpen && m_bHasLastFind);
    MenuEnable(pBar, 3, IDM_SEARCH_GOTO, bOpen);
    MenuEnable(pBar, 3, IDM_SEARCH_REPLACE, bOpen && !CurModel()->IsReadOnly());
    MenuEnable(pBar, 3, IDM_SEARCH_CHECKSUM, bOpen);
}

void CMainFrame::OnPopMenu(IEvtArgs *pEvt)
{
    EventPopMenu *e = sobj_cast<EventPopMenu>(pEvt);
    if (!e)
        return;
    UpdateMenuStates();
}

// 追加一个命令行文件。实现在 cpp（不内联）：此函数在 Create 之前被调用，
// 一旦内联并与 SStringW 操作交叉，在真机构建下反复触发 GS 栈 cookie 失败
// （0xc0000409）；保持独立函数体 + 仅操作 POD 缓冲是稳定形态。
void CMainFrame::AddCmdFile(const wchar_t *pszPath)
{
    if (!pszPath || !pszPath[0])
        return;
    if (m_nCmdFileCount >= kMaxCmdFiles)
        return;
    wcsncpy(m_arCmdFiles[m_nCmdFileCount], pszPath, 511);
    m_arCmdFiles[m_nCmdFileCount][511] = 0;
    m_nCmdFileCount++;
}

// 大 bpr 延迟刷新：CHexView::SetBytesPerRow 只投递本消息；消息栈退净后
// 在此安全地更新滚动条并重绘。栈内同步刷新会触发 soui4.dll 的
// _PaintChildren 悬空时序缺陷（0xc000041d）。
LRESULT CMainFrame::OnBprDeferredRefresh(UINT, WPARAM, LPARAM)
{
    if (m_pHex)
    {
        m_pHex->RefreshAfterBprChange();
        UpdateStatus();
    }
    return 0;
}

// 输入超限时由 OnBprEditChanged 投递：在 EN_CHANGE 消息栈退净后把组合框
// 文本回写为钳制后的规范值（避免回调内 SetWindowText 造成事件重入）。
LRESULT CMainFrame::OnBprDeferredSyncUi(UINT, WPARAM wParam, LPARAM)
{
    SyncBprCombo((int)wParam);
    UpdateStatus();
    return 0;
}

LRESULT CMainFrame::OnTabDeferredBtnUpdate(UINT, WPARAM, LPARAM)
{
    // 容器 SetVisible(TRUE,TRUE) 递归激活子窗口会把按钮一并显示；
    // 且当时 tabctrl 布局未收敛（客户区 0 宽）无法判定溢出。
    // 消息队列尾（布局已收敛）再按内容宽度校正按钮显隐。
    UpdateTabScrollButtons();
    return 0;
}

BOOL CMainFrame::OnHostMouseWheel(UINT nFlags, short zDelta, CPoint pt)
{
    if (!m_pHex)
    {
        SetMsgHandled(FALSE);
        return FALSE;
    }
    // 屏幕坐标 → 宿主客户区坐标，再检查是否落在 hexview 窗口矩形内
    CPoint ptClient(pt);
    ScreenToClient(&ptClient);
    CRect rcHex = m_pHex->GetWindowRect();
    if (!rcHex.PtInRect(ptClient))
    {
        SetMsgHandled(FALSE);
        return FALSE; // 不在 hexview 上：交给 SOUI 默认处理（其它控件）
    }
    // Shift+滚轮：横向；普通：纵向（按 3 行/格）；无纵向条时普通滚轮也转横向
    if ((nFlags & MK_SHIFT) || GetKeyState(VK_SHIFT) < 0)
    {
        m_pHex->OnHostWheelH(zDelta);
    }
    else if (m_pHex->HasVScrollBar())
    {
        int nLines = zDelta > 0 ? -3 : 3;
        m_pHex->OnHostWheelV(nLines);
    }
    else
    {
        m_pHex->OnHostWheelH(zDelta); // 大 bpr 下纵向无需滚动，滚轮直接横向
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// 数据检查器窗口：dock/floating 双态（HWND 父子关系切换实现）
// docked  = 主窗口的子窗口，贴主窗口客户区右缘，hexview 让位
// floating= 顶层 WS_POPUP 悬浮，可拖到任意位置；拖动结束时贴近主窗口右缘
//           （或左/上/下缘）则自动 re-dock
// ---------------------------------------------------------------------------
void CMainFrame::EnsureInspectorWnd()
{
    if (m_pInspWnd && ::IsWindow(m_pInspWnd->m_hWnd))
        return;
    if (!m_pInspWnd)
    {
        m_pInspWnd = new CInspectorWnd();
        m_pInspWnd->m_pfnOnMoveEnd = [](void *ctx) {
            ((CMainFrame *)ctx)->OnInspWndMove();
        };
        m_pInspWnd->m_pfnOnResizeEnd = [](void *ctx) {
            ((CMainFrame *)ctx)->OnInspWndResized();
        };
        m_pInspWnd->m_pfnOnCaptionDrag = [](void *ctx) {
            ((CMainFrame *)ctx)->UndockInspectorWnd();
        };
        m_pInspWnd->m_pMoveEndCtx = this;
    }
    if (!::IsWindow(m_pInspWnd->m_hWnd))
    {
        // 按 WS_POPUP 创建（SHostWnd 对 WS_CHILD 的 auto-size 流程不兼容——
        // 实测 child 直接创建得到 0 尺寸窗口）。随后 DockInspectorWnd 走
        // 完整转换（SetWindowLongPtr 改 WS_CHILD + SetParent）实现初始停靠。
        m_pInspWnd->CreateEx(m_hWnd, WS_POPUP | WS_CLIPCHILDREN,
                             WS_EX_TOOLWINDOW, 0, 0, 270, 430);
    }
}

// ---- 查找/替换/转到 独立非模态浮窗 ----
void CMainFrame::EnsureFindWnd()
{
    if (m_pFindWnd && ::IsWindow(m_pFindWnd->m_hWnd))
        return;
    if (!m_pFindWnd)
    {
        m_pFindWnd = new CFindWnd();
        m_pFindWnd->SetListener(this);
    }
    if (!::IsWindow(m_pFindWnd->m_hWnd))
    {
        m_pFindWnd->CreateEx(m_hWnd, WS_POPUP | WS_CLIPCHILDREN, WS_EX_TOOLWINDOW, 0, 0, 470,
                             236);
        m_pFindWnd->SetHexMode(m_bSearchHex);
        // 首次创建后按当前主题上色（之后由 ApplyTheme 统一刷新）
        m_pFindWnd->ApplyTheme(Theme::Get(m_nTheme));
    }
}

// ---- 关于窗口 ----
void CMainFrame::EnsureAboutWnd()
{
    if (m_pAboutWnd && ::IsWindow(m_pAboutWnd->m_hWnd))
        return;
    if (!m_pAboutWnd)
        m_pAboutWnd = new CAboutWnd();
    if (!::IsWindow(m_pAboutWnd->m_hWnd))
    {
        m_pAboutWnd->CreateEx(m_hWnd, WS_POPUP | WS_CLIPCHILDREN, WS_EX_TOOLWINDOW, 0, 0, 440,
                              264);
        m_pAboutWnd->RefreshText();
        m_pAboutWnd->ApplyTheme(Theme::Get(m_nTheme));
    }
}

void CMainFrame::ShowFindWnd(int nMode)
{
    EnsureFindWnd();
    if (!m_pFindWnd || !::IsWindow(m_pFindWnd->m_hWnd))
        return;
    if (!::IsWindowVisible(m_pFindWnd->m_hWnd))
    {
        RECT rcMain, rcFind;
        ::GetWindowRect(m_hWnd, &rcMain);
        ::GetWindowRect(m_pFindWnd->m_hWnd, &rcFind);
        int x = rcMain.right - (rcFind.right - rcFind.left) - 40;
        int y = rcMain.top + 80;
        ::SetWindowPos(m_pFindWnd->m_hWnd, NULL, x, y, 0, 0,
                       SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    m_pFindWnd->SetHexMode(m_bSearchHex);
    if (nMode == 1)
        m_pFindWnd->ShowAsReplace();
    else if (nMode == 2)
        m_pFindWnd->ShowAsGoto();
    else
        m_pFindWnd->ShowAsFind();
}

// ---- IFindWndListener ----
void CMainFrame::OnFindWndFindNext()
{
    OnBtnFindNext();
}
void CMainFrame::OnFindWndFindPrev()
{
    OnBtnFindPrev();
}
void CMainFrame::OnFindWndReplace()
{
    OnBtnReplace();
}
void CMainFrame::OnFindWndReplaceAll()
{
    OnBtnReplaceAll();
}
void CMainFrame::OnFindWndHexToggle()
{
    OnBtnFindHex();
}
void CMainFrame::OnFindWndGoto()
{
    OnBtnGotoOk();
}
void CMainFrame::OnFindWndClose()
{
    if (m_pHex)
        m_pHex->SetHitRange(0, 0);
    Notify(NULL);
}

void CMainFrame::LayoutInspectorDock()
{
    // docked 态：检查器贴【编辑区（hexview 所在的 mainarea）】的指定边缘。
    // 不用主窗口客户区——那包含标题栏/工具栏/状态栏，会盖住主窗关闭按钮。
    if (!IsInspectorDocked() || !m_pInspWnd || !m_pHex)
        return;
    m_pInspWnd->SetDockEdge(m_nInspDockEdge); // 贴边侧让出主窗口缩放热区
    CRect rcArea = m_pHex->GetParent()->GetWindowRect(); // 宿主客户区系
    // 用户拉伸后的厚度（左右停靠用宽、上下停靠用高），并钳制到区域尺寸内
    int nInspW = max(180, min(m_cxInspUser, rcArea.Width() * 4 / 5));
    int nInspH = max(200, min(m_cyInspUser, rcArea.Height() * 4 / 5));
    CRect rcInsp;
    switch (m_nInspDockEdge)
    {
    case 0: // 左
        rcInsp = CRect(rcArea.left, rcArea.top, rcArea.left + nInspW, rcArea.bottom);
        break;
    case 1: // 右
        rcInsp = CRect(rcArea.right - nInspW, rcArea.top, rcArea.right, rcArea.bottom);
        break;
    case 2: // 上
        rcInsp = CRect(rcArea.left, rcArea.top, rcArea.right, rcArea.top + nInspH);
        break;
    default: // 下
        rcInsp = CRect(rcArea.left, rcArea.bottom - nInspH, rcArea.right, rcArea.bottom);
        break;
    }
    ::SetWindowPos(m_pInspWnd->m_hWnd, HWND_TOP,
                   rcInsp.left, rcInsp.top, rcInsp.Width(), rcInsp.Height(),
                   SWP_SHOWWINDOW | SWP_FRAMECHANGED);
    // SetWindowPos 改变窗口尺寸后 SOUI 内部布局不会自动重跑（内容仍按旧
    // 高度布局——实测内容在窗口拉伸/换边后被截断，"文件"行整段消失）。
    // 同步 root relayout 不完整——用投递 WM_SIZE 让 SOUI 走完整 OnSize
    // 布局链（实测有效：内容铺满整个窗口高度）。
    ::PostMessage(m_pInspWnd->m_hWnd, WM_SIZE, SIZE_RESTORED,
                  MAKELPARAM(rcInsp.Width(), rcInsp.Height()));
    // hexview 让位：手动收缩（SOUI 布局引擎不可靠，同 LayoutTabStrip 教训）
    CRect rcHex = rcArea;
    switch (m_nInspDockEdge)
    {
    case 0: rcHex.left = rcInsp.right; break;
    case 1: rcHex.right = rcInsp.left; break;
    case 2: rcHex.top = rcInsp.bottom; break;
    default: rcHex.bottom = rcInsp.top; break;
    }
    m_pHex->Move(&rcHex);
}

void CMainFrame::DockInspectorWnd()
{
    if (!m_pInspWnd || !::IsWindow(m_pInspWnd->m_hWnd))
        return;
    if (IsInspectorDocked())
    {
        // 已是子窗口（如创建时 parent 即主窗口）：只补状态位和布局
        LayoutInspectorDock();
        return;
    }
    // 记住悬浮位置（下次拖出恢复）
    ::GetWindowRect(m_pInspWnd->m_hWnd, &m_rcInspFloat);
    m_bInspFloatSaved = true;
    LONG_PTR dwStyle = ::GetWindowLongPtrW(m_pInspWnd->m_hWnd, GWL_STYLE);
    dwStyle = (dwStyle & ~WS_POPUP) | WS_CHILD;
    ::SetWindowLongPtrW(m_pInspWnd->m_hWnd, GWL_STYLE, dwStyle);
    ::SetParent(m_pInspWnd->m_hWnd, m_hWnd);
    LayoutInspectorDock();
}

void CMainFrame::UndockInspectorWnd()
{
    if (!m_pInspWnd || !::IsWindow(m_pInspWnd->m_hWnd) || !IsInspectorDocked())
        return;
    m_pInspWnd->SetDockEdge(-1); // 悬浮：恢复检查器自身的四边缩放
    m_bInspJustUndocked = true; // 拖出后第一次松手不做 dock 检测
    // 当前客户区矩形（WS_CHILD 的 GetWindowRect 已是屏幕系）
    RECT rc;
    ::GetWindowRect(m_pInspWnd->m_hWnd, &rc);
    LONG_PTR dwStyle = ::GetWindowLongPtrW(m_pInspWnd->m_hWnd, GWL_STYLE);
    dwStyle = (dwStyle & ~WS_CHILD) | WS_POPUP;
    ::SetWindowLongPtrW(m_pInspWnd->m_hWnd, GWL_STYLE, dwStyle);
    ::SetParent(m_pInspWnd->m_hWnd, NULL);
    // 位置：优先【光标 - 拖动偏移】（标题栏跟光标走，SCaption SC_MOVE
    // 紧接着接管拖动）；无偏移时贴主窗右侧。
    // 尺寸：上/下停靠时宽度是编辑区全宽——浮出时恢复用户记忆宽。
    // 偏移钳制：停靠时窗口可能是全宽（如上/下停靠 1098px），光标按当时
    // 窗口记录的偏移可能远超新宽度（270）——undock 后窗口会"飞"到编辑区
    // 左半边（看起来像立即停靠左边）。按新尺寸钳制偏移。
    int x = rc.left, y = rc.top;
    int cx = rc.right - rc.left;
    int cy = rc.bottom - rc.top;
    if (m_nInspDockEdge >= 2)
        cx = m_cxInspUser; // 上下停靠的浮出：宽度回到用户尺寸
    POINT ptCur;
    ::GetCursorPos(&ptCur);
    if (m_pInspWnd->m_bDragOffsetValid)
    {
        int nOffX = m_pInspWnd->m_ptDragOffset.x;
        int nOffY = m_pInspWnd->m_ptDragOffset.y;
        // 钳制偏移到新尺寸内（留 4px 边距，保证光标仍落在标题栏内）
        if (nOffX > cx - 8)
            nOffX = cx / 2;
        if (nOffX < 8)
            nOffX = cx / 2;
        if (nOffY > 28)
            nOffY = 14; // 光标不在标题栏带内 → 用标题栏中线
        x = ptCur.x - nOffX;
        y = ptCur.y - nOffY;
    }
    else
    {
        RECT rcMain;
        ::GetWindowRect(m_hWnd, &rcMain);
        x = rcMain.right + 6;
        y = rcMain.top + 60;
    }
    ::SetWindowPos(m_pInspWnd->m_hWnd, 0, x, y, cx, cy,
                   SWP_NOZORDER | SWP_SHOWWINDOW);
    // hexview 恢复占满 mainarea
    if (m_pHex)
    {
        CRect rcHex = m_pHex->GetParent()->GetWindowRect();
        m_pHex->Move(&rcHex);
    }
}

int CMainFrame::rcMain2Right()
{
    RECT rc;
    ::GetClientRect(m_hWnd, &rc);
    // 找到 mainarea 的右缘：用 hexview 的父容器
    if (m_pHex && m_pHex->GetParent())
    {
        CRect rcArea = m_pHex->GetParent()->GetWindowRect();
        return rcArea.right;
    }
    return rc.right;
}

void CMainFrame::OnInspWndResized()
{
    // 用户拉伸结束：记住窗口尺寸（dock 布局与后续浮窗都沿用）
    if (!m_pInspWnd || !::IsWindow(m_pInspWnd->m_hWnd))
        return;
    RECT rc;
    ::GetWindowRect(m_pInspWnd->m_hWnd, &rc);
    int cx = rc.right - rc.left;
    int cy = rc.bottom - rc.top;
    // docked 态拉伸的是【垂直于停靠边】的厚度；floating 态两个方向都可能改。
    // 记忆规则：左右停靠记宽，上下停靠记高；floating 记录完整尺寸。
    if (IsInspectorDocked())
    {
        if (m_nInspDockEdge <= 1)
            m_cxInspUser = cx;
        else
            m_cyInspUser = cy;
        // 重新贴边布局（拉伸后位置可能脱离边缘）
        LayoutInspectorDock();
    }
    else
    {
        m_cxInspUser = cx;
        m_cyInspUser = cy;
    }
}

void CMainFrame::OnInspWndMove()
{
    // 拖动结束（位置变化）：先记录用户尺寸，再做悬浮态贴近编辑区检测。
    // 纯拉伸走 OnInspWndResized（不 dock——浮窗悬在编辑区上调整尺寸
    // 不应被吸附）。
    OnInspWndResized();
    if (!m_pInspWnd || !::IsWindow(m_pInspWnd->m_hWnd) || IsInspectorDocked() || !m_pHex)
        return;
    // 刚从停靠拖出的第一次松手：跳过 dock 检测——用户按住标题栏把窗口
    // 拖出来（光标自然还在编辑区里），原地松手是"完成拖出"不是"想停靠"，
    // 立即吸回会造成"按下就乱跳"。
    if (m_bInspJustUndocked)
    {
        m_bInspJustUndocked = false;
        return;
    }
    RECT rcInsp, rcAreaS;
    ::GetWindowRect(m_pInspWnd->m_hWnd, &rcInsp);
    // 编辑区（宿主客户区系）→ 屏幕系
    CRect rcArea = m_pHex->GetParent()->GetWindowRect();
    RECT rcMain;
    ::GetWindowRect(m_hWnd, &rcMain);
    rcArea.OffsetRect(rcMain.left, rcMain.top);
    // 用【光标位置】而不是窗口中心判定（中心判定对左/下边缘不灵敏——
    // 窗口一半都进编辑区才可能命中；光标靠哪条边近就停哪条，四边灵敏度一致）
    POINT ptCur;
    ::GetCursorPos(&ptCur);
    if (ptCur.x < rcArea.left || ptCur.x > rcArea.right
        || ptCur.y < rcArea.top || ptCur.y > rcArea.bottom)
        return; // 光标不在编辑区 → 保持悬浮
    // 计算光标与四条边的距离，取最小
    int dL = ptCur.x - rcArea.left;
    int dR = rcArea.right - ptCur.x;
    int dT = ptCur.y - rcArea.top;
    int dB = rcArea.bottom - ptCur.y;
    int nMin = min(min(dL, dR), min(dT, dB));
    if (nMin == dL)
        m_nInspDockEdge = 0;
    else if (nMin == dR)
        m_nInspDockEdge = 1;
    else if (nMin == dT)
        m_nInspDockEdge = 2;
    else
        m_nInspDockEdge = 3;
    DockInspectorWnd();
}

void CMainFrame::ShowInspectorWnd(BOOL bShow)
{
    EnsureInspectorWnd();
    if (!m_pInspWnd || !::IsWindow(m_pInspWnd->m_hWnd))
        return;
    if (bShow)
    {
        if (IsInspectorDocked())
            LayoutInspectorDock(); // 布局可能因主窗口缩放而变化
        m_pInspWnd->ShowWindow(SW_SHOWNOACTIVATE);
    }
    else
        m_pInspWnd->ShowWindow(SW_HIDE);
}

void CMainFrame::OnActCloseInspector()
{
    ShowInspectorWnd(FALSE);
}

void CMainFrame::OnMenuSelected(IEvtArgs *pEvt)
{
    EventSelectMenu *e = sobj_cast<EventSelectMenu>(pEvt);
    if (e)
        OnCommand(0, (int)e->nMenuId, NULL);

}

// ---------------------------------------------------------------------------
// 菜单命令（SMenu 通过 WM_COMMAND 把 item id 发回宿主窗口）
// ---------------------------------------------------------------------------
void CMainFrame::OnCommand(UINT uNotifyCode, int nID, HWND wndCtl)
{
    switch (nID)
    {
    case IDM_FILE_NEW:        OnActNew(); break;
    case IDM_FILE_OPEN:       OnActOpen(); break;
    case IDM_FILE_SAVE:       OnActSave(); break;
    case IDM_FILE_SAVEAS:     OnActSaveAs(); break;
    case IDM_FILE_EXIT:       OnClose(); break;
    case IDM_FILE_READONLY:   OnActToggleReadOnly(); break;
    case IDM_FILE_FASTSAVE:   OnActToggleFastSave(); break;
    case IDM_FILE_PROPERTIES: OnActFileProperties(); break;

    case IDM_EDIT_UNDO:      OnActUndo(); break;
    case IDM_EDIT_REDO:      OnActRedo(); break;
    case IDM_EDIT_CUT:       OnActCut(); break;
    case IDM_EDIT_COPY:      OnActCopy(); break;
    case IDM_EDIT_PASTE:     OnActPaste(); break;
    case IDM_EDIT_DELETE:    OnActDelete(); break;
    case IDM_EDIT_INSERT:    OnActInsertByte(); break;
    case IDM_EDIT_SELECTALL: OnActSelectAll(); break;
    case IDM_EDIT_INSMODE:   OnActToggleInsertMode(); break;
    case IDM_EDIT_FILL:      OnActFillSelection(); break;

    case IDM_VIEW_BPR16:     OnActSetBpr(16); break;
    case IDM_VIEW_BPR24:     OnActSetBpr(24); break;
    case IDM_VIEW_BPR32:     OnActSetBpr(32); break;
    case IDM_VIEW_BPR48:     OnActSetBpr(48); break;
    case IDM_VIEW_INSPECTOR: OnActToggleInspector(); break;
    case IDM_VIEW_STATUSBAR: OnActToggleStatusBar(); break;
    case IDM_VIEW_TOOLBAR:   OnActToggleToolbar(); break;
    case IDM_VIEW_CHARSET:   OnActCharset(); break;
    case IDM_VIEW_THEME_LIGHT: OnActSetTheme(Theme::LIGHT); break;
    case IDM_VIEW_THEME_DARK:  OnActSetTheme(Theme::DARK); break;
    case IDM_VIEW_LANG_ZH:     OnActLanguage(I18n::ZH); break;
    case IDM_VIEW_LANG_EN:     OnActLanguage(I18n::EN); break;

    case IDM_SEARCH_FIND:     OnActFind(); break;
    case IDM_SEARCH_NEXT:     OnActFindNext(); break;
    case IDM_SEARCH_PREV:     OnActFindPrev(); break;
    case IDM_SEARCH_GOTO:     OnActGoto(); break;
    case IDM_SEARCH_REPLACE:  OnActReplace(); break;
    case IDM_SEARCH_CHECKSUM: OnActChecksum(); break;

    case IDM_WIN_MIN:        OnMinimize(); break;
    case IDM_WIN_MAXRESTORE: m_bMaximized ? OnRestore() : OnMaximize(); break;
    case IDM_WIN_CLOSE:      OnClose(); break;

    case IDM_HELP_ABOUT: OnActAbout(); break;
    case IDM_HELP_KEYS:  OnActKeys(); break;

    default:
        SetMsgHandled(FALSE);
        break;
    }
}

void CMainFrame::OnTimer(UINT_PTR nIDEvent)
{
    if (nIDEvent == TIMER_SEARCH)
    {
        if (m_search.IsRunning())
            return;
        KillTimer(TIMER_SEARCH);
        FinishSearch();
    }
    SetMsgHandled(FALSE);
}

// ---------------------------------------------------------------------------
// 多文件标签页（HxD 风格）
// ---------------------------------------------------------------------------
hexmodel::HexFileModel *CMainFrame::CurModel()
{
    DocTab *p = CurTab();
    return p ? p->pModel : m_pEmptyModel;
}

void CMainFrame::RememberBpr(int nBpr)
{
    DocTab *p = CurTab();
    if (p)
        p->nBpr = nBpr;
}

void CMainFrame::SnapStatusBar()
{
    // 状态栏永远钉在主窗口客户区最底部（24px 高、全宽）。
    // mainarea 被 LayoutTabStrip Move 成 float 后，vbox 布局跳过它，
    // relayout 时状态栏会被排到工具栏下方（跑上面去）。
    SWindow *pStatus = m_pStatusBar;
    if (pStatus && pStatus->IsVisible(TRUE))
    {
        CRect rcRoot;
        ::GetClientRect(m_hWnd, &rcRoot);
        CRect rcS(rcRoot.left, rcRoot.bottom - 24, rcRoot.right, rcRoot.bottom);
        pStatus->Move(&rcS);
    }
}

void CMainFrame::ShowTabStrip()
{
    // 显隐由容器（tabbar_wrap，含滚动按钮）承担；tabctrl 恒随容器。
    // 有文件打开（1 个及以上）即显示——关闭到剩 1 个时用户需要看到
    // "当前打开的是哪个文件"；无文件（全关闭/未打开）才隐藏。
    BOOL bShow = !m_tabs.empty();
    if (m_pTabBarWrap)
    {
        BOOL bWasShown = m_pTabBarWrap->IsVisible(TRUE);
        m_pTabBarWrap->SetAttribute(L"display", bShow ? L"1" : L"0");
        m_pTabBarWrap->SetVisible(bShow, TRUE);
        // display 0→1 切换时布局是延迟的：容器矩形未收敛（实测读回 0 宽），
        // 标签条在其内的布局也不会执行 → 强制从 root 同步布局消除这个空窗。
        if (bShow && !bWasShown && GetRoot())
        {
            GetRoot()->RequestRelayout();
            GetRoot()->UpdateLayout();
            // SOUI 布局引擎对这个"运行时才显示的容器"的子项分配不可靠
            // （实测 tabctrl 一直占满宿主宽）→ 手动 Move 四个子窗口
            LayoutTabStrip();
            UpdateTabScrollButtons();
            // 状态栏钉回窗口最底部（mainarea 为 float 后 vbox 排布不可靠）
            SnapStatusBar();
        }
    }
    if (m_pTabFilesEx && bShow)
        UpdateTabScrollButtons();
    // 标签条隐藏（全关闭/无文件）时把 mainarea 恢复全高：mainarea 此前被
    // LayoutTabStrip Move 成浮动定位，SOUI vbox 不会自动还原（跳过 float）。
    // top 上移 26px、bottom 延伸到状态栏顶——只移 top 会把下方状态栏顶上去。
    if (!bShow)
    {
        SWindow *pArea = m_pHex ? m_pHex->GetParent() : NULL;
        if (pArea)
        {
            CRect rcArea = pArea->GetWindowRect();
            // 状态栏顶 = mainarea 的 bottom 边界（状态栏永远在最下方）
            SWindow *pStatus = m_pStatusBar; // OnInitDialog 已缓存
            int nBottom = rcArea.bottom;
            if (pStatus && pStatus->IsVisible(TRUE))
                nBottom = pStatus->GetWindowRect().top;
            CRect rcNew(rcArea.left, rcArea.top - 26, rcArea.right, nBottom);
            pArea->Move(&rcNew);
            if (m_pHex)
            {
                CRect rcHex;
                if (IsInspectorDocked())
                {
                    int nInspW = max(180, min(m_cxInspUser, rcNew.Width() * 4 / 5));
                    rcHex = CRect(rcNew.left, rcNew.top, rcNew.right - nInspW, rcNew.bottom);
                }
                else
                    rcHex = rcNew;
                m_pHex->Move(&rcHex);
            }
            if (IsInspectorDocked())
                LayoutInspectorDock();
            // 状态栏钉回最底部（vbox 对 float mainarea 的排布不可靠）
            SnapStatusBar();
        }
    }
}

int CMainFrame::TabStripContentWidth() const
{
    // 标签条内容自然总宽（未裁剪）：逐项累加 MeasureTabWidthPublic。
    // 不能用 GetItemRect：它对越出标签条的部分做 IntersectRect 裁剪，
    // 溢出时返回的是截断矩形，无法反映真实内容宽度。
    if (!m_pTabFilesEx)
        return 0;
    int nCount = m_pTabFilesEx->GetItemCount();
    int cx = 0;
    for (int i = 0; i < nCount; i++)
        cx += m_pTabFilesEx->MeasureTabWidthPublic(i);
    return cx;
}

void CMainFrame::UpdateTabScrollButtons()
{
    // 按钮恒显示恒占位（布局引擎限制：IsVisible(FALSE) 子项不占位、运行时
    // display 切换不重排，"按需出现"无法稳定实现）。此函数现在只负责在
    // 标签集合/标题变化后维持自动滚动状态的一致性。
    // 溢出状态下确保当前 tabPos 仍在合法范围（关闭标签后内容变短等场景）。
    if (!m_pTabFilesEx || m_tabs.empty())
        return;
    CRect rcClient;
    m_pTabFilesEx->GetClientRect(&rcClient);
    if (rcClient.Width() < 100)
        return; // 布局未就绪
    int cxContent = TabStripContentWidth();
    int nMaxLeft = cxContent > rcClient.Width() ? rcClient.Width() - cxContent : 0;
    // 内容不再溢出 → 回到最左；溢出但 tabPos 超界 → 钳回
    if (m_nTabPos < nMaxLeft)
        m_nTabPos = nMaxLeft;
    if (m_nTabPos > 0)
        m_nTabPos = 0;
    SStringW s;
    s.Format(L"%d", m_nTabPos);
    m_pTabFilesEx->SetAttribute(L"tabPos", s.c_str());
}

void CMainFrame::OnTabScrollLeft()
{
    // 【向左箭头】= 查看左侧被遮挡的标签 → 内容向右移动（tabPos 增大，趋 0）
    if (!m_pTabFilesEx || m_tabs.empty())
        return;
    int nStep = 120;
    m_nTabPos += nStep;
    if (m_nTabPos > 0)
        m_nTabPos = 0; // 回到最左
    SStringW s;
    s.Format(L"%d", m_nTabPos);
    m_pTabFilesEx->SetAttribute(L"tabPos", s.c_str());
    m_pTabFilesEx->Invalidate();
}

void CMainFrame::OnTabScrollRight()
{
    // 【向右箭头】= 查看右侧被遮挡的标签 → 内容向左移动（tabPos 越负）
    if (!m_pTabFilesEx || m_tabs.empty())
        return;
    int nStep = 120;
    CRect rcClient;
    m_pTabFilesEx->GetClientRect(&rcClient);
    int cxContent = TabStripContentWidth();
    // 最负偏移 = 让内容右缘恰好贴可视右缘
    int nMaxLeft = cxContent > rcClient.Width() ? rcClient.Width() - cxContent : 0;
    m_nTabPos -= nStep;
    if (m_nTabPos < nMaxLeft)
        m_nTabPos = nMaxLeft;
    SStringW s;
    s.Format(L"%d", m_nTabPos);
    m_pTabFilesEx->SetAttribute(L"tabPos", s.c_str());
    m_pTabFilesEx->Invalidate();
}

void CMainFrame::OnTabListFiles()
{
    // ▾ 按钮：弹出【打开的文件列表】菜单，点击项切换到对应标签。
    // 用 SOUI 自绘的 SMenuEx（原生 Win32 菜单不跟随本程序的深色主题）。
    if (m_tabs.empty())
        return;
    SWindow *pBtn = FindChildByName2<SWindow>(L"tab_list_files");
    if (!pBtn)
        return;

    SMenuEx *pMenu = new SMenuEx();
    if (!pMenu->LoadMenu(L"SMENU:MENU_TABLIST"))
    {
        pMenu->Release();
        return;
    }

    // 配色用统一的下发函数（root 上的勾选/箭头皮肤 + 每个条目的背景皮肤）。
    // 这个菜单是临时建的，条目建完再下发最省事；主菜单那条路径也是同一套逻辑。
    // 菜单项 id = 标签索引+1（从 1 开始）——TPM_RETURNCMD 取消时返回 0，
    // 不能与"第 0 个标签"冲突。当前激活的打勾。
    for (size_t i = 0; i < m_tabs.size(); i++)
    {
        UINT f = MF_STRING | MF_ENABLED;
        if ((int)i == m_nCurTab)
            f |= MF_CHECKED;
        pMenu->AppendMenu(f, (int)(i + 1), m_tabs[i].title.c_str());
    }
    ApplyMenuTheme(pMenu);

    // 按钮下方弹出
    CRect rcBtn = pBtn->GetWindowRect(); // 宿主客户区系
    CPoint pt(rcBtn.left, rcBtn.bottom);
    ::ClientToScreen(m_hWnd, &pt);
    int nCmd = pMenu->TrackPopupMenu(TPM_RETURNCMD | TPM_LEFTALIGN, pt.x, pt.y, m_hWnd,
                                    GetScale());
    pMenu->Release();
    // 0=菜单外点击取消（保持当前标签不变）；1..n=切换到第 n-1 个标签
    if (nCmd >= 1 && nCmd <= (int)m_tabs.size())
        ActivateTab(nCmd - 1); // 含视图状态恢复 + 标签滚入可视区
}

void CMainFrame::EnsureTabVisible(int nIndex)
{
    // 自动滚动标签条：使 nIndex 标签完整进入可视区（新建/打开/激活标签时调用）。
    // 用【自然宽度累加】计算目标标签的自然左/右缘（GetItemRect 被裁剪不可用）：
    //   自然左缘 = 前面所有标签宽之和；屏幕左缘 = + tabPos
    if (!m_pTabFilesEx)
        return;
    int nCount = m_pTabFilesEx->GetItemCount();
    if (nIndex < 0 || nIndex >= nCount)
        return;
    CRect rcClient;
    m_pTabFilesEx->GetClientRect(&rcClient);
    // 布局未就绪护栏：容器 display 刚切换时 tabctrl 自身布局可能未收敛
    // （实测客户区读回 0 宽），此时计算会把 tabPos 置成大负值、把已有标签
    // 全部滚出视图。客户区异常窄时跳过本次（下一次操作布局收敛后自然正常）。
    if (rcClient.Width() < 100)
        return;
    // 目标标签的自然左缘（不含当前偏移）
    int xLeft = 0;
    for (int i = 0; i < nIndex; i++)
        xLeft += m_pTabFilesEx->MeasureTabWidthPublic(i);
    int xRight = xLeft + m_pTabFilesEx->MeasureTabWidthPublic(nIndex);
    int nOldPos = m_nTabPos;
    // 标签的屏幕右缘超出可视右缘 → 左移使其恰好可见
    if (xRight + m_nTabPos > rcClient.right)
    {
        m_nTabPos = rcClient.right - xRight;
        if (m_nTabPos > 0)
            m_nTabPos = 0;
    }
    // 标签的屏幕左缘滚出左侧 → 右移补偿
    else if (xLeft + m_nTabPos < 0)
    {
        m_nTabPos = -xLeft;
        if (m_nTabPos > 0)
            m_nTabPos = 0;
    }
    if (m_nTabPos != nOldPos)
    {
        SStringW s;
        s.Format(L"%d", m_nTabPos);
        m_pTabFilesEx->SetAttribute(L"tabPos", s.c_str());
        m_pTabFilesEx->Invalidate();
    }
}

void CMainFrame::SyncTabTitle(int nIndex)
{
    if (!m_pTabFiles || nIndex < 0 || nIndex >= (int)m_tabs.size())
        return;
    DocTab &t = m_tabs[nIndex];
    SStringT title = t.title;
    if (t.pModel && t.pModel->IsModified())
        title += L" *";
    m_pTabFiles->SetItemTitle(nIndex, title);
    UpdateTabScrollButtons(); // 标题变化（修改标记）影响总宽，重算按钮显隐
}

void CMainFrame::ActivateTab(int nIndex)
{
    if (nIndex < 0 || nIndex >= (int)m_tabs.size())
        return;
    if (nIndex == m_nCurTab)
        return;
    // 保存当前标签的视图状态
    DocTab *pOld = CurTab();
    if (pOld && m_pHex)
    {
        pOld->nBpr = m_pHex->GetBytesPerRow();
        pOld->caret = m_pHex->GetCaret();
        m_pHex->GetSelection(pOld->selStart, pOld->selEnd);
    }
    m_nCurTab = nIndex;
    BindViewToTab(nIndex);
    EnsureTabVisible(nIndex); // 切换到的标签若被滚出可视区则自动滚入
}

void CMainFrame::BindViewToTab(int nIndex)
{
    DocTab &t = m_tabs[nIndex];
    m_bInTabSync = true; // 程序化切换不触发 OnTabSelChanged 的重复处理
    if (m_pTabFiles)
        m_pTabFiles->SetCurSel(nIndex);
    m_bInTabSync = false;

    m_source.Rebind(t.pModel);
    m_bHasLastFind = false;
    m_search.Stop();
    if (t.pModel)
        t.pModel->SetFastSaveEnabled(m_bFastSaveLarge);
    if (m_pHex)
    {
        m_pHex->SetBytesPerRow(t.nBpr);
        SyncBprCombo(t.nBpr);
        m_pHex->BindModel(t.pModel);
        m_pHex->SetSelection(t.selStart, t.selEnd);
        m_pHex->ScrollToOffset(t.caret, false); // 光标处可见即可，不强制居中
    }
    UpdateTitle();
    UpdateStatus();
    UpdateInspector();
    SyncModeUi();
}

void CMainFrame::CloseTab(int nIndex)
{
    if (nIndex < 0 || nIndex >= (int)m_tabs.size())
        return;
    DocTab &t = m_tabs[nIndex];
    // 有修改先询问保存
    if (t.pModel && t.pModel->IsOpen() && t.pModel->IsModified())
    {
        SStringT msg;
        msg.Format(I18n::T(L"关闭 %s 前是否保存修改？"), t.title.c_str());
        UINT nRet = SMessageBox(m_hWnd, msg, L"HexEditor", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (nRet == IDCANCEL)
            return;
        if (nRet == IDYES)
        {
            if (t.pModel->GetSavePath().empty())
            {
                SStringT path;
                if (!PickFile(m_hWnd, TRUE, t.pModel->Path(), &path))
                    return;
                if (!t.pModel->Save(path.c_str()))
                {
                    SMessageBox(m_hWnd, I18n::T(L"保存失败！"), L"HexEditor", MB_OK | MB_ICONERROR);
                    return;
                }
            }
            else if (!t.pModel->Save())
            {
                SMessageBox(m_hWnd, I18n::T(L"保存失败！"), L"HexEditor", MB_OK | MB_ICONERROR);
                return;
            }
        }
    }
    delete t.pModel;
    t.pModel = NULL;
    m_tabs.erase(m_tabs.begin() + nIndex);
    if (m_pTabFiles)
        m_pTabFiles->RemoveItem(nIndex);

    if (m_tabs.empty())
    {
        m_nCurTab = -1;
        m_source.Rebind(m_pEmptyModel);
        if (m_pHex)
        {
            m_pHex->SetBytesPerRow(16);
            SyncBprCombo(16);
            m_pHex->BindModel(m_pEmptyModel);
        }
        UpdateTitle();
        UpdateStatus();
        UpdateInspector();
    }
    else
    {
        // 关闭的是当前标签 → 切到相邻标签
        int nNew = nIndex;
        if (nNew >= (int)m_tabs.size())
            nNew = (int)m_tabs.size() - 1;
        m_nCurTab = -1; // 强制 ActivateTab 重新绑定
        ActivateTab(nNew);
    }
    ShowTabStrip();
}

void CMainFrame::OnTabSelChanged(IEvtArgs *pEvt)
{
    if (m_bInTabSync)
        return;
    EventTabSelChanged *e = sobj_cast<EventTabSelChanged>(pEvt);
    if (!e || !m_pTabFiles)
        return;
    int nSel = m_pTabFiles->GetCurSel();
    if (nSel >= 0 && nSel < (int)m_tabs.size() && nSel != m_nCurTab)
        ActivateTab(nSel);
}

void CMainFrame::OnTabCtxMenu(IEvtArgs *pEvt)
{
    EventCtxMenu *e = sobj_cast<EventCtxMenu>(pEvt);
    if (!e || !m_pTabFiles || m_tabs.empty())
        return;
    // 坐标系确认（SwndContainerImpl::OnFrameMouseEvent hover 分支）：
    // e->pt 是【宿主客户区坐标】（SOUI 派发给 hover 窗口时不做本地变换）。
    CRect rcTab = m_pTabFiles->GetWindowRect(); // 宿主客户区系下的 tabctrl 矩形
    int xInTab = e->pt.x - rcTab.left;          // tabctrl 内部相对 x
    int yInTab = e->pt.y - rcTab.top;
    int nCount = m_pTabFiles->GetItemCount();
    if (nCount <= 0 || xInTab < 0 || xInTab >= rcTab.Width() || yInTab < 0 || yInTab >= rcTab.Height())
        return; // 不在标签条上

    // 精确命中：逐项累加【真实标签宽度】（MeasureTabWidthPublic，随字体/DPI），
    // 再叠加当前滚动偏移 m_nTabPos —— 屏幕上标签 i 的左缘 = 累计宽 + tabPos。
    // 旧的字符宽度估算（每字 9px）在滚动/不同 DPI 下会命中错误标签。
    int nHit = -1;
    int xAcc = 0;
    for (int i = 0; i < nCount && i < (int)m_tabs.size(); i++)
    {
        int cxItem = m_pTabFilesEx->MeasureTabWidthPublic(i);
        int xScreenLeft = xAcc + m_nTabPos;  // 该标签在 tabctrl 内的实际左缘
        int xScreenRight = xScreenLeft + cxItem;
        if (xInTab >= xScreenLeft && xInTab < xScreenRight)
        {
            nHit = i;
            break;
        }
        xAcc += cxItem;
    }
    if (nHit < 0)
        nHit = nCount - 1; // 落在末尾空白 → 尾标签

    // 用 SOUI 自绘菜单 SMenuEx，而非原生 HMENU：swinx 在 Linux/macOS 不实现
    // CreatePopupMenu/TrackPopupMenu 等原生菜单，且自绘菜单能跟随应用深/浅主题。
    SMenuEx *pMenu = new SMenuEx();
    if (!pMenu->LoadMenu(L"SMENU:MENU_TABLIST")) // 复用「空根节点」模板
    {
        pMenu->Release();
        return;
    }
    SStringT txt;
    txt.Format(I18n::T(L"关闭 %s"), m_tabs[nHit].title.c_str());
    pMenu->AppendMenu(MF_STRING | MF_ENABLED, 1, txt.c_str());
    ApplyMenuTheme(pMenu);

    // 宿主客户区坐标 → 屏幕坐标（只此一次变换）
    CPoint ptScreen = e->pt;
    ::ClientToScreen(m_hWnd, &ptScreen);
    // SMenuEx 内部处理屏幕边界；TPM_RIGHTBUTTON 保持右键语义。
    UINT uFlags = TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN;
    int nCmd = (int)pMenu->TrackPopupMenu(uFlags, ptScreen.x, ptScreen.y, m_hWnd, GetScale());
    pMenu->Release();
    if (nCmd == 1)
        CloseTab(nHit);
    e->handled++; // 标记事件已处理（阻止默认行为）
}

// ---------------------------------------------------------------------------
// 打开 / 保存
// ---------------------------------------------------------------------------
BOOL CMainFrame::OpenFile(const TCHAR *pszPath)
{
    if (!pszPath || !pszPath[0])
        return FALSE;
    // 已打开同名文件 → 切到该标签（HxD 行为）
    for (size_t i = 0; i < m_tabs.size(); i++)
    {
        if (m_tabs[i].pModel && m_tabs[i].pModel->IsOpen()
            && _tcsicmp(m_tabs[i].pModel->Path(), pszPath) == 0)
        {
            ActivateTab((int)i);
            return TRUE;
        }
    }
    // 新标签打开（原标签的内容/滚动位置保留在各自标签里）
    hexmodel::HexFileModel *pModel = new hexmodel::HexFileModel();
    if (!pModel->Open(pszPath))
    {
        delete pModel;
        SMessageBox(m_hWnd, I18n::T(L"打开文件失败！文件可能不存在或被占用。"), L"HexEditor",
                    MB_OK | MB_ICONERROR);
        return FALSE;
    }
    DocTab t;
    t.pModel = pModel;
    t.nBpr = 16; // 打开新文件：每行字节数回到默认 16（与 HxD 行为一致）
    const TCHAR *p = pModel->Path();
    LPCTSTR q = _tcsrchr(p, '\\');
    if (!q)
        q = _tcsrchr(p, '/');
    t.title = q ? q + 1 : p;
    int nIndex = (int)m_tabs.size();
    m_tabs.push_back(t);
    // 同步 tabctrl：插入页并选中新页
    if (m_pTabFiles)
    {
        SStringW xml;
        xml.Format(L"<page title=\"%s\"/>", t.title.c_str());
        m_bInTabSync = true;
        m_pTabFiles->InsertItem(xml);
        m_bInTabSync = false;
    }
    // 保存旧标签状态再激活新标签
    DocTab *pOld = CurTab();
    if (pOld && m_pHex)
    {
        pOld->nBpr = m_pHex->GetBytesPerRow();
        pOld->caret = m_pHex->GetCaret();
        m_pHex->GetSelection(pOld->selStart, pOld->selEnd);
    }
    m_nCurTab = nIndex;
    BindViewToTab(nIndex);
    ShowTabStrip();
    EnsureTabVisible(nIndex); // 标签条放不下时自动左移，新标签立即完整可见
    return TRUE;
}

void CMainFrame::OnActOpen()
{
    SStringT path;
    hexmodel::HexFileModel *pM = CurModel();
    if (PickFile(m_hWnd, FALSE, (pM && pM->IsOpen()) ? pM->Path() : NULL, &path))
        OpenFile(path);
}

void CMainFrame::OnActSave()
{
    hexmodel::HexFileModel *pM = CurModel();
    if (!pM || !pM->IsOpen())
    {
        Notify(I18n::T(L"当前没有打开的文件。"));
        return;
    }
    // 新建且尚未保存过：走另存为
    if (pM->GetSavePath().empty())
    {
        OnActSaveAs();
        return;
    }
    if (!pM->Save())
        SMessageBox(m_hWnd, I18n::T(L"保存失败！"), L"HexEditor", MB_OK | MB_ICONERROR);
    else
    {
        SyncTabTitle(m_nCurTab);
        UpdateTitle();
    }
    UpdateStatus();
    if (pM->IsOpen() && pM->LastSaveWasFast())
    {
        SStringT msg;
        msg.Format(I18n::T(L"已保存（快速，仅写修改页）: %s"), pM->Path());
        Notify(msg);
    }
}

void CMainFrame::OnActSaveAs()
{
    hexmodel::HexFileModel *pM = CurModel();
    if (!pM || !pM->IsOpen())
        return;
    SStringT path;
    if (PickFile(m_hWnd, TRUE, pM->Path(), &path))
    {
        if (!pM->Save(path.c_str()))
            SMessageBox(m_hWnd, I18n::T(L"保存失败！"), L"HexEditor", MB_OK | MB_ICONERROR);
        else
        {
            // 另存为后标签标题可能变化（新路径的文件名）
            DocTab *pT = CurTab();
            if (pT)
            {
                const TCHAR *p = pM->Path();
                LPCTSTR q = _tcsrchr(p, '\\');
                if (!q)
                    q = _tcsrchr(p, '/');
                pT->title = q ? q + 1 : p;
                SyncTabTitle(m_nCurTab);
            }
            UpdateTitle();
            UpdateStatus();
        }
    }
}

void CMainFrame::OnActNew()
{
    // 新建 0 字节空文件（HxD 行为：新建后处于插入模式，从空开始）
    // 每次新建都开新标签（旧标签内容原样保留）
    static int s_newSeq = 0;
    wchar_t name[32];
    swprintf(name, 31, I18n::T(L"新建文件%d.bin"), ++s_newSeq);
    hexmodel::HexFileModel *pModel = new hexmodel::HexFileModel();
    if (!pModel->CreateNew(name))
    {
        delete pModel;
        Notify(I18n::T(L"新建文件失败。"));
        return;
    }
    DocTab t;
    t.pModel = pModel;
    t.nBpr = 16;
    t.title = name;
    int nIndex = (int)m_tabs.size();
    m_tabs.push_back(t);
    if (m_pTabFiles)
    {
        SStringW xml;
        xml.Format(L"<page title=\"%s\"/>", t.title.c_str());
        m_bInTabSync = true;
        m_pTabFiles->InsertItem(xml);
        m_bInTabSync = false;
    }
    DocTab *pOld = CurTab();
    if (pOld && m_pHex)
    {
        pOld->nBpr = m_pHex->GetBytesPerRow();
        pOld->caret = m_pHex->GetCaret();
        m_pHex->GetSelection(pOld->selStart, pOld->selEnd);
    }
    m_nCurTab = nIndex;
    BindViewToTab(nIndex);
    ShowTabStrip();
    EnsureTabVisible(nIndex); // 标签条放不下时自动左移，新标签立即完整可见
    // 空文件必须插入模式才能输入
    if (m_pHex)
        m_pHex->SetInsertMode(true);
    SyncModeUi();
    Notify(I18n::T(L"已新建空文件，直接输入即可写入（插入模式）。"));
}

void CMainFrame::OnActFind()
{
    ShowFindWnd(0);
}

void CMainFrame::OnActFindNext()
{
    if (m_pFindWnd && ::IsWindowVisible(m_pFindWnd->m_hWnd))
        OnBtnFindNext();
    else
        ShowFindWnd(0);
}

void CMainFrame::OnActFindPrev()
{
    if (m_pFindWnd && ::IsWindowVisible(m_pFindWnd->m_hWnd))
        OnBtnFindPrev();
    else
        ShowFindWnd(0);
}

void CMainFrame::OnActGoto()
{
    ShowFindWnd(2);
}

void CMainFrame::OnActUndo()
{
    if (m_pHex)
        m_pHex->Undo();
}

void CMainFrame::OnActRedo()
{
    if (m_pHex)
        m_pHex->Redo();
}

void CMainFrame::OnActCut()
{
    if (m_pHex)
        m_pHex->Cut();
}

void CMainFrame::OnActCopy()
{
    if (m_pHex)
        m_pHex->Copy();
}

void CMainFrame::OnActPaste()
{
    if (m_pHex)
        m_pHex->Paste();
}

void CMainFrame::OnActDelete()
{
    if (m_pHex)
        m_pHex->DeleteSelection();
}

void CMainFrame::OnActSelectAll()
{
    if (m_pHex)
        m_pHex->SelectAll();
}

void CMainFrame::OnActToggleInsertMode()
{
    if (!m_pHex)
        return;
    m_pHex->SetInsertMode(!m_pHex->IsInsertMode());
    SyncModeUi();
    UpdateStatus();
}

void CMainFrame::SyncModeUi()
{
    // 工具栏“模式”按钮：插入模式下高亮提示
    SWindow *pBtn = FindChildByName2<SWindow>(L"tb_mode");
    if (pBtn && m_pHex)
    {
        bool bIns = m_pHex->IsInsertMode();
        pBtn->SetWindowText(bIns ? L"\xE710" : L"\xE70F");
    }
    if (m_pTxtMode && m_pHex)
        m_pTxtMode->SetWindowText(m_pHex->IsInsertMode() ? I18n::T(L"模式: 插入") : I18n::T(L"模式: 覆盖"));
}

void CMainFrame::OnActToggleInspector()
{
    BOOL bShow = TRUE;
    if (m_pInspWnd && ::IsWindow(m_pInspWnd->m_hWnd))
        bShow = !::IsWindowVisible(m_pInspWnd->m_hWnd);
    ShowInspectorWnd(bShow);
}

void CMainFrame::OnActToggleStatusBar()
{
    if (!m_pStatusBar)
        return;
    BOOL bShow = !m_pStatusBar->IsVisible(TRUE);
    m_pStatusBar->SetAttribute(L"display", bShow ? L"1" : L"0");
    m_pStatusBar->SetVisible(bShow, TRUE);
}

void CMainFrame::OnActToggleToolbar()
{
    if (!m_pToolbar)
        return;
    BOOL bShow = !m_pToolbar->IsVisible(TRUE);
    m_pToolbar->SetAttribute(L"display", bShow ? L"1" : L"0");
    m_pToolbar->SetVisible(bShow, TRUE);
}

// ---------------------------------------------------------------------------
// 每行字节数：可编辑下拉组合框（可输入任意整数，下拉选 12 个预置值）。
// kMinBpr/kMaxBpr、ParseBprRaw/ClampBpr 见文件顶部匿名 namespace：
// 所有入口统一钳到 1..4096，任何超限（含超 int 范围）都变成 4096。
// 输入过程中一旦数值 >4096 就立即按 4096 生效并延迟回写组合框，
// 不等失焦/回车（组合框没有回车提交事件，否则会一直显示非法假值）。
// 4 列制 nMax 等规避仍在 UpdateScrollBar 中保留，横向浮层滚动条对所有 bpr 显示；
// 大 bpr 的滚动条刷新走 SetBytesPerRow 的延迟路径，避免消息重入。
// ---------------------------------------------------------------------------
static const UINT WM_APP_BPR_SYNC_UI = WM_APP + 0x32; // 输入超限时延迟回写规范值

bool CMainFrame::ApplyBprText(const SStringT &str, bool bCommit)
{
    if (!m_pHex || !m_pCbxBpr || m_bInBprSync)
        return false; // 同步期间忽略重入（SetWindowText 触发的 EN_CHANGE 等）
    SStringT t = str;
    t.TrimBlank();
    if (t.IsEmpty() && !bCommit)
        return false; // 输入中：允许暂时为空
    long long n64 = 0;
    if (!ParseBprRaw(t.c_str(), n64))
    {
        if (bCommit)
        {
            Notify(I18n::T(L"每行字节数无效：请输入 1-4096 的整数。"));
            SyncBprCombo(m_pHex->GetBytesPerRow()); // 回退到当前值
        }
        return false;
    }
    // 超 4096（含 2147483648 这类会令 %d 回绕的大数）统一变成 4096
    const int v = ClampBpr(n64);
    m_pHex->SetBytesPerRow(v);
    RememberBpr(v); // 记入当前标签（切换标签时恢复）
    if (bCommit)
    {
        SStringT norm;
        norm.Format(L"%d", v);
        if (norm != t)
            SyncBprCombo(v); // 仅当显示文本与规范值不同才回写（避免事件重入）
    }
    UpdateStatus();
    return true;
}

void CMainFrame::SyncBprCombo(int nBpr)
{
    // 组合框文本 = 当前值
    if (!m_pCbxBpr || m_bInBprSync)
        return;
    m_bInBprSync = true;
    SStringT t;
    t.Format(L"%d", nBpr);
    m_pCbxBpr->SetWindowText(t);
    m_bInBprSync = false;
}

void CMainFrame::OnBprSelChanged(IEvtArgs *pEvt)
{
    // 下拉列表选择：取项文本提交
    EventCBSelChange *p = sobj_cast<EventCBSelChange>(pEvt);
    if (!p || !m_pCbxBpr)
        return;
    int idx = m_pCbxBpr->GetCurSel();
    if (idx < 0)
        return;
    SStringT txt;
    if (m_pCbxBpr->GetItemText(idx, FALSE, &txt))
        ApplyBprText(txt, true);
}

void CMainFrame::OnBprEditChanged(IEvtArgs *pEvt)
{
    // 可编辑模式：输入变化（EN_CHANGE 经 EVT_RE_NOTIFY 冒泡）。只在值
    // 合法且与当前不同时实时应用（1..4096；大值走延迟刷新避免重入）。
    if (!m_pCbxBpr || !m_pHex)
        return;
    SStringT t = m_pCbxBpr->GetWindowText();
    t.TrimBlank();
    if (t.IsEmpty())
        return;
    long long n64 = 0;
    if (!ParseBprRaw(t.c_str(), n64))
        return;
    if (n64 > kMaxBpr)
    {
        // 超过上限（含 2147483648 这类会令 %d 回绕的超大数）：输入过程中就
        // 立即按 4096 处理，避免组合框长期停留在一个永远不生效的假值。
        // 此处不能直接 SetWindowText（会重入 EN_CHANGE），投递到消息栈退净后
        // 由 OnBprDeferredSyncUi 统一回写规范值。
        const int v = ClampBpr(n64);
        m_pHex->SetBytesPerRow(v);
        RememberBpr(v);
        ::PostMessageW(m_hWnd, WM_APP_BPR_SYNC_UI, (WPARAM)v, 0);
        return;
    }
    if (n64 < kMinBpr)
        return; // 输入中：0 / 负数暂不应用（失焦时钳制并回写为 1）
    if ((int)n64 == m_pHex->GetBytesPerRow())
        return;
    ApplyBprText(t, false);
}

void CMainFrame::OnBprEditKillFocus(IEvtArgs *pEvt)
{
    // 失焦：提交最终值（无效输入回退）
    if (!m_pCbxBpr || !m_pHex)
        return;
    ApplyBprText(m_pCbxBpr->GetWindowText(), true);
}

void CMainFrame::OnActSetBpr(int nBpr)
{
    // 菜单命令（查看→每行 N 字节）：与组合框同一入口
    SStringT t;
    t.Format(L"%d", nBpr);
    m_bInBprSync = true;
    if (m_pCbxBpr)
        m_pCbxBpr->SetWindowText(t);
    m_bInBprSync = false;
    ApplyBprText(t, true);
}

void CMainFrame::OnActAbout()
{
    // 关于窗口改用 SOUI 自绘（CAboutWnd），随应用主题浅/深切换。
    // 原生 MessageBox 的深色只认系统全局设置，无法 per-app 跟随。
    EnsureAboutWnd();
    if (!m_pAboutWnd || !::IsWindow(m_pAboutWnd->m_hWnd))
        return;
    m_pAboutWnd->RefreshText();
    m_pAboutWnd->ApplyTheme(Theme::Get(m_nTheme));
    m_pAboutWnd->ShowCentered(m_hWnd);
}

void CMainFrame::OnActKeys()
{
    SStringT keys = I18n::T(L"Ctrl+O 打开        Ctrl+S 保存        Ctrl+F 查找\n")
                  + I18n::T(L"Ctrl+G 转到        Ctrl+Z 撤销        Ctrl+Y 重做\n")
                  + I18n::T(L"Ctrl+A 全选        Ctrl+C/V/X 复制/粘贴/剪切\n")
                  + I18n::T(L"F3 查找下一个      Shift+F3 查找上一个\n")
                  + I18n::T(L"Tab 切换输入列     Ins 切换插入/覆盖\n")
                  + I18n::T(L"Home/End 行首行尾（Ctrl 为文件首尾）  PgUp/PgDn 翻页");
    SMessageBox(m_hWnd, keys, I18n::T(L"快捷键一览"), MB_OK | MB_ICONINFORMATION);
}

void CMainFrame::OnActNotImplemented(const TCHAR *pszName)
{
    SStringT msg;
    msg.Format(I18n::T(L"「%s」功能尚未实现，后续版本会补上。"), pszName);
    Notify(msg);
}

// ---------------------------------------------------------------------------
// 文件属性 / 校验和 / 只读 / 插入字节 / 填充 / 字符集 / 替换
// ---------------------------------------------------------------------------
void CMainFrame::OnActFileProperties()
{
    hexmodel::HexFileModel *pM = CurModel();
    if (!pM || !pM->IsOpen())
    {
        Notify(I18n::T(L"当前没有打开的文件。"));
        return;
    }
    static const wchar_t *kCharset[] = {L"ANSI", L"UTF-8", L"UTF-16 LE"};
    int cs = m_pHex ? m_pHex->GetCharset() : 0;
    if (cs < 0 || cs > 2)
        cs = 0;
    SStringT msg;
    SStringT fmt = I18n::T(L"路径:\t%s\n\n大小:\t%llu 字节\n原始大小:\t%llu 字节\n")
                 + I18n::T(L"修改状态:\t%s\n只读:\t%s\n每行字节数:\t%d\n字符集:\t%s");
    // 注意：Format 是可变参数函数，实参不会做隐式类型转换，必须显式 .c_str() 传指针。
    // 直接传 SStringW 对象在 MSVC/GCC 下靠对象布局"碰巧能用"，clang 会直接报错
    // (cannot pass object of non-trivial type through variadic method)。
    msg.Format(fmt,
               pM->Path(), (unsigned long long)pM->Size(),
               (unsigned long long)pM->OriginalSize(), (pM->IsModified() ? I18n::T(L"已修改") : I18n::T(L"未修改")).c_str(),
               (pM->IsReadOnly() ? I18n::T(L"是") : I18n::T(L"否")).c_str(), m_pHex ? m_pHex->GetBytesPerRow() : 16,
               kCharset[cs]);
    SMessageBox(m_hWnd, msg, I18n::T(L"文件属性"), MB_OK | MB_ICONINFORMATION);
}

void CMainFrame::OnActChecksum()
{
    hexmodel::HexFileModel *pM = CurModel();
    if (!pM || !pM->IsOpen())
    {
        Notify(I18n::T(L"当前没有打开的文件。"));
        return;
    }
    hexmodel::u64 start = 0, len = pM->Size();
    SStringT scope = I18n::T(L"整个文件");
    hexmodel::u64 s = 0, e = 0;
    if (m_pHex && m_pHex->GetSelection(s, e) && e > s)
    {
        start = s;
        len = e - s;
        scope.Format(I18n::T(L"选区 [0x%llX, 0x%llX)"), (unsigned long long)s, (unsigned long long)e);
    }
    if (len == 0)
    {
        Notify(I18n::T(L"文件为空，无法计算校验和。"));
        return;
    }
    HCURSOR hOld = ::SetCursor(::LoadCursor(NULL, IDC_WAIT));
    uint32_t crc = 0;
    std::vector<hexmodel::u8> md5, sha1, sha256;
    bool ok = HashRange(pM, start, len, crc, md5, sha1, sha256);
    ::SetCursor(hOld);
    if (!ok)
    {
        SMessageBox(m_hWnd, I18n::T(L"计算失败：系统不支持所需的哈希算法。"), I18n::T(L"校验和"),
                    MB_OK | MB_ICONERROR);
        return;
    }
    SStringT msg;
    msg.Format(I18n::T(L"%s\n偏移: 0x%llX\n长度: %llu 字节\n\nCRC32:\t%08X\nMD5:\t%s\nSHA-1:\t%s\nSHA-256:\t%s"),
               scope.c_str(), (unsigned long long)start, (unsigned long long)len, crc,
               ToHex(md5).c_str(), ToHex(sha1).c_str(), ToHex(sha256).c_str());
    SMessageBox(m_hWnd, msg, I18n::T(L"校验和"), MB_OK | MB_ICONINFORMATION);
}

void CMainFrame::OnActToggleReadOnly()
{
    hexmodel::HexFileModel *pM = CurModel();
    if (!pM || !pM->IsOpen())
    {
        Notify(I18n::T(L"当前没有打开的文件。"));
        return;
    }
    pM->SetReadOnly(!pM->IsReadOnly());
    UpdateTitle();
    UpdateStatus();
    Notify(pM->IsReadOnly() ? I18n::T(L"已切换为只读模式：所有编辑操作将被拒绝。")
                            : I18n::T(L"已关闭只读模式。"));
}

void CMainFrame::OnActToggleFastSave()
{
    m_bFastSaveLarge = !m_bFastSaveLarge;
    for (size_t i = 0; i < m_tabs.size(); i++)
    {
        if (m_tabs[i].pModel)
            m_tabs[i].pModel->SetFastSaveEnabled(m_bFastSaveLarge);
    }
    Notify(m_bFastSaveLarge
               ? I18n::T(L"已开启快速保存：大文件纯覆盖编辑只写修改的页（快，但非原子）。")
               : I18n::T(L"已关闭快速保存：保存时始终整文件重写（原子，慢）。"));
}

void CMainFrame::OnActInsertByte()
{
    hexmodel::HexFileModel *pM = CurModel();
    if (!pM || !pM->IsOpen())
    {
        Notify(I18n::T(L"当前没有打开的文件。"));
        return;
    }
    if (pM->IsReadOnly())
    {
        Notify(I18n::T(L"文件为只读，无法插入字节。"));
        return;
    }
    std::wstring cnt;
    if (!PromptText(m_hWnd, I18n::T(L"插入字节"), I18n::T(L"要插入的字节数（默认 1）："), L"1", cnt))
        return;
    int n = _wtoi(cnt.c_str());
    if (n <= 0)
    {
        Notify(I18n::T(L"字节数无效。"));
        return;
    }
    if (n > 1024 * 1024)
    {
        Notify(I18n::T(L"一次最多插入 1 MB。"));
        return;
    }
    hexmodel::u64 pos = m_pHex ? m_pHex->GetCaret() : 0;
    if (pos > pM->Size())
        pos = pM->Size();
    std::vector<hexmodel::u8> z((size_t)n, 0);
    pM->BeginUndo();
    bool ok = pM->Insert(pos, &z[0], (hexmodel::u64)n);
    pM->EndUndo();
    if (!ok)
    {
        Notify(I18n::T(L"插入失败。"));
        return;
    }
    if (m_pHex)
    {
        m_pHex->RefreshView();
        m_pHex->SetSelection(pos + n, pos + n);
        m_pHex->ScrollToOffset(pos, false);
    }
    SStringT m;
    m.Format(I18n::T(L"已在偏移 0x%llX 插入 %d 个 0x00 字节。"), (unsigned long long)pos, n);
    Notify(m);
}

void CMainFrame::OnActFillSelection()
{
    hexmodel::HexFileModel *pM = CurModel();
    if (!pM || !pM->IsOpen())
    {
        Notify(I18n::T(L"当前没有打开的文件。"));
        return;
    }
    if (pM->IsReadOnly())
    {
        Notify(I18n::T(L"文件为只读，无法填充。"));
        return;
    }
    hexmodel::u64 s = 0, e = 0;
    if (!m_pHex || !m_pHex->GetSelection(s, e) || e <= s)
    {
        Notify(I18n::T(L"请先选择要填充的区间。"));
        return;
    }
    std::wstring val;
    if (!PromptText(m_hWnd, I18n::T(L"填充选区"), I18n::T(L"填充字节（十六进制，如 FF；默认 00）："), L"00", val))
        return;
    std::vector<hexmodel::u8> pat;
    if (!ParseByteText(val.c_str(), true, pat) || pat.size() != 1)
    {
        SMessageBox(m_hWnd, I18n::T(L"请输入 1 个字节的十六进制值，如 00、FF、7F。"), I18n::T(L"填充选区"),
                    MB_OK | MB_ICONWARNING);
        return;
    }
    hexmodel::u8 v = pat[0];
    const size_t CH = 64 * 1024;
    std::vector<hexmodel::u8> buf(CH, v);
    pM->BeginUndo();
    for (hexmodel::u64 p = s; p < e; p += CH)
    {
        size_t n = (size_t)((e - p) < CH ? (e - p) : CH);
        pM->Write(p, &buf[0], (hexmodel::u64)n);
    }
    pM->EndUndo();
    if (m_pHex)
        m_pHex->RefreshView();
    SStringT m;
    m.Format(I18n::T(L"已用 0x%02X 填充 %llu 字节。"), v, (unsigned long long)(e - s));
    Notify(m);
}

void CMainFrame::OnActCharset()
{
    if (!m_pHex)
        return;
    int cs = (m_pHex->GetCharset() + 1) % 3;
    m_bInCharsetSync = true;
    if (m_pCbxCharset)
        m_pCbxCharset->SetCurSel(cs);
    m_bInCharsetSync = false;
    m_pHex->SetCharset(cs);
    static const wchar_t *kNames[] = {L"ANSI", L"UTF-8", L"UTF-16 LE"};
    SStringT m;
    m.Format(I18n::T(L"字符集已切换为 %s。"), kNames[cs]);
    Notify(m);
}

// ---------------------------------------------------------------------------
// 主题：浅色 / 深色
// ---------------------------------------------------------------------------
void CMainFrame::LoadTheme()
{
    m_nTheme = Theme::LoadSavedIndex();
}

void CMainFrame::SaveTheme()
{
    Theme::SaveIndex(m_nTheme);
}

void CMainFrame::OnActSetTheme(int nTheme)
{
    if (nTheme < 0 || nTheme >= Theme::GetCount())
        nTheme = Theme::LIGHT;
    m_nTheme = nTheme;
    // 原生菜单（主菜单下拉、标签栏 ▾ 的文件列表、右键菜单）走 Windows 的暗色菜单开关
    Theme::ApplyNativeMenuTheme(Theme::Get(m_nTheme).bDark);
    Theme::AllowDarkWindow(GetHwnd(), Theme::Get(m_nTheme).bDark);
    ApplyTheme();
    SaveTheme();
    Notify(SStringT(I18n::T(L"已切换到")) + I18n::T(Theme::Get(m_nTheme).pszName) + I18n::T(L"主题。"));
}

void CMainFrame::OnActLanguage(int nLang)
{
    if (nLang == I18n::Current())
        return;
    if (!I18n::SetLanguage(nLang))
    {
        SMessageBox(m_hWnd, I18n::T(L"切换语言失败。"), L"HexEditor", MB_OK | MB_ICONERROR);
        return;
    }
    // UM_SETLANGUAGE 由 SHostMgr 用 PostMessage 异步广播，且会先把窗口标题
    // 重译成默认标题；这里再投递一条消息，等各宿主处理完语言切换后统一刷新
    // 动态文本（标题/状态栏/模式按钮/菜单勾选/检查器）。
    PostMessage(WM_APP + 0x35);
}

LRESULT CMainFrame::OnDeferredLangRefresh(UINT, WPARAM, LPARAM)
{
    UpdateTitle();
    UpdateStatus();
    SyncModeUi();
    UpdateMenuStates();
    if (m_pInspWnd && ::IsWindow(m_pInspWnd->m_hWnd))
        UpdateInspector();
    if (m_pAboutWnd && ::IsWindow(m_pAboutWnd->m_hWnd))
        m_pAboutWnd->RefreshText();
    // 查找浮窗的 HEX/文本 切换按钮文字是代码动态设置的，翻译器不会重译，需手动刷新
    if (m_pFindWnd && ::IsWindow(m_pFindWnd->m_hWnd))
        m_pFindWnd->SetHexMode(m_bSearchHex);
    return 0;
}

// 十六进制区 + 窗口外框
void CMainFrame::ApplyTheme()
{
    const STheme &th = Theme::Get(m_nTheme);

    // ---- 十六进制视图 ----
    // HexView 的 ATTR_COLOR 都是 bInvalidate=FALSE，改完要手动 Invalidate。
    if (m_pHex)
    {
        m_pHex->SetAttribute(L"colorBg", ColorToAttr(th.hex.crBg));
        m_pHex->SetAttribute(L"colorBgAlt", ColorToAttr(th.hex.crBgAlt));
        m_pHex->SetAttribute(L"colorOffset", ColorToAttr(th.hex.crOffset));
        // 表头列偏移的高位小标（bpr>256 时显示）：用正文 hex 色，比淡灰的低位
        // 数字更醒目；深浅主题下都随 crHex 自动翻转。
        m_pHex->SetAttribute(L"colorHeader", ColorToAttr(th.hex.crHex));
        m_pHex->SetAttribute(L"colorHex", ColorToAttr(th.hex.crHex));
        m_pHex->SetAttribute(L"colorAscii", ColorToAttr(th.hex.crAscii));
        m_pHex->SetAttribute(L"colorSelBg", ColorToAttr(th.hex.crSelBg));
        m_pHex->SetAttribute(L"colorSelText", ColorToAttr(th.hex.crSelText));
        m_pHex->SetAttribute(L"colorCaret", ColorToAttr(th.hex.crCaret));
        m_pHex->SetAttribute(L"colorGrid", ColorToAttr(th.hex.crGrid));
        // 滚动条皮肤：深色下换成深色滚动条（浅色仍用系统默认）。
        // SPanel::OnAttrScrollbarSkin 运行时会替换 m_pSkinSb，下一次绘制即生效。
        m_pHex->SetAttribute(L"sbSkin",
                             th.bDark ? SStringW(L"skin_sb_dark") : SStringW(L"_skin.sys.scrollbar"));
        m_pHex->Invalidate();
    }

    ApplyChromeTheme();

    // ---- 数据检查器（独立 SHostWnd，需单独下发）----
    if (m_pInspWnd && ::IsWindow(m_pInspWnd->m_hWnd))
        m_pInspWnd->ApplyTheme(th);

    // ---- 查找 / 替换浮窗（独立 SHostWnd，需单独下发）----
    if (m_pFindWnd && ::IsWindow(m_pFindWnd->m_hWnd))
        m_pFindWnd->ApplyTheme(th);

    // ---- 关于窗口（独立 SHostWnd，需单独下发）----
    if (m_pAboutWnd && ::IsWindow(m_pAboutWnd->m_hWnd))
        m_pAboutWnd->ApplyTheme(th);
}

// 标题栏 / 菜单栏 / 工具栏 / 标签栏 / 状态栏
void CMainFrame::ApplyChromeTheme()
{
    const SChrome &c = Theme::Get(m_nTheme).chrome;

    const SStringW strCaption = ColorToAttr(c.crCaption);
    const SStringW strBar = ColorToAttr(c.crBar);
    const SStringW strStatus = ColorToAttr(c.crStatus);
    const SStringW strContent = ColorToAttr(c.crContent);
    const SStringW strBorder = ColorToAttr(c.crBorder);
    const SStringW strTitle = ColorToAttr(c.crTitle);
    const SStringW strText = ColorToAttr(c.crText);
    const SStringW strDim = ColorToAttr(c.crTextDim);
    const SStringW strGlyph = ColorToAttr(c.crGlyph);
    const bool     bDark = Theme::Get(m_nTheme).bDark;

    // 根窗口：内容区底色（hexview 之外的空隙由它填）
    if (SWindow *pRoot = GetRoot())
        pRoot->SetAttribute(L"colorBkgnd", strContent);

    // ---- 标题栏 ----
    if (SWindow *p = FindChildByName(L"caption"))
        p->SetAttribute(L"colorBkgnd", strCaption);
    if (m_pTxtTitle)
        m_pTxtTitle->SetAttribute(L"colorText", strTitle);
    const wchar_t *arrCapBtn[] = {L"btn_min", L"btn_max", L"btn_restore", L"btn_close"};
    for (int i = 0; i < _countof(arrCapBtn); i++)
    {
        if (SWindow *p = FindChildByName(arrCapBtn[i]))
            p->SetAttribute(L"colorText", strGlyph);
    }

    // ---- 菜单栏 + 下拉弹层 ----
    // 下拉是 SOUI 自绘的 SMenuEx。这里只改菜单栏自身颜色；下拉弹层的配色要重建
    // （条目构造时已固化 itemSkin），重建推迟到下次弹出前（OnPopMenu）。
    if (SWindow *pMenuBar = FindChildByName(L"menubar"))
    {
        pMenuBar->SetAttribute(L"colorBkgnd", strBar);
        for (SWindow *p = pMenuBar->GetWindow(GSW_FIRSTCHILD); p; p = p->GetWindow(GSW_NEXTSIBLING))
            p->SetAttribute(L"colorText", strText);
    }

    // ---- 工具栏：底色 + 所有子项文字（按钮字形 / "每行""字符集"标签）----
    if (m_pToolbar)
    {
        m_pToolbar->SetAttribute(L"colorBkgnd", strBar);
        for (SWindow *p = m_pToolbar->GetWindow(GSW_FIRSTCHILD); p; p = p->GetWindow(GSW_NEXTSIBLING))
            p->SetAttribute(L"colorText", strText);
    }
    const wchar_t *arrSep[] = {L"sep1", L"sep2", L"sep3"};
    for (int i = 0; i < _countof(arrSep); i++)
    {
        if (SWindow *p = FindChildByName(arrSep[i]))
            p->SetAttribute(L"colorBkgnd", strBorder);
    }
    // 两个下拉框：底色跟随工具栏（内部 edit 会把父窗口背景 BitBlt 过来）。
    // 另外三处要单独处理：内部 edit 的文字色、下拉箭头皮肤、下拉列表底色。
    SWindow *arrCbx[] = {m_pCbxBpr, m_pCbxCharset};
    for (int i = 0; i < _countof(arrCbx); i++)
    {
        SWindow *pCbx = arrCbx[i];
        if (!pCbx)
            continue;
        pCbx->SetAttribute(L"colorBkgnd", strBar);
        pCbx->SetAttribute(L"btnSkin", bDark ? L"skin_dropbtn_dark" : L"_skin.sys.dropbtn");
        // 内部 edit 是独立子窗口，文字色不继承
        for (SWindow *p = pCbx->GetWindow(GSW_FIRSTCHILD); p; p = p->GetWindow(GSW_NEXTSIBLING))
            p->SetAttribute(L"colorText", strText);
        // 下拉列表（SComboBox 在 InitFromXml 时就建好了，不用等展开）
        SComboBox *pCombo = sobj_cast<SComboBox>(pCbx);
        if (pCombo)
        {
            SListBox *pList = pCombo->GetListBox();
            if (pList)
            {
                pList->SetAttribute(L"colorBkgnd", bDark ? strContent : SStringW(L"#ffffff"));
                pList->SetAttribute(L"colorText", strText);
                pList->Invalidate();
            }
        }
        pCbx->Invalidate();
    }

    // ---- 标签条（多文件时显示）----
    if (m_pTabBarWrap)
        m_pTabBarWrap->SetAttribute(L"colorBkgnd", strStatus);
    if (m_pTabFilesEx)
    {
        // 标签页皮肤：深色下换成 colorrect（选中态走 PushDown），浅色回系统皮肤
        m_pTabFilesEx->SetAttribute(L"tabSkin",
                                    bDark ? L"skin_tab_dark" : L"_skin.sys.tab.header");
        // 标签条空白处（末标签右侧）也要跟随外框底色，否则会露出另一种灰。
        m_pTabFilesEx->SetAttribute(L"colorBkgnd", strStatus);
        m_pTabFilesEx->SetAttribute(L"colorText", strDim);
        m_pTabFilesEx->SetAttribute(L"colorTextHover", strTitle);
        m_pTabFilesEx->Invalidate();
    }
    const wchar_t *arrTabBtn[] = {L"tab_scroll_left", L"tab_scroll_right", L"tab_list_files"};
    for (int i = 0; i < _countof(arrTabBtn); i++)
    {
        if (SWindow *p = FindChildByName(arrTabBtn[i]))
        {
            p->SetAttribute(L"colorBkgnd", strStatus);
            p->SetAttribute(L"colorText", strGlyph);
        }
    }

    // ---- 状态栏 ----
    if (m_pStatusBar)
        m_pStatusBar->SetAttribute(L"colorBkgnd", strStatus);
    SWindow *arrStat[] = {m_pTxtSize, m_pTxtOffset, m_pTxtSel, m_pTxtMode};
    for (int i = 0; i < _countof(arrStat); i++)
    {
        if (arrStat[i])
            arrStat[i]->SetAttribute(L"colorText", strDim);
    }

    // ---- 下拉弹层（SMenuEx）----
    // 菜单窗口是复用的，只改属性不重建，所以这里可以立即下发：不用重启，也不用等下次弹出。
    ApplyMenuThemeAll();
}

// 把当前配色下发给一个下拉菜单（递归到子菜单）。
// 只改已有窗口的属性，**不销毁、不重建**：
//   1. 条目背景皮肤是构造时从 root 的 itemSkin 拷贝的，但 SWindow 自己的 skin
//      属性可以在运行中改，改完 OnEraseBkgnd 立刻用新皮肤；
//   2. 文字色 / 勾选图标 / 子菜单箭头是绘制时现查的，改 root 上的属性即生效；
//   3. 菜单窗口是复用的（ShowMenu 每次都重新 CalcMenuSize + 重绘），
//      所以下次弹出时尺寸和配色都是新的。
// 反过来「销毁 + LoadMenu2」的做法不能用：重建出来的窗口是黑的（不再绘制），
// 在菜单显示期间重建还会把 SMenuBar::m_pNowMenu 卡死，菜单从此打不开。
void CMainFrame::ApplyMenuTheme(SMenuEx *pMenu)
{
    if (!pMenu)
        return;
    const STheme &th = Theme::Get(m_nTheme);
    const bool bDark = th.bDark;
    const SStringW strText = ColorToAttr(th.chrome.crText);
    const SStringW strItemSkin = bDark ? L"skin_menu_dark" : L"_skin.sys.menu.skin";
    const SStringW strSepSkin = bDark ? L"skin_menu_sep_dark" : L"_skin.sys.menu.sep";

    if (SWindow *pRoot = pMenu->GetRoot())
    {
        pRoot->SetAttribute(L"colorText", strText);
        pRoot->SetAttribute(L"colorBkgnd", bDark ? L"#252526" : L"#FFFFFF");
        pRoot->SetAttribute(L"checkSkin",
                            bDark ? L"skin_menu_check_dark" : L"_skin.sys.menu.check");
        pRoot->SetAttribute(L"arrowSkin",
                            bDark ? L"skin_menu_arrow_dark" : L"_skin.sys.menu.arrow");
        pRoot->Invalidate();
    }

    int nCount = pMenu->GetMenuItemCount();
    for (int i = 0; i < nCount; i++)
    {
        SMenuExItem *pItem = pMenu->GetMenuItem(i, FALSE);
        if (!pItem)
            continue;
        pItem->SetAttribute(L"skin", pItem->IsSeperator() ? strSepSkin : strItemSkin);
        pItem->SetAttribute(L"colorText", strText);
        pItem->Invalidate();
        if (SMenuEx *pSub = pItem->GetSubMenu())
            ApplyMenuTheme(pSub);
    }
}

// 所有主菜单下拉（顺序必须与 main.xml 里 <menus> 的声明顺序无关，按索引遍历即可）
void CMainFrame::ApplyMenuThemeAll()
{
    SMenuBar *pMenuBar = FindChildByName2<SMenuBar>(L"menubar");
    if (!pMenuBar)
        return;
    // GetMenuEx 越界返回 NULL；菜单数量少，直接按索引扫（容忍个别菜单加载失败）
    for (DWORD i = 0; i < 16; i++)
    {
        SMenuEx *pMenu = pMenuBar->GetMenuEx(i);
        if (!pMenu)
            continue;
        ApplyMenuTheme(pMenu);
    }
}

void CMainFrame::OnCharsetSelChanged(IEvtArgs *pEvt)
{
    if (m_bInCharsetSync || !m_pCbxCharset || !m_pHex)
        return;
    int idx = m_pCbxCharset->GetCurSel();
    if (idx < 0 || idx > 2)
        return;
    m_pHex->SetCharset(idx);
    static const wchar_t *kNames[] = {L"ANSI", L"UTF-8", L"UTF-16 LE"};
    SStringT m;
    m.Format(I18n::T(L"字符集已切换为 %s。"), kNames[idx]);
    Notify(m);
}

bool CMainFrame::FindSync(const std::vector<hexmodel::u8> &pat, hexmodel::u64 from,
                          bool bBackward, hexmodel::u64 &pos)
{
    hexmodel::HexFileModel *pM = CurModel();
    if (!pM->IsOpen() || pat.empty())
        return false;
    if (m_search.IsRunning())
    {
        m_search.Stop();
        m_search.Wait();
    }
    KillTimer(TIMER_SEARCH);
    hexmodel::u64 end = pM->Size() ? pM->Size() - 1 : 0;
    if (from > end)
        from = 0;
    if (!m_search.Start(&m_source, &pat[0], pat.size(), from, end, bBackward))
        return false;
    m_search.Wait();
    if (m_search.GetState() == hexsearch::HexSearch::FOUND)
    {
        pos = m_search.GetResult();
        return true;
    }
    return false;
}

void CMainFrame::OnActReplace()
{
    ShowFindWnd(1);
    if (!CurModel()->IsOpen())
        Notify(I18n::T(L"请先打开文件。"));
}

void CMainFrame::OnBtnReplace()
{
    hexmodel::HexFileModel *pM = CurModel();
    if (!pM || !pM->IsOpen())
    {
        Notify(I18n::T(L"当前没有打开的文件。"));
        return;
    }
    if (pM->IsReadOnly())
    {
        Notify(I18n::T(L"文件为只读，无法替换。"));
        return;
    }
    std::vector<hexmodel::u8> pat, rep;
    if (!ParseByteText(m_pFindWnd ? m_pFindWnd->GetFindText() : SStringT(), m_bSearchHex, pat))
    {
        Notify(I18n::T(L"查找内容无效。"));
        return;
    }
    SStringT repStr = m_pFindWnd ? m_pFindWnd->GetReplaceText() : SStringT();
    repStr.TrimBlank();
    if (!repStr.IsEmpty() && !ParseByteText(repStr, m_bSearchHex, rep))
    {
        Notify(I18n::T(L"替换内容无效。"));
        return;
    }

    hexmodel::u64 s = 0, e = 0;
    bool hasSel = m_pHex && m_pHex->GetSelection(s, e);
    bool matchSel = false;
    if (hasSel && (e - s) == pat.size())
    {
        std::vector<hexmodel::u8> cur((size_t)(e - s));
        pM->Read(s, &cur[0], e - s);
        matchSel = (cur == pat);
    }
    hexmodel::u64 pos = 0;
    if (matchSel)
        pos = s;
    else if (!FindSync(pat, hasSel ? e : 0, false, pos))
    {
        Notify(I18n::T(L"未找到匹配内容。"));
        return;
    }

    pM->BeginUndo();
    pM->Delete(pos, (hexmodel::u64)pat.size());
    if (!rep.empty())
        pM->Insert(pos, &rep[0], (hexmodel::u64)rep.size());
    pM->EndUndo();

    if (m_pHex)
    {
        m_pHex->RefreshView();
        m_pHex->SetHitRange(pos, pos + rep.size());
        m_pHex->SetSelection(pos + rep.size(), pos + rep.size());
        m_pHex->ScrollToOffset(pos, false);
    }
    Notify(I18n::T(L"已替换 1 处。"));
}

void CMainFrame::OnBtnReplaceAll()
{
    hexmodel::HexFileModel *pM = CurModel();
    if (!pM || !pM->IsOpen())
    {
        Notify(I18n::T(L"当前没有打开的文件。"));
        return;
    }
    if (pM->IsReadOnly())
    {
        Notify(I18n::T(L"文件为只读，无法替换。"));
        return;
    }
    std::vector<hexmodel::u8> pat, rep;
    if (!ParseByteText(m_pFindWnd ? m_pFindWnd->GetFindText() : SStringT(), m_bSearchHex, pat))
    {
        Notify(I18n::T(L"查找内容无效。"));
        return;
    }
    SStringT repStr = m_pFindWnd ? m_pFindWnd->GetReplaceText() : SStringT();
    repStr.TrimBlank();
    if (!repStr.IsEmpty() && !ParseByteText(repStr, m_bSearchHex, rep))
    {
        Notify(I18n::T(L"替换内容无效。"));
        return;
    }

    hexmodel::u64 from = 0, pos = 0;
    int count = 0;
    pM->BeginUndo();
    while (from <= pM->Size() && FindSync(pat, from, false, pos))
    {
        if (!pM->Delete(pos, (hexmodel::u64)pat.size()))
            break;
        if (!rep.empty())
            pM->Insert(pos, &rep[0], (hexmodel::u64)rep.size());
        from = pos + rep.size();
        if (++count >= 1000000)
            break;
    }
    pM->EndUndo();

    if (m_pHex)
    {
        m_pHex->RefreshView();
        m_pHex->SetHitRange(0, 0);
    }
    SStringT m;
    m.Format(I18n::T(L"全部替换完成，共替换 %d 处。"), count);
    Notify(m);
}

void CMainFrame::ShowFindBar(BOOL bShow)
{
    if (bShow)
        ShowFindWnd(0);
    else if (m_pFindWnd && ::IsWindow(m_pFindWnd->m_hWnd))
        m_pFindWnd->ShowWindow(SW_HIDE);
}

void CMainFrame::ShowGotoBar(BOOL bShow)
{
    if (bShow)
        ShowFindWnd(2);
    else if (m_pFindWnd && ::IsWindow(m_pFindWnd->m_hWnd))
        m_pFindWnd->ShowWindow(SW_HIDE);
}

// ---------------------------------------------------------------------------
// 查找
// ---------------------------------------------------------------------------
bool CMainFrame::ParsePattern(std::vector<u8> &out)
{
    if (!m_pFindWnd)
        return false;
    SStringT str = m_pFindWnd->GetFindText();
    out.clear();
    if (m_bSearchHex)
    {
        // 允许 "DE AD BE EF"、"DEADBEEF"、"de ad be ef" 等写法
        SStringA a = S_CT2A(str, CP_UTF8);
        int byte = -1;
        for (int i = 0; i < a.GetLength(); i++)
        {
            char ch = a[i];
            int v = -1;
            if (ch >= '0' && ch <= '9')
                v = ch - '0';
            else if (ch >= 'a' && ch <= 'f')
                v = 10 + ch - 'a';
            else if (ch >= 'A' && ch <= 'F')
                v = 10 + ch - 'A';
            if (v < 0)
            {
                if (ch == ' ' || ch == '\t')
                    continue;
                return false;
            }
            if (byte < 0)
                byte = v;
            else
            {
                out.push_back((u8)((byte << 4) | v));
                byte = -1;
            }
        }
        if (byte >= 0)
            return false; // 奇数个 nibble
    }
    else
    {
        SStringA a = S_CT2A(str, CP_UTF8);
        if (a.IsEmpty())
            return false;
        out.assign((const u8 *)a.c_str(), (const u8 *)a.c_str() + a.GetLength());
    }
    return !out.empty();
}

void CMainFrame::RunSearch(bool bBackward)
{
    if (!CurModel()->IsOpen())
        return;
    std::vector<u8> pat;
    if (!m_bHasLastFind || true)
    {
        if (!ParsePattern(pat))
        {
            Notify(I18n::T(L"查找内容无效。十六进制模式请按字节对输入，如 4D 5A。"));
            return;
        }
        m_lastPattern = pat;
        m_bHasLastFind = true;
    }
    else
    {
        pat = m_lastPattern;
    }

    if (m_search.IsRunning())
        return;

    u64 from = 0, to = CurModel()->Size() - 1;
    u64 s, e;
    if (m_pHex && m_pHex->GetSelection(s, e))
        from = bBackward ? s : e;
    if (from >= CurModel()->Size())
        from = 0;

    if (!m_search.Start(&m_source, &m_lastPattern[0], m_lastPattern.size(), from, to, bBackward))
    {
        Notify(I18n::T(L"无法启动搜索。"));
        return;
    }
    SetTimer(TIMER_SEARCH, 100);
}

void CMainFrame::FinishSearch()
{
    hexsearch::HexSearch::State st = m_search.GetState();
    if (st == hexsearch::HexSearch::FOUND)
    {
        u64 pos = m_search.GetResult();
        u64 end = pos + m_lastPattern.size();
        if (end > CurModel()->Size())
            end = CurModel()->Size();
        if (m_pHex)
        {
            m_pHex->SetHitRange(pos, end);
            m_pHex->SetSelection(pos, end);
            m_pHex->ScrollToOffset(pos, false);
        }
        Notify(NULL);
    }
    else if (st == hexsearch::HexSearch::NOT_FOUND)
    {
        m_search.Stop();
        m_search.Wait();
        if (m_pHex)
            m_pHex->SetHitRange(0, 0);
        Notify(I18n::T(L"未找到匹配内容。"));
    }
    else if (st == hexsearch::HexSearch::STOPPED)
    {
        m_search.Stop();
        m_search.Wait();
    }
}

void CMainFrame::OnBtnFindNext()
{
    RunSearch(false);
}

void CMainFrame::OnBtnFindPrev()
{
    RunSearch(true);
}

void CMainFrame::OnBtnFindHex()
{
    m_bSearchHex = !m_bSearchHex;
    if (m_pFindWnd)
    {
        m_pFindWnd->SetHexMode(m_bSearchHex);
        m_pFindWnd->SetStatus(m_bSearchHex ? I18n::T(L"十六进制模式：按字节对输入，如 4D 5A")
                                           : I18n::T(L"文本模式"));
    }
    if (m_pHex)
        Notify(m_bSearchHex ? I18n::T(L"已切换为十六进制查找（按字节输入）") : I18n::T(L"已切换为文本查找"));
}

void CMainFrame::OnBtnFindClose()
{
    ShowFindBar(FALSE);
    if (m_pHex)
        m_pHex->SetHitRange(0, 0);
    Notify(NULL);
}

void CMainFrame::OnBtnGotoOk()
{
    if (!m_pFindWnd)
        return;
    SStringT str = m_pFindWnd->GetGotoText();
    str.TrimBlank();
    u64 pos = 0;
    BOOL bOk = FALSE;
    if (str.GetLength() > 2 && (str.Left(2).CompareNoCase(L"0x") == 0))
    {
        bOk = 1 == swscanf(str, L"0x%llx", (unsigned long long *)&pos);
    }
    else
    {
        bOk = 1 == swscanf(str, L"%llu", (unsigned long long *)&pos);
    }
    if (!bOk)
    {
        Notify(I18n::T(L"偏移格式无效。"));
        return;
    }
    if (m_pHex)
    {
        if (pos >= CurModel()->Size() && CurModel()->Size() > 0)
            pos = CurModel()->Size() - 1;
        m_pHex->SetSelection(pos, pos);
        m_pHex->ScrollToOffset(pos, true);
    }
    ShowGotoBar(FALSE);
    Notify(NULL);
}

void CMainFrame::OnBtnGotoClose()
{
    ShowGotoBar(FALSE);
    Notify(NULL);
}

void CMainFrame::Notify(const TCHAR *pszMsg)
{
    if (!m_pTxtSel)
        return;
    if (!pszMsg)
    {
        m_pTxtSel->SetWindowText(L"");
        return;
    }
    m_pTxtSel->SetWindowText(pszMsg);
}

// ---------------------------------------------------------------------------
// 状态栏与数据检查器
// ---------------------------------------------------------------------------
void CMainFrame::UpdateStatus()
{
    wchar_t buf[256];
    if (m_pTxtSize)
    {
        if (CurModel()->IsOpen())
            swprintf(buf, 255, I18n::T(L"大小: %llu 字节"), (unsigned long long)CurModel()->Size());
        else
            swprintf(buf, 255, I18n::T(L"大小: 0"));
        m_pTxtSize->SetWindowText(buf);
    }
    if (m_pTxtOffset && m_pHex)
    {
        swprintf(buf, 255, I18n::T(L"偏移: 0x%llX"), (unsigned long long)m_pHex->GetCaret());
        m_pTxtOffset->SetWindowText(buf);
    }
    if (m_pTxtSel && m_pHex)
    {
        u64 s, e;
        if (m_pHex->GetSelection(s, e))
            swprintf(buf, 255, I18n::T(L"选择: %llu 字节"), (unsigned long long)(e - s));
        else
            swprintf(buf, 255, I18n::T(L"选择: 0"));
        m_pTxtSel->SetWindowText(buf);
    }
    if (m_pTxtMode && m_pHex)
    {
        m_pTxtMode->SetWindowText(m_pHex->IsInsertMode() ? I18n::T(L"模式: 插入") : I18n::T(L"模式: 覆盖"));
    }
    // 当前文件名 → 检查器悬浮窗口最后一行。主窗口自身的 txt_file 控件已随
    // 内嵌面板移除（改独立窗口），历史代码把写入挂在 if (m_pTxtFile) 下——
    // m_pTxtFile 为 NULL 时整块跳过导致检查器一直显示"未打开文件"。
    if (CurModel()->IsOpen())
    {
        const TCHAR *p = CurModel()->Path();
        LPCTSTR q = _tcsrchr(p, '\\');
        if (!q)
            q = _tcsrchr(p, '/');
        q = q ? q + 1 : p;
        // 必须走 SStringT::Format，不能裸 swprintf：glibc 的 swprintf 把 %s 当
        // char*（MSVC 才当 wchar_t*），宽字符串会被按窄字符解释、遇到第一个
        // 0x00 就截断——Linux 下这里只显示首字母 "s"。
        SStringT sName = SStringT().Format(L"%s%s", q, CurModel()->IsModified() ? L" *" : L"");
        if (m_pTxtFile)
            m_pTxtFile->SetWindowText(sName);
        if (m_pTxtFileInsp)
            m_pTxtFileInsp->SetWindowText(sName);
    }
    else
    {
        if (m_pTxtFile)
            m_pTxtFile->SetWindowText(I18n::T(L"未打开文件"));
        if (m_pTxtFileInsp)
            m_pTxtFileInsp->SetWindowText(I18n::T(L"未打开文件"));
    }
    UpdateUndoButtons();
}

void CMainFrame::UpdateUndoButtons()
{
    SWindow *pUndo = FindChildByName2<SWindow>(L"tb_undo");
    SWindow *pRedo = FindChildByName2<SWindow>(L"tb_redo");
    if (pUndo)
        pUndo->EnableWindow(CurModel()->CanUndo(), TRUE);
    if (pRedo)
        pRedo->EnableWindow(CurModel()->CanRedo(), TRUE);
}

static u64 rd16(const u8 *p)
{
    return (u64)p[0] | ((u64)p[1] << 8);
}
static u64 rd32(const u8 *p)
{
    return (u64)p[0] | ((u64)p[1] << 8) | ((u64)p[2] << 16) | ((u64)p[3] << 24);
}
static u64 rd64(const u8 *p)
{
    return rd32(p) | (rd32(p + 4) << 32);
}
static i64 s64(u64 v)
{
    return (i64)v;
}
static float f32(const u8 *p)
{
    uint32_t v = (uint32_t)rd32(p);
    float f;
    memcpy(&f, &v, 4);
    return f;
}
static double f64(const u8 *p)
{
    u64 v = rd64(p);
    double d;
    memcpy(&d, &v, 8);
    return d;
}

void CMainFrame::OnInspRowSetFocus(int nWidth)
{
    // 检查器值行获得焦点：hex 列把 [caret, caret+宽度) 框选。
    // 字节/字符/有符号8 → 1 字节；字/有符号16 → 2；双字/有符号32/浮点32 → 4；
    // 四字/有符号64/浮点64 → 8。超出文件尾时钳到 Size。
    // SetSelectionKeepCaret：不移动光标——光标留在原字节（框选只是临时视觉），
    // 下次联动仍以原 caret 为基准，否则选区逐次漂移。
    if (!m_pHex)
        return;
    u64 caret = m_pHex->GetCaret();
    u64 size = CurModel()->IsOpen() ? CurModel()->Size() : 0;
    u64 end = caret + (u64)nWidth;
    if (end > size)
        end = size;
    if (end <= caret)
        return; // 空文件/越界：不框选
    m_pHex->SetSelectionKeepCaret(caret, end);
}

void CMainFrame::OnInspRowKillFocus()
{
    // 值行失焦（点回 hex 区或点其它行）：恢复单点选区（无蓝底框选），
    // 光标保持在原字节不动。
    if (!m_pHex)
        return;
    u64 caret = m_pHex->GetCaret();
    m_pHex->SetSelectionKeepCaret(caret, caret);
}

void CMainFrame::UpdateInspector()
{
    if (!m_pValByte)
        return;
    u8 data[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    u64 caret = m_pHex ? m_pHex->GetCaret() : 0;
    if (CurModel()->IsOpen() && caret < CurModel()->Size())
        CurModel()->Read(caret, data, 8);

    wchar_t buf[96];
    if (caret < CurModel()->Size())
    {
        // 无符号系列（LE 小端序，仅十进制）：与有符号系列对照
        swprintf(buf, 95, L"UInt8: %u", (unsigned)data[0]);
        m_pValByte->SetWindowText(buf);
        swprintf(buf, 95, L"UInt16: %u", (unsigned)rd16(data));
        m_pValWord->SetWindowText(buf);
        swprintf(buf, 95, L"UInt32: %u", (unsigned)rd32(data));
        m_pValDword->SetWindowText(buf);
        swprintf(buf, 95, L"UInt64: %llu", (unsigned long long)rd64(data));
        m_pValQword->SetWindowText(buf);

        swprintf(buf, 95, L"Int8: %d", (int)(int8_t)data[0]);
        m_pValInt8->SetWindowText(buf);
        swprintf(buf, 95, L"Int16: %d", (int)(int16_t)rd16(data));
        m_pValInt16->SetWindowText(buf);
        swprintf(buf, 95, L"Int32: %d", (int)(int32_t)rd32(data));
        m_pValInt32->SetWindowText(buf);
        swprintf(buf, 95, L"Int64: %lld", (long long)s64(rd64(data)));
        m_pValInt64->SetWindowText(buf);

        // 科学计数法：%.15g——15 位有效数字，自动选定点/科学计数较短者并去尾零
        swprintf(buf, 95, L"Float32: %.15g", (double)f32(data));
        m_pValFloat->SetWindowText(buf);
        swprintf(buf, 95, L"Double64: %.15g", f64(data));
        m_pValDouble->SetWindowText(buf);

        u8 c = data[0];
        if (c >= 0x20 && c < 0x7F)
            swprintf(buf, 95, I18n::T(L"字符: %c"), (char)c);
        else
            swprintf(buf, 95, I18n::T(L"字符: ."));
        m_pValChar->SetWindowText(buf);
    }
    else
    {
        m_pValByte->SetWindowText(L"UInt8: --");
        m_pValWord->SetWindowText(L"UInt16: --");
        m_pValDword->SetWindowText(L"UInt32: --");
        m_pValQword->SetWindowText(L"UInt64: --");
        m_pValInt8->SetWindowText(L"Int8: --");
        m_pValInt16->SetWindowText(L"Int16: --");
        m_pValInt32->SetWindowText(L"Int32: --");
        m_pValInt64->SetWindowText(L"Int64: --");
        m_pValFloat->SetWindowText(L"Float32: --");
        m_pValDouble->SetWindowText(L"Double64: --");
        m_pValChar->SetWindowText(I18n::T(L"字符: --"));
    }
}

void CMainFrame::UpdateTitle()
{
    SStringT title;
    SStringT cap;
    if (CurModel()->IsOpen())
    {
        const TCHAR *p = CurModel()->Path();
        LPCTSTR q = _tcsrchr(p, '\\');
        if (!q)
            q = _tcsrchr(p, '/');
        q = q ? q + 1 : p;
        const wchar_t *pMod = CurModel()->IsModified() ? L" *" : L"";
        const wchar_t *pRo = CurModel()->IsReadOnly() ? I18n::T(L" [只读]") : L"";
        title.Format(L"HexEditor - %s%s%s", q, pMod, pRo);
        cap.Format(L"%s%s%s - HexEditor", q, pMod, pRo);
    }
    else
    {
        title = I18n::T(L"HexEditor - 基于 SOUI4 的十六进制编辑器");
        cap = I18n::T(L"未命名 - HexEditor");
    }
    SetWindowText(title);
    if (m_pTxtTitle)
        m_pTxtTitle->SetWindowText(cap);
}

// ---------------------------------------------------------------------------
// IHexViewListener
// ---------------------------------------------------------------------------
void CMainFrame::OnHexViewCaretChanged(CHexView *pView)
{
    UpdateStatus();
    UpdateInspector();
}

void CMainFrame::OnHexViewModifiedChanged(CHexView *pView)
{
    SyncTabTitle(m_nCurTab); // 标签标题显示/清除 " *" 修改标记
    UpdateTitle();
    UpdateStatus();
    UpdateUndoButtons();
}

void CMainFrame::OnHexViewCommand(CHexView *pView, int nCmd)
{
    switch (nCmd)
    {
    case 1:
        OnActFind();
        break;
    case 2:
        OnActGoto();
        break;
    case 3:
        OnActSave();
        break;
    case 4:
        RunSearch(false);
        break;
    default:
        break;
    }
}
