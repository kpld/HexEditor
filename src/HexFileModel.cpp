#include "HexFileModel.h"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace hexmodel
{

// ---------------------------------------------------------------------------
// 跨平台辅助
// ---------------------------------------------------------------------------
#ifdef _WIN32
static std::string Narrow(const std::wstring &s)
{
    if (s.empty())
        return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0, NULL, NULL);
    std::string out;
    out.resize(n);
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, NULL, NULL);
    return out;
}
#else
static std::string Narrow(const std::wstring &s)
{
    // 非 Windows 平台 wchar_t 为 4 字节，存放 UTF-32 码点；而 POSIX 的
    // open()/rename()/unlink() 接收的是 UTF-8 字节串。这里手写 UTF-32 -> UTF-8，
    // 不依赖 locale（进程 locale 未必是 UTF-8），中文/其它 Unicode 路径才不会丢。
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++)
    {
        uint32_t cp = static_cast<uint32_t>(s[i]);
        if (cp < 0x80)
        {
            out.push_back(static_cast<char>(cp));
        }
        else if (cp < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp < 0x10000)
        {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp < 0x110000)
        {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else
        {
            out.push_back('?'); // 非法码点（含 UTF-16 代理区）
        }
    }
    return out;
}
#endif

#ifndef _WIN32
static void Msleep(unsigned ms)
{
    usleep(ms * 1000);
}
#else
#define Msleep(ms) Sleep(ms)
#endif

// ---------------------------------------------------------------------------
// MappedFile
// ---------------------------------------------------------------------------
MappedFile::MappedFile()
    : m_pData(NULL)
    , m_size(0)
#ifdef _WIN32
    , m_hFile(INVALID_HANDLE_VALUE)
    , m_hMapping(NULL)
#else
    , m_fd(-1)
#endif
    , m_bOk(false)
{
}

MappedFile::~MappedFile()
{
    Close();
}

bool MappedFile::Open(const wchar_t *pszPath)
{
    Close();
    if (!pszPath)
        return false;

#ifdef _WIN32
    m_hFile = CreateFileW(pszPath, GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (m_hFile == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER li;
    if (!GetFileSizeEx(m_hFile, &li))
    {
        Close();
        return false;
    }
    m_size = (u64)li.QuadPart;
    if (m_size == 0)
    {
        m_bOk = true; // 空文件：无需映射
        return true;
    }
    m_hMapping = CreateFileMappingW(m_hFile, NULL, PAGE_READONLY, li.HighPart, li.LowPart, NULL);
    if (!m_hMapping)
    {
        Close();
        return false;
    }
    m_pData = (u8 *)MapViewOfFile(m_hMapping, FILE_MAP_READ, 0, 0, 0);
    if (!m_pData)
    {
        Close();
        return false;
    }
#else
    m_fd = open(Narrow(pszPath).c_str(), O_RDONLY);
    if (m_fd < 0)
        return false;
    struct stat st;
    if (fstat(m_fd, &st) != 0)
    {
        Close();
        return false;
    }
    m_size = (u64)st.st_size;
    if (m_size == 0)
    {
        m_bOk = true;
        return true;
    }
    m_pData = (u8 *)mmap(NULL, (size_t)m_size, PROT_READ, MAP_PRIVATE, m_fd, 0);
    if (m_pData == MAP_FAILED)
    {
        m_pData = NULL;
        Close();
        return false;
    }
    madvise(m_pData, m_size, MADV_RANDOM);
#endif
    m_bOk = true;
    return true;
}

void MappedFile::Close()
{
#ifdef _WIN32
    if (m_pData)
        UnmapViewOfFile(m_pData);
    if (m_hMapping)
        CloseHandle(m_hMapping);
    if (m_hFile != INVALID_HANDLE_VALUE)
        CloseHandle(m_hFile);
    m_pData = NULL;
    m_hMapping = NULL;
    m_hFile = INVALID_HANDLE_VALUE;
#else
    if (m_pData && m_pData != MAP_FAILED)
        munmap(m_pData, m_size);
    if (m_fd >= 0)
        close(m_fd);
    m_pData = NULL;
    m_fd = -1;
#endif
    m_size = 0;
    m_bOk = false;
}

// ---------------------------------------------------------------------------
// HexFileModel
// ---------------------------------------------------------------------------
HexFileModel::HexFileModel()
    : m_bOpen(false)
    , m_size(0)
    , m_baseSize(0)
    , m_bModified(false)
    , m_bReadOnly(false)
    , m_bFastSaveEnabled(true)
    , m_bLastSaveFast(false)
    , m_bInUndo(false)
    , m_bSuppress(false)
{
}

HexFileModel::~HexFileModel()
{
    Close();
}

bool HexFileModel::Open(const wchar_t *pszPath)
{
    Close();
    if (!pszPath)
        return false;
    if (!m_mmap.Open(pszPath))
        return false;
    m_strPath = pszPath;
    m_strSavePath = pszPath;
    m_size = m_mmap.Size();
    m_baseSize = m_mmap.Size();
    m_overlay.clear();
    m_pieces.clear();
    if (m_mmap.Size() > 0)
    {
        // 初始状态：整个文件就是一个基流 Piece
        Piece pc;
        pc.logicalStart = 0;
        pc.srcOff = 0;
        pc.length = m_mmap.Size();
        pc.bInsert = false;
        m_pieces.push_back(pc);
    }
    m_store.clear();
    m_undoStack.clear();
    m_redoStack.clear();
    m_pending.clear();
    m_bModified = false;
    m_bReadOnly = false;
    m_bInUndo = false;
    m_bOpen = true;
    return true;
}

void HexFileModel::Close()
{
    ClearEdits();
    m_mmap.Close();
    m_strPath.clear();
    m_strSavePath.clear();
    m_size = 0;
    m_baseSize = 0;
    m_bModified = false;
    m_bOpen = false;
}

bool HexFileModel::CreateNew(const wchar_t *pszName, u64 initSize)
{
    // 新建：不落盘的空逻辑流，首次保存（Save）时若 m_strSavePath 为空由宿主另存。
    // initSize 用于新建时的初始长度（HxD 新建文件从 0 开始）。
    Close();
    if (!pszName)
        return false;
    m_strPath = pszName;
    m_strSavePath.clear(); // 未保存过：保存时走"另存为"
    m_size = initSize;
    m_baseSize = 0;
    if (initSize > 0)
    {
        Piece pc;
        pc.logicalStart = 0;
        pc.srcOff = 0;
        pc.length = initSize;
        pc.bInsert = true; // 内容来自插入缓冲（全零）
        m_pieces.push_back(pc);
        m_store.assign((size_t)initSize, 0);
    }
    m_bModified = initSize > 0;
    m_bInUndo = false;
    m_bOpen = true;
    return true;
}

void HexFileModel::ClearEdits()
{
    m_overlay.clear();
    m_store.clear();
    m_pieces.clear();
    m_undoStack.clear();
    m_redoStack.clear();
    m_pending.clear();
}

// ---------------------------------------------------------------------------
// 逻辑流 = 一串 Piece：
//   基流 Piece：存活的基流区间，内容来自 mmap/页覆盖层
//   插入 Piece：插入的内容，来自 m_store
// 覆盖写只碰页覆盖层与 store（不改变 Piece 结构）；插入是插入一个 Piece；
// 删除是丢弃被覆盖的 Piece。逻辑流长度 = 各 Piece 长度之和。
// ---------------------------------------------------------------------------

void HexFileModel::RebuildPieces()
{
    u64 pos = 0;
    for (size_t i = 0; i < m_pieces.size(); i++)
    {
        m_pieces[i].logicalStart = pos;
        pos += m_pieces[i].length;
    }
    m_size = pos;
}

void HexFileModel::MergePieces()
{
    if (m_pieces.size() < 2)
        return;
    std::vector<Piece> out;
    out.reserve(m_pieces.size());
    out.push_back(m_pieces[0]);
    for (size_t i = 1; i < m_pieces.size(); i++)
    {
        const Piece &p = m_pieces[i];
        Piece &last = out.back();
        if (last.bInsert == p.bInsert &&
            last.srcOff + last.length == p.srcOff && p.length > 0 && last.length > 0)
        {
            last.length += p.length; // 同类且数据连续，合并
            continue;
        }
        out.push_back(p);
    }
    m_pieces.swap(out);
}

int HexFileModel::FindPiece(u64 pos) const
{
    if (m_pieces.empty() || pos < m_pieces[0].logicalStart)
        return -1;
    if (pos >= m_size)
        return -1;
    int lo = 0, hi = (int)m_pieces.size() - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        const Piece &p = m_pieces[mid];
        if (pos < p.logicalStart)
            hi = mid - 1;
        else if (pos >= p.logicalStart + p.length)
            lo = mid + 1;
        else
            return mid;
    }
    return -1;
}

int HexFileModel::SplitPiece(int idx, u64 pos)
{
    if (idx < 0 || idx >= (int)m_pieces.size())
        return -1;
    Piece p = m_pieces[idx];
    if (pos <= p.logicalStart || pos >= p.logicalStart + p.length)
        return -1;
    u64 cut = pos - p.logicalStart;
    Piece left = p;
    left.length = cut;
    Piece right = p;
    right.logicalStart = pos;
    right.srcOff = p.srcOff + cut;
    right.length = p.length - cut;
    m_pieces[idx] = left;
    m_pieces.insert(m_pieces.begin() + idx + 1, right);
    return idx + 1;
}

u8 HexFileModel::BaseByte(u64 pos) const
{
    if (pos >= m_baseSize)
        return 0;
    u64 page = pos / PAGE_SIZE;
    std::map<u64, PagePtr>::const_iterator it = m_overlay.find(page);
    if (it != m_overlay.end() && it->second)
        return it->second[pos % PAGE_SIZE];
    if (m_mmap.Data() && pos < m_mmap.Size())
        return m_mmap.Data()[pos];
    return 0; // 追加出来的空白区域
}

u64 HexFileModel::Read(u64 pos, u8 *buf, u64 len) const
{
    if (!m_bOpen || !buf || len == 0)
        return 0;
    if (pos >= m_size)
        return 0;
    if (len > m_size - pos)
        len = m_size - pos;

    u64 done = 0;
    while (done < len)
    {
        u64 p = pos + done;
        int idx = FindPiece(p);
        if (idx < 0)
        { // 理论上不该发生：越界保护
            buf[done++] = 0;
            continue;
        }
        const Piece &pc = m_pieces[idx];
        u64 off = p - pc.logicalStart;
        u64 n = pc.length - off;
        if (n > len - done)
            n = len - done;
        if (pc.bInsert)
        {
            if (pc.srcOff + off + n <= m_store.size() && n)
                memcpy(buf + done, &m_store[pc.srcOff + off], (size_t)n);
            else
                memset(buf + done, 0, (size_t)n);
        }
        else
        {
            // 基流按 4KB 页分段批量拷贝。旧实现逐字节调用 BaseByte，每个字节
            // 都要做一次 FindPiece 二分 + std::map::find，bpr=256 时一帧上万次。
            // 这里每个 piece 只查一次、每页只查一次 overlay，段内直接 memcpy。
            // 语义与 BaseByte 逐字节完全一致：页覆盖层优先、其次 mmap、越界补零。
            u64 src = pc.srcOff + off;
            u64 left = n;
            u64 seg = 0; // 本 piece 内已写入的偏移（done 由外层统一推进）
            while (left > 0)
            {
                u64 page = src / PAGE_SIZE;
                u64 inPage = src % PAGE_SIZE;
                u64 chunk = PAGE_SIZE - inPage;
                if (chunk > left)
                    chunk = left;

                // 基流有效长度之外的空洞补零（对应 BaseByte 的首个判断）
                u64 inBase = (src < m_baseSize) ? (m_baseSize - src) : 0;
                u64 use = chunk;
                if (use > inBase)
                    use = inBase;

                if (use > 0)
                {
                    std::map<u64, PagePtr>::const_iterator it = m_overlay.find(page);
                    if (it != m_overlay.end() && it->second)
                    {
                        memcpy(buf + done + seg, &it->second[(size_t)inPage], (size_t)use);
                    }
                    else if (m_mmap.Data() && src < m_mmap.Size())
                    {
                        u64 avail = m_mmap.Size() - src;
                        if (avail > use)
                            avail = use;
                        memcpy(buf + done + seg, m_mmap.Data() + src, (size_t)avail);
                        if (avail < use)
                            memset(buf + done + seg + avail, 0, (size_t)(use - avail));
                    }
                    else
                    {
                        memset(buf + done + seg, 0, (size_t)use);
                    }
                }
                if (use < chunk)
                    memset(buf + done + seg + use, 0, (size_t)(chunk - use));

                seg += chunk;
                src += chunk;
                left -= chunk;
            }
        }
        done += n;
    }
    return len;
}

u8 HexFileModel::At(u64 pos) const
{
    if (!m_bOpen || pos >= m_size)
        return 0;
    u8 v = 0;
    Read(pos, &v, 1);
    return v;
}

void HexFileModel::DumpSegs() const
{
    printf("pieces: baseSize=%llu size=%llu store=%u count=%u\n", (unsigned long long)m_baseSize,
           (unsigned long long)m_size, (unsigned)m_store.size(), (unsigned)m_pieces.size());
    for (size_t i = 0; i < m_pieces.size(); i++)
    {
        const Piece &p = m_pieces[i];
        printf("  [%u] %s logical=%llu src=%llu len=%llu\n", (unsigned)i,
               p.bInsert ? "INS" : "BASE", (unsigned long long)p.logicalStart,
               (unsigned long long)p.srcOff, (unsigned long long)p.length);
    }
}

void HexFileModel::ApplyOverwrite(u64 bpos, const u8 *data, u64 len)
{
    for (u64 i = 0; i < len; i++)
    {
        u64 off = bpos + i;
        u64 page = off / PAGE_SIZE;
        std::map<u64, PagePtr>::iterator it = m_overlay.find(page);
        if (it == m_overlay.end() || !it->second)
        {
            PagePtr pg(new u8[PAGE_SIZE]());
            u64 pageBase = page * PAGE_SIZE;
            for (u64 j = 0; j < PAGE_SIZE; j++)
            {
                u64 o = pageBase + j;
                if (m_mmap.Data() && o < m_mmap.Size())
                    pg[j] = m_mmap.Data()[o];
            }
            std::pair<std::map<u64, PagePtr>::iterator, bool> r =
                m_overlay.insert(std::make_pair(page, pg));
            it = r.first;
        }
        it->second[off % PAGE_SIZE] = data[i];
    }
    if (bpos + len > m_baseSize)
        m_baseSize = bpos + len;
}

bool HexFileModel::GrowTo(u64 newSize)
{
    if (newSize <= m_size)
        return true;
    u64 n = newSize - m_size;
    size_t off = m_store.size();
    m_store.resize(off + (size_t)n, 0);
    Piece p;
    p.logicalStart = m_size;
    p.srcOff = off;
    p.length = n;
    p.bInsert = true;
    m_pieces.push_back(p);
    RebuildPieces();
    m_bModified = true;
    return true;
}

u64 HexFileModel::RemoveInsertPieces(u64 pos, u64 len, u64 storeOff)
{
    // 撤销插入：LIFO 保证此刻 [pos, pos+len) 正好是当初插入的内容，
    // 因此按逻辑区间匹配即可（相邻插入段可能已被 MergePieces 合并）
    u64 end = pos + len;
    u64 nRemoved = 0;
    std::vector<Piece> out;
    out.reserve(m_pieces.size() + 2);
    for (size_t i = 0; i < m_pieces.size(); i++)
    {
        Piece p = m_pieces[i];
        u64 pEnd = p.logicalStart + p.length;
        if (pEnd <= pos || p.logicalStart >= end || !p.bInsert)
        {
            out.push_back(p);
            continue;
        }
        if (p.logicalStart < pos)
        {
            Piece left = p;
            left.length = pos - p.logicalStart;
            out.push_back(left);
        }
        if (pEnd > end)
        {
            u64 cut = end - p.logicalStart;
            Piece right = p;
            right.srcOff = p.srcOff + cut;
            right.length = pEnd - end;
            out.push_back(right);
        }
        u64 ovStart = p.logicalStart < pos ? pos : p.logicalStart;
        u64 ovEnd = pEnd > end ? end : pEnd;
        if (ovEnd > ovStart)
            nRemoved += ovEnd - ovStart;
    }
    (void)storeOff;
    m_pieces.swap(out);
    RebuildPieces();
    MergePieces();
    return nRemoved;
}

bool HexFileModel::Write(u64 pos, const u8 *data, u64 len)
{
    if (!m_bOpen || len == 0 || m_bReadOnly)
        return false;
    if (pos > m_size)
        pos = m_size;
    u64 oldSize = m_size;
    if (pos + len > m_size)
    {
        if (!GrowTo(pos + len))
            return false;
    }

    if (!m_bSuppressUndo())
    {
        UndoRec rec;
        rec.type = UndoRec::OVR;
        rec.pos = pos;
        rec.len = len;
        rec.data.assign(len, 0);
        Read(pos, &rec.data[0], len);
        rec.newData.assign(data, data + len);
        rec.oldSize = oldSize;
        rec.newSize = m_size;
        m_pending.push_back(rec);
    }

    // 覆盖写不改变 Piece 结构：只写页覆盖层 / store
    u64 done = 0;
    while (done < len)
    {
        u64 p = pos + done;
        int idx = FindPiece(p);
        if (idx < 0)
            break;
        const Piece &pc = m_pieces[idx];
        u64 off = p - pc.logicalStart;
        u64 n = pc.length - off;
        if (n > len - done)
            n = len - done;
        if (pc.bInsert)
        {
            for (u64 i = 0; i < n; i++)
                m_store[pc.srcOff + off + i] = data[done + i];
        }
        else
        {
            ApplyOverwrite(pc.srcOff + off, data + done, n);
        }
        done += n;
    }

    m_bModified = true;
    return true;
}

bool HexFileModel::Insert(u64 pos, const u8 *data, u64 len)
{
    if (!m_bOpen || len == 0 || m_bReadOnly)
        return false;
    if (pos > m_size)
        pos = m_size;

    // 起点落在某个 Piece 内部：先拆成左右两段
    int idx = FindPiece(pos);
    if (idx >= 0 && pos > m_pieces[idx].logicalStart)
        SplitPiece(idx, pos);

    size_t storeOff = m_store.size();
    m_store.insert(m_store.end(), data, data + len);
    Piece p;
    p.logicalStart = pos;
    p.srcOff = storeOff;
    p.length = len;
    p.bInsert = true;
    // 插入位置：第一段 logicalStart >= pos 的 Piece 之前
    size_t insAt = m_pieces.size();
    for (size_t i = 0; i < m_pieces.size(); i++)
    {
        if (m_pieces[i].logicalStart >= pos)
        {
            insAt = i;
            break;
        }
    }
    m_pieces.insert(m_pieces.begin() + insAt, p);
    RebuildPieces();
    MergePieces();
    m_bModified = true;

    if (!m_bSuppressUndo())
    {
        UndoRec rec;
        rec.type = UndoRec::INS;
        rec.pos = pos;
        rec.len = len;
        rec.storeOff = storeOff;
        rec.newData.assign(data, data + len); // 重做需要
        m_pending.push_back(rec);
    }
    return true;
}

bool HexFileModel::Delete(u64 pos, u64 len)
{
    if (!m_bOpen || len == 0 || pos >= m_size || m_bReadOnly)
        return false;
    if (len > m_size - pos)
        len = m_size - pos;

    UndoRec rec;
    rec.type = UndoRec::DEL;
    rec.pos = pos;
    rec.len = len;
    if (len <= 1024 * 1024)
    {
        rec.data.assign(len, 0);
        Read(pos, &rec.data[0], len);
    }

    u64 end = pos + len;
    std::vector<Piece> out;
    out.reserve(m_pieces.size() + 2);
    for (size_t i = 0; i < m_pieces.size(); i++)
    {
        Piece p = m_pieces[i];
        u64 pEnd = p.logicalStart + p.length;
        if (pEnd <= pos || p.logicalStart >= end)
        {
            out.push_back(p); // 与区间无交
            continue;
        }
        if (p.logicalStart < pos)
        {
            Piece left = p;
            left.length = pos - p.logicalStart;
            out.push_back(left);
        }
        if (pEnd > end)
        {
            u64 cut = end - p.logicalStart;
            Piece right = p;
            right.srcOff = p.srcOff + cut;
            right.length = pEnd - end;
            out.push_back(right);
        }
        // 其余部分被删除，直接丢弃
    }
    m_pieces.swap(out);
    RebuildPieces();
    MergePieces();
    m_bModified = true;

    if (!m_bSuppressUndo())
        m_pending.push_back(rec);
    return true;
}

bool HexFileModel::Truncate(u64 newSize)
{
    if (!m_bOpen || m_bReadOnly)
        return false;
    if (newSize >= m_size)
        return true;
    return Delete(newSize, m_size - newSize);
}

void HexFileModel::BeginUndo()
{
    if (m_bInUndo)
        return;
    m_bInUndo = true;
    m_pending.clear();
}

void HexFileModel::EndUndo()
{
    if (!m_bInUndo)
        return;
    m_bInUndo = false;
    if (!m_pending.empty())
    {
        m_undoStack.push_back(m_pending);
        if (m_undoStack.size() > 200)
            m_undoStack.erase(m_undoStack.begin());
        m_redoStack.clear();
    }
    m_pending.clear();
}

bool HexFileModel::Undo()
{
    if (m_undoStack.empty())
        return false;
    UndoGroup group = m_undoStack.back();
    m_undoStack.pop_back();
    SetSuppressUndo(true);
    for (int i = (int)group.size() - 1; i >= 0; i--)
    {
        UndoRec &r = group[i];
        switch (r.type)
        {
        case UndoRec::OVR:
            if (!r.data.empty())
                Write(r.pos, &r.data[0], (u64)r.data.size());
            if (m_size > r.oldSize)
                Delete(r.oldSize, m_size - r.oldSize);
            break;
        case UndoRec::INS:
            RemoveInsertPieces(r.pos, r.len, r.storeOff);
            break;
        case UndoRec::DEL:
            if (!r.data.empty())
                Insert(r.pos, &r.data[0], (u64)r.data.size());
            break;
        }
    }
    SetSuppressUndo(false);
    m_redoStack.push_back(group);
    if (m_undoStack.empty())
        m_bModified = false;
    return true;
}

bool HexFileModel::Redo()
{
    if (m_redoStack.empty())
        return false;
    UndoGroup group = m_redoStack.back();
    m_redoStack.pop_back();
    SetSuppressUndo(true);
    for (size_t i = 0; i < group.size(); i++)
    {
        UndoRec &r = group[i];
        switch (r.type)
        {
        case UndoRec::OVR:
            if (!r.newData.empty())
                Write(r.pos, &r.newData[0], (u64)r.newData.size());
            if (m_size < r.newSize)
                GrowTo(r.newSize);
            break;
        case UndoRec::INS:
            if (!r.newData.empty())
                Insert(r.pos, &r.newData[0], (u64)r.newData.size());
            break;
        case UndoRec::DEL:
            Delete(r.pos, r.len);
            break;
        }
    }
    SetSuppressUndo(false);
    m_undoStack.push_back(group);
    m_bModified = true;
    return true;
}

// 快速保存：纯覆盖编辑（逻辑流 = 原文件、长度不变）时，只把 overlay 里的
// 脏页按 4KB 粒度写回原文件对应偏移，避免整文件复制。
// 4GB+ 文件改几个字节，保存从「分钟级」降到「毫秒级」。
// 注意：原地写回不保证崩溃原子性（写到一半掉电可能新旧混合）；需要原子
// 保存时用「另存为」。任何一步失败都返回 false，由调用方回退整文件重写。
bool HexFileModel::SaveOverwriteInPlace(const std::wstring &dst)
{
#ifdef _WIN32
    // 只读映射仍在时，用共享写句柄打开同一文件（原 CreateFile 已带
    // FILE_SHARE_WRITE，因此允许并存）；写完再关闭映射重新打开。
    HANDLE h = CreateFileW(dst.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    bool ok = true;
    for (std::map<u64, PagePtr>::const_iterator it = m_overlay.begin(); it != m_overlay.end(); ++it)
    {
        u64 pageOff = it->first * PAGE_SIZE;
        if (pageOff >= m_baseSize)
            continue;
        u64 n = m_baseSize - pageOff;
        if (n > PAGE_SIZE)
            n = PAGE_SIZE;
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)pageOff;
        if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN))
        {
            ok = false;
            break;
        }
        DWORD written = 0;
        if (!WriteFile(h, it->second.get(), (DWORD)n, &written, NULL) || written != (DWORD)n)
        {
            ok = false;
            break;
        }
    }
    if (ok)
        FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok)
        return false;

    // 重新映射：让内存视图与磁盘一致，同时清空编辑历史
    m_mmap.Close();
    std::wstring path = dst;
    if (!Open(path.c_str()))
        return false;
    m_strSavePath = path;
    return true;
#else
    (void)dst;
    return false; // 非 Windows 暂走整文件重写
#endif
}

bool HexFileModel::Save(const wchar_t *pszPath)
{
    if (!m_bOpen)
        return false;
    std::wstring dst = pszPath ? pszPath : m_strSavePath;
    if (dst.empty())
        return false;

    bool bSameFile = false;
    if (!m_strPath.empty())
    {
#ifdef _WIN32
        bSameFile = _wcsicmp(dst.c_str(), m_strPath.c_str()) == 0;
#else
        bSameFile = (dst == m_strPath);
#endif
    }

    m_bLastSaveFast = false;

    // 保存到原文件且没有任何修改：无需写盘
    if (bSameFile && !m_bModified)
        return true;

    // ---- 快速路径：纯覆盖编辑（无插入/删除/截断、长度不变）且文件较大 ----
    // 8MB 以下整文件重写本来就是毫秒级，没必要牺牲原子性。
    // m_pieces 只剩一个覆盖 [0, baseSize) 的基流段，说明结构未被改动，
    // 所有差异都在 overlay 页里 —— 只写这些脏页即可。
    const u64 kDirtyMin = 8ull * 1024 * 1024;
    if (m_bFastSaveEnabled && bSameFile && m_size == m_baseSize && m_size >= kDirtyMin
        && !m_overlay.empty() && m_pieces.size() == 1 && !m_pieces[0].bInsert
        && m_pieces[0].srcOff == 0 && m_pieces[0].length == m_baseSize)
    {
        if (SaveOverwriteInPlace(dst))
        {
            m_bLastSaveFast = true;
            return true;
        }
        // 失败：回退到下面的整文件重写
    }

    // Windows 不允许覆盖仍有映射的文件（ERROR_USER_MAPPED_FILE），
    // 因此先写到同目录的临时文件，再关闭映射并原子替换。
    std::wstring tmp = dst + L".~hextmp";

    const u64 CHUNK = 1 << 20;
    std::vector<u8> buf(CHUNK);
    bool bOk = true;

#ifdef _WIN32
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    u64 off = 0;
    while (off < m_size)
    {
        u64 n = m_size - off;
        if (n > CHUNK)
            n = CHUNK;
        Read(off, &buf[0], n);
        DWORD written = 0;
        if (!WriteFile(h, &buf[0], (DWORD)n, &written, NULL) || written != n)
        {
            bOk = false;
            break;
        }
        off += n;
    }
    CloseHandle(h);
    if (!bOk)
    {
        DeleteFileW(tmp.c_str());
        return false;
    }

    // 释放映射后替换目标文件
    m_mmap.Close();
    if (!MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        DeleteFileW(tmp.c_str());
        Open(dst.c_str()); // 尽量恢复现场
        return false;
    }
#else
    int fd = open(Narrow(tmp).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    u64 off = 0;
    while (off < m_size)
    {
        u64 n = m_size - off;
        if (n > CHUNK)
            n = CHUNK;
        Read(off, &buf[0], n);
        ssize_t w = write(fd, &buf[0], (size_t)n);
        if (w != (ssize_t)n)
        {
            bOk = false;
            break;
        }
        off += n;
    }
    close(fd);
    if (!bOk)
    {
        unlink(Narrow(tmp).c_str());
        return false;
    }
    m_mmap.Close();
    if (rename(Narrow(tmp).c_str(), Narrow(dst).c_str()) != 0)
    {
        unlink(Narrow(tmp).c_str());
        Open(dst.c_str());
        return false;
    }
#endif

    // 重新映射新文件，编辑历史清空
    std::wstring path = dst;
    Open(path.c_str());
    m_strSavePath = path;
    return true;
}

} // namespace hexmodel
