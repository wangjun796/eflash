# eFlash.h 公共 API 设计说明

## 设计原则

`eflash.h` 作为 eFlash 库的**公共 API 头文件**，遵循以下设计原则：

1. **最小暴露原则**：只暴露用户必需的接口和数据类型
2. **信息隐藏**：内部实现细节对用户透明
3. **ABI 稳定**：公共接口保持稳定，便于库升级

## eflash.h 包含的内容

### ✅ 公共配置常量
```c
#define EFLASH_PAGE_SIZE        512
#define EFLASH_TOTAL_PAGES      2048
#define USER_DATA_SIZE          464
#define BASE_HEADER_CAPACITY    232
// ... 等
```

### ✅ 公共数据结构
```c
// 对象头结构 - 用户需要知道其布局
typedef struct {
    uint16_t    pkg_id;
    uint16_t    class_id;
    uint8_t     type;
    uint8_t     reserved[3];
    uint32_t    body_addr;
    uint32_t    body_size;
} obj_header_t;
```

### ✅ 不透明类型
```c
// FTL 上下文 - 内部结构对用户隐藏
// 使用静态分配，无需 malloc/free（适合嵌入式系统）
typedef struct eflash_ftl eflash_ftl_t;
```

### ✅ 公共 API 函数
```c
// 初始化（使用全局静态实例 g_ftl_instance）
int eflash_ftl_init(void);

// 数据 I/O
int eflash_ftl_write(uint16_t sector_id, const uint8_t *data);
int eflash_ftl_read(uint16_t sector_id, uint8_t *data);

// 事务管理
void eflash_ftl_txn_begin(void);
int eflash_ftl_txn_commit(void);
int eflash_ftl_txn_commit_with_update(void);  // 优化版本（需要硬件支持）
void eflash_ftl_txn_abort(void);

// 对象管理
uint16_t eflash_ftl_obj_alloc_header(void);
int eflash_ftl_obj_set_header(uint16_t obj_id, const obj_header_t *hdr);
int eflash_ftl_obj_get_header(uint16_t obj_id, obj_header_t *hdr);

// GC 管理
int eflash_ftl_gc_trigger(void);
int eflash_ftl_gc_collect(uint16_t pages_to_free);
uint32_t eflash_ftl_get_free_pages(void);

// 空间管理
int eflash_mgr_alloc(uint32_t size, uint32_t *out_logical_addr);
void eflash_mgr_free(uint32_t logical_addr, uint32_t size);
uint32_t eflash_mgr_get_free_bytes(void);

// Flash 模拟（仅用于测试/开发）
int eflash_init(const char *filename);
void eflash_deinit(void);
int eflash_hw_erase(uint16_t page_addr);
// ... 等
```

## ❌ eflash.h 不包含的内容

- `ftl_meta_t` 结构（内部元数据格式）
- `eflash_ftl_t` 的内部字段（通过全局变量 `g_ftl_instance` 访问）
- `eflash_mgr_t` 结构（通过宏 `MGR` 访问）
- BCH/ECC 内部实现细节
- Radix Tree 内部结构
- 任何 `_internal` 或 `_private` 命名的函数

**注意：** eflash.h 中定义了便捷宏：
```c
#define FTL (&g_ftl_instance)        // 访问全局 FTL 实例
#define MGR (&g_ftl_instance.spc_mgr) // 访问空间管理器
```
这些宏简化了对全局实例的访问，但用户通常不需要直接使用它们。

## 使用方式

### 场景 1: 普通用户使用（推荐）

**只需包含 eflash.h**

```c
#include "eflash.h"

int main() {
    // 初始化 Flash
    eflash_init("flash.bin");
    
    // 初始化 FTL（使用全局静态实例）
    eflash_ftl_init();
    
    // 使用公共 API
    uint8_t data[USER_DATA_SIZE];
    eflash_ftl_txn_begin();
    eflash_ftl_write(100, data);
    eflash_ftl_txn_commit();
    
    // 清理
    eflash_deinit();
    
    return 0;
}
```

**优点：**
- ✅ 代码简洁，无需管理 FTL 实例
- ✅ 不依赖内部实现
- ✅ 库升级时不受影响

### 场景 2: 高级用户/测试代码

**可以同时包含内部头文件**

```c
// 先包含内部头文件获取完整类型定义
#include "eflash_ftl.h"
#include "eflash_mgr.h"
#include "eflash_sim.h"

// 再包含公共 API（会跳过重复定义）
#include "eflash.h"

int main() {
    // 可以直接访问全局实例
    printf("Root page: %d\n", FTL->root_page);
    printf("GC head: %d\n", FTL->gc_head_page);
    
    // ... 使用公共 API（不需要传递参数）
    eflash_ftl_init();
    eflash_ftl_write(100, data);
}
```

**注意：**
- ⚠️ 需要按正确顺序包含头文件
- ⚠️ 依赖内部实现，库升级可能需要修改代码
- ⚠️ 仅用于测试、调试或特殊需求

## 头文件包含顺序规则

### 规则 1: 纯公共 API 使用
```c
#include "eflash.h"  // 只需要这个
```

### 规则 2: 需要访问内部结构
```c
// 1. 先包含内部头文件（获取完整类型定义）
#include "eflash_ftl.h"
#include "eflash_mgr.h"

// 2. 再包含公共 API（会自动跳过已定义的类型）
#include "eflash.h"
```

**原理：**
- `eflash_ftl.h` 中有条件编译：如果 `EFLASH_H` 已定义，则跳过 `eflash_ftl_t` 的定义
- 这样避免了重复定义冲突

## 实例管理

### 全局静态实例（唯一方式）

```c
// 直接调用 API，无需获取实例
eflash_ftl_init();

// 使用...
eflash_ftl_write(100, data);

// 如果需要访问内部字段，可以使用宏 FTL
printf("Root page: %d\n", FTL->root_page);
```

**优点：**
- ✅ 无动态内存分配（适合嵌入式系统）
- ✅ 无需管理生命周期
- ✅ 零开销（全局静态变量 `g_ftl_instance`）
- ✅ 通过宏 `FTL` 和 `MGR` 简化访问

**注意：**
- ⚠️ 全局只有一个 FTL 实例
- ⚠️ 如果需要多个 FTL 实例，请直接使用内部头文件并自行管理

## 示例代码

查看以下示例了解正确使用方式：

1. **example_simple.c** - 完整的公共 API 示例（推荐）
   ```c
   #include "eflash.h"
   
   eflash_ftl_init();  // 直接使用，无需获取实例
   // ... 使用各种 API
   ```
   
   此示例演示了：
   - 基本初始化和清理
   - 事务管理（begin/commit/abort）
   - 数据读写操作
   - 对象头管理
   - 两种提交方法（通用 vs 优化）
   - 掉电恢复模拟
   - 垃圾回收状态查询

## 编译和链接

### CMake
```cmake
target_link_libraries(your_app eflash ecc_lib)
target_include_directories(your_app PRIVATE path/to/eflash_ftl)
```

### 命令行
```bash
# Windows MSVC
cl your_app.c eflash.lib ecc_lib.lib

# Linux GCC
gcc your_app.c -leflash -lecc_lib -o your_app
```

## 最佳实践

1. **优先使用公共 API**
   - 99% 的场景只需要 `eflash.h`
   - 避免依赖内部实现

2. **直接调用 API，无需管理实例**
   - 所有 API 函数都不需要传递 `ftl` 或 `mgr` 参数
   - 使用全局静态实例 `g_ftl_instance`
   - 通过宏 `FTL` 和 `MGR` 访问内部字段（如需要）

3. **仅在必要时包含内部头文件**
   - 测试代码
   - 调试目的
   - 需要多个 FTL 实例时

4. **阅读 example_simple.c**
   - 展示了纯公共 API 的正确用法
   - 可以作为模板使用

---

## 最近的重要改进 (v1.2.0)

### 1. 空闲块合并逻辑优化

**问题：** 之前的 `eflash_mgr_free()` 使用错误的参数查找前驱相邻块

**修复：**
- 新增辅助函数 `remove_node_ending_at(target_addr)` 
- 正确查找满足 `node.addr + node.size == target_addr` 的前驱块
- 使用 `assert(prev_addr + prev_size == logical_addr)` 验证相邻关系

**代码示例：**
```c
void eflash_mgr_free(uint32_t logical_addr, uint32_t size) {
    // 查找并删除前驱相邻块
    uint32_t prev_size = remove_node_ending_at(logical_addr);
    if (prev_size > 0) {
        uint32_t prev_addr = logical_addr - prev_size;
        assert(prev_addr + prev_size == logical_addr);  // 验证相邻性
        logical_addr = prev_addr;
        size += prev_size;
    }
    
    // 查找并删除后继相邻块
    uint32_t next_size = remove_node_from_table(logical_addr + size);
    if (next_size > 0) {
        size += next_size;
    }
    
    // 插入合并后的节点
    insert_node_to_table(logical_addr, size);
}
```

### 2. 测试用例增强

**新增测试：** `test_variable_size_alloc_random_order()`

**特性：**
- 生成两个独立的随机排列（Fisher-Yates 算法）
- 按随机顺序分配和释放内存块
- 验证在不同操作顺序下的鲁棒性
- 数据填充值为索引 i，释放前验证数据完整性

**关键改进：**
- ✅ 使用栈缓冲区替代 malloc/free（`uint8_t data_buf[512]`）
- ✅ 使用 `USER_DATA_SIZE` 而非 `EFLASH_PAGE_SIZE` 计算偏移量
- ✅ 跨页边界数据处理正确性验证
- ✅ 固定随机种子（srand(42)）保证可重复性

### 3. 代码质量提升

**断言验证：**
```c
#include <assert.h>

// 在关键逻辑处添加断言，异常时尽早发现
assert(prev_addr + prev_size == logical_addr);
assert(data_valid);  // 数据完整性验证
```

**设计原则：**
- 嵌入式代码保持简洁
- 避免创建不必要的辅助函数
- 在关键路径使用 assert 进行防御性编程

---

**版本**: 1.2.0  
**更新日期**: 2026-04-23
