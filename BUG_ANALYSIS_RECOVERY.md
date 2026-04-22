# eFlash FTL 掉电恢复Bug分析报告

## Bug 摘要

**严重程度**: 🔴 Critical  
**影响范围**: 所有掉电恢复场景  
**状态**: 已定位根因，待修复  

---

## 问题现象

在 `test_init_recovery` 测试中，程序执行以下步骤时失败：

1. 初始化 Flash 和 FTL
2. 写入数据到 sector 100
3. 模拟掉电（调用 `eflash_deinit()`）
4. 重新启动并恢复
5. **尝试读取 sector 100 时失败**

错误信息：
```
[ASSERTION FAILED] Data verification failed after recovery 
- read data does not match written pattern
```

---

## 调试日志分析

### 第一阶段：初始化和写入

```
[INIT] Starting eflash_ftl_init
[INIT] Scanning 2048 pages for valid root...
[INIT] No root page found - performing full initialization

// 初始化系统页 LPN 8
[SYS_WRITE] Writing system page LPN=8 (sector_id=8)
[WRITE] sector_id=8
[ALLOC_PHYS] Allocated physical page 1
[WRITE] Success: root_page=1, next_count=3

// 写入测试数据 sector 100
[WRITE] sector_id=100
[ALLOC_PHYS] Allocated physical page 2
[WRITE] Success: root_page=2, next_count=4
[INIT] === Minimal initialization completed ===
```

**此时的 Radix Tree 结构**：
```
root_page = 2 (物理页2)
  └─ sector_id = 100 (用户数据)
     └─ alt[9] = 1 (指向物理页1)
        └─ sector_id = 8 (系统页)
```

✅ **此时系统状态正确**：可以通过 root_page=2 访问到 sector 100

---

### 第二阶段：掉电恢复

```
[EFLASH_INIT] WARNING: File already exists, opening in rb+ mode

// 扫描找到之前的 root
[INIT] Scanning 2048 pages for valid root...
[INIT] Found valid root at page 2, epoch=0, count=4
[INIT] Scan complete. Root page: 2, next_count: 4, epoch: 0

// 开始恢复系统页
[INIT] Root found, checking free list initialization...
[INIT] Initializing only LPN 8 (free list head)...

// ❌ 问题出现：再次写入系统页 LPN 8
[SYS_WRITE] Writing system page LPN=8 (sector_id=8)
[WRITE] sector_id=8
[ALLOC_PHYS] Allocated physical page 3
[WRITE] Success: root_page=3, next_count=5

// 继续初始化，又写了一次
[SYS_WRITE] Writing system page LPN=8 (sector_id=8)
[WRITE] sector_id=8
[ALLOC_PHYS] Allocated physical page 4
[WRITE] Success: root_page=4, next_count=6
```

**此时的 Radix Tree 结构被破坏**：
```
root_page = 4 (物理页4) ← 最新的 root
  └─ sector_id = 8 (系统页)
     └─ alt[9] = 3 (指向物理页3)
        └─ sector_id = 8 (又是系统页！)
           └─ alt[...] = ? (之前的树丢失了)
```

❌ **问题**：sector 100 的数据页（物理页2）从树中断开了！

---

### 第三阶段：读取失败

```
[READ] LPN=100, root_page=4
[READ] depth=0, cur_sector=8, alt[depth]=3
[READ] depth=1, cur_sector=8, alt[depth]=65535 (PAGE_NONE)
...
[READ] depth=12, cur_sector=8, alt[depth]=65535
[READ] ERROR: Path not found at depth=12
```

读取过程：
1. 从 root_page=4 开始（这是 sector 8）
2. 沿着 radix tree 路径查找 sector 100
3. 但是 tree 中只有 sector 8 的信息
4. **找不到 sector 100，读取失败** ❌

---

## 根本原因

### 代码位置

文件：`eflash_ftl.c`  
函数：`eflash_ftl_init()`  
行数：约 780-880

### 问题逻辑

```c
// Step 3: Determine if initialization is needed
bool need_init = false;

if (ftl->root_page == PAGE_NONE) {
    // No root found - first power-on
    need_init = true;
} else {
    // Root found - check if free list is initialized
    // ❌ 这里有问题：即使找到了 root，仍然会执行下面的初始化代码
    
    // ... 扫描扩展头 ...
    
    // ❌ 关键问题：无论是否需要，都会写入系统页
    if (need_init || !free_list_initialized) {
        // Write system page through FTL layer
        write_system_page(ftl, SYS_FREE_LIST_BASE_LPN, empty_page);
        // ↑ 这个写入操作会更新 ftl->root_page，破坏原有树结构！
    }
}
```

### 核心问题

**在恢复阶段，不应该通过 `write_system_page()` 来更新系统页**，因为：

1. `write_system_page()` 内部调用 `eflash_ftl_write()`
2. `eflash_ftl_write()` 会创建新的 radix tree 节点
3. 这会更新 `ftl->root_page` 指针
4. **原有的用户数据映射关系被破坏**

正确的做法应该是：
- 在恢复阶段，只**读取**现有的系统页
- 如果需要更新系统页，应该使用**底层直接写入**，不经过 FTL 层
- 或者确保更新操作不会改变 root_page 指向的用户数据路径

---

## 修复方案

### 方案1：分离系统页初始化和用户数据写入（推荐）

修改 `eflash_ftl_init()` 中的恢复逻辑：

```c
// 在恢复阶段，不要通过 FTL 层写入系统页
// 而是直接更新内存中的空间管理器状态

if (ftl->root_page != PAGE_NONE) {
    // Recovery mode: only read existing system pages
    uint16_t phys_page = find_phys_page_by_sector(ftl, SYS_FREE_LIST_BASE_LPN);
    if (phys_page != PAGE_NONE) {
        ftl->spc_mgr.free_node_pages[0] = phys_page;
        // Don't call write_system_page() here!
    }
}
```

### 方案2：使用影子提交机制

在初始化完成前，所有系统页写入都使用 shadow_root，最后再统一提交：

```c
// 初始化期间使用 shadow_root
ftl->active_txn_id = INIT_TXN_ID;
ftl->shadow_root = ftl->root_page;

// ... 执行所有系统页初始化 ...

// 初始化完成后，一次性提交
ftl->root_page = ftl->shadow_root;
ftl->active_txn_id = TXN_ID_NONE;
```

### 方案3：改进 write_system_page 实现

让 `write_system_page()` 在恢复模式下不更新 root_page：

```c
static int write_system_page(eflash_ftl_t *ftl, uint16_t lpn, const uint8_t *data) {
    if (ftl->is_in_recovery_mode) {
        // 直接写入物理页，不更新 radix tree
        return direct_physical_write(lpn, data);
    } else {
        // 正常模式：通过 FTL 层写入
        return eflash_ftl_write(ftl, lpn, data);
    }
}
```

---

## 影响评估

### 受影响的场景

1. ✅ **首次初始化**：不受影响（没有现有数据）
2. ❌ **掉电恢复**：严重受影响（数据丢失）
3. ❌ **正常重启**：受影响（可能丢失最近的写入）
4. ❌ **事务回滚**：可能受影响

### 数据安全性

- **风险等级**: High
- **潜在后果**: 
  - 用户数据在重启后无法访问
  - 文件系统元数据损坏
  - 数据一致性无法保证

---

## 测试建议

修复后需要验证以下场景：

1. ✅ 写入数据 → 掉电 → 恢复 → 读取数据
2. ✅ 多次写入不同 sector → 掉电 → 恢复 → 验证所有数据
3. ✅ 事务进行中掉电 → 恢复 → 验证事务原子性
4. ✅ 系统页更新 → 掉电 → 恢复 → 验证系统状态
5. ✅ GC 过程中掉电 → 恢复 → 验证数据完整性

---

## 相关代码文件

- `eflash_ftl/eflash_ftl.c`: 主要逻辑（需要修复）
- `eflash_ftl/eflash_ftl_tests.c`: 测试用例（当前失败）
- `eflash_ftl/eflash_ftl.h`: 数据结构定义

---

## 时间线

- **2026-04-22**: 发现问题并定位根因
- **待定**: 实施修复
- **待定**: 回归测试
- **待定**: 代码审查和合并

---

## 参考资料

- Dhara NAND flash management library design
- Radix tree structure for logical-to-physical mapping
- Power-loss atomicity in flash storage systems
