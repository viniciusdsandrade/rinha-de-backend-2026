CC ?= gcc
BUILD_DIR := build

COMMON_CFLAGS := -std=c11 -O3 -DNDEBUG -Wall -Wextra -Wpedantic -D_GNU_SOURCE \
	-march=x86-64-v3 -mavx2 -mfma -flto -fno-semantic-interposition \
	-fno-math-errno -fno-trapping-math
CPPFLAGS := -Iinclude
LDLIBS := -lz -lm

CORE_OBJS := $(BUILD_DIR)/rinha.o

.PHONY: all clean test

all: $(BUILD_DIR)/rinha-c $(BUILD_DIR)/prepare_refs_c $(BUILD_DIR)/core_tests

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/rinha.o: src/rinha.c include/rinha.h | $(BUILD_DIR)
	$(CC) $(COMMON_CFLAGS) $(CPPFLAGS) -c $< -o $@

$(BUILD_DIR)/server.o: src/server.c include/rinha.h | $(BUILD_DIR)
	$(CC) $(COMMON_CFLAGS) $(CPPFLAGS) -c $< -o $@

$(BUILD_DIR)/prepare_refs.o: src/prepare_refs.c include/rinha.h | $(BUILD_DIR)
	$(CC) $(COMMON_CFLAGS) $(CPPFLAGS) -c $< -o $@

$(BUILD_DIR)/core_tests.o: tests/core_tests.c include/rinha.h | $(BUILD_DIR)
	$(CC) $(COMMON_CFLAGS) $(CPPFLAGS) -DRINHA_REPO_ROOT=\"$(abspath ..)\" -c $< -o $@

$(BUILD_DIR)/rinha-c: $(CORE_OBJS) $(BUILD_DIR)/server.o
	$(CC) $(COMMON_CFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/prepare_refs_c: $(CORE_OBJS) $(BUILD_DIR)/prepare_refs.o
	$(CC) $(COMMON_CFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/core_tests: $(CORE_OBJS) $(BUILD_DIR)/core_tests.o
	$(CC) $(COMMON_CFLAGS) $^ $(LDLIBS) -o $@

test: $(BUILD_DIR)/core_tests
	./$(BUILD_DIR)/core_tests

clean:
	rm -rf $(BUILD_DIR)
