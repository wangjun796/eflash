CC       = gcc
CFLAGS   = -std=c11 -Wall -Wextra \
           -Wno-unused-parameter -Wno-unused-variable \
           -Wno-sign-compare -Wno-address \
           -g -O0 -DFTL_DEBUG_ENABLE=0 -DEFLASH_TEST_MODE

# Debug build with logging enabled (use for troubleshooting test failures)
CFLAGS_DEBUG = -std=c11 -Wall -Wextra \
           -Wno-unused-parameter -Wno-unused-variable \
           -Wno-sign-compare -Wno-address \
           -g -O0 -DFTL_DEBUG_ENABLE=1 -DEFLASH_TEST_MODE

INC_DIRS = -Ieflash_ftl -Iecc

SRC_BASIC = eflash_ftl/eflash_ftl.c \
      eflash_ftl/eflash_ftl_tests.c \
      eflash_ftl/eflash_mgr.c \
      eflash_ftl/eflash_sim.c \
      ecc/bch.c \
      ecc/gf13.c

SRC_CODE_REGION = eflash_ftl/eflash_ftl.c \
      eflash_ftl/eflash_ftl_tests_code_region.c \
      eflash_ftl/eflash_mgr.c \
      eflash_ftl/eflash_sim.c \
      ecc/bch.c \
      ecc/gf13.c

SRC_EXTENSION = eflash_ftl/eflash_ftl.c \
      eflash_ftl/eflash_ftl_tests_extension.c \
      eflash_ftl/eflash_mgr.c \
      eflash_ftl/eflash_sim.c \
      eflash_ftl/eflash_ftl_visual.c \
      ecc/bch.c \
      ecc/gf13.c

SRC_STABILITY = eflash_ftl/eflash_ftl.c \
      eflash_ftl/eflash_ftl_tests_stability.c \
      eflash_ftl/eflash_mgr.c \
      eflash_ftl/eflash_sim.c \
      eflash_ftl/eflash_ftl_visual.c \
      ecc/bch.c \
      ecc/gf13.c

TARGET_BASIC         = eflash_ftl_tests.exe
TARGET_CODE_REGION = eflash_ftl_tests_code_region.exe
TARGET_EXTENSION   = eflash_ftl_tests_extension.exe
TARGET_STABILITY   = eflash_ftl_tests_stability.exe

# Debug targets with logging enabled
TARGET_BASIC_DBG         = eflash_ftl_tests_debug.exe
TARGET_CODE_REGION_DBG = eflash_ftl_tests_code_region_debug.exe
TARGET_EXTENSION_DBG   = eflash_ftl_tests_extension_debug.exe
TARGET_STABILITY_DBG   = eflash_ftl_tests_stability_debug.exe

.PHONY: test test-basic test-extension test-stability test-all clean
.PHONY: test-debug test-debug-basic test-debug-extension test-debug-stability test-debug-all

$(TARGET_BASIC): $(SRC_BASIC)
	$(CC) $(CFLAGS) $(INC_DIRS) -o $@ $^

$(TARGET_CODE_REGION): $(SRC_CODE_REGION)
	$(CC) $(CFLAGS) $(INC_DIRS) -o $@ $^

$(TARGET_EXTENSION): $(SRC_EXTENSION)
	$(CC) $(CFLAGS) $(INC_DIRS) -o $@ $^

$(TARGET_STABILITY): $(SRC_STABILITY)
	$(CC) $(CFLAGS) $(INC_DIRS) -o $@ $^

# Debug build targets with logging enabled
$(TARGET_BASIC_DBG): $(SRC_BASIC)
	$(CC) $(CFLAGS_DEBUG) $(INC_DIRS) -o $@ $^

$(TARGET_CODE_REGION_DBG): $(SRC_CODE_REGION)
	$(CC) $(CFLAGS_DEBUG) $(INC_DIRS) -o $@ $^

$(TARGET_EXTENSION_DBG): $(SRC_EXTENSION)
	$(CC) $(CFLAGS_DEBUG) $(INC_DIRS) -o $@ $^

$(TARGET_STABILITY_DBG): $(SRC_STABILITY)
	$(CC) $(CFLAGS_DEBUG) $(INC_DIRS) -o $@ $^

test: $(TARGET_CODE_REGION)
	./$(TARGET_CODE_REGION)

test-basic: $(TARGET_BASIC)
	./$(TARGET_BASIC)

test-extension: $(TARGET_EXTENSION)
	./$(TARGET_EXTENSION)

test-stability: $(TARGET_STABILITY)
	./$(TARGET_STABILITY)

test-all: $(TARGET_BASIC) $(TARGET_CODE_REGION) $(TARGET_EXTENSION) $(TARGET_STABILITY)
	@echo "========================================"
	@echo " Running ALL Test Suites"
	@echo "========================================"
	@echo ""
	@echo "--- Suite 1: Basic Tests ---"
	./$(TARGET_BASIC) || exit 1
	@echo ""
	@echo "--- Suite 2: Code Region Tests ---"
	./$(TARGET_CODE_REGION) || exit 1
	@echo ""
	@echo "--- Suite 3: Extension Tests ---"
	./$(TARGET_EXTENSION) || exit 1
	@echo ""
	@echo "--- Suite 4: Stability Tests ---"
	./$(TARGET_STABILITY) || exit 1
	@echo ""
	@echo "========================================"
	@echo " ALL TESTS PASSED"
	@echo "========================================"

clean:
	rm -f $(TARGET_BASIC) $(TARGET_CODE_REGION) $(TARGET_EXTENSION) $(TARGET_STABILITY)
	rm -f $(TARGET_BASIC_DBG) $(TARGET_CODE_REGION_DBG) $(TARGET_EXTENSION_DBG) $(TARGET_STABILITY_DBG)

# Debug test targets (with logging enabled)
test-debug: $(TARGET_CODE_REGION_DBG)
	./$(TARGET_CODE_REGION_DBG)

test-debug-basic: $(TARGET_BASIC_DBG)
	./$(TARGET_BASIC_DBG)

test-debug-extension: $(TARGET_EXTENSION_DBG)
	./$(TARGET_EXTENSION_DBG)

test-debug-stability: $(TARGET_STABILITY_DBG)
	./$(TARGET_STABILITY_DBG)

test-debug-all: $(TARGET_BASIC_DBG) $(TARGET_CODE_REGION_DBG) $(TARGET_EXTENSION_DBG) $(TARGET_STABILITY_DBG)
	@echo "========================================"
	@echo " Running ALL Test Suites (DEBUG with logging)"
	@echo "========================================"
	@echo ""
	@echo "--- Suite 1: Basic Tests (with logging) ---"
	./$(TARGET_BASIC_DBG) || exit 1
	@echo ""
	@echo "--- Suite 2: Code Region Tests (with logging) ---"
	./$(TARGET_CODE_REGION_DBG) || exit 1
	@echo ""
	@echo "--- Suite 3: Extension Tests (with logging) ---"
	./$(TARGET_EXTENSION_DBG) || exit 1
	@echo ""
	@echo "--- Suite 4: Stability Tests (with logging) ---"
	./$(TARGET_STABILITY_DBG) || exit 1
	@echo ""
	@echo "========================================"
	@echo " ALL DEBUG TESTS PASSED"
	@echo "========================================"