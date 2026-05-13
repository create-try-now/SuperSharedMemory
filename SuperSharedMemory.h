// SuperSharedMemory.h
// 高性能跨进程通信框架 v1.0-1 
// 基于 Windows 命名共享内存，无外部依赖。直接包含本文件即可使用。
// 修改 SHM_NAME_PREFIX 可避免不同应用间的命名冲突。
//
// 特性：
// - 多槽位（Slot）读写，支持 FIFO 或 LIFO 队列模式（通过 Create 的 fifo_enabled 选择）。
// - Token 机制：将槽位绑定到逻辑通道，支持读写分离，不同 Token 可共享同一物理槽。
// - 伙伴系统内存池：高效分配/释放变长数据块，使用自旋锁保护空闲链表。
// - 零拷贝读取：通过 DataPtr（RAII）直接访问共享数据，避免拷贝。
// - 维护线程（可选）：扫描死进程 Token 并接管所有权，对残留 USING 槽进行怠速超时回收并释放数据块。
// - 手动清理 API：允许主动回收公共链表中的孤儿 USING 槽。
// - Token 名称：创建时可指定名称标识，方便识别僵尸 Token。
//
// 并发说明：
// - 公共模式（token_id=0）仅允许单线程/单进程使用，内部游标非线程安全。
// - 多线程/多进程应使用 Token 各自操作，互不干扰。
// - 共享数据结构通过自旋锁或原子操作保护，保证跨进程安全。
//
// 移动语义警告：
// - 本类可移动（移动构造/赋值未删除），但移动后维护线程内部指针会失效。
// - 如果启用了维护线程，移动前必须停止维护线程，移动后重新绑定或不再使用。
// - 推荐使用 std::unique_ptr<SuperSharedMemory> 管理生命周期，避免移动大对象。

#pragma once

#define NOMINMAX
#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <memory>
#include <algorithm>
#include <stdexcept>
#include <chrono>
#include <vector>
#include <mutex>
#include <unordered_map>

namespace SuperShm {

    // ===================================================================
    // 用户可配置常量
    // ===================================================================
    constexpr const char* SHM_NAME_PREFIX = "__SuperShm__";

    // ===================================================================
    // 内部常量
    // ===================================================================
    constexpr uint32_t MAGIC_NUMBER = 0x5353484D;   // "SSHM"
    constexpr uint32_t VERSION = 4;                  // 内部迭代版本
    constexpr size_t   CACHE_LINE_SIZE = 64;
    constexpr uint32_t MAX_SLOTS_PER_TOKEN = 64;
    constexpr uint32_t MAX_TOKENS = 256;
    constexpr uint32_t NULL_OFFSET = 0;
    constexpr uint32_t SLOT_STATE_FREE = 0;
    constexpr uint32_t SLOT_STATE_USING = 1;
    constexpr uint32_t SLOT_STATE_FILL = 2;
    constexpr uint32_t SLOT_STATE_OFFLINE = 3;

    constexpr size_t   SHM_RESERVED_SIZE = 1ULL * 1024 * 1024 * 1024;   // 1 GB

    constexpr uint32_t DEFAULT_BUDDY_MAX_ORDER = 24;   // 2^24 = 16 MB
    constexpr uint32_t BUDDY_MIN_ORDER = 6;

    inline std::string MakeMutexName(const std::string& user_name) {
        return std::string(SHM_NAME_PREFIX) + "_Mutex_" + user_name;
    }

    // ===================================================================
    // 基础工具
    // ===================================================================
    template<typename T>
    inline T* PtrFromOffset(uint8_t* base, uint32_t offset) {
        return offset ? reinterpret_cast<T*>(base + offset) : nullptr;
    }

    template<typename T>
    inline uint32_t OffsetFromPtr(uint8_t* base, T* ptr) {
        return ptr ? static_cast<uint32_t>(reinterpret_cast<uint8_t*>(ptr) - base) : 0;
    }

    inline uint32_t NextPowerOfTwo(uint32_t v) {
        v--;
        v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
        return v + 1;
    }

    inline uint32_t Log2Floor(uint32_t v) {
        uint32_t r = 0;
        while (v >>= 1) r++;
        return r;
    }

    // ===================================================================
    // 数据池块头 (伙伴系统)
    // ===================================================================
    struct BuddyBlockHeader {
        std::atomic<uint32_t> ref_count;
        uint32_t              block_size;
        uint32_t              order;
        std::atomic<uint32_t> next_free;
        uint32_t              next_token_held;   // 保留字段，不再使用
    };

    // ===================================================================
    // 槽位节点 (缓存行对齐)
    // ===================================================================
    struct alignas(CACHE_LINE_SIZE) Slot {
        std::atomic<int>       state;
        uint32_t               data_offset;
        uint32_t               data_size;
        std::atomic<uint32_t>  next_read;
        std::atomic<uint32_t>  next_write;
    };

    // ===================================================================
    // Token 条目
    // ===================================================================
    struct TokenEntry {
        std::atomic<uint32_t> id;
        std::atomic<uint32_t> owner_pid;
        std::atomic<uint32_t> read_count;
        std::atomic<uint32_t> write_count;
        uint32_t              read_slots[MAX_SLOTS_PER_TOKEN];
        uint32_t              write_slots[MAX_SLOTS_PER_TOKEN];
        std::atomic<uint32_t> flags;
        char                  name[64];                // Token 名称
    };

    // ===================================================================
    // 共享内存控制头
    // reserved[0] 用于 FIFO 标志
    // reserved[1] 用于 slot_list_lock (槽链表锁)
    // reserved[2] 用于 pool_lock (伙伴系统锁)
    // 注意：reserved 数组已改为原子类型，保证跨进程安全与类型匹配
    // ===================================================================
    struct ShmHeader {
        uint32_t              magic;
        uint32_t              version;
        uint32_t              max_slots;
        uint32_t              slot_stride;
        std::atomic<uint32_t> active_slot_count;
        std::atomic<uint32_t> read_head;
        std::atomic<uint32_t> write_tail;
        uint32_t              data_pool_offset;
        uint32_t              data_pool_size;
        uint32_t              token_table_offset;
        uint32_t              max_tokens;
        uint32_t              buddy_max_order;
        uint32_t              buddy_free_heads_offset;
        uint32_t              committed_watermark;
        uint32_t              initial_slots;
        std::atomic<uint32_t> maintenance_enabled;
        std::atomic<uint32_t> token_table_lock;
        std::atomic<uint32_t> owner_tracking_enabled;
        std::atomic<uint32_t> next_token_id;       // 全局 Token ID 生成器
        std::atomic<uint32_t> reserved[5];          // [0] FIFO, [1] slot_list_lock, [2] pool_lock
    };

    // ===================================================================
    // 抽象核心接口
    // ===================================================================
    class IShmCore {
    public:
        virtual ~IShmCore() = default;
        virtual uint32_t AllocBlock(uint32_t size) = 0;
        virtual void RetainBlock(uint32_t offset) = 0;
        virtual void ReleaseBlock(uint32_t offset) = 0;
        virtual void* GetDataPoolPtr(uint32_t offset) = 0;
        virtual const void* GetDataPoolPtr(uint32_t offset) const = 0;
        virtual uint32_t GetBlockSize(uint32_t offset) const = 0;
    };

    class ShmStrategy {
    public:
        virtual ~ShmStrategy() = default;
        virtual bool WriteToSlot(Slot& slot, const void* data, size_t len, IShmCore* core) = 0;
        virtual const void* OnDataReady(Slot& slot, size_t& out_len, IShmCore* core) = 0;
        virtual void OnDataCopy(Slot& slot, void* buffer, size_t& in_out_len, IShmCore* core) = 0;
    };

    // ===================================================================
    // 默认策略
    // ===================================================================
    class DefaultStrategy : public ShmStrategy {
    public:
        bool WriteToSlot(Slot& slot, const void* data, size_t len, IShmCore* core) override;
        const void* OnDataReady(Slot& slot, size_t& out_len, IShmCore* core) override;
        void OnDataCopy(Slot& slot, void* buffer, size_t& in_out_len, IShmCore* core) override;
    };

    // ===================================================================
    // 数据池 (伙伴系统，使用自旋锁保护空闲链表)
    // ===================================================================
    class DataPool {
    public:
        DataPool() = default;
        ~DataPool() = default;

        DataPool(const DataPool&) = delete;
        DataPool& operator=(const DataPool&) = delete;

        // 假移动，仅为编译通过；移动后两个对象指向同一共享内存视图，需确保源对象不再使用
        DataPool(DataPool&&) noexcept = default;
        DataPool& operator=(DataPool&&) noexcept = default;

        void Bind(uint8_t* base, uint32_t pool_offset, uint32_t pool_size,
            std::atomic<uint32_t>* buddy_free_heads, uint32_t max_order,
            std::atomic<uint32_t>* pool_lock);
        void InitAsCreator(uint8_t* base, uint32_t pool_offset, uint32_t pool_size,
            std::atomic<uint32_t>* buddy_free_heads, uint32_t max_order,
            std::atomic<uint32_t>* pool_lock);

        uint32_t AllocBlock(uint32_t size);
        void Retain(uint32_t block_offset);
        void Release(uint32_t block_offset);
        void* GetDataPtr(uint32_t block_offset);
        const void* GetDataPtr(uint32_t block_offset) const;
        uint32_t GetBlockSize(uint32_t block_offset) const;
        uint32_t GetPoolSize() const { return pool_size_; }
        uint32_t GetMaxOrder() const { return max_order_; }

    private:
        uint32_t BuddyAlloc(uint32_t order);
        void BuddyFree(uint32_t offset, uint32_t order);
        void AddToFreeList(uint32_t offset, uint32_t order);
        uint32_t PopFromFreeList(uint32_t order);
        void RemoveFromFreeList(uint32_t offset, uint32_t order);
        uint32_t GetBuddyOffset(uint32_t offset, uint32_t order) const;
        bool IsBuddyFree(uint32_t buddy_offset, uint32_t order) const;

        void LockPool();
        void UnlockPool();

        uint8_t* base_ = nullptr;
        uint32_t               pool_offset_ = 0;
        uint32_t               pool_size_ = 0;
        std::atomic<uint32_t>* buddy_free_heads_ = nullptr;
        uint32_t               max_order_ = 0;
        std::atomic<uint32_t>* pool_lock_ = nullptr;
    };

    // ===================================================================
    // DataPtr (RAII)
    // ===================================================================
    class DataPtr {
    public:
        DataPtr() = default;
        DataPtr(uint8_t* base, uint32_t offset, DataPool* pool, bool add_ref = true);
        ~DataPtr();
        DataPtr(DataPtr&& other) noexcept;
        DataPtr& operator=(DataPtr&& other) noexcept;
        DataPtr(const DataPtr&) = delete;
        DataPtr& operator=(const DataPtr&) = delete;

        const void* Get() const;
        size_t      Size() const;
        size_t      CopyTo(void* dest, size_t max_len) const;
        void        Release();
        void        Detach();
        bool        IsValid() const;

    private:
        uint8_t* base_ = nullptr;
        uint32_t   offset_ = 0;
        DataPool* pool_ = nullptr;
        bool       valid_ = false;
    };

    // ===================================================================
    // 前置声明
    // ===================================================================
    template<typename Strategy = DefaultStrategy>
    class SuperSharedMemory;
    template<typename S>
    class MaintenanceManager;

    enum class TransferResult : uint32_t {
        Success = 0,
        ErrNotOwner,
        ErrSlotInUse,
        ErrSlotNotFound,
        ErrTokenFull,
    };

    struct ZombieTokenInfo {
        uint32_t token_id;
        char     name[64];
    };

    // ===================================================================
    // 维护管理器
    // ===================================================================
    template<typename S>
    class MaintenanceManager {
    public:
        MaintenanceManager(const std::string& user_name, SuperSharedMemory<S>* shm,
            uint32_t orphan_using_timeout_ms = 5000);
        ~MaintenanceManager();
        bool Start();
        void Stop();
        bool IsRunning() const;

        std::vector<ZombieTokenInfo> GetZombieTokens() const;
        bool ReleaseZombieToken(uint32_t token_id);

    private:
        void MaintenanceLoop();
        void ProcessDeadToken(TokenEntry* token);

        std::string                  mutex_name_;
        HANDLE                       hMutex_ = nullptr;
        std::unique_ptr<std::thread> worker_;
        SuperSharedMemory<S>* shm_;
        std::atomic<bool>            running_{ false };
        uint32_t                     orphan_using_timeout_ms_;

        struct PendingUsingSlot {
            uint32_t slot_offset;
            uint32_t record_time_ms;
            uint32_t data_offset_snap;
            uint32_t data_size_snap;
        };

        mutable std::mutex                     pending_mutex_;
        std::unordered_map<uint32_t, std::vector<PendingUsingSlot>> pending_using_slots_;

        mutable std::mutex                     zombie_list_mutex_;
        std::vector<ZombieTokenInfo>           zombie_tokens_;
    };

    // ===================================================================
    // 超级共享内存主类
    // ===================================================================
    template<typename Strategy>
    class SuperSharedMemory : public IShmCore {
        static_assert(std::is_base_of_v<ShmStrategy, Strategy>, "Strategy must derive from ShmStrategy");
    public:
        friend class ShmStrategy;
        friend class DefaultStrategy;
        template<typename> friend class MaintenanceManager;

        static bool Exists(const std::string& name) {
            std::string full_name = std::string(SHM_NAME_PREFIX) + name;
            HANDLE h = OpenFileMappingA(FILE_MAP_READ, FALSE, full_name.c_str());
            if (h != nullptr) { CloseHandle(h); return true; }
            return false;
        }

        // ---------- 创建 / 打开 ----------
        static SuperSharedMemory Create(const std::string& name,
            uint32_t initial_slots = 256,
            uint32_t max_slots = 4096,
            uint32_t data_pool_size = 16 * 1024 * 1024,
            uint32_t buddy_max_order = DEFAULT_BUDDY_MAX_ORDER,
            bool enable_owner_tracking = false,
            bool fifo_enabled = false,
            Strategy strategy = Strategy()) {
            SuperSharedMemory shm;
            shm.name_ = name;
            shm.strategy_ = std::move(strategy);
            shm.owner_tracking_enabled_ = enable_owner_tracking;
            shm.buddy_max_order_ = buddy_max_order;
            shm.fifo_enabled_ = fifo_enabled;
            shm.MapMemory(name, true);
            shm.InitHeader(initial_slots, max_slots, data_pool_size, buddy_max_order, enable_owner_tracking, fifo_enabled);
            return shm;
        }

        static SuperSharedMemory Open(const std::string& name,
            Strategy strategy = Strategy()) {
            SuperSharedMemory shm;
            shm.name_ = name;
            shm.strategy_ = std::move(strategy);
            shm.MapMemory(name, false);
            shm.owner_tracking_enabled_ = (shm.header_->owner_tracking_enabled.load(std::memory_order_acquire) != 0);
            shm.buddy_max_order_ = shm.header_->buddy_max_order;
            shm.fifo_enabled_ = (shm.header_->reserved[0].load(std::memory_order_acquire) != 0);
            return shm;
        }

        SuperSharedMemory(const SuperSharedMemory&) = delete;
        SuperSharedMemory& operator=(const SuperSharedMemory&) = delete;
        // 允许移动，但移动后维护线程的内部指针会失效，需谨慎使用
        SuperSharedMemory(SuperSharedMemory&& other) noexcept
            : name_(std::move(other.name_)), hMapFile_(other.hMapFile_), base_ptr_(other.base_ptr_),
            header_(other.header_), slots_(other.slots_),
            data_pool_(std::move(other.data_pool_)), strategy_(std::move(other.strategy_)),
            maintenance_(std::move(other.maintenance_)),
            write_cursor_(other.write_cursor_), read_cursor_(other.read_cursor_),
            owner_tracking_enabled_(other.owner_tracking_enabled_),
            buddy_max_order_(other.buddy_max_order_),
            fifo_enabled_(other.fifo_enabled_) {
            other.hMapFile_ = nullptr; other.base_ptr_ = nullptr;
            other.header_ = nullptr; other.slots_ = nullptr;
            other.write_cursor_ = NULL_OFFSET; other.read_cursor_ = NULL_OFFSET;
            other.owner_tracking_enabled_ = false;
            other.buddy_max_order_ = DEFAULT_BUDDY_MAX_ORDER;
            other.fifo_enabled_ = false;
            // 注意：maintenance_ 内部 shm_ 仍指向旧对象，移动后需重新绑定或停止维护线程
        }
        SuperSharedMemory& operator=(SuperSharedMemory&& other) noexcept {
            if (this != &other) {
                Cleanup();
                name_ = std::move(other.name_);
                hMapFile_ = other.hMapFile_;
                base_ptr_ = other.base_ptr_;
                header_ = other.header_;
                slots_ = other.slots_;
                data_pool_ = std::move(other.data_pool_);
                strategy_ = std::move(other.strategy_);
                maintenance_ = std::move(other.maintenance_);
                write_cursor_ = other.write_cursor_;
                read_cursor_ = other.read_cursor_;
                owner_tracking_enabled_ = other.owner_tracking_enabled_;
                buddy_max_order_ = other.buddy_max_order_;
                fifo_enabled_ = other.fifo_enabled_;
                other.hMapFile_ = nullptr; other.base_ptr_ = nullptr;
                other.header_ = nullptr; other.slots_ = nullptr;
                other.write_cursor_ = NULL_OFFSET; other.read_cursor_ = NULL_OFFSET;
                other.owner_tracking_enabled_ = false;
                other.buddy_max_order_ = DEFAULT_BUDDY_MAX_ORDER;
                other.fifo_enabled_ = false;
                // 注意：maintenance_ 内部 shm_ 仍指向旧对象，移动后需重新绑定或停止维护线程
            }
            return *this;
        }

        ~SuperSharedMemory() { Cleanup(); }

        // ===================================================================
        // 公共接口
        // ===================================================================
        bool Write(const void* data, size_t len, uint32_t token_id = 0) {
            if (token_id != 0) {
                TokenEntry* token = GetTokenById(token_id);
                if (!token || token->owner_pid.load(std::memory_order_acquire) != GetCurrentProcessId())
                    return false;
                for (uint32_t i = 0; i < token->write_count; ++i) {
                    uint32_t off = token->write_slots[i];
                    Slot* slot = PtrFromOffset<Slot>(base_ptr_, off);
                    int expected = SLOT_STATE_FREE;
                    if (slot->state.compare_exchange_strong(expected, SLOT_STATE_USING,
                        std::memory_order_acquire)) {
                        if (!strategy_.WriteToSlot(*slot, data, len, this)) {
                            slot->state.store(SLOT_STATE_FREE, std::memory_order_release);
                            continue;
                        }
                        slot->state.store(SLOT_STATE_FILL, std::memory_order_release);
                        return true;
                    }
                }
                return false;
            }
            else {
                uint32_t slot_off = FindFreeSlot(write_cursor_);
                if (slot_off == NULL_OFFSET) return false;
                Slot* slot = PtrFromOffset<Slot>(base_ptr_, slot_off);
                if (!strategy_.WriteToSlot(*slot, data, len, this)) {
                    slot->state.store(SLOT_STATE_FREE, std::memory_order_release);
                    return false;
                }
                slot->state.store(SLOT_STATE_FILL, std::memory_order_release);
                return true;
            }
        }

        DataPtr ReadZeroCopy(uint32_t token_id = 0) {
            Slot* slot = nullptr;
            if (token_id != 0) {
                TokenEntry* token = GetTokenById(token_id);
                if (!token || token->owner_pid.load(std::memory_order_acquire) != GetCurrentProcessId())
                    return DataPtr();
                for (uint32_t i = 0; i < token->read_count; ++i) {
                    uint32_t off = token->read_slots[i];
                    Slot* s = PtrFromOffset<Slot>(base_ptr_, off);
                    int expected = SLOT_STATE_FILL;
                    if (s->state.compare_exchange_strong(expected, SLOT_STATE_USING,
                        std::memory_order_acquire)) {
                        slot = s; break;
                    }
                }
            }
            else {
                uint32_t off = FindFillSlot(read_cursor_);
                if (off != NULL_OFFSET) slot = PtrFromOffset<Slot>(base_ptr_, off);
            }
            if (!slot) return DataPtr();
            size_t len = 0;
            const void* data = strategy_.OnDataReady(*slot, len, this);
            if (!data) {
                slot->state.store(SLOT_STATE_FREE, std::memory_order_release);
                return DataPtr();
            }
            DataPtr dp(base_ptr_, slot->data_offset, &data_pool_, false);
            slot->data_offset = 0;
            slot->state.store(SLOT_STATE_FREE, std::memory_order_release);
            return dp;
        }

        bool ReadCopy(void* buffer, size_t& in_out_len, uint32_t token_id = 0) {
            Slot* slot = nullptr;
            if (token_id != 0) {
                TokenEntry* token = GetTokenById(token_id);
                if (!token || token->owner_pid.load(std::memory_order_acquire) != GetCurrentProcessId())
                    return false;
                for (uint32_t i = 0; i < token->read_count; ++i) {
                    uint32_t off = token->read_slots[i];
                    Slot* s = PtrFromOffset<Slot>(base_ptr_, off);
                    int expected = SLOT_STATE_FILL;
                    if (s->state.compare_exchange_strong(expected, SLOT_STATE_USING,
                        std::memory_order_acquire)) {
                        slot = s; break;
                    }
                }
            }
            else {
                uint32_t off = FindFillSlot(read_cursor_);
                if (off != NULL_OFFSET) slot = PtrFromOffset<Slot>(base_ptr_, off);
            }
            if (!slot) return false;
            strategy_.OnDataCopy(*slot, buffer, in_out_len, this);
            slot->state.store(SLOT_STATE_FREE, std::memory_order_release);
            return true;
        }

        uint32_t AcquireSlot(bool blocking = true, int mode = 0) {
            uint32_t slot_off = NULL_OFFSET;
            while (true) {
                slot_off = FindFreeSlot(write_cursor_);
                if (slot_off) break;
                if (!blocking) return 0;
                YieldProcessor();
            }
            if (mode == 0 || mode == 1) RemoveFromReadList(slot_off);
            if (mode == 0 || mode == 2) RemoveFromWriteList(slot_off);
            return slot_off;
        }

        bool ReleaseSlot(uint32_t slot_offset, int mode = 0) {
            Slot* slot = PtrFromOffset<Slot>(base_ptr_, slot_offset);
            if (!slot) return false;
            CleanSlotData(slot_offset);
            if (mode == 0 || mode == 1) InsertToReadHead(slot_offset);
            if (mode == 0 || mode == 2) InsertToWriteTail(slot_offset);
            slot->state.store(SLOT_STATE_FREE, std::memory_order_release);
            return true;
        }

        // ---------- Token 管理 ----------
        uint32_t CreateToken(uint32_t read_count, uint32_t write_count,
            bool blocking = true, int alloc_mode = 0,
            const char* token_name = "") {
            if (alloc_mode == 0) return CreateTokenDefault(read_count, write_count, blocking, token_name);
            else if (alloc_mode == 1) return CreateTokenForceSameSlot(read_count, write_count, blocking, token_name);
            else if (alloc_mode == 2) return CreateTokenForceSeparateSlot(read_count, write_count, blocking, token_name);
            else if (alloc_mode == 3) return CreateTokenTrySameSlot(read_count, write_count, blocking, token_name);
            else if (alloc_mode == 4) return CreateTokenTrySeparateSlot(read_count, write_count, blocking, token_name);
            return 0;
        }

        uint32_t ReleaseTokenHeldBlocks(uint32_t token_id) {
            LockTokenTable();
            TokenEntry* token = GetTokenById(token_id);
            if (!token) { UnlockTokenTable(); return 0; }

            uint32_t freed = 0;
            auto clean_slot = [&](uint32_t slot_off) {
                Slot* s = PtrFromOffset<Slot>(base_ptr_, slot_off);
                int expected = SLOT_STATE_FILL;
                if (s->state.compare_exchange_strong(expected, SLOT_STATE_USING,
                    std::memory_order_acquire)) {
                    if (s->data_offset) {
                        data_pool_.Release(s->data_offset);
                        freed++;
                    }
                    s->data_offset = 0;
                    s->data_size = 0;
                    s->state.store(SLOT_STATE_FREE, std::memory_order_release);
                }
                };

            for (uint32_t i = 0; i < token->read_count.load(std::memory_order_acquire); ++i)
                clean_slot(token->read_slots[i]);
            for (uint32_t i = 0; i < token->write_count.load(std::memory_order_acquire); ++i)
                clean_slot(token->write_slots[i]);

            UnlockTokenTable();
            return freed;
        }

        TransferResult TransferSlot(uint32_t src_token, uint32_t dst_token,
            uint32_t slot_offset, bool read_mode, bool write_mode,
            bool blocking = true) {
            TokenEntry* src = GetTokenById(src_token);
            TokenEntry* dst = GetTokenById(dst_token);
            if (!src || !dst) return TransferResult::ErrNotOwner;
            if (src->owner_pid.load(std::memory_order_acquire) != GetCurrentProcessId() ||
                dst->owner_pid.load(std::memory_order_acquire) != GetCurrentProcessId())
                return TransferResult::ErrNotOwner;

            Slot* slot = PtrFromOffset<Slot>(base_ptr_, slot_offset);
            if (!slot) return TransferResult::ErrSlotNotFound;

            int prev_state = SLOT_STATE_FREE;
            while (true) {
                int expected = slot->state.load(std::memory_order_acquire);
                if (expected == SLOT_STATE_USING) {
                    if (!blocking) return TransferResult::ErrSlotInUse;
                    YieldProcessor();
                    continue;
                }
                if (slot->state.compare_exchange_strong(expected, SLOT_STATE_USING,
                    std::memory_order_acquire)) {
                    prev_state = expected;
                    break;
                }
            }
            bool removed_read = false, removed_write = false;
            for (uint32_t i = 0; i < src->read_count; ++i) {
                if (src->read_slots[i] == slot_offset) {
                    uint32_t last = src->read_count.fetch_sub(1) - 1;
                    if (i != last) src->read_slots[i] = src->read_slots[last];
                    removed_read = true; break;
                }
            }
            for (uint32_t i = 0; i < src->write_count; ++i) {
                if (src->write_slots[i] == slot_offset) {
                    uint32_t last = src->write_count.fetch_sub(1) - 1;
                    if (i != last) src->write_slots[i] = src->write_slots[last];
                    removed_write = true; break;
                }
            }
            if (!removed_read && !removed_write) {
                slot->state.store(prev_state, std::memory_order_release);
                return TransferResult::ErrSlotNotFound;
            }
            bool added = false;
            if (read_mode) {
                uint32_t idx = dst->read_count.fetch_add(1);
                if (idx < MAX_SLOTS_PER_TOKEN) { dst->read_slots[idx] = slot_offset; added = true; }
                else dst->read_count.fetch_sub(1);
            }
            if (write_mode) {
                uint32_t idx = dst->write_count.fetch_add(1);
                if (idx < MAX_SLOTS_PER_TOKEN) { dst->write_slots[idx] = slot_offset; added = true; }
                else dst->write_count.fetch_sub(1);
            }
            if (!added) {
                if (read_mode && removed_read) {
                    uint32_t idx = src->read_count.fetch_add(1);
                    if (idx < MAX_SLOTS_PER_TOKEN) src->read_slots[idx] = slot_offset;
                }
                if (write_mode && removed_write) {
                    uint32_t idx = src->write_count.fetch_add(1);
                    if (idx < MAX_SLOTS_PER_TOKEN) src->write_slots[idx] = slot_offset;
                }
                slot->state.store(prev_state, std::memory_order_release);
                return TransferResult::ErrTokenFull;
            }
            slot->state.store(prev_state, std::memory_order_release);
            return TransferResult::Success;
        }

        // 查询接口
        uint32_t GetTokenReadCount(uint32_t token_id) const {
            TokenEntry* token = GetTokenById(token_id);
            return token ? token->read_count.load(std::memory_order_acquire) : 0;
        }
        uint32_t GetTokenWriteCount(uint32_t token_id) const {
            TokenEntry* token = GetTokenById(token_id);
            return token ? token->write_count.load(std::memory_order_acquire) : 0;
        }
        const uint32_t* GetTokenReadSlots(uint32_t token_id, uint32_t* out_count) const {
            TokenEntry* token = GetTokenById(token_id);
            if (!token) { if (out_count) *out_count = 0; return nullptr; }
            if (out_count) *out_count = token->read_count.load(std::memory_order_acquire);
            return token->read_slots;
        }
        const uint32_t* GetTokenWriteSlots(uint32_t token_id, uint32_t* out_count) const {
            TokenEntry* token = GetTokenById(token_id);
            if (!token) { if (out_count) *out_count = 0; return nullptr; }
            if (out_count) *out_count = token->write_count.load(std::memory_order_acquire);
            return token->write_slots;
        }
        bool GetTokenReadSlotAt(uint32_t token_id, uint32_t index, uint32_t* out_slot) const {
            TokenEntry* token = GetTokenById(token_id);
            if (!token || index >= token->read_count.load(std::memory_order_acquire)) return false;
            *out_slot = token->read_slots[index];
            return true;
        }
        bool GetTokenWriteSlotAt(uint32_t token_id, uint32_t index, uint32_t* out_slot) const {
            TokenEntry* token = GetTokenById(token_id);
            if (!token || index >= token->write_count.load(std::memory_order_acquire)) return false;
            *out_slot = token->write_slots[index];
            return true;
        }

        bool GetTokenName(uint32_t token_id, char* out_name, size_t out_len) const {
            TokenEntry* token = GetTokenById(token_id);
            if (!token) return false;
            strncpy_s(out_name, out_len, token->name, _TRUNCATE);
            return true;
        }

        void DestroyToken(uint32_t token_id) {
            LockTokenTable();
            TokenEntry* token = GetTokenById(token_id);
            if (token && token->owner_pid.load(std::memory_order_acquire) == GetCurrentProcessId()) {
                if (owner_tracking_enabled_)
                    ReleaseTokenHeldBlocks(token_id);
                for (uint32_t i = 0; i < token->read_count; ++i) InsertToReadHead(token->read_slots[i]);
                for (uint32_t i = 0; i < token->write_count; ++i) InsertToWriteTail(token->write_slots[i]);
                FreeTokenEntry(token);
            }
            UnlockTokenTable();
        }

        bool TransferToken(uint32_t token_id, uint32_t target_pid) {
            LockTokenTable();
            TokenEntry* token = GetTokenById(token_id);
            if (!token) { UnlockTokenTable(); return false; }
            uint32_t expected = GetCurrentProcessId();
            bool success = token->owner_pid.compare_exchange_strong(expected, target_pid, std::memory_order_acq_rel);
            UnlockTokenTable();
            return success;
        }

        bool AddReadSlot(uint32_t token_id, bool blocking = true) {
            TokenEntry* token = GetTokenById(token_id);
            if (!token || token->owner_pid.load(std::memory_order_acquire) != GetCurrentProcessId()) return false;
            uint32_t slot_off = NULL_OFFSET;
            while (true) {
                uint32_t cur = header_->read_head.load(std::memory_order_acquire);
                while (cur) {
                    Slot* s = PtrFromOffset<Slot>(base_ptr_, cur);
                    int expected = SLOT_STATE_FREE;
                    if (s->state.compare_exchange_strong(expected, SLOT_STATE_USING, std::memory_order_acquire)) {
                        slot_off = cur; break;
                    }
                    cur = s->next_read.load(std::memory_order_acquire);
                }
                if (slot_off) break;
                if (!blocking) return false;
                YieldProcessor();
            }
            RemoveFromReadList(slot_off);
            RemoveFromWriteList(slot_off);
            Slot* slot = PtrFromOffset<Slot>(base_ptr_, slot_off);
            slot->state.store(SLOT_STATE_FREE, std::memory_order_release);
            uint32_t idx = token->read_count.fetch_add(1);
            if (idx < MAX_SLOTS_PER_TOKEN) token->read_slots[idx] = slot_off;
            else { token->read_count.fetch_sub(1); InsertToReadHead(slot_off); InsertToWriteTail(slot_off); return false; }
            return true;
        }

        bool AddWriteSlot(uint32_t token_id, bool blocking = true) {
            TokenEntry* token = GetTokenById(token_id);
            if (!token || token->owner_pid.load(std::memory_order_acquire) != GetCurrentProcessId()) return false;
            uint32_t slot_off = NULL_OFFSET;
            while (true) {
                uint32_t cur = header_->write_tail.load(std::memory_order_acquire);
                while (cur) {
                    Slot* s = PtrFromOffset<Slot>(base_ptr_, cur);
                    int expected = SLOT_STATE_FREE;
                    if (s->state.compare_exchange_strong(expected, SLOT_STATE_USING, std::memory_order_acquire)) {
                        slot_off = cur; break;
                    }
                    cur = s->next_write.load(std::memory_order_acquire);
                }
                if (slot_off) break;
                if (!blocking) return false;
                YieldProcessor();
            }
            RemoveFromWriteList(slot_off);
            RemoveFromReadList(slot_off);
            Slot* slot = PtrFromOffset<Slot>(base_ptr_, slot_off);
            slot->state.store(SLOT_STATE_FREE, std::memory_order_release);
            uint32_t idx = token->write_count.fetch_add(1);
            if (idx < MAX_SLOTS_PER_TOKEN) token->write_slots[idx] = slot_off;
            else { token->write_count.fetch_sub(1); InsertToWriteTail(slot_off); InsertToReadHead(slot_off); return false; }
            return true;
        }

        bool RemoveReadSlot(uint32_t token_id, uint32_t slot_offset) {
            TokenEntry* token = GetTokenById(token_id);
            if (!token) return false;
            for (uint32_t i = 0; i < token->read_count; ++i) {
                if (token->read_slots[i] == slot_offset) {
                    InsertToReadHead(slot_offset);
                    uint32_t last = token->read_count.fetch_sub(1) - 1;
                    if (i != last) token->read_slots[i] = token->read_slots[last];
                    return true;
                }
            }
            return false;
        }

        bool RemoveWriteSlot(uint32_t token_id, uint32_t slot_offset) {
            TokenEntry* token = GetTokenById(token_id);
            if (!token) return false;
            for (uint32_t i = 0; i < token->write_count; ++i) {
                if (token->write_slots[i] == slot_offset) {
                    InsertToWriteTail(slot_offset);
                    uint32_t last = token->write_count.fetch_sub(1) - 1;
                    if (i != last) token->write_slots[i] = token->write_slots[last];
                    return true;
                }
            }
            return false;
        }

        bool AssignSlotToToken(uint32_t token_id, uint32_t slot_offset, bool read_mode, bool write_mode) {
            TokenEntry* token = GetTokenById(token_id);
            if (!token) return false;
            if (read_mode) {
                uint32_t idx = token->read_count.fetch_add(1);
                if (idx < MAX_SLOTS_PER_TOKEN) token->read_slots[idx] = slot_offset;
                else { token->read_count.fetch_sub(1); return false; }
            }
            if (write_mode) {
                uint32_t idx = token->write_count.fetch_add(1);
                if (idx < MAX_SLOTS_PER_TOKEN) token->write_slots[idx] = slot_offset;
                else { token->write_count.fetch_sub(1); return false; }
            }
            if (read_mode) RemoveFromReadList(slot_offset);
            if (write_mode) RemoveFromWriteList(slot_offset);
            return true;
        }

        bool ExpandSlots(uint32_t additional_count) {
            uint32_t current = header_->active_slot_count.load();
            uint32_t max = header_->max_slots;
            if (current + additional_count > max) return false;
            uint32_t new_total = current + additional_count;
            size_t commit_size = (new_total - header_->committed_watermark) * sizeof(Slot);
            if (commit_size > 0) {
                VirtualAlloc(base_ptr_ + sizeof(ShmHeader) + header_->committed_watermark * sizeof(Slot),
                    commit_size, MEM_COMMIT, PAGE_READWRITE);
            }
            for (uint32_t i = current; i < new_total; ++i) {
                Slot* s = &slots_[i];
                s->state.store(SLOT_STATE_FREE, std::memory_order_relaxed);
                s->data_offset = 0; s->data_size = 0;
                InsertToReadHead(OffsetFromPtr(base_ptr_, s));
                InsertToWriteTail(OffsetFromPtr(base_ptr_, s));
            }
            header_->active_slot_count.store(new_total, std::memory_order_release);
            header_->committed_watermark = new_total;
            return true;
        }

        bool StartMaintenance(uint32_t orphan_using_timeout_ms = 5000) {
            if (header_->maintenance_enabled.load(std::memory_order_acquire)) return false;
            maintenance_ = std::make_unique<MaintenanceManager<Strategy>>(name_, this, orphan_using_timeout_ms);
            return maintenance_->Start();
        }
        void StopMaintenance() { if (maintenance_) maintenance_->Stop(); }
        uint32_t GetActiveSlotCount() const { return header_->active_slot_count.load(); }
        uint32_t GetMaxSlots() const { return header_->max_slots; }

        std::vector<ZombieTokenInfo> GetZombieTokens() const {
            if (maintenance_) return maintenance_->GetZombieTokens();
            return {};
        }
        bool ReleaseZombieToken(uint32_t token_id) {
            if (!maintenance_) return false;
            return maintenance_->ReleaseZombieToken(token_id);
        }

        void ReclaimOrphanSlots(uint32_t timeout_ms) {
            std::vector<uint32_t> candidates;
            uint32_t cur = header_->read_head.load(std::memory_order_acquire);
            while (cur) {
                Slot* s = PtrFromOffset<Slot>(base_ptr_, cur);
                if (s->state.load(std::memory_order_acquire) == SLOT_STATE_USING)
                    candidates.push_back(cur);
                cur = s->next_read.load(std::memory_order_acquire);
            }
            cur = header_->write_tail.load(std::memory_order_acquire);
            while (cur) {
                Slot* s = PtrFromOffset<Slot>(base_ptr_, cur);
                if (s->state.load(std::memory_order_acquire) == SLOT_STATE_USING)
                    candidates.push_back(cur);
                cur = s->next_write.load(std::memory_order_acquire);
            }
            std::sort(candidates.begin(), candidates.end());
            candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

            TokenEntry* tokens = reinterpret_cast<TokenEntry*>(base_ptr_ + header_->token_table_offset);
            candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](uint32_t off) {
                for (uint32_t i = 0; i < MAX_TOKENS; ++i) {
                    if (tokens[i].id.load(std::memory_order_acquire) == 0) continue;
                    for (uint32_t r = 0; r < tokens[i].read_count; ++r)
                        if (tokens[i].read_slots[r] == off) return true;
                    for (uint32_t w = 0; w < tokens[i].write_count; ++w)
                        if (tokens[i].write_slots[w] == off) return true;
                }
                return false;
                }), candidates.end());

            if (candidates.empty()) return;

            struct Snap { uint32_t offset; uint32_t data_offset; uint32_t data_size; };
            std::vector<Snap> snaps;
            for (auto off : candidates) {
                Slot* s = PtrFromOffset<Slot>(base_ptr_, off);
                snaps.push_back({ off, s->data_offset, s->data_size });
            }

            if (timeout_ms > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));

            for (const auto& snap : snaps) {
                Slot* s = PtrFromOffset<Slot>(base_ptr_, snap.offset);
                int expected = SLOT_STATE_USING;
                if (s->state.load(std::memory_order_acquire) == SLOT_STATE_USING &&
                    s->data_offset == snap.data_offset && s->data_size == snap.data_size) {
                    if (s->data_offset) {
                        ReleaseBlock(s->data_offset);
                        s->data_offset = 0;
                    }
                    s->state.store(SLOT_STATE_FREE, std::memory_order_release);
                }
            }
        }

        // IShmCore 实现
        uint32_t AllocBlock(uint32_t size) override { return data_pool_.AllocBlock(size); }
        void RetainBlock(uint32_t offset) override { data_pool_.Retain(offset); }
        void ReleaseBlock(uint32_t offset) override { data_pool_.Release(offset); }
        void* GetDataPoolPtr(uint32_t offset) override { return data_pool_.GetDataPtr(offset); }
        const void* GetDataPoolPtr(uint32_t offset) const override { return data_pool_.GetDataPtr(offset); }
        uint32_t GetBlockSize(uint32_t offset) const override { return data_pool_.GetBlockSize(offset); }

    private:
        SuperSharedMemory() : write_cursor_(NULL_OFFSET), read_cursor_(NULL_OFFSET),
            owner_tracking_enabled_(false), buddy_max_order_(DEFAULT_BUDDY_MAX_ORDER),
            fifo_enabled_(false) {
        }

        void CleanSlotData(uint32_t slot_offset) {
            Slot* slot = PtrFromOffset<Slot>(base_ptr_, slot_offset);
            if (!slot || !slot->data_offset) return;
            data_pool_.Release(slot->data_offset);
            slot->data_offset = 0;
            slot->data_size = 0;
        }

        // ---------- 内部 Token 创建辅助 ----------
        uint32_t CreateTokenDefault(uint32_t read_count, uint32_t write_count, bool blocking, const char* token_name) {
            LockTokenTable();
            TokenEntry* token = AllocTokenEntry();
            if (!token) { UnlockTokenTable(); return 0; }
            token->id.store(header_->next_token_id.fetch_add(1), std::memory_order_release);
            token->owner_pid.store(GetCurrentProcessId(), std::memory_order_release);
            token->read_count = 0; token->write_count = 0;
            strncpy_s(token->name, token_name, _TRUNCATE);
            for (uint32_t i = 0; i < read_count; ++i) {
                if (!AddReadSlot(token->id.load(), blocking)) {
                    DestroyToken(token->id.load()); UnlockTokenTable(); return 0;
                }
            }
            for (uint32_t i = 0; i < write_count; ++i) {
                if (!AddWriteSlot(token->id.load(), blocking)) {
                    DestroyToken(token->id.load()); UnlockTokenTable(); return 0;
                }
            }
            UnlockTokenTable();
            return token->id.load();
        }

        uint32_t CreateTokenForceSameSlot(uint32_t read_count, uint32_t write_count, bool blocking, const char* token_name) {
            if (read_count != write_count) return 0;
            LockTokenTable();
            TokenEntry* token = AllocTokenEntry();
            if (!token) { UnlockTokenTable(); return 0; }
            token->id.store(header_->next_token_id.fetch_add(1), std::memory_order_release);
            token->owner_pid.store(GetCurrentProcessId(), std::memory_order_release);
            token->read_count = 0; token->write_count = 0;
            strncpy_s(token->name, token_name, _TRUNCATE);
            for (uint32_t i = 0; i < read_count; ++i) {
                uint32_t slot_off = NULL_OFFSET;
                while (true) {
                    uint32_t cur = header_->write_tail.load(std::memory_order_acquire);
                    while (cur) {
                        Slot* s = PtrFromOffset<Slot>(base_ptr_, cur);
                        int expected = SLOT_STATE_FREE;
                        if (s->state.compare_exchange_strong(expected, SLOT_STATE_USING, std::memory_order_acquire)) {
                            slot_off = cur; break;
                        }
                        cur = s->next_write.load(std::memory_order_acquire);
                    }
                    if (slot_off) break;
                    if (!blocking) { DestroyToken(token->id.load()); UnlockTokenTable(); return 0; }
                    YieldProcessor();
                }
                RemoveFromWriteList(slot_off);
                RemoveFromReadList(slot_off);
                Slot* slot = PtrFromOffset<Slot>(base_ptr_, slot_off);
                slot->state.store(SLOT_STATE_FREE, std::memory_order_release);
                if (!AddSlotToBoth(token, slot_off)) {
                    InsertToWriteTail(slot_off); InsertToReadHead(slot_off);
                    DestroyToken(token->id.load()); UnlockTokenTable(); return 0;
                }
            }
            UnlockTokenTable();
            return token->id.load();
        }

        uint32_t CreateTokenForceSeparateSlot(uint32_t read_count, uint32_t write_count, bool blocking, const char* token_name) {
            LockTokenTable();
            TokenEntry* token = AllocTokenEntry();
            if (!token) { UnlockTokenTable(); return 0; }
            token->id.store(header_->next_token_id.fetch_add(1), std::memory_order_release);
            token->owner_pid.store(GetCurrentProcessId(), std::memory_order_release);
            token->read_count = 0; token->write_count = 0;
            strncpy_s(token->name, token_name, _TRUNCATE);
            std::uint32_t write_slots_arr[MAX_SLOTS_PER_TOKEN];
            uint32_t w_cnt = 0;
            for (uint32_t i = 0; i < write_count; ++i) {
                uint32_t slot_off = NULL_OFFSET;
                while (true) {
                    uint32_t cur = header_->write_tail.load(std::memory_order_acquire);
                    while (cur) {
                        Slot* s = PtrFromOffset<Slot>(base_ptr_, cur);
                        int expected = SLOT_STATE_FREE;
                        if (s->state.compare_exchange_strong(expected, SLOT_STATE_USING, std::memory_order_acquire)) {
                            slot_off = cur; break;
                        }
                        cur = s->next_write.load(std::memory_order_acquire);
                    }
                    if (slot_off) break;
                    if (!blocking) {
                        for (uint32_t j = 0; j < w_cnt; ++j) InsertToWriteTail(write_slots_arr[j]);
                        DestroyToken(token->id.load()); UnlockTokenTable(); return 0;
                    }
                    YieldProcessor();
                }
                RemoveFromWriteList(slot_off);
                RemoveFromReadList(slot_off);
                Slot* slot = PtrFromOffset<Slot>(base_ptr_, slot_off);
                slot->state.store(SLOT_STATE_FREE, std::memory_order_release);
                write_slots_arr[w_cnt++] = slot_off;
                if (!AddSlotToWriteOnly(token, slot_off)) {
                    for (uint32_t j = 0; j < w_cnt; ++j) InsertToWriteTail(write_slots_arr[j]);
                    DestroyToken(token->id.load()); UnlockTokenTable(); return 0;
                }
            }
            for (uint32_t i = 0; i < read_count; ++i) {
                uint32_t slot_off = NULL_OFFSET;
                while (true) {
                    uint32_t cur = header_->read_head.load(std::memory_order_acquire);
                    bool found = false;
                    while (cur) {
                        Slot* s = PtrFromOffset<Slot>(base_ptr_, cur);
                        bool overlap = false;
                        for (uint32_t j = 0; j < w_cnt; ++j) {
                            if (write_slots_arr[j] == cur) { overlap = true; break; }
                        }
                        if (!overlap) {
                            int expected = SLOT_STATE_FREE;
                            if (s->state.compare_exchange_strong(expected, SLOT_STATE_USING, std::memory_order_acquire)) {
                                slot_off = cur; found = true; break;
                            }
                        }
                        cur = s->next_read.load(std::memory_order_acquire);
                    }
                    if (!found) {
                        cur = header_->write_tail.load(std::memory_order_acquire);
                        while (cur) {
                            Slot* s = PtrFromOffset<Slot>(base_ptr_, cur);
                            bool overlap = false;
                            for (uint32_t j = 0; j < w_cnt; ++j) {
                                if (write_slots_arr[j] == cur) { overlap = true; break; }
                            }
                            if (!overlap) {
                                int expected = SLOT_STATE_FREE;
                                if (s->state.compare_exchange_strong(expected, SLOT_STATE_USING, std::memory_order_acquire)) {
                                    slot_off = cur; found = true; break;
                                }
                            }
                            cur = s->next_write.load(std::memory_order_acquire);
                        }
                    }
                    if (slot_off) break;
                    if (!blocking) {
                        for (uint32_t j = 0; j < w_cnt; ++j) {
                            RemoveWriteSlot(token->id.load(), write_slots_arr[j]);
                            InsertToWriteTail(write_slots_arr[j]);
                        }
                        DestroyToken(token->id.load()); UnlockTokenTable(); return 0;
                    }
                    YieldProcessor();
                }
                RemoveFromReadList(slot_off);
                RemoveFromWriteList(slot_off);
                Slot* slot = PtrFromOffset<Slot>(base_ptr_, slot_off);
                slot->state.store(SLOT_STATE_FREE, std::memory_order_release);
                if (!AddSlotToReadOnly(token, slot_off)) {
                    for (uint32_t j = 0; j < w_cnt; ++j) {
                        RemoveWriteSlot(token->id.load(), write_slots_arr[j]);
                        InsertToWriteTail(write_slots_arr[j]);
                    }
                    DestroyToken(token->id.load()); UnlockTokenTable(); return 0;
                }
            }
            UnlockTokenTable();
            return token->id.load();
        }

        uint32_t CreateTokenTrySameSlot(uint32_t read_count, uint32_t write_count, bool blocking, const char* token_name) {
            uint32_t target = (std::min)(read_count, write_count);
            if (target == 0) return 0;
            LockTokenTable();
            TokenEntry* token = AllocTokenEntry();
            if (!token) { UnlockTokenTable(); return 0; }
            token->id.store(header_->next_token_id.fetch_add(1), std::memory_order_release);
            token->owner_pid.store(GetCurrentProcessId(), std::memory_order_release);
            token->read_count = 0; token->write_count = 0;
            strncpy_s(token->name, token_name, _TRUNCATE);
            uint32_t allocated = 0;
            for (; allocated < target; ++allocated) {
                uint32_t slot_off = NULL_OFFSET;
                while (true) {
                    uint32_t cur = header_->write_tail.load(std::memory_order_acquire);
                    while (cur) {
                        Slot* s = PtrFromOffset<Slot>(base_ptr_, cur);
                        int expected = SLOT_STATE_FREE;
                        if (s->state.compare_exchange_strong(expected, SLOT_STATE_USING, std::memory_order_acquire)) {
                            slot_off = cur; break;
                        }
                        cur = s->next_write.load(std::memory_order_acquire);
                    }
                    if (slot_off) break;
                    if (!blocking) {
                        if (allocated == 0) { DestroyToken(token->id.load()); UnlockTokenTable(); return 0; }
                        UnlockTokenTable(); return token->id.load();
                    }
                    YieldProcessor();
                }
                RemoveFromWriteList(slot_off);
                RemoveFromReadList(slot_off);
                Slot* slot = PtrFromOffset<Slot>(base_ptr_, slot_off);
                slot->state.store(SLOT_STATE_FREE, std::memory_order_release);
                AddSlotToBoth(token, slot_off);
            }
            UnlockTokenTable();
            return token->id.load();
        }

        uint32_t CreateTokenTrySeparateSlot(uint32_t read_count, uint32_t write_count, bool blocking, const char* token_name) {
            LockTokenTable();
            TokenEntry* token = AllocTokenEntry();
            if (!token) { UnlockTokenTable(); return 0; }
            token->id.store(header_->next_token_id.fetch_add(1), std::memory_order_release);
            token->owner_pid.store(GetCurrentProcessId(), std::memory_order_release);
            token->read_count = 0; token->write_count = 0;
            strncpy_s(token->name, token_name, _TRUNCATE);
            std::uint32_t write_slots_arr[MAX_SLOTS_PER_TOKEN];
            uint32_t w_cnt = 0;
            for (uint32_t i = 0; i < write_count; ++i) {
                uint32_t slot_off = NULL_OFFSET;
                while (true) {
                    uint32_t cur = header_->write_tail.load(std::memory_order_acquire);
                    while (cur) {
                        Slot* s = PtrFromOffset<Slot>(base_ptr_, cur);
                        int expected = SLOT_STATE_FREE;
                        if (s->state.compare_exchange_strong(expected, SLOT_STATE_USING, std::memory_order_acquire)) {
                            slot_off = cur; break;
                        }
                        cur = s->next_write.load(std::memory_order_acquire);
                    }
                    if (slot_off) break;
                    if (!blocking) { i = write_count; break; }
                    YieldProcessor();
                }
                if (slot_off) {
                    RemoveFromWriteList(slot_off);
                    RemoveFromReadList(slot_off);
                    Slot* slot = PtrFromOffset<Slot>(base_ptr_, slot_off);
                    slot->state.store(SLOT_STATE_FREE, std::memory_order_release);
                    write_slots_arr[w_cnt++] = slot_off;
                    AddSlotToWriteOnly(token, slot_off);
                }
            }
            uint32_t r_cnt = 0;
            for (uint32_t i = 0; i < read_count; ++i) {
                uint32_t slot_off = NULL_OFFSET;
                while (true) {
                    uint32_t cur = header_->read_head.load(std::memory_order_acquire);
                    bool found = false;
                    while (cur) {
                        Slot* s = PtrFromOffset<Slot>(base_ptr_, cur);
                        bool overlap = false;
                        for (uint32_t j = 0; j < w_cnt; ++j) {
                            if (write_slots_arr[j] == cur) { overlap = true; break; }
                        }
                        if (!overlap) {
                            int expected = SLOT_STATE_FREE;
                            if (s->state.compare_exchange_strong(expected, SLOT_STATE_USING, std::memory_order_acquire)) {
                                slot_off = cur; found = true; break;
                            }
                        }
                        cur = s->next_read.load(std::memory_order_acquire);
                    }
                    if (!found) {
                        cur = header_->write_tail.load(std::memory_order_acquire);
                        while (cur) {
                            Slot* s = PtrFromOffset<Slot>(base_ptr_, cur);
                            bool overlap = false;
                            for (uint32_t j = 0; j < w_cnt; ++j) {
                                if (write_slots_arr[j] == cur) { overlap = true; break; }
                            }
                            if (!overlap) {
                                int expected = SLOT_STATE_FREE;
                                if (s->state.compare_exchange_strong(expected, SLOT_STATE_USING, std::memory_order_acquire)) {
                                    slot_off = cur; found = true; break;
                                }
                            }
                            cur = s->next_write.load(std::memory_order_acquire);
                        }
                    }
                    if (slot_off) break;
                    if (!blocking) { i = read_count; break; }
                    YieldProcessor();
                }
                if (slot_off) {
                    RemoveFromReadList(slot_off);
                    RemoveFromWriteList(slot_off);
                    Slot* slot = PtrFromOffset<Slot>(base_ptr_, slot_off);
                    slot->state.store(SLOT_STATE_FREE, std::memory_order_release);
                    AddSlotToReadOnly(token, slot_off);
                    r_cnt++;
                }
            }
            if (w_cnt == 0 && r_cnt == 0) {
                DestroyToken(token->id.load()); UnlockTokenTable(); return 0;
            }
            UnlockTokenTable();
            return token->id.load();
        }

        bool AddSlotToBoth(TokenEntry* token, uint32_t slot_offset) {
            uint32_t ridx = token->read_count.fetch_add(1);
            if (ridx >= MAX_SLOTS_PER_TOKEN) { token->read_count.fetch_sub(1); return false; }
            token->read_slots[ridx] = slot_offset;
            uint32_t widx = token->write_count.fetch_add(1);
            if (widx >= MAX_SLOTS_PER_TOKEN) { token->read_count.fetch_sub(1); return false; }
            token->write_slots[widx] = slot_offset;
            return true;
        }
        bool AddSlotToReadOnly(TokenEntry* token, uint32_t slot_offset) {
            uint32_t idx = token->read_count.fetch_add(1);
            if (idx >= MAX_SLOTS_PER_TOKEN) { token->read_count.fetch_sub(1); return false; }
            token->read_slots[idx] = slot_offset;
            return true;
        }
        bool AddSlotToWriteOnly(TokenEntry* token, uint32_t slot_offset) {
            uint32_t idx = token->write_count.fetch_add(1);
            if (idx >= MAX_SLOTS_PER_TOKEN) { token->write_count.fetch_sub(1); return false; }
            token->write_slots[idx] = slot_offset;
            return true;
        }

        // ---------- 内存映射、初始化等内部实现 ----------
        void InitHeader(uint32_t initial_slots, uint32_t max_slots, uint32_t data_pool_size,
            uint32_t buddy_max_order, bool enable_owner_tracking, bool fifo) {
            size_t buddy_heads_count = buddy_max_order + 1;
            size_t buddy_heads_size = buddy_heads_count * sizeof(std::atomic<uint32_t>);
            size_t header_slots_size = sizeof(ShmHeader) + max_slots * sizeof(Slot) + buddy_heads_size;
            size_t total_commit = header_slots_size + data_pool_size + MAX_TOKENS * sizeof(TokenEntry);
            VirtualAlloc(base_ptr_, total_commit, MEM_COMMIT, PAGE_READWRITE);

            header_->magic = MAGIC_NUMBER;
            header_->version = VERSION;
            header_->max_slots = max_slots;
            header_->slot_stride = sizeof(Slot);
            header_->active_slot_count.store(initial_slots, std::memory_order_relaxed);
            header_->read_head.store(NULL_OFFSET, std::memory_order_relaxed);
            header_->write_tail.store(NULL_OFFSET, std::memory_order_relaxed);
            header_->data_pool_offset = static_cast<uint32_t>(header_slots_size);
            header_->data_pool_size = data_pool_size;
            header_->token_table_offset = header_->data_pool_offset + data_pool_size;
            header_->max_tokens = MAX_TOKENS;
            header_->buddy_max_order = buddy_max_order;
            header_->buddy_free_heads_offset = static_cast<uint32_t>(sizeof(ShmHeader) + max_slots * sizeof(Slot));
            header_->committed_watermark = initial_slots;
            header_->initial_slots = initial_slots;
            header_->maintenance_enabled.store(0, std::memory_order_relaxed);
            header_->token_table_lock.store(0, std::memory_order_relaxed);
            header_->owner_tracking_enabled.store(enable_owner_tracking ? 1 : 0, std::memory_order_relaxed);
            header_->next_token_id.store(1, std::memory_order_relaxed);
            header_->reserved[0].store(fifo ? 1 : 0, std::memory_order_relaxed);
            header_->reserved[1].store(0, std::memory_order_relaxed); // slot_list_lock
            header_->reserved[2].store(0, std::memory_order_relaxed); // pool_lock

            slots_ = reinterpret_cast<Slot*>(base_ptr_ + sizeof(ShmHeader));

            if (fifo) {
                for (uint32_t i = 0; i < initial_slots; ++i) {
                    Slot* s = &slots_[i];
                    s->state.store(SLOT_STATE_FREE, std::memory_order_relaxed);
                    s->data_offset = 0; s->data_size = 0;
                    if (i + 1 < initial_slots)
                        s->next_read.store(OffsetFromPtr(base_ptr_, &slots_[i + 1]), std::memory_order_relaxed);
                    else
                        s->next_read.store(NULL_OFFSET, std::memory_order_relaxed);
                    if (i + 1 < initial_slots)
                        s->next_write.store(OffsetFromPtr(base_ptr_, &slots_[i + 1]), std::memory_order_relaxed);
                    else
                        s->next_write.store(NULL_OFFSET, std::memory_order_relaxed);
                }
                if (initial_slots > 0) {
                    header_->read_head.store(OffsetFromPtr(base_ptr_, &slots_[0]), std::memory_order_release);
                    header_->write_tail.store(OffsetFromPtr(base_ptr_, &slots_[0]), std::memory_order_release);
                }
            }
            else {
                // LIFO 模式：读链表也为后进先出（头部插入），写链表反向（栈）
                for (uint32_t i = 0; i < initial_slots; ++i) {
                    Slot* s = &slots_[i];
                    s->state.store(SLOT_STATE_FREE, std::memory_order_relaxed);
                    s->data_offset = 0; s->data_size = 0;
                    if (i + 1 < initial_slots)
                        s->next_read.store(OffsetFromPtr(base_ptr_, &slots_[i + 1]), std::memory_order_relaxed);
                    else
                        s->next_read.store(NULL_OFFSET, std::memory_order_relaxed);
                    if (i == 0)
                        s->next_write.store(NULL_OFFSET, std::memory_order_relaxed);
                    else
                        s->next_write.store(OffsetFromPtr(base_ptr_, &slots_[i - 1]), std::memory_order_relaxed);
                }
                if (initial_slots > 0) {
                    header_->read_head.store(OffsetFromPtr(base_ptr_, &slots_[0]), std::memory_order_release);
                    header_->write_tail.store(OffsetFromPtr(base_ptr_, &slots_[initial_slots - 1]), std::memory_order_release);
                }
            }

            auto* buddy_heads = reinterpret_cast<std::atomic<uint32_t>*>(base_ptr_ + header_->buddy_free_heads_offset);
            for (uint32_t i = 0; i <= buddy_max_order; ++i)
                buddy_heads[i].store(NULL_OFFSET, std::memory_order_relaxed);
            data_pool_.InitAsCreator(base_ptr_, header_->data_pool_offset, data_pool_size, buddy_heads, buddy_max_order,
                &header_->reserved[2]);

            TokenEntry* tokens = reinterpret_cast<TokenEntry*>(base_ptr_ + header_->token_table_offset);
            std::memset(tokens, 0, MAX_TOKENS * sizeof(TokenEntry));

            write_cursor_ = NULL_OFFSET;
            read_cursor_ = NULL_OFFSET;
        }

        void MapMemory(const std::string& name, bool create) {
            std::string full_name = std::string(SHM_NAME_PREFIX) + name;
            if (create) {
                hMapFile_ = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE,
                    static_cast<DWORD>(SHM_RESERVED_SIZE >> 32),
                    static_cast<DWORD>(SHM_RESERVED_SIZE & 0xFFFFFFFF),
                    full_name.c_str());
                if (!hMapFile_ || GetLastError() == ERROR_ALREADY_EXISTS)
                    throw std::runtime_error("CreateFileMapping failed");
            }
            else {
                hMapFile_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, full_name.c_str());
                if (!hMapFile_) throw std::runtime_error("OpenFileMapping failed");
            }
            base_ptr_ = static_cast<uint8_t*>(MapViewOfFile(hMapFile_, FILE_MAP_ALL_ACCESS, 0, 0, 0));
            if (!base_ptr_) throw std::runtime_error("MapViewOfFile failed");
            header_ = reinterpret_cast<ShmHeader*>(base_ptr_);
            if (!create && header_->magic != MAGIC_NUMBER)
                throw std::runtime_error("Invalid magic number");
            slots_ = reinterpret_cast<Slot*>(base_ptr_ + sizeof(ShmHeader));
            if (!create) {
                auto* buddy_heads = reinterpret_cast<std::atomic<uint32_t>*>(base_ptr_ + header_->buddy_free_heads_offset);
                data_pool_.Bind(base_ptr_, header_->data_pool_offset, header_->data_pool_size,
                    buddy_heads, header_->buddy_max_order, &header_->reserved[2]);
            }
            write_cursor_ = NULL_OFFSET;
            read_cursor_ = NULL_OFFSET;
        }

        void Cleanup() {
            if (base_ptr_) { UnmapViewOfFile(base_ptr_); base_ptr_ = nullptr; }
            if (hMapFile_) { CloseHandle(hMapFile_); hMapFile_ = nullptr; }
        }

        bool IsValidSlotOffset(uint32_t offset) const {
            if (offset == NULL_OFFSET) return false;
            uint32_t slot_begin = OffsetFromPtr(base_ptr_, slots_);
            uint32_t slot_end = slot_begin + header_->max_slots * sizeof(Slot);
            return offset >= slot_begin && offset < slot_end;
        }

        uint32_t FindFreeSlot(uint32_t& cursor) {
            uint32_t remaining = header_->active_slot_count.load(std::memory_order_acquire);
            if (remaining == 0) return NULL_OFFSET;
            if (!IsValidSlotOffset(cursor)) cursor = header_->write_tail.load(std::memory_order_acquire);
            uint32_t start = cursor;
            do {
                if (!IsValidSlotOffset(cursor)) {
                    cursor = header_->write_tail.load(std::memory_order_acquire);
                    remaining = header_->active_slot_count.load(std::memory_order_acquire);
                    if (remaining == 0 || cursor == NULL_OFFSET) break;
                    start = cursor;
                }
                Slot* slot = PtrFromOffset<Slot>(base_ptr_, cursor);
                int expected = SLOT_STATE_FREE;
                if (slot->state.compare_exchange_strong(expected, SLOT_STATE_USING, std::memory_order_acquire))
                    return cursor;
                cursor = slot->next_write.load(std::memory_order_acquire);
                if (cursor == NULL_OFFSET) {
                    cursor = header_->write_tail.load(std::memory_order_acquire);
                    remaining = header_->active_slot_count.load(std::memory_order_acquire);
                    if (remaining == 0 || cursor == NULL_OFFSET) break;
                }
            } while (--remaining && cursor != start);
            return NULL_OFFSET;
        }

        uint32_t FindFillSlot(uint32_t& cursor) {
            uint32_t remaining = header_->active_slot_count.load(std::memory_order_acquire);
            if (remaining == 0) return NULL_OFFSET;
            if (!IsValidSlotOffset(cursor)) cursor = header_->read_head.load(std::memory_order_acquire);
            uint32_t start = cursor;
            do {
                if (!IsValidSlotOffset(cursor)) {
                    cursor = header_->read_head.load(std::memory_order_acquire);
                    remaining = header_->active_slot_count.load(std::memory_order_acquire);
                    if (remaining == 0 || cursor == NULL_OFFSET) break;
                    start = cursor;
                }
                Slot* slot = PtrFromOffset<Slot>(base_ptr_, cursor);
                int expected = SLOT_STATE_FILL;
                if (slot->state.compare_exchange_strong(expected, SLOT_STATE_USING, std::memory_order_acquire))
                    return cursor;
                cursor = slot->next_read.load(std::memory_order_acquire);
                if (cursor == NULL_OFFSET) {
                    cursor = header_->read_head.load(std::memory_order_acquire);
                    remaining = header_->active_slot_count.load(std::memory_order_acquire);
                    if (remaining == 0 || cursor == NULL_OFFSET) break;
                }
            } while (--remaining && cursor != start);
            return NULL_OFFSET;
        }

        TokenEntry* GetTokenById(uint32_t token_id) const {
            TokenEntry* tokens = reinterpret_cast<TokenEntry*>(base_ptr_ + header_->token_table_offset);
            for (uint32_t i = 0; i < MAX_TOKENS; ++i) {
                if (tokens[i].id.load(std::memory_order_acquire) == token_id) return &tokens[i];
            }
            return nullptr;
        }
        TokenEntry* AllocTokenEntry() {
            TokenEntry* tokens = reinterpret_cast<TokenEntry*>(base_ptr_ + header_->token_table_offset);
            for (uint32_t i = 0; i < MAX_TOKENS; ++i) {
                uint32_t expected = 0;
                if (tokens[i].id.compare_exchange_strong(expected, 1, std::memory_order_acquire))
                    return &tokens[i];
            }
            return nullptr;
        }
        void FreeTokenEntry(TokenEntry* token) { token->id.store(0, std::memory_order_release); }

        void LockSlotList() {
            while (header_->reserved[1].exchange(1, std::memory_order_acquire)) YieldProcessor();
        }
        void UnlockSlotList() {
            header_->reserved[1].store(0, std::memory_order_release);
        }

        void InsertToReadHead(uint32_t slot_offset) {
            LockSlotList();
            Slot* slot = PtrFromOffset<Slot>(base_ptr_, slot_offset);
            if (fifo_enabled_) {
                uint32_t head = header_->read_head.load(std::memory_order_relaxed);
                if (head == NULL_OFFSET) {
                    header_->read_head.store(slot_offset, std::memory_order_relaxed);
                    slot->next_read.store(NULL_OFFSET, std::memory_order_relaxed);
                }
                else {
                    uint32_t cur = head;
                    Slot* cur_slot = PtrFromOffset<Slot>(base_ptr_, cur);
                    while (cur_slot->next_read.load(std::memory_order_relaxed) != NULL_OFFSET) {
                        cur = cur_slot->next_read.load(std::memory_order_relaxed);
                        cur_slot = PtrFromOffset<Slot>(base_ptr_, cur);
                    }
                    cur_slot->next_read.store(slot_offset, std::memory_order_relaxed);
                    slot->next_read.store(NULL_OFFSET, std::memory_order_relaxed);
                }
            }
            else {
                uint32_t head = header_->read_head.load(std::memory_order_relaxed);
                slot->next_read.store(head, std::memory_order_relaxed);
                header_->read_head.store(slot_offset, std::memory_order_relaxed);
            }
            UnlockSlotList();
        }

        void InsertToWriteTail(uint32_t slot_offset) {
            LockSlotList();
            Slot* slot = PtrFromOffset<Slot>(base_ptr_, slot_offset);
            if (fifo_enabled_) {
                uint32_t head = header_->write_tail.load(std::memory_order_relaxed);
                if (head == NULL_OFFSET) {
                    header_->write_tail.store(slot_offset, std::memory_order_relaxed);
                    slot->next_write.store(NULL_OFFSET, std::memory_order_relaxed);
                }
                else {
                    uint32_t cur = head;
                    Slot* cur_slot = PtrFromOffset<Slot>(base_ptr_, cur);
                    while (cur_slot->next_write.load(std::memory_order_relaxed) != NULL_OFFSET) {
                        cur = cur_slot->next_write.load(std::memory_order_relaxed);
                        cur_slot = PtrFromOffset<Slot>(base_ptr_, cur);
                    }
                    cur_slot->next_write.store(slot_offset, std::memory_order_relaxed);
                    slot->next_write.store(NULL_OFFSET, std::memory_order_relaxed);
                }
            }
            else {
                uint32_t tail = header_->write_tail.load(std::memory_order_relaxed);
                slot->next_write.store(tail, std::memory_order_relaxed);
                header_->write_tail.store(slot_offset, std::memory_order_relaxed);
            }
            UnlockSlotList();
        }

        uint32_t RemoveFromReadList(uint32_t slot_offset) {
            LockSlotList();
            Slot* target = PtrFromOffset<Slot>(base_ptr_, slot_offset);
            uint32_t prev_off = NULL_OFFSET;
            uint32_t cur = header_->read_head.load(std::memory_order_relaxed);
            while (cur) {
                if (cur == slot_offset) {
                    if (prev_off == NULL_OFFSET) {
                        header_->read_head.store(target->next_read.load(std::memory_order_relaxed), std::memory_order_relaxed);
                    }
                    else {
                        Slot* prev = PtrFromOffset<Slot>(base_ptr_, prev_off);
                        prev->next_read.store(target->next_read.load(std::memory_order_relaxed), std::memory_order_relaxed);
                    }
                    target->next_read.store(NULL_OFFSET, std::memory_order_relaxed);
                    UnlockSlotList();
                    return target->next_read;
                }
                prev_off = cur;
                Slot* cur_slot = PtrFromOffset<Slot>(base_ptr_, cur);
                cur = cur_slot->next_read.load(std::memory_order_relaxed);
            }
            UnlockSlotList();
            return NULL_OFFSET;
        }

        uint32_t RemoveFromWriteList(uint32_t slot_offset) {
            LockSlotList();
            Slot* target = PtrFromOffset<Slot>(base_ptr_, slot_offset);
            uint32_t prev_off = NULL_OFFSET;
            uint32_t cur = header_->write_tail.load(std::memory_order_relaxed);
            while (cur) {
                if (cur == slot_offset) {
                    if (prev_off == NULL_OFFSET) {
                        header_->write_tail.store(target->next_write.load(std::memory_order_relaxed), std::memory_order_relaxed);
                    }
                    else {
                        Slot* prev = PtrFromOffset<Slot>(base_ptr_, prev_off);
                        prev->next_write.store(target->next_write.load(std::memory_order_relaxed), std::memory_order_relaxed);
                    }
                    target->next_write.store(NULL_OFFSET, std::memory_order_relaxed);
                    UnlockSlotList();
                    return target->next_write;
                }
                prev_off = cur;
                Slot* cur_slot = PtrFromOffset<Slot>(base_ptr_, cur);
                cur = cur_slot->next_write.load(std::memory_order_relaxed);
            }
            UnlockSlotList();
            return NULL_OFFSET;
        }

        void LockTokenTable() {
            while (header_->token_table_lock.exchange(1, std::memory_order_acquire) == 1) YieldProcessor();
        }
        void UnlockTokenTable() {
            header_->token_table_lock.store(0, std::memory_order_release);
        }

        void ShrinkSlots(uint32_t target_slots) {
            uint32_t current = header_->active_slot_count.load();
            if (target_slots >= current) return;
            if (target_slots < header_->initial_slots) return;
            uint32_t shrink_start = target_slots;
            uint32_t shrink_count = current - target_slots;
            for (uint32_t i = shrink_start; i < current; ++i) {
                Slot* s = &slots_[i];
                if (s->state.load(std::memory_order_acquire) != SLOT_STATE_FREE) return;
            }
            for (uint32_t i = shrink_start; i < current; ++i) {
                Slot* s = &slots_[i];
                s->state.store(SLOT_STATE_OFFLINE, std::memory_order_release);
                RemoveFromReadList(OffsetFromPtr(base_ptr_, s));
                RemoveFromWriteList(OffsetFromPtr(base_ptr_, s));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            size_t decommit_size = shrink_count * sizeof(Slot);
            uint8_t* decommit_addr = base_ptr_ + sizeof(ShmHeader) + shrink_start * sizeof(Slot);
            VirtualFree(decommit_addr, decommit_size, MEM_DECOMMIT);
            header_->active_slot_count.store(shrink_start, std::memory_order_release);
            header_->committed_watermark = shrink_start;
        }

        std::string                              name_;
        HANDLE                                   hMapFile_ = nullptr;
        uint8_t* base_ptr_ = nullptr;
        ShmHeader* header_ = nullptr;
        Slot* slots_ = nullptr;
        DataPool                                 data_pool_;
        Strategy                                 strategy_;
        uint32_t                                 write_cursor_;
        uint32_t                                 read_cursor_;
        bool                                     owner_tracking_enabled_;
        uint32_t                                 buddy_max_order_;
        bool                                     fifo_enabled_;
        std::unique_ptr<MaintenanceManager<Strategy>> maintenance_;
    };

    // ===================================================================
    // 默认策略实现
    // ===================================================================
    inline bool DefaultStrategy::WriteToSlot(Slot& slot, const void* data, size_t len, IShmCore* core) {
        if (slot.data_offset != 0) {
            core->ReleaseBlock(slot.data_offset);
            slot.data_offset = 0;
        }
        uint32_t off = core->AllocBlock(static_cast<uint32_t>(len));
        if (!off) return false;
        void* dst = core->GetDataPoolPtr(off);
        std::memcpy(dst, data, len);
        slot.data_offset = off;
        slot.data_size = static_cast<uint32_t>(len);
        return true;
    }

    inline const void* DefaultStrategy::OnDataReady(Slot& slot, size_t& out_len, IShmCore* core) {
        out_len = slot.data_size;
        return core->GetDataPoolPtr(slot.data_offset);
    }

    inline void DefaultStrategy::OnDataCopy(Slot& slot, void* buffer, size_t& in_out_len, IShmCore* core) {
        size_t sz = slot.data_size;
        size_t copy_len = (std::min)(sz, in_out_len);
        const void* src = core->GetDataPoolPtr(slot.data_offset);
        std::memcpy(buffer, src, copy_len);
        in_out_len = copy_len;
        core->ReleaseBlock(slot.data_offset);
        slot.data_offset = 0;
        slot.data_size = 0;
    }

    // ===================================================================
    // 数据池完整实现（使用自旋锁保护空闲链表）
    // ===================================================================
    inline void DataPool::Bind(uint8_t* base, uint32_t pool_offset, uint32_t pool_size,
        std::atomic<uint32_t>* buddy_free_heads, uint32_t max_order,
        std::atomic<uint32_t>* pool_lock) {
        base_ = base;
        pool_offset_ = pool_offset;
        pool_size_ = pool_size;
        buddy_free_heads_ = buddy_free_heads;
        max_order_ = max_order;
        pool_lock_ = pool_lock;
    }

    inline void DataPool::InitAsCreator(uint8_t* base, uint32_t pool_offset, uint32_t pool_size,
        std::atomic<uint32_t>* buddy_free_heads, uint32_t max_order,
        std::atomic<uint32_t>* pool_lock) {
        Bind(base, pool_offset, pool_size, buddy_free_heads, max_order, pool_lock);
        if (pool_size == 0) return;
        uint32_t initial_order = Log2Floor(pool_size);
        if (initial_order > max_order_) initial_order = max_order_;
        uint32_t block_size = 1u << initial_order;
        if (block_size > pool_size) block_size = 1u << (initial_order - 1);

        BuddyBlockHeader* hdr = reinterpret_cast<BuddyBlockHeader*>(base_ + pool_offset_);
        hdr->ref_count.store(0, std::memory_order_relaxed);
        hdr->block_size = 0;
        hdr->order = initial_order;
        hdr->next_token_held = NULL_OFFSET;
        AddToFreeList(pool_offset_, initial_order);
    }

    inline void DataPool::LockPool() {
        while (pool_lock_->exchange(1, std::memory_order_acquire)) YieldProcessor();
    }
    inline void DataPool::UnlockPool() {
        pool_lock_->store(0, std::memory_order_release);
    }

    inline uint32_t DataPool::AllocBlock(uint32_t size) {
        uint32_t total = size + sizeof(BuddyBlockHeader);
        uint32_t order = Log2Floor(total);
        if ((1u << order) < total) order++;
        if (order < BUDDY_MIN_ORDER) order = BUDDY_MIN_ORDER;

        uint32_t block_off = BuddyAlloc(order);
        if (block_off == NULL_OFFSET) return NULL_OFFSET;

        BuddyBlockHeader* hdr = PtrFromOffset<BuddyBlockHeader>(base_, block_off);
        hdr->ref_count.store(1, std::memory_order_release);
        hdr->block_size = size;
        hdr->next_token_held = NULL_OFFSET;
        return block_off;
    }

    inline void DataPool::Retain(uint32_t block_offset) {
        auto* hdr = PtrFromOffset<BuddyBlockHeader>(base_, block_offset);
        hdr->ref_count.fetch_add(1, std::memory_order_relaxed);
    }

    inline void DataPool::Release(uint32_t block_offset) {
        auto* hdr = PtrFromOffset<BuddyBlockHeader>(base_, block_offset);
        if (hdr->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            BuddyFree(block_offset, hdr->order);
        }
    }

    inline void* DataPool::GetDataPtr(uint32_t block_offset) {
        auto* hdr = PtrFromOffset<BuddyBlockHeader>(base_, block_offset);
        return hdr ? (hdr + 1) : nullptr;
    }

    inline const void* DataPool::GetDataPtr(uint32_t block_offset) const {
        auto* hdr = PtrFromOffset<BuddyBlockHeader>(base_, block_offset);
        return hdr ? (hdr + 1) : nullptr;
    }

    inline uint32_t DataPool::GetBlockSize(uint32_t block_offset) const {
        auto* hdr = PtrFromOffset<BuddyBlockHeader>(base_, block_offset);
        return hdr ? hdr->block_size : 0;
    }

    inline uint32_t DataPool::BuddyAlloc(uint32_t order) {
        LockPool();
        for (uint32_t k = order; k <= max_order_; ++k) {
            uint32_t block = PopFromFreeList(k);
            if (block != NULL_OFFSET) {
                while (k > order) {
                    k--;
                    uint32_t buddy = block + (1u << k);
                    BuddyBlockHeader* hdr = PtrFromOffset<BuddyBlockHeader>(base_, buddy);
                    hdr->order = k;
                    hdr->next_token_held = NULL_OFFSET;
                    AddToFreeList(buddy, k);
                }
                BuddyBlockHeader* hdr = PtrFromOffset<BuddyBlockHeader>(base_, block);
                hdr->order = order;
                UnlockPool();
                return block;
            }
        }
        UnlockPool();
        return NULL_OFFSET;
    }

    inline void DataPool::BuddyFree(uint32_t offset, uint32_t order) {
        LockPool();
        while (order < max_order_) {
            uint32_t buddy = GetBuddyOffset(offset, order);
            if (buddy == NULL_OFFSET) break;
            if (!IsBuddyFree(buddy, order)) break;

            RemoveFromFreeList(buddy, order);
            offset = (offset < buddy) ? offset : buddy;
            order++;
        }
        AddToFreeList(offset, order);
        UnlockPool();
    }

    inline void DataPool::AddToFreeList(uint32_t offset, uint32_t order) {
        auto& head = buddy_free_heads_[order];
        BuddyBlockHeader* hdr = PtrFromOffset<BuddyBlockHeader>(base_, offset);
        uint32_t old_head = head.load(std::memory_order_relaxed);
        hdr->next_free.store(old_head, std::memory_order_relaxed);
        head.store(offset, std::memory_order_relaxed);
    }

    inline uint32_t DataPool::PopFromFreeList(uint32_t order) {
        auto& head = buddy_free_heads_[order];
        uint32_t old_head = head.load(std::memory_order_relaxed);
        if (old_head == NULL_OFFSET) return NULL_OFFSET;
        BuddyBlockHeader* hdr = PtrFromOffset<BuddyBlockHeader>(base_, old_head);
        uint32_t next = hdr->next_free.load(std::memory_order_relaxed);
        head.store(next, std::memory_order_relaxed);
        return old_head;
    }

    inline void DataPool::RemoveFromFreeList(uint32_t offset, uint32_t order) {
        auto& head = buddy_free_heads_[order];
        uint32_t cur = head.load(std::memory_order_relaxed);
        uint32_t prev_off = NULL_OFFSET;
        while (cur) {
            if (cur == offset) {
                BuddyBlockHeader* node = PtrFromOffset<BuddyBlockHeader>(base_, cur);
                uint32_t next = node->next_free.load(std::memory_order_relaxed);
                if (prev_off == NULL_OFFSET) {
                    head.store(next, std::memory_order_relaxed);
                }
                else {
                    BuddyBlockHeader* prev = PtrFromOffset<BuddyBlockHeader>(base_, prev_off);
                    prev->next_free.store(next, std::memory_order_relaxed);
                }
                return;
            }
            prev_off = cur;
            BuddyBlockHeader* cur_node = PtrFromOffset<BuddyBlockHeader>(base_, cur);
            cur = cur_node->next_free.load(std::memory_order_relaxed);
        }
    }

    inline uint32_t DataPool::GetBuddyOffset(uint32_t offset, uint32_t order) const {
        uint32_t block_size = 1u << order;
        uint32_t relative = offset - pool_offset_;
        if (relative % (block_size * 2) == 0) {
            if (relative + block_size < pool_size_) return offset + block_size;
        }
        else {
            return offset - block_size;
        }
        return NULL_OFFSET;
    }

    inline bool DataPool::IsBuddyFree(uint32_t buddy_offset, uint32_t order) const {
        if (buddy_offset < pool_offset_ || buddy_offset >= pool_offset_ + pool_size_) return false;
        BuddyBlockHeader* hdr = PtrFromOffset<BuddyBlockHeader>(base_, buddy_offset);
        return hdr->order == order && hdr->ref_count.load(std::memory_order_acquire) == 0;
    }

    // ===================================================================
    // DataPtr 实现
    // ===================================================================
    inline DataPtr::DataPtr(uint8_t* base, uint32_t offset, DataPool* pool, bool add_ref)
        : base_(base), offset_(offset), pool_(pool), valid_(true) {
        if (add_ref && pool_ && offset_)
            pool_->Retain(offset_);
    }

    inline DataPtr::~DataPtr() { Release(); }
    inline DataPtr::DataPtr(DataPtr&& other) noexcept
        : base_(other.base_), offset_(other.offset_), pool_(other.pool_), valid_(other.valid_) {
        other.valid_ = false; other.offset_ = 0;
    }
    inline DataPtr& DataPtr::operator=(DataPtr&& other) noexcept {
        if (this != &other) {
            Release();
            base_ = other.base_; offset_ = other.offset_; pool_ = other.pool_; valid_ = other.valid_;
            other.valid_ = false; other.offset_ = 0;
        }
        return *this;
    }
    inline const void* DataPtr::Get() const { return valid_ ? pool_->GetDataPtr(offset_) : nullptr; }
    inline size_t DataPtr::Size() const { return valid_ ? pool_->GetBlockSize(offset_) : 0; }
    inline size_t DataPtr::CopyTo(void* dest, size_t max_len) const {
        if (!valid_) return 0;
        const void* src = Get();
        size_t sz = Size();
        size_t copy_len = (std::min)(sz, max_len);
        std::memcpy(dest, src, copy_len);
        return copy_len;
    }
    inline void DataPtr::Release() {
        if (valid_) { pool_->Release(offset_); valid_ = false; offset_ = 0; }
    }
    inline void DataPtr::Detach() { valid_ = false; offset_ = 0; }
    inline bool DataPtr::IsValid() const { return valid_; }

    // ===================================================================
    // 维护管理器实现
    // ===================================================================
    template<typename S>
    MaintenanceManager<S>::MaintenanceManager(const std::string& user_name, SuperSharedMemory<S>* shm,
        uint32_t orphan_using_timeout_ms)
        : mutex_name_(MakeMutexName(user_name)), shm_(shm), orphan_using_timeout_ms_(orphan_using_timeout_ms) {
    }

    template<typename S>
    MaintenanceManager<S>::~MaintenanceManager() { Stop(); }

    template<typename S>
    bool MaintenanceManager<S>::Start() {
        hMutex_ = CreateMutexA(nullptr, FALSE, mutex_name_.c_str());
        if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(hMutex_); hMutex_ = nullptr; return false; }
        running_ = true;
        shm_->header_->maintenance_enabled.store(1, std::memory_order_release);
        worker_ = std::make_unique<std::thread>(&MaintenanceManager::MaintenanceLoop, this);
        return true;
    }

    template<typename S>
    void MaintenanceManager<S>::Stop() {
        if (running_) {
            running_ = false;
            shm_->header_->maintenance_enabled.store(0, std::memory_order_release);
            if (worker_->joinable()) worker_->join();
            worker_.reset();
            if (hMutex_) { ReleaseMutex(hMutex_); CloseHandle(hMutex_); hMutex_ = nullptr; }
        }
    }

    template<typename S>
    bool MaintenanceManager<S>::IsRunning() const { return running_.load(); }

    template<typename S>
    std::vector<ZombieTokenInfo> MaintenanceManager<S>::GetZombieTokens() const {
        std::lock_guard<std::mutex> lk(zombie_list_mutex_);
        return zombie_tokens_;
    }

    template<typename S>
    bool MaintenanceManager<S>::ReleaseZombieToken(uint32_t token_id) {
        {
            std::lock_guard<std::mutex> lk(zombie_list_mutex_);
            auto it = std::find_if(zombie_tokens_.begin(), zombie_tokens_.end(),
                [token_id](const ZombieTokenInfo& info) { return info.token_id == token_id; });
            if (it != zombie_tokens_.end()) {
                zombie_tokens_.erase(it);
            }
            else {
                return false;
            }
        }
        {
            std::lock_guard<std::mutex> lk(pending_mutex_);
            pending_using_slots_.erase(token_id);
        }
        return true;
    }

    template<typename S>
    void MaintenanceManager<S>::MaintenanceLoop() {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(5));

            // 缩容逻辑
            uint32_t free_count = 0;
            uint32_t cur = shm_->header_->write_tail.load(std::memory_order_acquire);
            while (cur) {
                Slot* s = PtrFromOffset<Slot>(shm_->base_ptr_, cur);
                if (s->state.load(std::memory_order_acquire) == SLOT_STATE_FREE) ++free_count;
                cur = s->next_write.load(std::memory_order_acquire);
            }
            uint32_t active = shm_->header_->active_slot_count.load();
            if (active > 0 && free_count > active * 3 / 4) {
                uint32_t target = shm_->header_->initial_slots;
                if (active > target) shm_->ShrinkSlots(target);
            }

            // Token 表扫描，接管死进程 Token
            shm_->LockTokenTable();
            TokenEntry* tokens = reinterpret_cast<TokenEntry*>(shm_->base_ptr_ + shm_->header_->token_table_offset);
            for (uint32_t i = 0; i < shm_->header_->max_tokens; ++i) {
                if (tokens[i].id.load(std::memory_order_acquire) == 0) continue;
                uint32_t pid = tokens[i].owner_pid.load(std::memory_order_acquire);
                if (pid == 0) continue;
                HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (hProc == nullptr) {
                    ProcessDeadToken(&tokens[i]);
                }
                else {
                    CloseHandle(hProc);
                }
            }
            shm_->UnlockTokenTable();

            // 怠速扫描回收 USING 槽，并释放数据块
            {
                std::lock_guard<std::mutex> lk(pending_mutex_);
                auto it = pending_using_slots_.begin();
                while (it != pending_using_slots_.end()) {
                    auto& vec = it->second;
                    for (auto pit = vec.begin(); pit != vec.end(); ) {
                        Slot* s = PtrFromOffset<Slot>(shm_->base_ptr_, pit->slot_offset);
                        int state = s->state.load(std::memory_order_acquire);
                        if (state != SLOT_STATE_USING) {
                            pit = vec.erase(pit);
                            continue;
                        }
                        uint32_t now = GetTickCount();
                        if (now - pit->record_time_ms >= orphan_using_timeout_ms_) {
                            if (s->data_offset == pit->data_offset_snap && s->data_size == pit->data_size_snap) {
                                if (s->data_offset) {
                                    shm_->ReleaseBlock(s->data_offset);
                                    s->data_offset = 0;
                                }
                                s->state.store(SLOT_STATE_FREE, std::memory_order_release);
                            }
                            pit = vec.erase(pit);
                        }
                        else {
                            ++pit;
                        }
                    }
                    if (vec.empty()) {
                        it = pending_using_slots_.erase(it);
                    }
                    else {
                        ++it;
                    }
                }
            }
        }
    }

    template<typename S>
    void MaintenanceManager<S>::ProcessDeadToken(TokenEntry* token) {
        uint32_t tokenId = token->id.load(std::memory_order_acquire);
        token->owner_pid.store(GetCurrentProcessId(), std::memory_order_release);

        std::vector<PendingUsingSlot> usingSlots;
        uint32_t now = GetTickCount();

        auto record = [&](uint32_t slot_off) {
            Slot* s = PtrFromOffset<Slot>(shm_->base_ptr_, slot_off);
            if (s->state.load(std::memory_order_acquire) == SLOT_STATE_USING) {
                usingSlots.push_back({ slot_off, now, s->data_offset, s->data_size });
            }
            };

        for (uint32_t r = 0; r < token->read_count.load(std::memory_order_acquire); ++r)
            record(token->read_slots[r]);
        for (uint32_t w = 0; w < token->write_count.load(std::memory_order_acquire); ++w)
            record(token->write_slots[w]);

        std::sort(usingSlots.begin(), usingSlots.end(),
            [](const PendingUsingSlot& a, const PendingUsingSlot& b) { return a.slot_offset < b.slot_offset; });
        usingSlots.erase(std::unique(usingSlots.begin(), usingSlots.end(),
            [](const PendingUsingSlot& a, const PendingUsingSlot& b) { return a.slot_offset == b.slot_offset; }),
            usingSlots.end());

        if (!usingSlots.empty()) {
            std::lock_guard<std::mutex> lk(pending_mutex_);
            pending_using_slots_[tokenId] = std::move(usingSlots);
        }

        ZombieTokenInfo info;
        info.token_id = tokenId;
        strncpy_s(info.name, token->name, _TRUNCATE);
        {
            std::lock_guard<std::mutex> lk(zombie_list_mutex_);
            zombie_tokens_.push_back(info);
        }
    }

} // namespace SuperShm