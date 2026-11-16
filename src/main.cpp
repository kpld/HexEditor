//
// HexEditor 入口：初始化 SOUI 应用环境，创建主窗口
//

#include "MainFrame.h"
#include "I18n.h"

#include <SAppCfg.h>

#include <shellapi.h>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h> // access()

// 非 Windows 平台的命令行入口适配：
//   swinx 没有 CommandLineToArgvW，且 argv 是 UTF-8 窄字符、内部 TCHAR 是
//   4 字节 wchar_t(UTF-32, WCHAR_SIZE=4)。main() 里把 argv 逐个转成宽字符、
//   按存在性过滤后存入 g_cmdFiles，_tWinMain 里统一消费。
// 手写 UTF-8 -> UTF-32，不依赖 locale（进程 locale 未必是 UTF-8），中文路径不丢。
static std::wstring Utf8ToWide(const char *s)
{
    std::wstring out;
    if (!s)
        return out;
    const unsigned char *p = (const unsigned char *)s;
    while (*p)
    {
        unsigned char c = *p;
        unsigned int cp;
        int need;
        if (c < 0x80)
        {
            cp = c;
            need = 0;
        }
        else if ((c & 0xE0) == 0xC0)
        {
            cp = c & 0x1F;
            need = 1;
        }
        else if ((c & 0xF0) == 0xE0)
        {
            cp = c & 0x0F;
            need = 2;
        }
        else if ((c & 0xF8) == 0xF0)
        {
            cp = c & 0x07;
            need = 3;
        }
        else
        {
            ++p;
            continue; // 非法前导字节
        }
        int k = 0;
        for (; k < need; ++k)
            if ((p[k + 1] & 0xC0) != 0x80)
                break;
        if (k < need)
        {
            ++p;
            continue; // 序列不完整，跳过
        }
        ++p;
        for (k = 0; k < need; ++k)
        {
            cp = (cp << 6) | (*p & 0x3F);
            ++p;
        }
        out.push_back((wchar_t)cp);
    }
    return out;
}

static std::vector<std::wstring> g_cmdFiles;
#endif

using namespace SOUI;

static const TCHAR *kPath_SysRes = _T("/soui-sys-resource");
static const TCHAR *kPath_UiRes = _T("/uires");

static SStringT getAppDir()
{
    TCHAR szPath[MAX_PATH * 2];
    szPath[0] = 0;
    GetModuleFileName(NULL, szPath, MAX_PATH * 2);
    SStringT str = szPath;
    int n = str.ReverseFind(_T('\\'));
    if (n < 0)
        n = str.ReverseFind(_T('/'));
    if (n > 0)
        str = str.Left(n);
    return str;
}

int WINAPI _tWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, LPTSTR lpstrCmdLine,
                     int /*nCmdShow*/)
{
    HRESULT hRes = OleInitialize(NULL);
    SASSERT(SUCCEEDED(hRes));

    // 原生 Win32 菜单（主菜单下拉 / 标签栏 ▾ 列表 / 右键菜单）的暗色开关。
    // SetPreferredAppMode 必须在本进程创建任何窗口之前调用，否则不生效——
    // 所以放在这里（OleInitialize 之后、SOUI 初始化之前）。
    Theme::ApplyNativeMenuTheme(Theme::Get(Theme::LoadSavedIndex()).bDark);

    int nRet = 0;
    SApplication app(hInstance);
    SAppCfg cfg;
    SStringT appDir = getAppDir();
    const int nLang = I18n::LoadSaved(); // 0=中文，1=英文

#ifdef _WIN32
    // Windows：系统资源直接由 soui-sys-resource.dll 提供（与 exe 同目录）
    cfg.SetSysResPeFile(_T("soui-sys-resource.dll"));
#else
    // 其他平台：系统资源 zip
    cfg.SetSysResZip(appDir + _T("/soui-sys-resource.zip"));
#endif

    cfg.SetRender(Render_Skia)
        .SetImgDecoder(ImgDecoder_Stb)
        .SetAppDir(appDir)
        .SetAppResFile(appDir + kPath_UiRes)
        .SetLog(TRUE)
        .EnableMultiLang(I18n::ResId(nLang)); // 建立翻译器并加载初始语言

    if (!cfg.DoConfig(&app))
    {
#ifdef _WIN32
        MessageBoxW(NULL, L"SOUI 初始化失败：无法加载系统资源。", L"HexEditor", MB_OK | MB_ICONERROR);
#endif
        return -1;
    }

    // 注册自定义控件
    app.RegisterWindowClass<CHexView>();
    app.RegisterWindowClass<STabCtrlEx>(); // 标签控件派生（暴露 GetItemRect）

    I18n::Init(nLang); // 翻译器已就绪，之后 T() 才能安全调用



    {
        CMainFrame wndMain;
        // 命令行参数：把存在的文件都加入打开列表（HexEditor a.bin b.bin ...）。
        // AddCmdFile 只写 POD 缓冲，避免 SStringW 在 Create 之前的栈操作
        //（此前会触发 GS cookie 失败）。
#ifdef _WIN32
        // Windows：用 CommandLineToArgvW 正确解析带空格的引号路径。
        if (lpstrCmdLine && lpstrCmdLine[0])
        {
            int nArgc = 0;
            LPWSTR *ppArgv = ::CommandLineToArgvW(::GetCommandLineW(), &nArgc);
            for (int i = 1; i < nArgc; i++)
            {
                if (::GetFileAttributesW(ppArgv[i]) == INVALID_FILE_ATTRIBUTES)
                    continue;
                wndMain.AddCmdFile(ppArgv[i]);
            }
            if (ppArgv)
                ::LocalFree(ppArgv);
        }
#else
        // 非 Windows：argv 已由 main() 转成宽字符并按存在性过滤，直接消费。
        //（swinx 未提供 CommandLineToArgvW / GetFileAttributesW 组合）
        for (size_t i = 0; i < g_cmdFiles.size(); i++)
            wndMain.AddCmdFile(g_cmdFiles[i].c_str());
#endif
        if (!wndMain.Create(GetActiveWindow()))
        {
            MessageBoxW(NULL, L"主窗口创建失败：请检查 uires/layout/main.xml 与菜单资源。", L"HexEditor",
                        MB_OK | MB_ICONERROR);
            return -2;
        }
        wndMain.SendMessage(WM_INITDIALOG);
        wndMain.CenterWindow(wndMain.m_hWnd);
        wndMain.ShowWindow(SW_SHOWNORMAL);
        nRet = app.Run(wndMain.m_hWnd);
    }

    OleUninitialize();
    return nRet;
}

#if !defined(_WIN32) || defined(__MINGW32__)
int main(int argc, char **argv)
{
#if !defined(_WIN32)
    // argv[1..] 是 UTF-8 路径：过滤不存在文件后转成宽字符，交给 _tWinMain。
    // 与 Windows 分支的 GetFileAttributesW 过滤语义保持一致。
    for (int i = 1; i < argc; i++)
    {
        if (::access(argv[i], F_OK) != 0)
            continue;
        g_cmdFiles.push_back(Utf8ToWide(argv[i]));
    }
#else
    (void)argc;
    (void)argv;
#endif
    HINSTANCE hInst = GetModuleHandle(NULL);
    return _tWinMain(hInst, 0, NULL, SW_SHOWNORMAL);
}
#endif //_WIN32
