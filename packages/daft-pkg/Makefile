# packages/daft-pkg/Makefile
PREFIX ?= /usr/local
BIN    := daft-pkg
SCRIPT := daft-pkg.sh

install:
	install -Dm755 $(SCRIPT) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	install -Dm644 ../deb-adapter/daft-deb-adapter.sh $(DESTDIR)$(PREFIX)/bin/daft-deb
	@echo "daft-pkg installed to $(DESTDIR)$(PREFIX)/bin"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN) $(DESTDIR)$(PREFIX)/bin/daft-deb

test:
	bash -n $(SCRIPT) && echo "syntax ok"

.PHONY: install uninstall test
