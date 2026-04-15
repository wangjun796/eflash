#include "mini_ftl.h"
#include "eflash_sim.h"
#include <stdio.h>
#include <string.h>

// 定义对象头结构 (16 字节)
// 注意：如果 mini_ftl.h 中已定义此结构，请删除此处的定义以避免重定义错误
typedef struct {
    uint16_t pkg_id;
    uint16_t class_id;
    uint8_t  type;
    uint8_t  reserved[3];
    uint32_t body_addr;
    uint32_t body_size;
} obj_header_t;

int main() {
    mini_ftl_t ftl;
    const char *flash_file = "test_flash.bin";
    
    // 1. 初始化并擦除
    printf("Initializing simulated eFlash (Erasing to 0xFF)...\n");
    eflash_init(flash_file);
    for (int i = 0; i < EFLASH_TOTAL_PAGES; i++) {
        eflash_hw_erase(i);
    }

    // 2. 启动 FTL
    mini_ftl_init(&ftl);
    printf("FTL Init: Base Header at Logic Page %d\n", ftl.base_hdr_addr);

    // 3. 写入基础区对象头 (ID: 0)
    obj_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.pkg_id = 0x1000;
    hdr.class_id = 0x2000;
    mini_ftl_obj_set_header(&ftl, 0, &hdr);
    printf("Set header for ID 0 (Base Level)\n");

    // 4. 写入扩展区对象头 (ID: 250 -> 触发第一级扩展)
    hdr.pkg_id = 0x9999;
    mini_ftl_obj_set_header(&ftl, 250, &hdr);
    printf("Set header for ID 250 (Extended Level 1)\n");

    // 5. 读取验证
    obj_header_t read_hdr;
    if (mini_ftl_obj_get_header(&ftl, 250, &read_hdr) == 0) {
        printf("Read ID 250: PkgID=0x%04X\n", read_hdr.pkg_id);
    } else {
        printf("Failed to read ID 250\n");
    }

    // 6. 模拟掉电恢复
    printf("\nSimulating Power Cycle...\n");
    eflash_deinit();
    eflash_init(flash_file);
    mini_ftl_init(&ftl);

    // 7. 再次读取
    if (mini_ftl_obj_get_header(&ftl, 250, &read_hdr) == 0) {
        printf("After Reboot - Read ID 250: PkgID=0x%04X\n", read_hdr.pkg_id);
    } else {
        printf("After Reboot - Failed to read ID 250\n");
    }

    eflash_deinit();
    return 0;
}