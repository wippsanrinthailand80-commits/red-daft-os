# Red Daft AI-Kernel v0.2 "Demo Box" — from-scratch x86_64.
# Output name kept identical to the ISO pipeline contract: build/ai-kernel.elf

CC      ?= gcc
CFLAGS  = -m64 -ffreestanding -nostdlib -nostdinc -fno-builtin -fno-pie \
          -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx \
          -Wall -Wextra -O2 -I src
ASFLAGS = -m64
LDFLAGS = -T linker.ld -nostdlib -Wl,--build-id=none -static -no-pie
BUILD   = build

OBJS = $(BUILD)/boot.o $(BUILD)/kernel.o $(BUILD)/serial.o $(BUILD)/console.o \
       $(BUILD)/string.o $(BUILD)/printf.o $(BUILD)/pmm.o $(BUILD)/heap.o \
       $(BUILD)/locks.o $(BUILD)/intr.o $(BUILD)/pci.o $(BUILD)/hmm.o \
       $(BUILD)/models.o $(BUILD)/shell.o

all: $(BUILD)/ai-kernel.elf

$(BUILD)/ai-kernel.elf: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.S | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

run: all
	qemu-system-x86_64 -kernel $(BUILD)/ai-kernel.elf \
	    -m 256 -display none -serial stdio

smoke: all
	@timeout 20 qemu-system-x86_64 -kernel $(BUILD)/ai-kernel.elf \
	    -m 256 -display none -serial stdio </dev/null | tee smoke.log
	@grep -q "ALL MODELS MATCH" smoke.log && echo "SMOKE PASS"

clean:
	rm -rf $(BUILD) smoke.log

.PHONY: all run clean smoke
