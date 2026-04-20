# eFlash FTL 调试日志控制指南

## 📊 性能对比

| 模式 | 测试时间 | 适用场景 |
|------|---------|---------|
| **默认模式** (日志关闭) | ~48秒 | ✅ 日常开发、CI/CD、常规测试 |
| **调试模式** (日志开启) | ~85秒 | 🔍 故障排查、问题定位 |

**性能提升**: 40%+ ⚡

---

## 🎯 快速开始

### 1️⃣ 默认模式（推荐 - 快速测试）

```bash
# 正常编译和测试（无调试输出）
.\build_vs2022.bat

# 运行测试
cd build_vs2022\Release
.\eflash_ftl_tests.exe
```

### 2️⃣ 调试模式（故障排查时启用）

#### 方法 A: 使用 CMake 选项（推荐）

```bash
# 创建独立的调试构建目录
mkdir build_debug && cd build_debug

# 启用调试日志
cmake -DFTL_DEBUG_ENABLE=ON ..

# 编译
cmake --build . --config Release

# 运行测试（会输出详细日志）
.\Release\eflash_ftl_tests.exe
```

#### 方法 B: 临时修改代码（快速调试）

编辑 `eflash_ftl/mini_ftl.c` 第 17 行：

```c
// 临时改为 1 开启调试
#define FTL_DEBUG_ENABLE 1  // Enable debug for troubleshooting
```

**注意**: 调试完成后记得改回 `0`！

---

## 🔧 技术实现

### 代码层面 (`mini_ftl.c`)

```c
#ifndef FTL_DEBUG_ENABLE
#define FTL_DEBUG_ENABLE 0  // Default: OFF for better test performance
#endif

#if FTL_DEBUG_ENABLE
#define FTL_DEBUG(fmt, ...) printf("[FTL_DEBUG] " fmt, ##__VA_ARGS__)
#else
#define FTL_DEBUG(fmt, ...) do {} while(0)  // Compile to nothing when disabled
#endif
```

### CMake 配置 (`CMakeLists.txt`)

```cmake
option(FTL_DEBUG_ENABLE "Enable FTL debug output" OFF)
if(FTL_DEBUG_ENABLE)
    target_compile_definitions(eflash_ftl_tests PRIVATE FTL_DEBUG_ENABLE=1)
endif()
```

---

## 💡 最佳实践

### ✅ DO - 推荐做法

1. **日常开发**: 保持默认关闭状态
2. **CI/CD 流水线**: 使用默认配置（无 `-DFTL_DEBUG_ENABLE`）
3. **提交代码前**: 确认 `FTL_DEBUG_ENABLE = 0`
4. **问题诊断**: 临时开启日志，定位后关闭

### ❌ DON'T - 避免做法

1. ~~在代码中硬编码 `#define FTL_DEBUG_ENABLE 1`~~
2. ~~将开启日志的代码提交到版本库~~
3. ~~在 CI/CD 中启用调试日志~~
4. ~~忘记恢复默认配置~~

---

## 🐛 故障排查示例

### 场景 1: 事务提交失败

```bash
# 步骤 1: 启用调试日志
mkdir build_debug && cd build_debug
cmake -DFTL_DEBUG_ENABLE=ON ..
cmake --build . --config Release

# 步骤 2: 运行测试查看详细日志
.\Release\eflash_ftl_tests.exe 2>&1 | Select-String "TXN_COMMIT"

# 步骤 3: 根据日志定位问题
# [FTL_DEBUG] [TXN_COMMIT] Committing transaction on page 15
# [FTL_DEBUG] [TXN_COMMIT] Current status: 0xAD
# [FTL_DEBUG] [TXN_COMMIT] Status updated to COMMITTED successfully
```

### 场景 2: ECC 校验错误

```bash
# 查看 ECC 相关日志
.\Release\eflash_ftl_tests.exe 2>&1 | Select-String "ECC"

# 输出示例:
# [FTL_DEBUG] [ECC] Verifying page: protected_len=507, ecc at offset 507
# [FTL_DEBUG] [ECC] Verify result: 0 (0=ok, >0=errors, <1=uncorrectable)
```

### 场景 3: GC 触发问题

```bash
# 查看 GC 相关日志
.\Release\eflash_ftl_tests.exe 2>&1 | Select-String "GC"

# 输出示例:
# [FTL_DEBUG] [GC_TRIGGER] Free pages: 150, threshold: 100
# [FTL_DEBUG] [GC_COLLECT] Collecting 10 pages from tail
```

---

## 📝 日志级别说明

当前 `FTL_DEBUG` 宏提供统一的调试输出，涵盖：

- **[ALLOC_PHYS]**: 物理页分配
- **[TRACE]**: 基数树路径追踪
- **[TXN_COMMIT]**: 事务提交流程
- **[ECC]**: ECC 校验与纠错
- **[PAGE_VALID]**: 页面有效性检查
- **[GC_***]**: 垃圾回收操作

所有日志统一使用前缀 `[FTL_DEBUG]` 便于过滤。

---

## 🔄 切换模式对比

| 特性 | 默认模式 | 调试模式 |
|------|---------|---------|
| 编译定义 | `FTL_DEBUG_ENABLE=0` | `FTL_DEBUG_ENABLE=1` |
| 日志输出 | 无 | 详细 |
| 测试速度 | ⚡ 快 (~48s) | 🐢 慢 (~85s) |
| 适用场景 | 日常测试 | 问题诊断 |
| 代码修改 | 无需 | 可选 |

---

## ⚠️ 注意事项

1. **零开销原则**: 当 `FTL_DEBUG_ENABLE=0` 时，所有 `FTL_DEBUG()` 调用编译为空操作，无任何运行时开销
2. **线程安全**: 调试输出使用 `printf`，多线程环境下可能交错（仅调试时使用）
3. **缓冲区刷新**: 调试模式下建议定期调用 `fflush(stdout)` 确保日志及时输出
4. **生产环境**: 永远不要在发布版本中启用调试日志

---

## 📚 相关文档

- [eFlash FTL 项目架构](../README.md)
- [测试规范](../tests/README.md)
- [ECC 纠错机制](../ecc/README.md)
