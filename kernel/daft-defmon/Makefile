# kernel/daft-defmon/Makefile
obj-m += daft-defmon.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PREFIX ?= /usr/local

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install: all
	install -Dm644 daft-defmon.ko $(DESTDIR)$(PREFIX)/lib/modules/extra/daft-defmon.ko
	@echo "install with: insmod $(DESTDIR)$(PREFIX)/lib/modules/extra/daft-defmon.ko"

.PHONY: all clean install
