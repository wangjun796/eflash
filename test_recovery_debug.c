// 简单的调试测试程序 - 用于诊断恢复失败问题
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "eflash_ftl.h"
#include "eflash_sim.h"

#define TEST_FLASH_FILE "eflash_test.bin"

int main(void) {
    eflash_ftl_t ftl;
    uint8_t write_data[USER_DATA_SIZE];
    uint8_t read_data[USER_DATA_SIZE];
    
    printf("=== Debug Test: Recovery Issue ===\n\n");
    
    // 清理旧文件
    remove(TEST_FLASH_FILE);
    
    // 初始化 Flash
    printf("Step 1: Initialize flash...\n");
    int ret = eflash_init(TEST_FLASH_FILE);
    if (ret != 0) {
        printf("ERROR: Failed to init flash\n");
        return -1;
    }
    
    // 初始化 FTL
    printf("Step 2: Initialize FTL...\n");
    memset(&ftl, 0, sizeof(ftl));
    ret = eflash_ftl_init(&ftl);
    if (ret != 0) {
        printf("ERROR: Failed to init FTL\n");
        return -1;
    }
    
    printf("  root_page=%d, next_count=%d\n", ftl.root_page, ftl.next_count);
    
    // 写入数据
    printf("\nStep 3: Write data to sector 100...\n");
    for (int i = 0; i < USER_DATA_SIZE; i++) {
        write_data[i] = (uint8_t)(0xAA);
    }
    
    ret = eflash_ftl_write(&ftl, 100, write_data);
    if (ret != 0) {
        printf("ERROR: Write failed\n");
        return -1;
    }
    
    printf("  Write successful. root_page=%d, next_count=%d\n", ftl.root_page, ftl.next_count);
    
    // 立即读取验证
    printf("\nStep 4: Read back immediately...\n");
    memset(read_data, 0, USER_DATA_SIZE);
    ret = eflash_ftl_read(&ftl, 100, read_data);
    if (ret != 0) {
        printf("ERROR: Read failed\n");
        return -1;
    }
    
    if (memcmp(write_data, read_data, USER_DATA_SIZE) == 0) {
        printf("  ✓ Data matches before deinit\n");
    } else {
        printf("  ✗ Data MISMATCH before deinit!\n");
        printf("  Expected first byte: 0x%02X, Got: 0x%02X\n", write_data[0], read_data[0]);
    }
    
    // 模拟掉电
    printf("\nStep 5: Simulate power failure (deinit without clean shutdown)...\n");
    eflash_deinit();
    
    // 重新启动
    printf("\nStep 6: Restart and reinitialize...\n");
    ret = eflash_init(TEST_FLASH_FILE);
    if (ret != 0) {
        printf("ERROR: Failed to reinit flash\n");
        return -1;
    }
    
    memset(&ftl, 0, sizeof(ftl));
    ret = eflash_ftl_init(&ftl);
    if (ret != 0) {
        printf("ERROR: Failed to reinit FTL\n");
        return -1;
    }
    
    printf("  After recovery: root_page=%d, next_count=%d\n", ftl.root_page, ftl.next_count);
    
    // 尝试读取
    printf("\nStep 7: Try to read sector 100 after recovery...\n");
    memset(read_data, 0, USER_DATA_SIZE);
    ret = eflash_ftl_read(&ftl, 100, read_data);
    if (ret != 0) {
        printf("ERROR: Read after recovery failed (ret=%d)\n", ret);
        printf("  This is the BUG - data should be recoverable!\n");
        
        // 尝试读取一些调试信息
        printf("\nDebugging: Let's check what's in the radix tree...\n");
        printf("  Root page: %d\n", ftl.root_page);
        
        return -1;
    }
    
    // 验证数据
    printf("  Read successful!\n");
    if (memcmp(write_data, read_data, USER_DATA_SIZE) == 0) {
        printf("  ✓ Data matches after recovery - TEST PASSED\n");
    } else {
        printf("  ✗ Data MISMATCH after recovery!\n");
        printf("  Expected first byte: 0x%02X, Got: 0x%02X\n", write_data[0], read_data[0]);
        printf("  First 16 bytes expected: ");
        for (int i = 0; i < 16; i++) printf("%02X ", write_data[i]);
        printf("\n  First 16 bytes got:     ");
        for (int i = 0; i < 16; i++) printf("%02X ", read_data[i]);
        printf("\n");
    }
    
    // 清理
    eflash_deinit();
    remove(TEST_FLASH_FILE);
    
    printf("\n=== Test Complete ===\n");
    return 0;
}
