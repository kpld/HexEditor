//
// CrtCompat.h - 跨平台 CRT 兼容层
//
// swinx 在 Linux/macOS 上补齐了 windows.h 等 Win32 API 头，但没有补齐 MSVC 专有的
// CRT 符号。本头只在「非 Windows」平台提供这些符号的标准 C99 等价实现；
// Windows 下本头完全为空（应用继续使用 MSVC 原生 CRT），确保 Windows 构建逐字节不变。
//
// 全 src 扫描确认，需要补齐的仅两处：
//   _wcstoi64   : MainFrame.cpp / ParseBprRaw  —— 64 位宽字符串转整数
//   _stprintf_s : Theme.cpp     / SaveIndex    —— 格式化到固定数组
//
// 条件写成 (!_WIN32 || HEX_FORCE_CRT_COMPAT)：后者仅供移植自检使用，可在 Windows 上
// 强制编译这段兼容分支做语法/行为验证；产品默认不定义该宏，无任何影响。
//
#ifndef HEX_CRT_COMPAT_H
#define HEX_CRT_COMPAT_H

// _countof：MSVC 在 <stdlib.h> 提供，GCC/Clang 没有。语义纯数组容量、完全可移植，
// 因此【两平台都补】——Windows 下若 MSVC 已定义则 #ifndef 跳过，零影响。
// 放在分支外，好让只关心这一个宏的头文件也能安全 include 本头。
#ifndef _countof
#define _countof(_arr) (sizeof(_arr) / sizeof((_arr)[0]))
#endif

#if !defined(_WIN32) || defined(HEX_FORCE_CRT_COMPAT)

#include <stddef.h>  // size_t
#include <stdarg.h>  // va_list / va_start / va_end
#include <wchar.h>   // ::wcstoll / ::vswprintf (C99)
#include <stdlib.h>

// _wcstoi64(s, &end, base)
// 标准 ::wcstoll 语义完全一致：10 进制、溢出饱和到 LLONG_MAX/LLONG_MIN 并置
// errno=ERANGE、end 回写首个非法字符位置、返回 long long(64 位)。ParseBprRaw
// 依赖的「超大数先饱和、再钳制到 1..4096」行为与 MSVC 版逐位相同。
#ifndef _wcstoi64
inline long long CompatWcsToI64(const wchar_t *s, wchar_t **end, int base)
{
    return ::wcstoll(s, end, base);
}
#define _wcstoi64 CompatWcsToI64
#endif

// _stprintf_s(buf, fmt, ...)
// MSVC 版是按数组容量推导的安全格式化；这里用数组引用模板 + ::vswprintf 等价实现，
// 编译期推导容量，避免用宏把 sizeof(指针) 误当缓冲大小。buf 必须是真实数组
//（本项目唯一调用点是 Theme.cpp 的 TCHAR szBuf[16]）。
#ifndef _stprintf_s
template <size_t N>
inline int CompatStprintfS(wchar_t (&buf)[N], const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = ::vswprintf(buf, N, fmt, ap);
    va_end(ap);
    return r;
}
#define _stprintf_s CompatStprintfS
#endif

#endif // !_WIN32 || HEX_FORCE_CRT_COMPAT
#endif // HEX_CRT_COMPAT_H
