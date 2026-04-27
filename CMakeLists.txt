# eFlash FTL - Flash Translation Layer for NAND flash
# CMake configuration for Visual Studio 2022

cmake_minimum_required(VERSION 3.15)

# 设置项目名称和版本
project(eflash_ftl VERSION 1.0.0 LANGUAGES C)

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
set(ECC_SOURCES
    ecc/bch.c
    ecc/gf13.c
)

set(EFLASH_FTL_SOURCES
    eflash_ftl/eflash_ftl.c
    eflash_ftl/eflash_mgr.c
    eflash_ftl/eflash_sim.c
)

set(EFLASH_FTL_TEST_SOURCES
    eflash_ftl/eflash_ftl_tests.c
)

# Extension tests (separate executable for detailed debugging)
set(EFLASH_FTL_TEST_EXTENSION_SOURCES
    eflash_ftl/eflash_ftl_tests_extension.c
)

# 添加头文件目录
include_directories(
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/ecc
    ${PROJECT_SOURCE_DIR}/eflash_ftl
)

# 创建ECC库
add_library(ecc_lib STATIC ${ECC_SOURCES})
target_include_directories(ecc_lib PUBLIC ${PROJECT_SOURCE_DIR}/ecc)

# 创建eFlash FTL库（输出名为 eflash）
add_library(eflash STATIC ${EFLASH_FTL_SOURCES})
target_include_directories(eflash PUBLIC ${PROJECT_SOURCE_DIR}/eflash_ftl)
target_link_libraries(eflash ecc_lib)

# 编译选项
if(MSVC)
    target_compile_options(ecc_lib PRIVATE /W4)
    target_compile_options(eflash PRIVATE /W4)
else()
    target_compile_options(ecc_lib PRIVATE -Wall -O1 -ggdb)
    target_compile_options(eflash PRIVATE -Wall -O1 -ggdb)
endif()

# 创建eFlash FTL测试程序
add_executable(eflash_ftl_tests ${EFLASH_FTL_TEST_SOURCES} ${EFLASH_FTL_SOURCES} ${ECC_SOURCES})
target_compile_definitions(eflash_ftl_tests PRIVATE _CRT_SECURE_NO_WARNINGS)

# 创建eFlash FTL扩展测试程序（独立可执行文件，便于调试）
add_executable(eflash_ftl_tests_extension ${EFLASH_FTL_TEST_EXTENSION_SOURCES} ${EFLASH_FTL_SOURCES} ${ECC_SOURCES})
target_compile_definitions(eflash_ftl_tests_extension PRIVATE _CRT_SECURE_NO_WARNINGS)

# 可选：启用FTL调试输出，用于故障排查（同时应用到所有测试程序）
# 要启用调试输出，请使用：cmake -DFTL_DEBUG_ENABLE=ON ..
option(FTL_DEBUG_ENABLE "Enable FTL debug output" OFF)
if(FTL_DEBUG_ENABLE)
    target_compile_definitions(eflash_ftl_tests PRIVATE FTL_DEBUG_ENABLE=1)
    target_compile_definitions(eflash PRIVATE FTL_DEBUG_ENABLE=1)
    target_compile_definitions(eflash_ftl_tests_extension PRIVATE FTL_DEBUG_ENABLE=1)
endif()

# 关键修复：在Release模式下也启用assert，确保测试失败时立即终止
if(MSVC)
    # 移除NDEBUG定义，让assert生效
    string(REPLACE "/DNDEBUG" "" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
    string(REPLACE "/DNDEBUG" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
endif()

# 简单示例程序（仅使用公共 API）
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/eflash_ftl/example_simple.c")
    add_executable(example_simple eflash_ftl/example_simple.c)
    target_link_libraries(example_simple eflash ecc_lib)
endif()

# 定义测试命令
enable_testing()

# 添加eFlash FTL主测试
add_test(NAME eflash_ftl_tests COMMAND eflash_ftl_tests)

# 添加eFlash FTL扩展测试
add_test(NAME eflash_ftl_tests_extension COMMAND eflash_ftl_tests_extension)

# 创建一个运行所有测试的目标
add_custom_target(test_all
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS 
        eflash_ftl_tests
        eflash_ftl_tests_extension
    COMMENT "Running all eFlash FTL tests"
)

# 安装规则
install(TARGETS ecc_lib eflash
    ARCHIVE DESTINATION lib
)

install(DIRECTORY ecc/ eflash_ftl/
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)
