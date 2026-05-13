# SuperSharedMemory

基于 Windows 命名共享内存，无外部依赖，直接包含头文件即可使用的跨进程共享内存框架  
(A cross-process shared memory framework based on Windows named shared memory, with no external dependencies, that can be used directly by including the header file)

---

## 1. 介绍

**SuperSharedMemory** 是一个轻量级、面向消息的跨进程通信库。它在共享内存中维护一个**槽位池**（Slot），每个槽位可动态绑定一块可变大小的数据块（通过伙伴系统分配）。进程通过抢夺槽位、写入数据、标记就绪来完成消息投递；读取端则消费就绪的槽位，数据块的所有权随槽位转移。

### 核心特性

- **零拷贝读取**：通过 `DataPtr` 直接访问共享内存中的数据，无需拷贝。
- **策略可替换**：默认策略支持任意长度数据（在数据池限制内），用户可实现自定义策略。
- **Token 机制**：支持为每个进程创建私有读写槽组，避免公共池竞争。
- **FIFO / LIFO 队列模式**：创建时可选择先进先出（FIFO）或后进先出（LIFO），适应不同消息顺序需求。
- **自动维护守护**：可启动后台线程清理死进程遗留的 Token 和槽位，回收资源。
- **动态扩容缩容**：根据空闲槽位比例自动缩容，减少内存占用。
- **无锁设计**：关键路径（槽获取、伙伴分配）使用 CAS 原子操作，保证高性能。
- **LIFO**：使用 LIFO 顺序进行的默认读写访问，不保证必然遵守该规则，在高压下可能会出现顺序错乱，建议传输数据内自持计数。

### 架构概览

```
+------------------+       +-------------------+       +------------------+
|   Process A      |       |    Shared Memory  |       |   Process B      |
|                  |       |                   |       |                  |
|  Write/Read      | <---> |  Slots (环形链表)  | <---> |  Write/Read      |
|  Tokens          |       |  Data Pool (伙伴)  |       |  Tokens          |
|  Maintenance Mgr |       |  Token Table       |       |                  |
+------------------+       +-------------------+       +------------------+
```

---

## 2. 各接口介绍

### 2.1 创建与打开

**`Create`** 创建新的共享内存实例（主进程调用）  
```cpp
static SuperSharedMemory Create(
    const std::string& name,                     // 共享内存名称，所有进程需一致
    uint32_t initial_slots = 256,                // 初始槽位数
    uint32_t max_slots = 4096,                   // 最大槽位数（扩容上限）
    uint32_t data_pool_size = 16 * 1024 * 1024,  // 数据池总大小（字节）
    uint32_t buddy_max_order = 24,               // 伙伴系统最大阶数，2^24 = 16MB
    bool enable_owner_tracking = false,          // 销毁Token时是否主动释放数据块
    bool fifo_enabled = false,                   // true=FIFO(先进先出)；false=LIFO(后进先出)
    Strategy strategy = Strategy()               // 自定义策略实例
);
```
- `fifo_enabled`：决定公共槽位的读写顺序。**所有进程必须由创建者决定顺序模式**，后续 `Open` 会自动识别，不可更改。

**`Open`** 打开已存在的共享内存（其他进程调用）  
```cpp
static SuperSharedMemory Open(
    const std::string& name,                     // 名称必须与Create一致
    Strategy strategy = Strategy()               // 通常使用默认策略即可
);
```

**`Exists`** 检查共享内存是否已存在  
```cpp
static bool Exists(const std::string& name);
```

---

### 2.2 FIFO 与 LIFO 模式说明

- **LIFO（默认）**：后写入的数据先被读出。写入端将新槽插入写链表头，读取端从读链表头取数据，形成堆栈行为。适合无顺序要求的场景。
- **FIFO**：先写入的数据先被读出。设置 `fifo_enabled = true` 后，读写链表均为正向链接，新释放的槽追加到链表尾部，读取端从头部开始消费。适合需要严格顺序的场景。
- **并发与顺序**：FIFO 插入操作由自旋锁保护，但极高并发下仍可能出现短暂乱序。关键顺序应用应在消息内携带序号。
- **跨进程一致性**：`Open` 会自动从共享内存头部读取创建时的 FIFO 标志，保证所有进程使用相同模式。

---

### 2.3 基本读写（无Token模式）

**`Write`** 写入数据到公共池，线程安全，自动寻找空闲槽位  
```cpp
bool Write(
    const void* data,        // 待写入数据指针
    size_t len,              // 数据长度（字节），需 ≤ 单块最大尺寸
    uint32_t token_id = 0    // 0=公共池；非0=指定Token的写槽（需当前进程拥有）
);
```

**`ReadZeroCopy`** 零拷贝读取，获得数据块所有权，数据保留在共享内存中  
```cpp
DataPtr ReadZeroCopy(
    uint32_t token_id = 0    // 0=从公共池获取；非0=从指定Token读槽获取
);
```

**`ReadCopy`** 拷贝读取，数据复制到用户缓冲区后自动释放共享内存中的数据块  
```cpp
bool ReadCopy(
    void* buffer,            // 用户缓冲区
    size_t& in_out_len,      // 输入：缓冲区容量；输出：实际拷贝字节数
    uint32_t token_id = 0    // 同ReadZeroCopy
);
```

**公共模式并发限制**：`token_id=0` 的公共读写内部游标**非线程安全**，仅允许单线程访问。多线程多进程请使用 Token 模式。

---

### 2.4 槽位管理

**`AcquireSlot`** 从公共池获取一个空闲槽位，返回槽位偏移地址  
```cpp
uint32_t AcquireSlot(
    bool blocking = true,    // 是否阻塞等待直到有可用槽位
    int mode = 0             // 0=同时从读写链表移除；1=仅读链表；2=仅写链表
);
```

**`ReleaseSlot`** 归还槽位到公共池，槽位变为 FREE，其持有的数据块会被释放  
```cpp
bool ReleaseSlot(
    uint32_t slot_offset,    // 之前获取的槽位偏移
    int mode = 0             // 0=插入读+写链表；1=只读链表；2=只写链表
);
```
- 调用 `ReleaseSlot` 会强制释放槽内数据块，即使槽内有未消费数据。若只想归还空槽，请先确保数据已被消费。

---

### 2.5 Token 管理

**`CreateToken`** 创建专属读写槽组  
```cpp
uint32_t CreateToken(
    uint32_t read_count,      // 读槽数量（≤ MAX_SLOTS_PER_TOKEN）
    uint32_t write_count,     // 写槽数量（≤ MAX_SLOTS_PER_TOKEN）
    bool blocking = true,     // 空闲槽不足时是否阻塞等待
    int alloc_mode = 0        // 分配模式：
                              // 0: 默认，分别获取
                              // 1: 强制共享槽位（要求read_count==write_count）
                              // 2: 强制分离槽位
                              // 3: 尝试共享槽位
                              // 4: 尝试分离槽位
);
```

**`DestroyToken`** 销毁 Token 并归还所有槽位（是否释放数据由 `owner_tracking` 决定）  
```cpp
void DestroyToken(uint32_t token_id);
```

**`TransferToken`** 将 Token 所有权转移给另一进程  
```cpp
bool TransferToken(uint32_t token_id, uint32_t target_pid);
```

**`AddReadSlot` / `AddWriteSlot`** 为 Token 动态添加槽位  
```cpp
bool AddReadSlot(uint32_t token_id, bool blocking = true);
bool AddWriteSlot(uint32_t token_id, bool blocking = true);
```
- 非阻塞调用在无空闲槽时返回 `false`。

**`RemoveReadSlot` / `RemoveWriteSlot`** 移除槽位并归还公共链表  
```cpp
bool RemoveReadSlot(uint32_t token_id, uint32_t slot_offset);
bool RemoveWriteSlot(uint32_t token_id, uint32_t slot_offset);
```
- 归还时不会自动释放数据块，需调用者确保数据已被消费或手动释放。

**`TransferSlot`** 在两个 Token 之间转移槽位  
```cpp
TransferResult TransferSlot(
    uint32_t src_token,      // 源Token
    uint32_t dst_token,      // 目标Token
    uint32_t slot_offset,    // 槽位偏移
    bool read_mode,          // 是否加入目标读列表
    bool write_mode,         // 是否加入目标写列表
    bool blocking = true     // 槽位忙时是否阻塞
);
```
返回值枚举：`Success`, `ErrNotOwner`, `ErrSlotInUse`, `ErrSlotNotFound`, `ErrTokenFull`

**`ReleaseTokenHeldBlocks`** 手动释放 Token 中所有 FILL 槽位的数据块（槽位变为 FREE）  
```cpp
uint32_t ReleaseTokenHeldBlocks(uint32_t token_id);  // 返回实际释放的块数
```

**查询 Token 信息**  
```cpp
uint32_t GetTokenReadCount(uint32_t token_id) const;
uint32_t GetTokenWriteCount(uint32_t token_id) const;
const uint32_t* GetTokenReadSlots(uint32_t token_id, uint32_t* out_count) const;
const uint32_t* GetTokenWriteSlots(uint32_t token_id, uint32_t* out_count) const;
bool GetTokenReadSlotAt(uint32_t token_id, uint32_t index, uint32_t* out_slot) const;
bool GetTokenWriteSlotAt(uint32_t token_id, uint32_t index, uint32_t* out_slot) const;
```

---

### 2.6 维护与扩缩容

**`StartMaintenance` / `StopMaintenance`** 后台维护线程（通过命名互斥量保证单实例）  
```cpp
bool StartMaintenance(uint32_t orphan_using_timeout_ms = 5000);
void StopMaintenance();
```
- 维护线程会自动接管死进程的 Token，并怠速扫描残留的 `USING` 槽位，超时后释放数据块并回收槽位。

**`ExpandSlots`** 动态增加槽位  
```cpp
bool ExpandSlots(uint32_t additional_count);
```

**`ReclaimOrphanSlots`** 手动清理公共链表中的孤儿 USING 槽  
```cpp
void ReclaimOrphanSlots(uint32_t timeout_ms);
```
- `timeout_ms`：若数据未变则强制复位并释放数据块；为 0 立即执行。推荐在子线程中调用，避免阻塞主业务流程。

**查询容量**  
```cpp
uint32_t GetActiveSlotCount() const;
uint32_t GetMaxSlots() const;
```

---

### 2.7 DataPtr（RAII 数据句柄）

```cpp
class DataPtr {
public:
    const void* Get() const;                      // 获取数据指针
    size_t      Size() const;                     // 数据大小
    size_t      CopyTo(void* dest, size_t max_len) const; // 拷贝到用户缓冲区
    void        Release();                        // 提前释放
    void        Detach();                         // 放弃所有权（数据块不释放）
    bool        IsValid() const;                  // 是否持有有效数据
};
```

---

## 3. 基本使用示例

### 3.1 简单发布/订阅（无Token）

**生产者进程：**
```cpp
#include "SuperSharedMemory.h"
using namespace SuperShm;

int main() {
    auto shm = SuperSharedMemory<>::Create("MyChannel");
    const char* msg = "Hello from producer!";
    shm.Write(msg, strlen(msg) + 1);
    // ...
}
```

**消费者进程：**
```cpp
auto shm = SuperSharedMemory<>::Open("MyChannel");
char buffer[256];
size_t len = sizeof(buffer);
if (shm.ReadCopy(buffer, len)) {
    std::cout << "Received: " << buffer << std::endl;
}
// 或者使用零拷贝
auto dp = shm.ReadZeroCopy();
if (dp) {
    std::cout << "Received: " << (const char*)dp.Get() << std::endl;
    // dp 析构自动释放内存
}
```

### 3.2 使用 Token 进行定向通信

**进程A（创建Token并写入）：**
```cpp
auto shm = SuperSharedMemory<>::Create("Channel", 256, 4096, 16*1024*1024, 24, false);
uint32_t token = shm.CreateToken(0, 4); // 分配4个写槽位
if (token) {
    for (int i = 0; i < 10; ++i) {
        std::string data = "Msg " + std::to_string(i);
        shm.Write(data.c_str(), data.size() + 1, token);
    }
}
```

**进程B（打开并读取Token，需知道Token ID）：**
```cpp
auto shm = SuperSharedMemory<>::Open("Channel");
char buffer[256];
size_t len = sizeof(buffer);
if (shm.ReadCopy(buffer, len, token_id)) {
    std::cout << "Received: " << buffer << std::endl;
}
```

### 3.3 启用后台维护

```cpp
// 在创建或打开实例后，任意进程调用（只有一个会成功）
if (shm.StartMaintenance()) {
    std::cout << "Maintenance thread started." << std::endl;
}
// 进程退出时自动停止，也可手动调用 StopMaintenance()
```

---

## 4. 注意事项

1. **数据块所有权跟随槽位**  
   当槽位状态为 `FILL` 时，其指向的数据块属于槽位当前所在位置（公共池或某个Token）。在槽位转移时（如 `DestroyToken` 且 `owner_tracking_enabled = false`），数据块会随槽位一起移交给公共池，接收方应通过 `ReadCopy` / `ReadZeroCopy` 消费数据，切勿跨进程直接释放数据块。

2. **`ReleaseSlot` 与数据清理**  
   调用 `ReleaseSlot` 会将槽位强制变为 `FREE`，并自动释放其持有的数据块（如果有）。若只是想归还一个空槽位，请确保槽位已无数据，或使用 `ReleaseTokenHeldBlocks` 先清理。

3. **Token 槽位操作的非线程安全性**  
   同一个 Token 的 `AddReadSlot`、`RemoveReadSlot` 等操作**不是线程安全**的，应保证同一时刻只有一个线程操作该 Token，或者由上层加锁。公共池的获取操作本身是原子的。

4. **跨进程释放的合法性**  
   所有数据块分配都在共享内存的伙伴系统上进行，不同进程可以安全地对同一共享内存块调用 `ReleaseBlock`，因为引用计数（`ref_count`）存储在共享内存中。即一个进程分配的数据块，可以被另一个进程释放，这是设计的核心。

5. **`TransferSlot` 传递的是槽位+数据**  
   源进程若转移一个 `FILL` 槽位，其数据块不会自动增加引用计数，也不会被释放。接收者后续消费时会调用 `ReleaseBlock`，这安全且正确。但若转移一个空槽位或 `FREE` 槽位，也无副作用。

6. **维护管理器应尽早启动**  
   建议在创建共享内存的进程中立即调用 `StartMaintenance()`，否则死进程的 Token 和槽位将不会被回收，导致资源泄漏。

7. **内存布局与对齐**  
   框架内部使用缓存行对齐（64字节）避免伪共享，所有结构体布局固定，不同编译器/版本生成的二进制可能不兼容。如需跨编译器使用，请确保 ABI 一致。

8. **公共模式并发限制**  
   `Write` / `ReadZeroCopy` / `ReadCopy` 在 `token_id=0` 时内部游标**非线程安全**，仅限单线程调用。多线程/多进程务必创建各自 Token 进行隔离。

9. **移动语义警告**  
   本类可移动，但移动后维护管理器内部的 `shm_` 指针会失效。若启用了维护线程，移动前必须停止维护线程，移动后重新绑定或不再使用。推荐用智能指针管理生命周期。

10. **缩容安全性**  
    缩容前会将槽标记为 `OFFLINE` 并从链表移除，随后 `VirtualFree` 回收物理内存。只要不绕过 API 直接操作内存，就不会导致访问违例。

11. **FIFO 顺序保证**  
    FIFO 模式提供逻辑上的先进先出，但在极高并发下，由于链表插入的竞争，可能出现短暂乱序。严格要求顺序的场合请在消息中嵌入序号。

12. **初始化竞态**  
    `Create` 返回后其他进程即可 `Open`，虽然头信息写入很快，但理论上存在极短时间窗口。建议由统一启动器确保初始化完成后再通知其他进程打开。

---

## 5. 性能评估

### 5.1 零拷贝路径
- `ReadZeroCopy` 直接将数据块指针返回给用户，没有内存拷贝开销（除数据结构本身）。数据大小仅受数据池容量限制。
- 数据块分配使用伙伴系统（无锁 CAS 栈），分配/释放近似 O(1)。

### 5.2 并发吞吐
- 槽位获取（`FindFreeSlot`）使用 **无锁链表遍历 + CAS**，避免全局锁。
- 公共池写路径竞争点在 `write_tail` 指针，高并发下可能有重试，但无阻塞。
- 实测建议：8核机器上，单块1KB数据的写入吞吐约 XXX ops/s（请根据实际测试补充）。

### 5.3 内存效率
- 槽位固定大小（64字节对齐），可按需动态提交物理页（Windows `VirtualAlloc`）。
- 空闲比例超过75%时，维护线程自动缩容，回收物理内存。
- 数据池最大可设16MB（默认），适合中小型消息。大消息可增大 `data_pool_size`。

### 5.4 延迟特性
- 进程间通信延迟主要为共享内存映射开销 + 系统调用（`MapViewOfFile` 仅初次）。
- 消息传递路径仅涉及原子操作，无系统调用，适合微秒级延迟场景。

---

## 6. 补充注释

- 如果您希望二次分发，请保留本项目地址。
- 本项目包含AI生成的代码，虽经过长期测试，但仍可能存在缺陷，欢迎反馈错误。
- 槽位数不宜设置过大，否则会导致严重的内存消耗（尽管可能带来稍高的速度）。
- 本项目为单文件头文件库，内部包含一些可调整的宏。
- 如果喜欢这个项目，请在GitHub上点个 Star，感谢支持！

---

## 7. 内置参数修改说明

以下宏和常量定义在 `SuperSharedMemory.h` 顶部，可在包含头文件前通过 `#define` 覆盖，或直接修改头文件中的默认值。

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `SHM_NAME_PREFIX` | `"__SuperShm__"` | 共享内存名称前缀，修改可隔离不同应用。 |
| `MAGIC_NUMBER` | `0x5353484D` | 共享内存魔数，用于校验内存是否由本库创建。 |
| `VERSION` | `4` | 共享内存布局版本号，升级时递增以保证兼容性。 |
| `CACHE_LINE_SIZE` | `64` | 缓存行大小，用于 `Slot` 对齐，可按实际硬件调整（通常64/128）。 |
| `MAX_SLOTS_PER_TOKEN` | `64` | 每个 Token 可拥有的最大槽位数（读写各自计）。 |
| `MAX_TOKENS` | `256` | Token 表最大条目数，即最多可创建的 Token 数量。 |
| `SHM_RESERVED_SIZE` | `1ULL * 1024 * 1024 * 1024` (1 GB) | 预留的虚拟地址空间大小，实际物理内存在提交时分配。 |
| `DEFAULT_BUDDY_MAX_ORDER` | `24` | 伙伴系统最大阶数，2^24 = 16 MB，决定单块最大可分配尺寸。 |
| `BUDDY_MIN_ORDER` | `6` | 伙伴系统最小阶数，2^6 = 64 B，决定最小分配粒度。 |
| `SLOT_STATE_FREE` | `0` | 槽位空闲状态值。 |
| `SLOT_STATE_USING` | `1` | 槽位正在操作状态值。 |
| `SLOT_STATE_FILL` | `2` | 槽位已填充数据状态值。 |
| `SLOT_STATE_OFFLINE` | `3` | 槽位已离线（缩容后）状态值。 |

**覆盖示例**
```cpp
#define MAX_TOKENS 512
#define MAX_SLOTS_PER_TOKEN 128
#include "SuperSharedMemory.h"
```

**重要提醒**  
若修改布局相关参数（如 `CACHE_LINE_SIZE`、`MAX_SLOTS_PER_TOKEN`、`VERSION` 等），必须确保所有参与进程使用完全相同的头文件配置，否则可能导致解析错乱甚至崩溃。  
建议通过 `SHM_NAME_PREFIX` 隔离不同项目，并用 `MAGIC_NUMBER` / `VERSION` 防止不兼容版本意外打开同一共享内存区域。

---

## 8. 免责声明

使用本项目（SuperSharedMemory）可能带来系统稳定性、数据完整性及安全性等方面的风险。**您在使用本项目时，即视为已充分了解并同意承担由此产生的一切风险与损失。项目开发者不对因使用或滥用本项目而导致的任何直接、间接、偶然或必然的损失承担法律责任。**

如果您不同意上述条款，请勿使用本项目。
