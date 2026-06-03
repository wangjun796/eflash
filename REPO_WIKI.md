# eFlash FTL - Repo Wiki

## 项目概述

**eFlash FTL** 是一个轻量级嵌入式 Flash 转换层（Flash Translation Layer）库，专为资源受限的 NAND Flash 存储系统设计。项目实现了完整的闪存管理功能，包括磨损均衡、垃圾回收、事务支持和掉电恢复。

### 核心特性

- **磨损均衡** - 通过 Radix Tree 映射和循环分配延长 Flash 寿命
- **垃圾回收** - 自动空间管理，支持正常/紧急双模式
- **事务支持** - 原子性写操作，支持 begin/commit/abort
- **掉电恢复** - SuperBlock + Journal 机制实现 O(log N) 快速恢复
- **ECC 纠错** - BCH 3-bit 纠错能力
- **对象管理** - 灵活的元数据存储，支持动态扩展 LINK 链
- **零动态内存** - 全局静态实例，适合嵌入式环境
- **内存映射 Flash** - 高性能文件访问（基地址 0x80000000）
- **Code Region** - 专用代码区管理，支持代码搬移和掉电保护

### 技术规格

| 参数 | 值 | 说明 |
|------|-----|------|
| 页大小 | 512 字节 | Flash 物理页大小 |
| 总页数 | 2048 页 | 总容量 1MB |
| 用户数据 | 464 字节/页 | 每页可用数据空间 |
| 元数据 | 48 字节/页 | 包含映射信息和 ECC |
| Radix Tree 深度 | 16 | 地址映射树深度 |
| 基础对象容量 | 232 个 | 基础对象头表容量 |
| 扩展级别 | 最多 16 级 | 动态扩展对象头表 |

---

## 项目结构

```
eflash-master/
├── ecc/                          # ECC 纠错码库
│   ├── bch.c/h                   # BCH 编码/解码实现
│   └── gf13.c/h                  # Galois Field GF(2^13) 运算
│
├── eflash_ftl/                   # FTL 核心代码目录
│   ├── eflash.h                  # 公共 API 头文件（唯一需要包含的头文件）
│   ├── eflash_ftl.c/h            # FTL 核心实现 (~3600 行)
│   ├── eflash_mgr.c/h            # 空间管理器实现 (~1300 行)
│   ├── eflash_sim.c/h            # Flash 模拟器（仅用于测试）
│   ├── eflash_ftl_tests.c        # 基础测试套件 (21 个测试用例)
│   ├── eflash_ftl_tests_code_region.c  # Code Region 测试 (8 个测试用例)
│   ├── eflash_ftl_tests_extension.c    # 扩展测试套件 (28 个测试用例)
│   ├── eflash_ftl_tests_stability.c    # 长期稳定性测试 (独立文件)
│   ├── eflash_ftl_visual.c       # Radix Tree 可视化工具
│   ├── example_simple.c          # 公共 API 使用示例
│   └── API_DESIGN.md             # API 设计说明
│
├── README                        # 项目说明文档
├── CHANGELOG.md                  # 版本历史记录
├── DESIGN_OVERVIEW.md            # 设计概览
├── SUPERBLOCK_JOURNAL_IMPLEMENTATION.md  # SuperBlock 日志实现
└── CODE_REGION_TEST_SUMMARY.md   # Code Region 测试总结
```

---

## 核心架构

### 1. 系统架构概览

```
┌─────────────────────────────────────────────────────────┐
│                    应用层 (Application)                   │
└───────────────────────┬─────────────────────────────────┘
                        │
┌───────────────────────▼─────────────────────────────────┐
│              公共 API 层 (eflash.h)                       │
│  ? eflash_ftl_init()                                    │
│  ? eflash_ftl_write/read()                              │
│  ? eflash_ftl_txn_begin/commit/abort()                  │
│  ? eflash_ftl_obj_alloc/set/get_header()                │
│  ? eflash_ftl_gc_trigger/collect()                      │
└───────────────────────┬─────────────────────────────────┘
                        │
┌───────────────────────▼─────────────────────────────────┐
│              FTL 核心层 (eflash_ftl.c)                    │
│  ┌─────────────────┐  ┌──────────────┐  ┌────────────┐ │
│  │  Radix Tree     │  │  事务管理    │  │  GC 引擎   │ │
│  │  地址映射       │  │  状态机      │  │  垃圾回收  │ │
│  └─────────────────┘  └──────────────┘  └────────────┘ │
│  ┌─────────────────┐  ┌──────────────┐  ┌────────────┐ │
│  │  对象头管理     │  │  SuperBlock  │  │  Code      │ │
│  │  动态扩展       │  │  Journal     │  │  Region    │ │
│  └─────────────────┘  └──────────────┘  └────────────┘ │
└───────────────────────┬─────────────────────────────────┘
                        │
┌───────────────────────▼─────────────────────────────────┐
│           空间管理层 (eflash_mgr.c)                       │
│  ? 空闲节点管理 (Free Node Table)                         │
│  ? 动态地址分配                                           │
│  ? 相邻块合并                                             │
└───────────────────────┬─────────────────────────────────┘
                        │
┌───────────────────────▼─────────────────────────────────┐
│           Flash 抽象层 (eflash_sim.c)                      │
│  ? eflash_hw_erase()   ? eflash_hw_prog()                │
│  ? eflash_hw_read()    ? eflash_hw_word_update()         │
└───────────────────────┬─────────────────────────────────┘
                        │
┌───────────────────────▼─────────────────────────────────┐
│           物理 Flash / 文件模拟                            │
│  ? 内存映射文件 (0x80000000)                              │
│  ? 2048 页 × 512 字节 = 1MB                              │
└─────────────────────────────────────────────────────────┘
```

### 2. 数据流图

#### 写操作流程

```
应用层写入请求
    │
    ├─ 1. eflash_ftl_txn_begin()          # 开始事务
    │
    ├─ 2. eflash_ftl_write(sector, data)  # 写入数据
    │      │
    │      ├─ trace_tree()                # 遍历 Radix Tree
    │      │      └─ 构建新节点元数据模板
    │      │
    │      ├─ allocate_physical_page()    # 分配物理页
    │      │      └─ GC Head 指针前进
    │      │
    │      ├─ write_full_page()           # 写入完整页
    │      │      ├─ 填充用户数据 (464B)
    │      │      ├─ 填充元数据 (48B)
    │      │      ├─ 计算 ECC (5B)
    │      │      └─ eflash_hw_prog()     # 物理写入
    │      │
    │      └─ 更新 Radix Tree 映射
    │
    ├─ 3. eflash_ftl_txn_commit()         # 提交事务
    │      └─ 更新 Root 页状态
    │
    └─ 4. 数据持久化完成
```

#### 读操作流程

```
应用层读取请求
    │
    ├─ 1. eflash_ftl_read(sector, data)   # 读取数据
    │      │
    │      ├─ find_phys_page_by_sector()  # 查找物理页
    │      │      └─ trace_tree()         # 遍历 Radix Tree
    │      │
    │      ├─ eflash_hw_read(ppn, buf)    # 物理读取
    │      │
    │      ├─ verify_and_correct_page()   # ECC 校验
    │      │      └─ BCH 3-bit 纠错
    │      │
    │      └─ 提取用户数据 (464B)
    │
    └─ 2. 返回用户数据
```

### 3. 内存布局

#### Flash 物理布局

```
┌─────────────────────────────────────────────────────────┐
│  PPN 0 ~ (N-1)        │  Code Region (代码区)           │
│  - 固定起始于 PPN 0    │  - 存储执行代码                  │
│  - 动态扩展           │  - 绕过 FTL 管理                │
│  - GC 跳过保护        │  - 直接物理访问                  │
├─────────────────────────────────────────────────────────┤
│  PPN N ~ M            │  FTL 管理区                     │
│  - 用户数据页         │  - Radix Tree 管理              │
│  - 系统页映射         │  - GC Head/Tail 循环            │
│  - 对象头映射         │  - 磨损均衡                      │
└─────────────────────────────────────────────────────────┘
```

#### 逻辑页布局 (LPN)

```
┌─────────────────────────────────────────────────────────┐
│  LPN 0 ~ 7            │  基础对象头表 (8 页)             │
│  - 存储 232 个对象头   │  - 每页 29 个对象头              │
├─────────────────────────────────────────────────────────┤
│  LPN 8 ~ 11           │  空闲链表 (4 页)                 │
│  - Free Node Table    │  - 每页 57 个空闲节点            │
├─────────────────────────────────────────────────────────┤
│  LPN 12               │  Code Region 元数据              │
│  - 代码区状态信息     │  - 掉电恢复用                    │
├─────────────────────────────────────────────────────────┤
│  LPN 13+              │  动态分配区                      │
│  - 用户数据页         │  - 扩展对象头表                  │
│  - SuperBlock/Journal │  - 其他系统数据                  │
└─────────────────────────────────────────────────────────┘
```

---

## 核心模块详解

### 1. Radix Tree 地址映射

#### 数据结构

```c
typedef struct {
    uint32_t    global_count;      // 全局计数器（单调递增）
    uint16_t    sector_id;         // 逻辑扇区号
    uint16_t    epoch;             // 纪元计数器
    uint16_t    txn_id;            // 事务 ID
    uint16_t    adr[RADIX_DEPTH];  // 16 级地址指针数组
    uint8_t     status;            // 页状态
    uint8_t     ecc[5];            // ECC 校验码
} ftl_meta_t;
```

#### 映射原理

Radix Tree 是一种前缀树，用于将逻辑扇区号映射到物理页号：

```
逻辑扇区号 (16-bit)
    │
    ├─ Bit 15 (MSB) ──┐
    ├─ Bit 14         │
    ├─ Bit 13         │  遍历路径
    ├─ ...            │  (16 层深度)
    ├─ Bit 1          │
    └─ Bit 0 (LSB) ──┘
         │
         ▼
    物理页号 (PPN)
```

**查找过程**：
1. 从 Root 页开始
2. 逐位比较扇区号的二进制位
3. 如果位相同，继承当前节点的 `adr` 指针
4. 如果位不同，记录分歧点，继续向下查找
5. 遍历 16 层后得到完整映射路径

#### 更新操作

```c
// trace_tree() 返回值：
//   >0 : 更新写入，返回找到的物理页号
//   0  : 新写入，扇区号不在树中
//  <0 : 错误
int trace_result = trace_tree(base_root, sector_id, &new_meta);
```

### 2. 事务管理

#### 状态机

```
┌─────────┐    txn_begin()    ┌──────────┐
│  IDLE   │ ─────────────────? │  ACTIVE  │
│         │                    │          │
└─────────┘                    └────┬─────┘
                                    │
                    ┌───────────────┼───────────────┐
                    │               │               │
              txn_commit()    txn_abort()      掉电
                    │               │               │
                    ▼               ▼               ▼
              ┌──────────┐   ┌──────────┐   ┌──────────┐
              │ COMMITTED│   │ ABORTED  │   │ RECOVERY │
              └──────────┘   └──────────┘   └──────────┘
```

#### 事务流程

```c
// 1. 开始事务
eflash_ftl_txn_begin();
//    └─ 分配新事务 ID
//    └─ 设置 active_txn_id

// 2. 执行写操作
eflash_ftl_write(sector_id, data);
//    └─ 新页状态标记为 TXN_STATUS_READY (0xAD)

// 3a. 提交事务
eflash_ftl_txn_commit();
//    └─ Root 页状态更新为 TXN_STATUS_COMMITTED (0x21)
//    └─ 事务完成

// 3b. 中止事务
eflash_ftl_txn_abort();
//    └─ 清除 active_txn_id
//    └─ READY 状态的页将被 GC 回收
```

#### 两种提交方式

| 方式 | 函数 | 原理 | 适用场景 |
|------|------|------|---------|
| 通用提交 | `txn_commit()` | 重写整个页 | 所有硬件 |
| 优化提交 | `txn_commit_with_update()` | 仅更新元数据字 (1→0) | 支持字更新的硬件 |

### 3. 垃圾回收 (GC)

#### Head/Tail 循环缓冲模型

```
物理页空间
┌─────────────────────────────────────────────────────┐
│  0    1    2   ...  N   N+1  ...  M   M+1  ... 2047 │
│  ▲                   ▲              ▲               │
│  │                   │              │               │
│ Code Region      GC Tail        GC Head             │
│ (代码区)         (回收指针)      (分配指针)           │
└─────────────────────────────────────────────────────┘
         │                   │              │
         │                   │              └─ 分配新页
         │                   │                 Head++
         │                   │
         │                   └─ 回收旧页
         │                      检查页有效性
         │                      无效页直接释放
         │                      有效页迁移到新位置
         │                      Tail++
         │
         └─ 代码区保护
            GC 跳过此区域
```

#### GC 触发条件

```c
// 自动触发（在 eflash_ftl_write 中）
if (eflash_ftl_get_free_pages() < FTL->gc_threshold) {
    eflash_ftl_gc_trigger();
}

// 手动触发
eflash_ftl_gc_trigger();
eflash_ftl_gc_collect(pages_to_free);
eflash_ftl_gc_collect_all();  // 最大化回收
```

#### GC 两种模式

| 模式 | 触发条件 | 行为 | 性能影响 |
|------|---------|------|---------|
| 正常模式 | 空闲页 < 阈值 | 迁移有效页后回收 | 标准性能 |
| 紧急模式 | 空闲页极度紧张 | 直接覆盖无效页 | 避免写放大 |

#### GC 回收流程

```c
int eflash_ftl_gc_collect(uint16_t pages_to_free) {
    // 1. 从 Tail 指针开始扫描
    while (pages_freed < pages_to_free) {
        uint16_t ppn = FTL->gc_tail_page;
        
        // 2. 检查页是否在 Radix Tree 中
        if (!is_page_still_valid(ppn)) {
            // 3a. 无效页：直接擦除
            eflash_hw_erase(ppn);
            pages_freed++;
        } else {
            // 3b. 有效页：迁移到新位置
            uint16_t new_ppn = allocate_physical_page();
            migrate_page(ppn, new_ppn);
            update_radix_tree(sector_id, new_ppn);
            eflash_hw_erase(ppn);
            pages_freed++;
        }
        
        // 4. Tail 指针前进
        FTL->gc_tail_page++;
    }
}
```

### 4. 对象头管理

#### 对象头结构

```c
typedef struct {
    uint16_t    pkg_id;         // 包 ID
    uint16_t    class_id;       // 类 ID
    uint8_t     type;           // 类型 (NORMAL=0x00, LINK=0xFF)
    uint8_t     reserved[3];    // 保留
    uint32_t    body_addr;      // 数据体逻辑地址
    uint32_t    body_size;      // 数据体大小
} obj_header_t;  // 16 字节
```

#### 动态扩展机制

```
基础区域 (LPN 0-7)
┌──────────────────────────────────────┐
│  Obj 0 ~ 230  │  LINK (231)          │
│  232 个槽位   │  指向扩展区域         │
└───────┬──────────────────────────────┘
        │
        ▼
扩展区域 Level 1 (动态分配)
┌──────────────────────────────────────┐
│  Obj 232 ~ 348  │  LINK (349)        │
│  116 个槽位     │  指向 Level 2       │
└───────┬──────────────────────────────┘
        │
        ▼
扩展区域 Level 2 (动态分配)
┌──────────────────────────────────────┐
│  Obj 350 ~ 466  │  LINK (467)        │
│  116 个槽位     │  指向 Level 3       │
└──────────────────────────────────────┘
```

**扩展触发**：
```c
uint16_t eflash_ftl_obj_alloc_header(void) {
    // 计算下一个 obj_id
    uint16_t next_id = FTL->max_obj_id + 1;
    
    // 跳过 LINK 位置
    if (is_link_position(next_id)) {
        next_id++;
    }
    
    // 检查是否需要扩展
    if (next_id == BASE_HEADER_CAPACITY) {
        extend_headers();  // 首次扩展
    } else if (need_next_level_extension(next_id)) {
        extend_headers();  // 下一级扩展
    }
    
    return next_id;
}
```

### 5. Code Region 管理

#### 设计目标

Code Region 是 Flash 中专门用于存储可执行代码的区域，具有以下特点：
- **独立于 FTL 管理**：不受 Radix Tree 和 GC 影响
- **直接物理访问**：绕过映射层，提高执行效率
- **掉电保护**：迁移过程中掉电可恢复
- **动态扩展**：按需增加代码存储空间

#### 数据结构

```c
typedef struct {
    uint32_t    magic;            // 魔数 0xC0DE
    uint16_t    start_ppn;        // 起始物理页 (固定为 0)
    uint16_t    num_pages;        // 代码区页数
    uint8_t     status;           // 迁移状态
    uint16_t    src_lpn;          // 迁移源逻辑页
    uint16_t    dst_ppn;          // 迁移目标物理页
    uint16_t    pages_migrated;   // 已迁移页数 (断点续传)
    uint16_t    total_pages;      // 总迁移页数
    uint32_t    code_size_bytes;  // 代码总字节数
    uint16_t    checksum;         // 校验和
} code_region_info_t;
```

#### 迁移流程

```c
int eflash_ftl_code_migrate_from_logical(uint16_t src_lpn, uint16_t num_pages) {
    // 1. 初始化迁移状态
    g_code_region.status = CODE_MIGRATE_IN_PROGRESS;
    g_code_region.src_lpn = src_lpn;
    g_code_region.total_pages = num_pages;
    g_code_region.pages_migrated = 0;
    save_code_region_info();  // 保存状态
    
    // 2. 逐页迁移
    for (uint16_t i = 0; i < num_pages; i++) {
        // 2a. 从逻辑页读取用户数据 (464 字节)
        eflash_ftl_read(src_lpn + i, user_data);
        
        // 2b. 填充到物理页 (512 字节)
        memset(physical_page, 0xFF, 512);
        memcpy(physical_page, user_data, 464);
        
        // 2c. 直接写入物理页 (绕过 FTL)
        eflash_hw_prog(dst_ppn + i, physical_page);
        
        // 2d. 更新迁移进度
        g_code_region.pages_migrated = i + 1;
        save_code_region_info();  // 每页保存进度
    }
    
    // 3. 迁移完成
    g_code_region.status = CODE_MIGRATE_COMPLETE;
    g_code_region.num_pages += num_pages;
    
    // 4. 调整 GC 指针跳过代码区
    adjust_gc_pointers();
    
    // 5. 回收源逻辑页
    for (uint16_t i = 0; i < num_pages; i++) {
        eflash_ftl_trim(src_lpn + i);
    }
    
    // 6. 重置状态
    g_code_region.status = CODE_MIGRATE_IDLE;
    save_code_region_info();
}
```

#### 掉电恢复

```
正常流程：
  IDLE → IN_PROGRESS → COMPLETE → IDLE

掉电场景：
  IDLE → IN_PROGRESS → [掉电] → 上电恢复 → COMPLETE → IDLE

恢复逻辑：
  1. 加载 code_region_info
  2. 检查 status == IN_PROGRESS
  3. 从 pages_migrated 继续迁移
  4. 完成剩余页
  5. 更新状态
```

#### GC 保护机制

```c
int eflash_ftl_code_region_init(void) {
    // 加载代码区信息
    load_code_region_info();
    
    // 调整 GC 指针跳过代码区
    uint16_t code_end_ppn = start_ppn + num_pages;
    
    if (FTL->gc_head_page < code_end_ppn) {
        FTL->gc_head_page = code_end_ppn;
    }
    if (FTL->gc_tail_page < code_end_ppn) {
        FTL->gc_tail_page = code_end_ppn;
    }
}
```

### 6. ECC 纠错

#### BCH 编码

```c
// BCH 配置
static const struct bch_def *bch_cfg = &bch_3bit;

// 编码：生成 ECC 校验码
void calc_page_ecc(uint8_t *page_buf) {
    size_t protected_len = USER_DATA_SIZE + META_SIZE - 5;  // 464 + 43 = 507 字节
    uint8_t *ecc_ptr = page_buf + USER_DATA_SIZE + META_SIZE - 5;  // ECC 位置
    bch_generate(bch_cfg, page_buf, protected_len, ecc_ptr);
}
```

#### 纠错流程

```c
int verify_and_correct_page(uint8_t *page_buf) {
    // 1. 首次校验
    int verify_result = bch_verify(bch_cfg, data, len, ecc);
    
    if (verify_result == 0) {
        return 0;  // 无错误
    }
    
    // 2. 尝试纠错
    int result = bch_decode(bch_cfg, data, len, ecc);
    
    if (result < 0) {
        return -1;  // 不可纠正错误 (>3 bit)
    }
    
    if (result > 0) {
        // 纠错成功，复制回原缓冲区
        memcpy(page_buf, data_copy, EFLASH_PAGE_SIZE);
    }
    
    return 0;
}
```

**纠错能力**：
- 0 bit 错误：无操作
- 1-3 bit 错误：自动纠正
- >3 bit 错误：报告不可纠正

### 7. SuperBlock & Journal

#### 快速恢复机制

```c
typedef struct {
    uint32_t    magic;            // 0xEFLASH
    uint16_t    version;          // 0x0100
    uint16_t    root_page;        // 当前根页 PPN
    uint32_t    next_count;       // 下一个全局计数
    uint16_t    active_txn_id;    // 活动事务 ID
    uint16_t    gc_head_page;     // GC Head 指针
    uint16_t    gc_tail_page;     // GC Tail 指针
    uint32_t    valid_page_count; // 有效页数
    uint16_t    journal_start_idx; // Journal 起始索引
    uint16_t    journal_end_idx;   // Journal 结束索引
    uint16_t    current_epoch;    // 当前纪元
    uint16_t    checksum;         // 校验和
} superblock_header_t;
```

#### Journal 条目

```c
typedef struct {
    uint32_t    global_count;     // 全局计数器
    uint16_t    sector_id;        // 逻辑扇区号
    uint16_t    phys_ppn;         // 物理页号
    uint8_t     op_type;          // 操作类型 (WRITE/UPDATE/TRIM/GC)
    uint8_t     reserved[3];      // 填充
    uint16_t    checksum;         // 校验和
} journal_entry_t;
```

#### 恢复流程

```
上电 → 读取 SuperBlock
        │
        ├─ 验证 Magic 和 Checksum
        │
        ├─ 恢复 FTL 状态
        │   ├─ root_page
        │   ├─ gc_head_page
        │   ├─ gc_tail_page
        │   └─ next_count
        │
        └─ 重放 Journal
            ├─ 从 journal_start_idx 开始
            ├─ 按 global_count 排序
            └─ 重放未完成的写操作
```

---

## API 参考

### 初始化与清理

```c
// 初始化 Flash 模拟
int eflash_init(const char *filename);

// 初始化 FTL
int eflash_ftl_init(void);

// 清理资源
void eflash_deinit(void);
```

### 数据读写

```c
// 基于扇区号的读写（推荐）
int eflash_ftl_write(uint16_t sector_id, const uint8_t *data);
int eflash_ftl_read(uint16_t sector_id, uint8_t *data);

// 基于字节地址的读写
int eflash_ftl_write_logical(uint32_t logical_addr, const uint8_t *data, int16_t size);
int eflash_ftl_read_logical(uint32_t logical_addr, uint8_t *data, int16_t size);
```

### 事务管理

```c
// 开始事务
void eflash_ftl_txn_begin(void);

// 提交事务（通用方法）
int eflash_ftl_txn_commit(void);

// 提交事务（优化方法，需要硬件支持）
int eflash_ftl_txn_commit_with_update(void);

// 中止事务
void eflash_ftl_txn_abort(void);
```

### 对象管理

```c
// 分配对象 ID
uint16_t eflash_ftl_obj_alloc_header(void);

// 读写对象头
int eflash_ftl_obj_get_header(uint16_t obj_id, obj_header_t *hdr);
int eflash_ftl_obj_set_header(uint16_t obj_id, const obj_header_t *hdr);
```

### 垃圾回收

```c
// 触发 GC
int eflash_ftl_gc_trigger(void);

// 回收指定页数
int eflash_ftl_gc_collect(uint16_t pages_to_free);

// 最大化回收
int eflash_ftl_gc_collect_all(void);

// 获取空闲页数
uint32_t eflash_ftl_get_free_pages(void);
uint32_t eflash_ftl_get_real_free_pages(void);
```

### Code Region

```c
// 初始化代码区管理
int eflash_ftl_code_region_init(void);

// 从逻辑页迁移代码到物理代码区
int eflash_ftl_code_migrate_from_logical(uint16_t src_lpn, uint16_t num_pages);

// 扩展代码区
int eflash_ftl_code_region_expand(uint16_t additional_pages);

// 获取代码区大小
uint16_t eflash_ftl_get_code_region_size(void);

// 从代码区读取数据
int eflash_ftl_code_read(uint16_t page_offset, uint8_t *buffer, uint16_t size);

// 掉电恢复
int eflash_ftl_code_region_recover(void);
```

### 空间管理

```c
// 分配逻辑地址空间
int eflash_mgr_alloc(uint32_t size, uint32_t *out_logical_addr);

// 释放逻辑地址空间
void eflash_mgr_free(uint32_t logical_addr, uint32_t size);

// 获取空闲字节数
uint32_t eflash_mgr_get_free_bytes(void);
```

---

## 使用示例

### 基础读写示例

```c
#include "eflash.h"

int main(void) {
    // 1. 初始化
    eflash_init("flash.bin");
    eflash_ftl_init();
    
    // 2. 准备数据
    uint8_t data[USER_DATA_SIZE];
    memset(data, 0xAA, USER_DATA_SIZE);
    
    // 3. 写入数据（事务方式）
    eflash_ftl_txn_begin();
    if (eflash_ftl_write(100, data) != 0) {
        eflash_ftl_txn_abort();
        return -1;
    }
    eflash_ftl_txn_commit();
    
    // 4. 读取数据
    uint8_t read_data[USER_DATA_SIZE];
    eflash_ftl_read(100, read_data);
    
    // 5. 验证数据
    if (memcmp(data, read_data, USER_DATA_SIZE) == 0) {
        printf("Data verified!\n");
    }
    
    // 6. 清理
    eflash_deinit();
    return 0;
}
```

### 对象管理示例

```c
// 分配对象
uint16_t obj_id = eflash_ftl_obj_alloc_header();

// 设置对象头
obj_header_t header;
memset(&header, 0, sizeof(header));
header.pkg_id = 0x1234;
header.class_id = 0x5678;
header.type = OBJ_TYPE_NORMAL;
header.body_size = 1024;

eflash_ftl_obj_set_header(obj_id, &header);

// 读取对象头
obj_header_t read_header;
eflash_ftl_obj_get_header(obj_id, &read_header);
printf("PkgID: 0x%04X, ClassID: 0x%04X\n", 
       read_header.pkg_id, read_header.class_id);
```

### Code Region 示例

```c
// 1. 初始化代码区管理
eflash_ftl_code_region_init();

// 2. 准备源代码数据
uint8_t code_data[USER_DATA_SIZE];
memset(code_data, 0xCC, USER_DATA_SIZE);

// 3. 写入到逻辑页
eflash_ftl_txn_begin();
eflash_ftl_write(100, code_data);
eflash_ftl_txn_commit();

// 4. 迁移到代码区
eflash_ftl_code_migrate_from_logical(100, 1);

// 5. 从代码区读取
uint8_t read_code[USER_DATA_SIZE];
eflash_ftl_code_read(0, read_code, USER_DATA_SIZE);

// 6. 验证数据
assert(memcmp(code_data, read_code, USER_DATA_SIZE) == 0);
```

---

## 测试指南

### 编译测试

```bash
# 使用 GCC 编译
gcc -o eflash_ftl_tests_code_region.exe \
    eflash_ftl/eflash_ftl_tests_code_region.c \
    eflash_ftl/eflash_ftl.c \
    eflash_ftl/eflash_mgr.c \
    eflash_ftl/eflash_sim.c \
    ecc/bch.c \
    ecc/gf13.c \
    -Ieflash_ftl -Iecc -std=c99 -O2 -Wall
```

### 运行测试

```bash
# 运行 Code Region 测试
./eflash_ftl_tests_code_region.exe

# 运行基础测试
./eflash_ftl_tests.exe

# 运行扩展测试
./eflash_ftl_tests_extension.exe
```

### 测试覆盖

| 测试文件 | 测试用例数 | 覆盖模块 |
|---------|-----------|---------|
| `eflash_ftl_tests.c` | 21 | 基础功能、事务、GC、ECC |
| `eflash_ftl_tests_extension.c` | 26 | 扩展功能、压力测试 |
| `eflash_ftl_tests_code_region.c` | 8 | Code Region 管理 |
| **总计** | **55** | **全模块覆盖** |

---

## 配置常量

### 物理布局配置

```c
#define EFLASH_PAGE_SIZE        512     // 页大小（字节）
#define EFLASH_TOTAL_PAGES      2048    // 总页数（1MB）
#define META_SIZE               48      // 元数据大小
#define USER_DATA_SIZE          464     // 用户数据大小
#define RADIX_DEPTH             16      // Radix Tree 深度
```

### 对象头配置

```c
#define BASE_HEADER_PAGES       8       // 基础对象头页数
#define BASE_HEADER_CAPACITY    232     // 基础容量
#define EXT_HEADER_PAGES_UNIT   4       // 每扩展单元页数
#define EXT_HEADER_CAPACITY     116     // 每扩展单元容量
#define MAX_EXT_LEVELS          16      // 最大扩展级别
```

### 事务状态

```c
#define TXN_STATUS_BLANK        0xFF    // 空白页
#define TXN_STATUS_READY        0xAD    // 准备提交
#define TXN_STATUS_COMMITTED    0x21    // 已提交
#define TXN_STATUS_INVALID      0x00    // 无效页
```

---

## 调试技巧

### 启用调试输出

```c
// 在编译时定义
#define FTL_DEBUG_ENABLE 1

// 或使用编译标志
gcc -DFTL_DEBUG_ENABLE=1 ...
```

### 可视化 Radix Tree

```c
#ifdef FTL_DEBUG_ENABLE
// 输出到控制台
eflash_ftl_print_radix_tree_mermaid(ftl, root_page);

// 输出到文件
eflash_ftl_print_radix_tree_mermaid_to_file(ftl, root_page);
#endif
```

### 查询映射关系

```c
// 逻辑扇区 → 物理页
uint16_t ppn = find_phys_page_by_sector(sector_id);

// 物理页 → 逻辑扇区
uint16_t sector = find_sector_by_phys_page(ppn);
```

---

## 性能优化建议

### 1. 事务批量提交

```c
// 推荐：批量写入后一次提交
eflash_ftl_txn_begin();
for (int i = 0; i < 10; i++) {
    eflash_ftl_write(i, data[i]);
}
eflash_ftl_txn_commit();  // 一次提交
```

### 2. GC 阈值调整

```c
// 根据应用场景调整 GC 阈值
FTL->gc_threshold = 100;  // 提前触发 GC，避免空间紧张
```

### 3. 使用优化提交

```c
// 如果硬件支持字更新
int ret = eflash_ftl_txn_commit_with_update();
if (ret != 0) {
    // Fallback 到通用提交
    eflash_ftl_txn_commit();
}
```

### 4. Code Region 预分配

```c
// 预先扩展代码区，避免运行时扩展
eflash_ftl_code_region_expand(10);  // 预分配 10 页
```

---

## 注意事项

### 1. 事务管理

**所有写操作必须在事务中进行！**

```c
eflash_ftl_txn_begin();      // 必须
// ... 写操作 ...
eflash_ftl_txn_commit();     // 或 abort
```

### 2. 对象 ID 分配

- `obj_id=231` 被保留为 LINK 对象，会自动跳过
- 对象 ID 从 0 开始顺序分配
- 首次访问 `ID > 230` 时自动触发扩展

### 3. 线程安全

当前实现**不是线程安全的**，需要在外部同步：

```c
pthread_mutex_lock(&ftl_mutex);
eflash_ftl_write(sector, data);
pthread_mutex_unlock(&ftl_mutex);
```

### 4. 掉电恢复

FTL 支持掉电后自动恢复，只需重新调用初始化：

```c
// 系统重启后
eflash_ftl_init();  // 自动恢复上次状态
```

### 5. 嵌入式环境适配

- 无动态内存分配（malloc/free）
- 全局静态实例
- 确定性行为
- 低内存占用

---

## 常见问题

### Q: 测试失败如何调试？

A: 启用 `FTL_DEBUG_ENABLE=1` 查看详细日志，或使用调试器设置断点。

### Q: 如何只运行单个测试？

A: 修改测试文件的 `main()` 函数，注释掉不需要的 `RUN_TEST` 调用。

### Q: 写入后读取数据不一致？

A: 检查是否正确使用了事务（begin/commit），未提交的数据不会持久化。

### Q: 对象 ID 分配跳号？

A: `obj_id=231` 是保留的 LINK 对象，这是正常行为。

### Q: Code Region 迁移失败？

A: 检查代码区是否已初始化，GC 指针是否正确跳过代码区。

---

## 版本历史

### v1.8.0 (2026-05-08)

- 新增 Code Region 管理功能
- 新增 SuperBlock + Journal 快速恢复机制
- 新增 GC 紧急模式避免写放大
- 新增 8 个 Code Region 测试用例
- 修复对象头 LINK 链完整性测试
- 空闲链表扩展信息掉电恢复机制

### 早期版本

- v1.7.0: 基础 FTL 功能完善
- v1.6.0: GC 引擎优化
- v1.5.0: 事务管理增强
- v1.0.0: 初始版本发布

---

## 相关文档

- [README](README) - 项目说明和使用指南
- [CHANGELOG.md](CHANGELOG.md) - 完整版本历史
- [DESIGN_OVERVIEW.md](DESIGN_OVERVIEW.md) - 设计概览
- [API_DESIGN.md](eflash_ftl/API_DESIGN.md) - API 设计说明
- [SUPERBLOCK_JOURNAL_IMPLEMENTATION.md](SUPERBLOCK_JOURNAL_IMPLEMENTATION.md) - SuperBlock 实现
- [CODE_REGION_TEST_SUMMARY.md](CODE_REGION_TEST_SUMMARY.md) - Code Region 测试总结
- [TEST_COVERAGE_COMPREHENSIVE_REPORT.md](TEST_COVERAGE_COMPREHENSIVE_REPORT.md) - 测试覆盖率报告

---

## 许可证

本项目遵循嵌入式系统开源许可证，具体条款请参考项目根目录的 LICENSE 文件。

---

**最后更新**: 2026-05-26  
**维护者**: eFlash FTL 开发团队  
**联系方式**: 请参考项目主页
