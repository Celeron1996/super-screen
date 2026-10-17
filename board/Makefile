# board/Makefile — 板端交叉编译
# See docs/plan-imx6ull.md §6

ifeq ($(HOST),1)
CC := gcc
ARCHFLAGS :=
BUILD := build/host
else
ifeq ($(origin CC), default)
CC := arm-buildroot-linux-gnueabihf-gcc
endif
ARCHFLAGS := -mfpu=neon -mfloat-abi=hard
BUILD := build
endif

CFLAGS ?= -O2
CFLAGS += -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra $(ARCHFLAGS) \
          -Isrc -MMD -MP
LDLIBS := -lrt

CLIENT_OBJS := $(BUILD)/main.o $(BUILD)/net.o $(BUILD)/protocol.o \
               $(BUILD)/fb.o $(BUILD)/lz4.o
PATTERN_OBJS := $(BUILD)/test_pattern.o $(BUILD)/fb.o
DEPS := $(CLIENT_OBJS:.o=.d) $(PATTERN_OBJS:.o=.d)

.PHONY: all clean

all: $(BUILD)/super_screen_client $(BUILD)/test_pattern

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/lz4.o: src/lz4/lz4.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/test_pattern.o: tools/test_pattern.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/super_screen_client: $(CLIENT_OBJS)
	$(CC) $(CFLAGS) $(CLIENT_OBJS) -o $@ $(LDLIBS)

$(BUILD)/test_pattern: $(PATTERN_OBJS)
	$(CC) $(CFLAGS) $(PATTERN_OBJS) -o $@ $(LDLIBS)

clean:
	rm -rf build

-include $(DEPS)
