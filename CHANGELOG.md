# 版本历史记录 (Changelog)

## v1.3.0 (2026-04-24) - Free Node 页面布局修正

### 🔧 重大修正

修正了空闲链表（Free Node）页面布局中错误使用 `META_SIZE` 的问题，该问题导致每页浪费 48 字节空间，降低了存储效率。

### 📝 问题描述

**根本原因：**
- `eflash_ftl_read()` 返回的是纯用户数据区（USER_DATA_SIZE = 464 字节），不包含 META 和 ECC
- 但代码中多处错误地在用户数据区内再次跳过 `META_SIZE` (48 字节)
- 导致实际可用的节点存储空间减少

**影响范围：**
- 每页节点数：51 个（错误）→ 57 个（正确）
- 总容量：1020 节点（错误）→ 1140 节点（正确）
- 空间浪费：48 字节/页 × 20 页 = 960 字节

### ✅ 修正内容

#### 1. eflash_mgr.c

| 函数 | 行号 | 修正内容 |
|------|------|----------|
| `read_node_count` | 43-69 | count 偏移从 `META_SIZE` 改为 `0` |
| `write_node_count` | 71-92 | 改用 FTL 写入，count 偏移从 `META_SIZE` 改为 `0` |
| `read_free_node` | 142-165 | node 偏移从 `META_SIZE + FREE_NODE_HEADER_SIZE` 改为 `FREE_NODE_HEADER_SIZE` |
| `remove_node_from_table` | 173-290 | 所有偏移移除 `META_SIZE`（两处） |
| `insert_node_to_table` | 298-435 | count 和 node 偏移移除 `META_SIZE` |
| `eflash_mgr_init_free_list` | 690-795 | 初始化时 count 和 node 偏移修正，**初始化所有 4 个基础页** |
| `read_ext_link` | 94-112 | link 偏移从 `META_SIZE + link_offset` 改为 `link_offset` |
| `write_ext_link` | 114-134 | link 偏移从 `META_SIZE + link_offset` 改为 `link_offset` |
| `extend_free_node_table` | 525-620 | count 和 link 偏移移除 `META_SIZE`（两处） |

#### 2. eflash_ftl.c

| 函数 | 行号 | 修正内容 |
|------|------|----------|
| `eflash_ftl_init` | 1194-1265 | 初始化 free list 时 count 和 node 偏移修正，移除错误的手动 ECC 计算，**初始化所有 4 个基础页（LPN 8-11）** |

#### 🔑 关键修正：初始化所有基础页

**问题根源：**
- 之前只初始化了 LPN 8（count=1），LPN 9-11 未初始化
- `read_node_count` 读取未初始化的页面返回 -1
- `find_page_with_space` 跳过 count=-1 的页面
- 当 LPN 8 满（57 节点）后，找不到有空位的页面 → **错误触发扩展**

**解决方案：**
```c
// 初始化 LPN 8: count=1 (已有初始节点)
// 初始化 LPN 9-11: count=0 (空页)
for (int i = 1; i < FREE_NODE_PAGE_COUNT; i++) {
    uint16_t lpn = SYS_FREE_LIST_BASE_LPN + i;
    write_system_page(lpn, blank_page);  // blank_page: count=0
}
```

**效果：**
- ✅ LPN 8 满后，自动使用 LPN 9
- ✅ 不再错误触发扩展
- ✅ 避免扩展机制消耗用户空间

### 📊 修正效果

| 指标 | 修正前 | 修正后 | 改进 |
|------|--------|--------|------|
| **每页节点数** | 51 个 | 57 个 | +6 个 (+11.8%) |
| **基础层容量** | 204 节点 | 228 节点 | +24 节点 |
| **扩展层容量** | 816 节点 | 912 节点 | +96 节点 |
| **总容量** | 1020 节点 | 1140 节点 | +120 节点 (+11.8%) |
| **空间利用率** | 浪费 48 字节/页 | 无浪费 | 节省 960 字节 |

### ⚠️ 兼容性说明

**破坏性变更：**
- 此修正是**不兼容的布局变更**
- 旧的 Flash 文件使用错误的布局（count 在 offset 48）
- 新的代码使用正确的布局（count 在 offset 0）
- **必须重新初始化 Flash 文件**

**迁移指南：**
```bash
# 删除旧的测试文件
rm build_vs2022/test_flash*.bin

# 重新运行测试（会自动创建新文件）
./build_vs2022/Debug/eflash_ftl_tests_extension.exe
```

### 🧪 测试状态

- ✅ Phase 1: 分配 200 个小块 - **通过**
- ✅ Phase 2: 释放并验证数据 - **通过**（已修复）
- ✅ Phase 3-5: 扩展和压力测试 - **通过**
- ✅ 最终状态：free_bytes 和 free_nodes 完全恢复

### 📌 相关文件

- `eflash_ftl/eflash_mgr.c` - 空间管理器实现
- `eflash_ftl/eflash_ftl.c` - FTL 核心实现
- `eflash_ftl/eflash_ftl_tests_extension.c` - 扩展测试用例

### ⚠️ 已知问题

**无** - 所有已知问题已修复 ✅

### 🔍 技术细节

**正确的 Free Node 页面布局：**
```
User Data Region (464 bytes):
├─ [0 ... 1]     : count (uint16_t) - 节点数量
├─ [2 ... 457]   : 57 个 free_node_t (每个 8 字节)
└─ [458 ... 463] : 未使用（填充，6 字节）

Physical Page (512 bytes):
├─ [0 ... 463]   : User Data (上述内容)
├─ [464 ... 506] : META (43 bytes: sector_id, alt[] 等)
└─ [507 ... 511] : ECC (5 bytes: BCH 纠错码)
```

**关键理解：**
- `eflash_ftl_read(lpn, buf)` 返回的是 464 字节的纯用户数据
- `buf[0..1]` 就是 count，不需要再加 `META_SIZE`
- `META_SIZE` 只在物理页布局中使用，不在用户数据缓冲区中使用

---

## v1.2.0 (2026-04-23) - API 设计与测试增强

### ✨ 新增功能
- 添加可变大小分配的随机顺序测试
- 改进断言机制，支持 Release 模式下的强制断言
- 优化测试代码，使用栈缓冲区替代动态内存

### 🔧 改进
- 使用 Fisher-Yates 算法生成随机排列
- 添加数据完整性验证
- 固定随机种子保证可重复性

---

## v1.1.0 (2026-04-22) - 初始发布

### ✨ 核心功能
- Radix Tree 映射管理
- 磨损均衡
- 垃圾回收
- 事务支持
- BCH ECC 纠错
- 对象头管理

### 📦 组件
- eflash_ftl - FTL 核心
- eflash_mgr - 空间管理器
- eflash_sim - Flash 模拟器
- ecc - BCH 纠错码库

---

**维护者**: 
- v1.3.0: AI (Qwen/通义千问) + wangj
- v1.2.0: AI (Qwen/通义千问)
- v1.1.0: AI (Qwen/通义千问)

**最后更新**: 2026-04-24
