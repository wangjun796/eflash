CC       = gcc
CFLAGS   = -std=c11 -Wall -Wextra \
           -Wno-unused-parameter -Wno-unused-variable \
           -Wno-sign-compare -Wno-address \
           -g -O0 -DFTL_DEBUG_ENABLE=0

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

.PHONY: test test-basic test-extension test-stability test-all clean

$(TARGET_BASIC): $(SRC_BASIC)
	$(CC) $(CFLAGS) $(INC_DIRS) -o $@ $^

$(TARGET_CODE_REGION): $(SRC_CODE_REGION)
	$(CC) $(CFLAGS) $(INC_DIRS) -o $@ $^

$(TARGET_EXTENSION): $(SRC_EXTENSION)
	$(CC) $(CFLAGS) $(INC_DIRS) -o $@ $^

$(TARGET_STABILITY): $(SRC_STABILITY)
	$(CC) $(CFLAGS) $(INC_DIRS) -o $@ $^

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