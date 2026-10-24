CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11 -pthread
CFLAGS  += -Iinclude -MMD -MP
BUILD   := build
COMMON  := $(BUILD)/sha256.o $(BUILD)/meta.o $(BUILD)/protocol.o $(BUILD)/util.o

all: $(BUILD)/peer $(BUILD)/mkmeta

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/peer: $(BUILD)/peer.o $(COMMON)
	$(CC) $(CFLAGS) $^ -o $@ -pthread

$(BUILD)/mkmeta: $(BUILD)/mkmeta.o $(COMMON)
	$(CC) $(CFLAGS) $^ -o $@

clean:
	rm -rf $(BUILD)

distclean: clean
	rm -rf work logs results data

.PHONY: all clean distclean
-include $(wildcard $(BUILD)/*.d)
