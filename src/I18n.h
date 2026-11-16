//
// I18n - 中英文多语言支持
//
// 基于 SOUI 的 ITranslatorMgr：
//   - XML 里的 text/tip/title 在窗口创建时自动翻译（STrText::TranslateText）；
//   - C++ 字符串用 T(L"中文") 显式翻译；
//   - 切换语言时安装新翻译器并广播 UM_SETLANGUAGE，所有宿主窗口自动重译。
//
#ifndef I18N_H
#define I18N_H

#include <souistd.h>

using namespace SOUI;

namespace I18n
{
enum Lang
{
    ZH = 0,
    EN = 1,
};

/** 语言资源名："lang:zh" / "lang:en" */
const wchar_t *ResId(int lang);
/** 启动时读取保存的语言（HexEditor.ini 的 [View] Language） */
int  LoadSaved();
/** 保存语言选择 */
void Save(int lang);
/** 当前语言 */
int  Current();
/** SOUI 初始化完成后调用：标记翻译器可用并设置初始语言 */
void Init(int lang);
/** 安装指定语言的翻译器并广播 UM_SETLANGUAGE；返回是否成功 */
bool SetLanguage(int lang);
/** 翻译源字符串（默认上下文） */
SStringW T(const wchar_t *src);
} // namespace I18n

#endif // I18N_H
