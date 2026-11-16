#include "HexSearch.h"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

namespace hexsearch
{

static const u64 WINDOW = 1024 * 1024; // 扫描窗口 1MB

HexSearch::HexSearch()
{
    m_pSource = nullptr;
    m_start = 0;
    m_end = 0;
    m_bBackward = false;
    m_state = IDLE;
    m_result = 0;
    m_progress = 0.f;
    m_bStop = false;
    m_hThread = nullptr;
}

HexSearch::~HexSearch()
{
    Stop();
    Wait();
    m_hThread = nullptr;
}

bool HexSearch::BuildShift(const u8 *pat, u64 patLen)
{
    m_shift.assign(256, (size_t)patLen);
    for (u64 i = 0; i + 1 < patLen; i++)
        m_shift[pat[i]] = (size_t)(patLen - 1 - i);
    return true;
}

bool HexSearch::Start(const IHexSource *pSource, const u8 *pat, u64 patLen, u64 start, u64 end,
                      bool bBackward)
{
    Stop();
    Wait();
    if (!pSource || !pat || patLen == 0)
        return false;
    u64 size = pSource->GetSize();
    if (size == 0 || start >= size)
        return false;
    if (end >= size)
        end = size - 1;

    m_pSource = pSource;
    m_pat.assign(pat, pat + patLen);
    BuildShift(pat, patLen);
    m_start = start;
    m_end = end;
    m_bBackward = bBackward;
    m_state = RUNNING;
    m_progress = 0.f;
    m_result = 0;
    m_bStop = false;

#ifdef _WIN32
    unsigned tid = 0;
    m_hThread = (void *)_beginthreadex(NULL, 0, ThreadMain, this, 0, &tid);
    if (!m_hThread)
    {
        m_state = IDLE;
        return false;
    }
#else
    pthread_t *tid = new pthread_t;
    if (pthread_create(tid, NULL, ThreadMain, this) != 0)
    {
        delete tid;
        m_state = IDLE;
        return false;
    }
    pthread_detach(*tid);
    delete tid;
#endif
    return true;
}

void HexSearch::Stop()
{
    m_bStop = true;
}

bool HexSearch::IsRunning() const
{
    return m_state == RUNNING;
}

HexSearch::State HexSearch::GetState() const
{
    return m_state;
}

float HexSearch::GetProgress() const
{
    return m_progress;
}

u64 HexSearch::GetResult() const
{
    return m_result;
}

void HexSearch::Wait()
{
    if (!m_hThread)
        return;
#ifdef _WIN32
    WaitForSingleObject((HANDLE)m_hThread, 120000);
    CloseHandle((HANDLE)m_hThread);
#endif
    m_hThread = nullptr;
}

#ifdef _WIN32
unsigned __stdcall HexSearch::ThreadMain(void *pArg)
{
    ((HexSearch *)pArg)->Run();
    return 0;
}
#else
void *HexSearch::ThreadMain(void *pArg)
{
    ((HexSearch *)pArg)->Run();
    return NULL;
}
#endif

void HexSearch::Run()
{
    u64 found = 0;
    bool bOk = false;
    if (m_bBackward)
        bOk = SearchRange(m_start, m_end, false, found); // 反向：取范围内最后一个命中
    else
        bOk = SearchRange(m_start, m_end, true, found);

    if (m_bStop && m_state == RUNNING)
    {
        m_state = STOPPED;
        return;
    }
    if (bOk)
    {
        m_result = found;
        m_state = FOUND;
    }
    else
    {
        m_state = NOT_FOUND;
    }
}

/**
 * 以滑动窗口扫描 [from, to]。
 * bForward=true 找到第一个命中即返回；false 时返回范围内最后一个命中。
 */
bool HexSearch::SearchRange(u64 from, u64 to, bool bForward, u64 &found) const
{
    const u64 patLen = m_pat.size();
    if (patLen == 0)
        return false;
    std::vector<u8> buf(WINDOW + patLen);
    u64 pos = from;
    u64 lastFound = 0;
    bool bHas = false;
    float total = (float)(to - from + 1);

    while (pos + patLen - 1 <= to && !m_bStop)
    {
        const u8 *data = &buf[0];
        u64 scanLen = to - pos + 1;
        if (scanLen > WINDOW)
            scanLen = WINDOW;
        u64 readLen = scanLen + patLen - 1;
        if (readLen > to - pos + 1)
            readLen = to - pos + 1;
        if (readLen > buf.size())
            readLen = buf.size();
        m_pSource->Read(pos, &buf[0], readLen);
        if (readLen < patLen)
            break;

        u64 i = 0;
        while (i + patLen <= readLen && !m_bStop)
        {
            u64 j = patLen - 1;
            while (j > 0 && data[i + j] == m_pat[j])
                j--;
            if (data[i + j] == m_pat[j])
            {
                if (bForward)
                {
                    found = pos + i;
                    return true;
                }
                lastFound = pos + i;
                bHas = true;
            }
            size_t step = m_shift[data[i + patLen - 1]];
            if (step < 1)
                step = 1;
            i += step;
        }
        if (scanLen < WINDOW)
            break;
        pos += scanLen;
        if (total > 0)
            m_progress = (float)(pos - from) / total;
    }
    if (!bForward && bHas)
    {
        found = lastFound;
        return true;
    }
    return false;
}

} // namespace hexsearch
