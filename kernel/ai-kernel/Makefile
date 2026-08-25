# Build a from-scratch x86_64 kernel ELF. On an x86_64 host `gcc` works;
# cross builds can override CC=x86_64-elf-gcc. The Dockerfile provides the
# canonical x86_64 environment (so this builds/run anywhere).

CC      ?= gcc
CFLAGS  = -m64 -ffreestanding -nostdlib -nostdinc -fno-builtin \
          -fno-stack-protector -mno-red-zone -mgeneral-regs-only \
          -Wall -Wextra -O2 -I src
AS      = $(CC)
ASFLAGS = -m64
LD      = $(CC)
LDFLAGS = -T linker.ld -nostdlib -Wl,--build-id=none -static
BUILD   = build

OBJS = $(BUILD)/boot.o $(BUILD)/kernel.o $(BUILD)/serial.o $(BUILD)/printf.o \
       $(BUILD)/mm.o $(BUILD)/pva.o $(BUILD)/sandbox.o

all: $(BUILD)/ai-kernel.elf

$(BUILD)/ai-kernel.elf: $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.S | $(BUILD)
	$(AS) $(ASFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

run: all
	qemu-system-x86_64 -kernel $(BUILD)/ai-kernel.elf \
	    -m 128 -display none -serial stdio

clean:
	rm -rf $(BUILD)
