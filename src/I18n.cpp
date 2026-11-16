//
// I18n - 中英文多语言支持（实现）
//
#include "I18n.h"
#include "Theme.h"

#include <SApp.h>
#include <helper/SHostMgr.h>
#include <xml/SXml.h>
#include <interface/STranslator-i.h>

using namespace SOUI;

namespace I18n
{
static int  g_lang = ZH;
static bool g_ready = false;

const wchar_t *ResId(int lang)
{
    return lang == EN ? L"lang:en" : L"lang:zh";
}

int LoadSaved()
{
    int n = (int)::GetPrivateProfileInt(L"View", L"Language", ZH, Theme::IniPath().c_str());
    return n == EN ? EN : ZH;
}

void Save(int lang)
{
    ::WritePrivateProfileString(L"View", L"Language", lang == EN ? L"1" : L"0",
                                Theme::IniPath().c_str());
}

int Current()
{
    return g_lang;
}

void Init(int lang)
{
    g_lang = (lang == EN) ? EN : ZH;
    g_ready = true;
}

SStringW T(const wchar_t *src)
{
    if (!src)
        return SStringW();
    if (!g_ready || !*src)
        return SStringW(src);
    SApplication *app = SApplication::getSingletonPtr();
    if (!app || !app->GetTranslator()) // translator.dll 未加载时退回原文
        return SStringW(src);
    return app->tr(SStringW(src), SStringW(L""));
}

bool SetLanguage(int lang)
{
    if (lang != EN)
        lang = ZH;

    SApplication *app = SApplication::getSingletonPtr();
    if (!app)
        return false;
    ITranslatorMgr *mgr = app->GetTranslator();
    if (!mgr)
        return false;

    SXmlDoc xml;
    if (!app->LoadXmlDocment(xml, ResId(lang)))
        return false;
    SXmlNode node = xml.root().child(L"language");

    SAutoRefPtr<ITranslator> tr;
    mgr->CreateTranslator(&tr);
    if (!tr)
        return false;
    tr->Load(&node, 1); // 1 = LD_XML

    wchar_t szName[TR_MAX_NAME_LEN] = {0};
    tr->GetName(szName);
    mgr->SetLanguage(szName); // 清掉其他语言的模块
    mgr->InstallTranslator(tr);

    g_lang = lang;
    Save(lang);

    // 广播给所有宿主（主窗口 / 数据检查器 / 查找浮窗），自动重译 XML 文本与提示
    SHostMgr::getSingleton().DispatchMessage(UM_SETLANGUAGE);
    return true;
}
} // namespace I18n
