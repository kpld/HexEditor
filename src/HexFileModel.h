//
// HexFileModel - 跨平台大文件十六进制模型
//
// 设计要点（对应 HxD 的秒开需求）：
//   1. 打开文件只做一次只读 mmap，无论文件多大都是 O(1) 完成，
//      页缺失时才由操作系统按需换入，不会把整个文件读进内存。
//   2. 所有"写"操作一律不碰 mmap（只读映射，其他进程可以同时只读访问），
//      而是写入 4KB 粒度的页覆盖层（copy-on-write）。
//   3. 插入 / 删除用稀疏"编辑段"描述，尾部移动不需要搬移内存，
//      因此在超大文件中间插入删除也是 O(段数)。
//   4. 编辑日志支持多级撤销 / 重做。
//
#ifndef HEX_FILE_MODEL_H
#define HEX_FILE_MODEL_H

#include <stdint.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace hexmodel
{

typedef uint64_t u64;
typedef uint8_t u8;
typedef int64_t i64;

static const u64 PAGE_SIZE = 4096;

/** 只读内存映射文件，跨平台实现 */
class MappedFile
{
  public:
    MappedFile();
    ~MappedFile();

    bool Open(const wchar_t *pszPath);
    void Close();

    u64 Size() const
    {
        return m_size;
    }
    const u8 *Data() const
    {
        return m_pData;
    }

  private:
    u8 *m_pData;
    u64 m_size;
#ifdef _WIN32
    HANDLE m_hFile;
    HANDLE m_hMapping;
#else
    int m_fd;
#endif
    bool m_bOk;
};

/**
 * Piece：逻辑流的一段。基流 Piece 描述「存活的基流区间」，插入 Piece 描述插入内容。
 * 删除 = 丢弃被覆盖的 Piece，因此天然正确，不需要维护「洞」的位置。
 */
struct Piece
{
    u64 logicalStart; /**< 缓存：在逻辑流中的起始位置 */
    u64 srcOff;       /**< 基流 Piece：基流偏移；插入 Piece：store 偏移 */
    u64 length;       /**< 长度 */
    bool bInsert;     /**< true=插入内容(m_store)，false=基流存活区间 */
};

class HexFileModel
{
  public:
    HexFileModel();
    ~HexFileModel();

    bool Open(const wchar_t *pszPath);
    void Close();

    /** 新建：创建不落盘的空文件模型（保存时由宿主另存为） */
    bool CreateNew(const wchar_t *pszName, u64 initSize = 0);

    bool IsOpen() const
    {
        return m_bOpen;
    }

    /** 逻辑流大小 */
    u64 Size() const
    {
        return m_size;
    }

    /** 原文件大小（不含编辑） */
    u64 OriginalSize() const
    {
        return m_mmap.Size();
    }

    const wchar_t *Path() const
    {
        return m_strPath.c_str();
    }

    bool IsModified() const
    {
        return m_bModified;
    }

    /** 只读模式：所有写操作（Write/Insert/Delete/Truncate）直接拒绝 */
    bool IsReadOnly() const
    {
        return m_bReadOnly;
    }
    void SetReadOnly(bool b)
    {
        m_bReadOnly = b;
    }
    /** 大文件快速保存开关：开启时纯覆盖编辑只写脏页（非原子）；关闭则整文件重写 */
    bool IsFastSaveEnabled() const
    {
        return m_bFastSaveEnabled;
    }
    void SetFastSaveEnabled(bool b)
    {
        m_bFastSaveEnabled = b;
    }
    /** 上一次 Save 是否走了脏页回写快速路径（用于状态栏提示） */
    bool LastSaveWasFast() const
    {
        return m_bLastSaveFast;
    }

    /** 读取逻辑流数据，返回实际读取的字节数 */
    u64 Read(u64 pos, u8 *buf, u64 len) const;

    /** 读取单个字节（越界返回 0） */
    u8 At(u64 pos) const;

    /** 覆盖写（不改变长度，可越过文件尾追加，自动用 0 填充） */
    bool Write(u64 pos, const u8 *data, u64 len);

    /** 在 pos 处插入数据 */
    bool Insert(u64 pos, const u8 *data, u64 len);

    /** 删除 [pos, pos+len) 区间 */
    bool Delete(u64 pos, u64 len);

    /** 截断到 newSize */
    bool Truncate(u64 newSize);

    /** 撤销 / 重做 */
    void BeginUndo();
    void EndUndo();
    bool Undo();
    bool Redo();
    bool CanUndo() const
    {
        return !m_undoStack.empty();
    }
    bool CanRedo() const
    {
        return !m_redoStack.empty();
    }

    /**
     * 保存到指定路径（pszPath==NULL 表示保存到原文件）
     * 保存成功后重新打开新文件，编辑历史清空。
     */
    bool Save(const wchar_t *pszPath = nullptr);

    /** 调试：输出当前段表 */
    void DumpSegs() const;

    /** 供写入用的路径（如果不是另存为） */
    std::wstring GetSavePath() const
    {
        return m_strSavePath;
    }

  private:
    struct UndoRec
    {
        enum
        {
            OVR,
            DEL,
            INS
        } type;
        u64 pos;
        u64 len;
        std::vector<u8> data;    /**< OVR: 原数据 / DEL: 被删数据 */
        std::vector<u8> newData; /**< OVR: 新写数据 */
        u64 storeOff;            /**< INS: 数据在 store 中的偏移 */
        u64 oldSize;             /**< OVR: 操作前逻辑大小 */
        u64 newSize;             /**< OVR: 操作后逻辑大小 */
    };
    typedef std::vector<UndoRec> UndoGroup;
    typedef std::shared_ptr<u8[]> PagePtr;

    void ClearEdits();
    u8 BaseByte(u64 pos) const;
    u64 LogicalToBase(u64 pos) const;

    bool m_bSuppressUndo() const
    {
        return m_bSuppress;
    }
    void SetSuppressUndo(bool b)
    {
        m_bSuppress = b;
    }

    /** 结构变化后重建 logicalStart 缓存 */
    void RebuildPieces();
    /** 查找覆盖逻辑位置 pos 的 Piece 索引；pos==size 时返回末尾 */
    int FindPiece(u64 pos) const;
    /** 把 pos 处的 Piece 拆成两段，返回右段索引（pos 不在某段内部则返回 -1） */
    int SplitPiece(int idx, u64 pos);
    void ApplyOverwrite(u64 pos, const u8 *data, u64 len);
    /** 在逻辑流尾部补零到 newSize（不产生 Undo 记录） */
    bool GrowTo(u64 newSize);
    /** 快速保存：纯覆盖编辑（无插入/删除、长度不变）时只把脏页写回原文件 */
    bool SaveOverwriteInPlace(const std::wstring &dst);
    /** 移除一条插入记录对应的（可能已被拆分的）插入 Piece，返回移除的字节数 */
    u64 RemoveInsertPieces(u64 pos, u64 len, u64 storeOff);
    /** 合并相邻且连续的同类 Piece，抑制碎片 */
    void MergePieces();

  private:
    bool m_bOpen;
    std::wstring m_strPath;
    std::wstring m_strSavePath;
    MappedFile m_mmap;

    u64 m_size;
    u64 m_baseSize;
    bool m_bModified;
    bool m_bReadOnly;
    bool m_bFastSaveEnabled; // 大文件纯覆盖保存是否走脏页回写
    bool m_bLastSaveFast;    // 上一次 Save 是否走了快速路径

    // 页覆盖层：page index -> 数据
    std::map<u64, PagePtr> m_overlay;
    // 插入数据缓冲
    std::vector<u8> m_store;
    // Piece 表（按 logicalStart 升序）
    std::vector<Piece> m_pieces;

    // 撤销栈
    std::vector<UndoGroup> m_undoStack;
    std::vector<UndoGroup> m_redoStack;
    std::vector<UndoRec> m_pending;
    bool m_bInUndo;
    bool m_bSuppress;
};

} // namespace hexmodel

#endif // HEX_FILE_MODEL_H
