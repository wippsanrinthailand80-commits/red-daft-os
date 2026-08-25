#!/bin/sh
# Build (if needed) and run the kernel under QEMU with serial -> terminal.
set -e
make
qemu-system-x86_64 -kernel build/ai-kernel.elf \
    -m 128 -display none -serial stdio
