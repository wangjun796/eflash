# eFlash FTL 测试指南

## 快速开始

### 1. 构建项目

```batch
cd e:\SC17\dhara-master
build_vs2022.bat
```

这将生成 Visual Studio 2022 解决方案并编译所有项目。

### 2. 运行测试

#### 方法一：通过 CTest（推荐）

```batch
cd build_vs2022
ctest -V
```

#### 方法二：直接运行可执行文件

```batch
cd build_vs2022\Release
eflash_ftl_tests.exe
```

#### 方法三：在 VS2022 中调试

1. 打开 `build_vs2022\dhara.sln`
2. 在解决方案资源管理器中右键 `eflash_ftl_tests`
3. 选择"设为启动项目"
4. 按 `F5` 开始调试

### 3. 测试覆盖范围

| 测试名称 | 测试内容 |
|---------|---------|
| `test_init_recovery` | FTL 初始化、掉电恢复、Root 页重建 |
| `test_basic_read_write` | 单页/多页读写、覆盖写入 |
| `test_object_headers` | 对象头管理、动态扩展、跨级访问 |
| `test_transactions` | 事务提交/回滚、多操作事务 |
| `test_power_failure` | 事务掉电恢复、未完成事务回滚 |
| `test_space_management` | 位图分配、小对象精细分配 |
| `test_ecc_correction` | BCH 3-bit 纠错验证 |
| `test_radix_tree` | 基数树路径追踪完整性 |
| `test_stress` | 压力测试、混合事务场景 |

### 4. 预期输出

```
========================================
eFlash FTL Test Suite
========================================

[TEST] test_init_recovery: Starting...
  [PASS] Fresh initialization
  [PASS] Recovery after write
[PASS] test_init_recovery: Completed successfully

... (其他测试)

========================================
Test Results: 9 passed, 0 failed out of 9 total
========================================
```

### 5. 调试技巧

- **设置断点**：在 `tests/eflash_ftl_tests.c` 中的测试函数入口设置断点
- **查看状态**：使用"即时窗口"查看 [eflash_ftl_t](file://e:\SC17\dhara-master\eflash_ftl\eflash_ftl.h#L56-L69) 结构体字段
- **Flash 文件**：测试过程中会生成 `eflash_test.bin`，可用十六进制编辑器查看
- **详细日志**：测试输出包含 `[PASS]` 和 `[FAIL]` 标记，便于定位问题

### 6. 常见问题

**Q: 测试失败如何调试？**
A: 在 VS2022 中打开解决方案，设置断点后按 F5 调试，查看失败测试的断言信息。

**Q: 如何只运行单个测试？**
A: 修改 `tests/eflash_ftl_tests.c` 的 `main()` 函数，注释掉不需要的 `RUN_TEST` 调用。

**Q: 测试文件在哪里？**
A: 测试运行时在当前目录创建 `eflash_test.bin`，测试结束后自动删除。

### 7. 架构说明

测试框架仿照 Dhara 的 [all_tests.c](file://e:\SC17\dhara-master\tests\all_tests.c) 设计：
- 每个测试函数独立管理 Flash 生命周期
- 使用 [RUN_TEST](file://e:\SC17\dhara-master\tests\eflash_ftl_tests.c#L323-L331) 宏统一执行和统计
- 测试失败返回 `-1`，成功返回 `0`
- 集成到 CMake/CTest 体系，支持 CI/CD