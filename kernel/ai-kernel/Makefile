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
	grub-mkrescue -o build/smoke.iso isoboot 2>/dev/null
	qemu-system-x86_64 -cdrom build/smoke.iso \
	    -m 256 -display none -serial stdio

# Boots exactly like the Red Daft ISO dual-boot menu: GRUB -> multiboot2.
smoke: all isoboot/boot/ai-kernel.elf isoboot/boot/grub/grub.cfg
	-grub-mkrescue -o build/smoke.iso isoboot
	timeout 240 qemu-system-x86_64 -cdrom build/smoke.iso \
	    -m 256 -display none -no-reboot -serial file:smoke.log \
	    -debugcon file:build/debug.log -global isa-debugcon.iobase=0xe9 </dev/null || true
	echo "--- debugcon tail ---"; tail -c 400 build/debug.log 2>/dev/null || true; echo
	N=$$(grep -c '-> MATCH' smoke.log 2>/dev/null || echo 0); \
	  [ "$$N" -eq 4 ] || { echo "expected 4 MATCH, got $$N"; exit 1; }
	grep -q "WRITEBACK OK"  smoke.log
	grep -q "\[kv\] OK"     smoke.log
	grep -q "\[scratch\] OK" smoke.log
	grep -q "SELF-TEST PASS" smoke.log
	grep -q "elasticity"    smoke.log
	echo "SMOKE PASS"

isoboot/boot/ai-kernel.elf: $(BUILD)/ai-kernel.elf
	mkdir -p isoboot/boot
	cp $< $@

isoboot/boot/grub/grub.cfg: Makefile
	mkdir -p isoboot/boot/grub
	printf 'set timeout=0\nmenuentry "aik" {\n  multiboot2 /boot/ai-kernel.elf\n  boot\n}\n' > $@

clean:
	rm -rf $(BUILD) smoke.log isoboot

.PHONY: all run clean smoke
