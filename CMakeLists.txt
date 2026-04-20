# Dhara - NAND flash management layer
# CMake configuration for Visual Studio 2022

cmake_minimum_required(VERSION 3.15)

# 设置项目名称和版本
project(dhara VERSION 1.0.0 LANGUAGES C)

# 设置C标准
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 检测编译器类型
if(MSVC)
    # Visual Studio特定设置
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    
    # 添加MSVC特定的编译选项
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    add_compile_options(/wd4267 /wd4244 /wd4018 /wd4334 /utf-8)
endif()

# 定义源文件
set(DHARA_SOURCES
    dhara/error.c
    dhara/journal.c
    dhara/map.c
)

set(ECC_SOURCES
    ecc/bch.c
    ecc/gf13.c
    ecc/hamming.c
    ecc/crc32.c
)

set(EFLASH_FTL_SOURCES
    eflash_ftl/mini_ftl.c
    eflash_ftl/space_mgr.c
    eflash_ftl/eflash_sim.c
)

set(TEST_SOURCES
    tests/error.c
    tests/nand.c
    tests/journal.c
    tests/recovery.c
    tests/jfill.c
    tests/map.c
    tests/epoch_roll.c
    tests/bch.c
    tests/hamming.c
    tests/crc32.c
    tests/jtutil.c
    tests/sim.c
    tests/util.c
)

set(TOOL_SOURCES
    tools/gftool.c
    tools/gentab.c
)

# 添加头文件目录
include_directories(
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/dhara
    ${PROJECT_SOURCE_DIR}/ecc
    ${PROJECT_SOURCE_DIR}/tests
    ${PROJECT_SOURCE_DIR}/eflash_ftl
    ${PROJECT_SOURCE_DIR}/.
)

# 创建静态库
add_library(dhara_lib STATIC ${DHARA_SOURCES})
target_include_directories(dhara_lib PUBLIC ${PROJECT_SOURCE_DIR}/dhara)

add_library(ecc_lib STATIC ${ECC_SOURCES})
target_include_directories(ecc_lib PUBLIC ${PROJECT_SOURCE_DIR}/ecc)

add_library(eflash_ftl_lib STATIC ${EFLASH_FTL_SOURCES})
target_include_directories(eflash_ftl_lib PUBLIC ${PROJECT_SOURCE_DIR}/eflash_ftl)
target_link_libraries(eflash_ftl_lib ecc_lib)

# 编译选项
if(MSVC)
    target_compile_options(dhara_lib PRIVATE /W4)
    target_compile_options(ecc_lib PRIVATE /W4)
    target_compile_options(eflash_ftl_lib PRIVATE /W4)
else()
    target_compile_options(dhara_lib PRIVATE -Wall -O1 -ggdb)
    target_compile_options(ecc_lib PRIVATE -Wall -O1 -ggdb)
    target_compile_options(eflash_ftl_lib PRIVATE -Wall -O1 -ggdb)
endif()

# 定义Windows随机数和兼容函数源文件
set(WINDOWS_RAND_FILE ${CMAKE_CURRENT_SOURCE_DIR}/windows_rand.c)
set(WINDOWS_COMPAT_FILE ${CMAKE_CURRENT_SOURCE_DIR}/windows_compat.c)

# 创建整合测试程序
add_executable(all_tests tests/all_tests.c tests/sim.c tests/util.c tests/jtutil.c ${DHARA_SOURCES} ${WINDOWS_RAND_FILE})
target_link_libraries(all_tests ecc_lib)

# 创建eFlash FTL测试程序
add_executable(eflash_ftl_tests tests/eflash_ftl_tests.c ${EFLASH_FTL_SOURCES} ${ECC_SOURCES} ${WINDOWS_RAND_FILE})
target_compile_definitions(eflash_ftl_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
# 可选：启用FTL调试输出，用于故障排查
# 要启用调试输出，请使用：cmake -DFTL_DEBUG_ENABLE=ON ..
option(FTL_DEBUG_ENABLE "Enable FTL debug output" OFF)
if(FTL_DEBUG_ENABLE)
    target_compile_definitions(eflash_ftl_tests PRIVATE FTL_DEBUG_ENABLE=1)
endif()

# 关键修复：在Release模式下也启用assert，确保测试失败时立即终止
if(MSVC AND CMAKE_BUILD_TYPE STREQUAL "Release")
    # 移除NDEBUG定义，让assert生效
    string(REPLACE "/DNDEBUG" "" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
    string(REPLACE "/DNDEBUG" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
endif()

# 创建Flash模拟器演示程序
add_executable(flash_sim_demo sim_flash.c)
target_compile_definitions(flash_sim_demo PRIVATE SIM_FLASH_MAIN)
if(MSVC)
    target_link_libraries(flash_sim_demo)
endif()

# 创建Flash与Dhara结合的示例程序
add_executable(flash_with_dhara examples/flash_with_dhara.c sim_flash.c ${DHARA_SOURCES} ${ECC_SOURCES})
target_link_libraries(flash_with_dhara)

# 创建基于文件的 Flash 全面测试程序
add_executable(test_dhara_on_file_flash examples/test_dhara_on_file_flash.c sim_flash.c file_flash_nand.c tests/util.c tests/jtutil.c ${DHARA_SOURCES} ${ECC_SOURCES} ${WINDOWS_RAND_FILE})
target_link_libraries(test_dhara_on_file_flash dhara_lib ecc_lib)

# 保留独立的工具程序
add_executable(gftool tools/gftool.c ${ECC_SOURCES} ${WINDOWS_COMPAT_FILE})
target_compile_definitions(gftool PRIVATE _CRT_SECURE_NO_DEPRECATE)
target_link_libraries(gftool)

add_executable(gentab tools/gentab.c ${ECC_SOURCES})
target_link_libraries(gentab)

# 创建 gentab 测试程序
add_executable(test_gentab tests/test_gentab.c tools/gentab.c ${ECC_SOURCES})
target_compile_definitions(test_gentab PRIVATE _CRT_SECURE_NO_WARNINGS GENTAB_TEST_MODE)
if(MSVC)
    target_link_libraries(test_gentab)
else()
    target_link_libraries(test_gentab)
endif()

# 创建 CRC 多项式演示程序
add_executable(crc_poly_demo tools/crc_poly_demo.c)
if(MSVC)
    target_link_libraries(crc_poly_demo)
else()
    target_link_libraries(crc_poly_demo)
endif()

# 定义测试命令
enable_testing()

# 添加Dhara整合测试
add_test(NAME all_tests COMMAND all_tests)

# 添加eFlash FTL测试
add_test(NAME eflash_ftl_tests COMMAND eflash_ftl_tests)

# 添加 gentab 测试
add_test(NAME gentab_test COMMAND test_gentab)

# 创建一个运行所有测试的目标
add_custom_target(test_all
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS 
        all_tests
        eflash_ftl_tests
    COMMENT "Running all tests"
)
