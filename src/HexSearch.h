//
// HexSearch - 大文件模式检索
//
// 使用 Boyer-Moore-Horspool 坏字符跳转，在后台线程中以滑动窗口方式扫描
// 逻辑流（窗口与模式重叠一个字节，保证跨窗口命中），
// 遇到数十 GB 的文件也不会把内容读进内存。
//
#ifndef HEX_SEARCH_H
#define HEX_SEARCH_H

#include <stdint.h>
#include <stddef.h> // size_t：Windows 下由 windows.h 顺带提供，非 Win 需显式包含

#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace hexsearch
{

typedef uint64_t u64;
typedef uint8_t u8;

class IHexSource
{
  public:
    virtual u64 GetSize() const = 0;
    virtual u64 Read(u64 pos, u8 *buf, u64 len) const = 0;

  protected:
    virtual ~IHexSource()
    {
    }
};

class HexSearch
{
  public:
    HexSearch();
    ~HexSearch();

    enum State
    {
        IDLE,
        RUNNING,
        FOUND,
        NOT_FOUND,
        STOPPED
    };

    /** 启动后台检索；start/end 为闭区间偏移，bBackward 表示反向 */
    bool Start(const IHexSource *pSource, const u8 *pat, u64 patLen, u64 start, u64 end,
               bool bBackward);

    void Stop();
    bool IsRunning() const;
    State GetState() const;
    float GetProgress() const;
    u64 GetResult() const;

    /** 等待线程结束（析构前必须调用） */
    void Wait();

  private:
#ifdef _WIN32
    static unsigned __stdcall ThreadMain(void *pSelf);
#else
    static void *ThreadMain(void *pSelf);
#endif
    void Run();
    bool BuildShift(const u8 *pat, u64 patLen);
    bool SearchRange(u64 from, u64 to, bool bForward, u64 &found) const;

  private:
    const IHexSource *m_pSource;
    std::vector<u8> m_pat;
    std::vector<size_t> m_shift;
    u64 m_start, m_end;
    bool m_bBackward;
    State m_state;
    u64 m_result;
    mutable float m_progress;
    bool m_bStop;

#ifdef _WIN32
    void *m_hThread;
#else
    void *m_hThread;
#endif
};

} // namespace hexsearch

#endif // HEX_SEARCH_H
