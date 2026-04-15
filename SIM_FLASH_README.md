# 内存映射Flash模拟器

## 概述

内存映射Flash模拟器(sim_flash)是一个Windows平台上的模拟器，它可以创建一个512KB的文件并将其映射到内存地址0x80000000，模拟真实的Flash存储设备。

## 实现细节

- 创建一个名为 `sim_flash.bin` 的文件，大小为512KB
- 使用Windows API将此文件映射到内存地址 0x80000000
- 提供读写接口，对内存的修改会自动同步到文件
- 模拟Flash的基本操作：读取、写入、擦除

## API接口

- `init_flash_simulator()` - 初始化Flash模拟器
- `cleanup_flash_simulator()` - 清理和关闭模拟器
- `read_flash(addr, buffer, size)` - 从模拟Flash读取数据
- `write_flash(addr, data, size)` - 向模拟Flash写入数据
- `erase_flash_block(addr, size)` - 擦除Flash块
- `get_flash_ptr()` - 获取映射内存的指针

## 使用示例

运行Flash模拟器演示程序：

```
flash_sim_demo.exe
```

运行与Dhara库集成的示例：

```
flash_with_dhara.exe
```

## 与Dhara集成

`flash_with_dhara.c` 示例展示了如何将内存映射的Flash模拟器与Dhara库集成，实现一个完整的NAND Flash管理系统。

## 注意事项

- 在Windows上需要管理员权限才能映射特定内存地址（如果需要的话）
- 模拟器使用Windows内存映射文件API，仅限Windows平台
- 为确保数据一致性，在写入后会调用`FlushViewOfFile`强制将更改写入磁盘