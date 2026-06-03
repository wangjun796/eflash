============================================================
  eFlash FTL - 长期稳定性测试修复报告
  eFlash FTL - Long-term Stability Fix Report
============================================================
版本: v2.2.1 (commit 41e05fd)
日期: 2026-06-02

------------------------------------------------------------
一、Bug 概述
------------------------------------------------------------

test_long_term_stability 用例在 v2.1.6 引入缓存机制后开始失败，经定位发现
存在两个独立的 Bug：

  Bug A: allocate_physical_page 在 Flash 空间耗尽时仍返回有效 PPN，
         导致写入覆盖已有数据，数据静默丢失。

  Bug B: 断电恢复时 Radix Tree 根页定位错误，导致 Phase 3 断电恢复后
         数据无法恢复（"no data recovered"）。

------------------------------------------------------------
二、Bug A: 物理页分配耗尽时未返回错误
------------------------------------------------------------

【根因】
  allocate_physical_page() 函数（eflash_ftl.c）在分配物理页时：
  1. 无条件移动 gc_head_page 指针，不检查目标页是否已被有效数据占用
  2. 当 head 越过 EFLASH_TOTAL_PAGES 时回绕到 0，此时 tail 可能已被
     GC 向前推进，head 绕回后可能覆盖 tail 之前的有效数据
  3. 调用方 eflash_ftl_write() 不检查返回值，认为分配总是成功

  调用链：
    eflash_ftl_write()
      -> eflash_ftl_write_through()
        -> eflash_ftl_gc_trigger()      // 触发 GC，但可能回收不够
        -> allocate_physical_page()     // 无条件返回 PPN，不检查有效性
        -> eflash_hw_write()            // 覆盖有效数据

【影响】
  100,000 次写入后 Flash 接近满，GC 无法回收足够空间，后续写入覆盖
  已有数据，导致 Phase 4 最终验证时大量扇区读取失败。

【修复】
  在 allocate_physical_page() 中添加空间耗尽检查：

  修改位置: eflash_ftl/eflash_ftl.c -> allocate_physical_page()

  添加逻辑:
    if (FTL->gc_head_page == FTL->gc_tail_page && FTL->valid_page_count > 0) {
        if (is_page_still_valid(FTL->gc_head_page)) {
            // Flash 已满，返回 -1 通知调用方
            return -1;
        }
    }

  同时在测试用例中添加对写入返回值的断言：
    ASSERT(ret == 0, "Phase 3: Write failed - flash full");

------------------------------------------------------------
三、Bug B: 断电恢复时 Radix Tree 根页定位错误
------------------------------------------------------------

【根因】
  eflash_ftl_init() 在恢复模式下定位根页时，优先使用 find_root_binary()
  二分查找。该函数假设已提交（COMMITTED, status=0x21）的页是连续的，
  通过二分查找定位最后一个已提交页作为根页。

  问题：GC 运行后，会将旧的已提交页标记为 INVALID（status=0x00），
  导致已提交页不再连续。find_root_binary() 在这种情况下定位到错误的页，
  后续恢复逻辑无法正确重建 Radix Tree，Free List（LPN 8）恢复失败。

  现象：
    - Phase 3 断电恢复后 root_page 不是真正的根页
    - 根页的 sector_id 与写入时不一致（如 sector_id=13 而非实际值）
    - 导致所有扇区读取失败："no data recovered"

【修复】
  修改 eflash_ftl_init() 中的根页恢复逻辑，直接使用 find_root_full_scan()
  全量扫描代替二分查找：

  修改位置: eflash_ftl/eflash_ftl.c -> eflash_ftl_init()

  修改前:
    // Step 1: Binary search for root
    if (find_root_binary(...) != PAGE_NONE) { ... }
    // Step 2: Fallback to full scan
    FTL->root_page = find_root_full_scan(&root_meta);

  修改后:
    // 直接使用全量扫描，通过最高 epoch + global_count 定位根页
    FTL->root_page = find_root_full_scan(&root_meta);

  原理：find_root_full_scan() 遍历所有页，找到 epoch 最高且 global_count
  最高的已提交页，这在 GC 后仍然正确，因为真正的根页总是具有最高的序号。

------------------------------------------------------------
四、辅助调试手段
------------------------------------------------------------

1. 添加 PRINT_ROOT_STATE 宏，在断电前/恢复后打印根页的元数据：
   - root_page, next_count, valid_page_count
   - head, tail, epoch
   - sector_id, global_count, status, 前 256 字节校验和

2. 添加 dump_all_pages() 函数，全量扫描 Flash 页状态，输出：
   - PPN, sector_id, epoch, global_count, status, committed 标记
   - 统计 blank/valid/committed/ready/invalid 页数

3. 添加 allocate_physical_page() 详细日志，空间耗尽时打印：
   - head_page, tail_page, next_count, valid_pages
   - free(est), free(real), gc_in_progress

------------------------------------------------------------
五、测试结果
------------------------------------------------------------

make test-stability 运行结果：

  Phase 1: 100,000 次读写      -> PASS (0 errors)
  Phase 2: 1,000 次混合大小写入 -> PASS
  Phase 3: 10 次断电恢复        -> PASS (20/20 sectors verified each cycle)
  Phase 4: 最终验证 100 扇区    -> PASS (100/100 sectors)
  Overall: PASSED

------------------------------------------------------------
六、文件变更清单
------------------------------------------------------------

修改:
  eflash_ftl/eflash_ftl.c
    - allocate_physical_page(): 添加空间耗尽检查
    - eflash_ftl_init(): 根页恢复改用 find_root_full_scan()
    - 添加 is_page_still_valid() 辅助函数
    - 添加详细调试日志

  eflash_ftl/eflash_ftl_tests_extension.c
    - 恢复 28 个测试用例的 RUN_TEST 调用
    - test_long_term_stability 移至独立文件后注释掉

  Makefile
    - 新增 test-extension, test-stability 构建目标

新增:
  eflash_ftl/eflash_ftl_tests_stability.c
    - 独立长期稳定性测试文件
    - 包含 test_long_term_stability + dump_all_pages + PRINT_ROOT_STATE

------------------------------------------------------------
七、运行方式
------------------------------------------------------------

  # 单独运行长期稳定性测试
  make test-stability

  # 运行完整扩展测试套件（28 个用例）
  make test-extension

  # 手动编译运行
  gcc -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable \
      -Wno-sign-compare -Wno-address -g -O0 -DFTL_DEBUG_ENABLE=0 \
      -Ieflash_ftl -Iecc \
      -o test_stability.exe \
      eflash_ftl/eflash_ftl_tests_stability.c \
      eflash_ftl/eflash_ftl.c \
      eflash_ftl/eflash_mgr.c \
      eflash_ftl/eflash_sim.c \
      eflash_ftl/eflash_ftl_visual.c \
      ecc/bch.c ecc/gf13.c \
      && ./test_stability.exe

============================================================