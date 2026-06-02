CC       = gcc
CFLAGS   = -std=c11 -Wall -Wextra \
           -Wno-unused-parameter -Wno-unused-variable \
           -Wno-sign-compare -Wno-address \
           -g -O0 -DFTL_DEBUG_ENABLE=0

INC_DIRS = -Ieflash_ftl -Iecc

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

TARGET_CODE_REGION = eflash_ftl_tests_code_region.exe
TARGET_EXTENSION   = eflash_ftl_tests_extension.exe
TARGET_STABILITY   = eflash_ftl_tests_stability.exe

.PHONY: test test-extension test-stability clean

$(TARGET_CODE_REGION): $(SRC_CODE_REGION)
	$(CC) $(CFLAGS) $(INC_DIRS) -o $@ $^

$(TARGET_EXTENSION): $(SRC_EXTENSION)
	$(CC) $(CFLAGS) $(INC_DIRS) -o $@ $^

$(TARGET_STABILITY): $(SRC_STABILITY)
	$(CC) $(CFLAGS) $(INC_DIRS) -o $@ $^

test: $(TARGET_CODE_REGION)
	./$(TARGET_CODE_REGION)

test-extension: $(TARGET_EXTENSION)
	./$(TARGET_EXTENSION)

test-stability: $(TARGET_STABILITY)
	./$(TARGET_STABILITY)

clean:
	rm -f $(TARGET_CODE_REGION) $(TARGET_EXTENSION) $(TARGET_STABILITY)