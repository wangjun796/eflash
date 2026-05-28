CC       = gcc
CFLAGS   = -std=c11 -Wall -Wextra \
           -Wno-unused-parameter -Wno-unused-variable \
           -Wno-sign-compare -Wno-address \
           -g -O0 -DFTL_DEBUG_ENABLE=0

INC_DIRS = -Ieflash_ftl -Iecc

SRC = eflash_ftl/eflash_ftl.c \
      eflash_ftl/eflash_ftl_tests_code_region.c \
      eflash_ftl/eflash_mgr.c \
      eflash_ftl/eflash_sim.c \
      ecc/bch.c \
      ecc/gf13.c

TARGET = eflash_ftl_tests_code_region.exe

.PHONY: test clean

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(INC_DIRS) -o $@ $^

test: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)