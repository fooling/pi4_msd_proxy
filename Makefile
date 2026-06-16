CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -pthread
PREFIX   ?= /usr/local
BIN       = pi4_msd_proxy

$(BIN): $(BIN).cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	install -d $(DESTDIR)$(PREFIX)/etc
	[ -f $(DESTDIR)$(PREFIX)/etc/$(BIN).conf ] || install -m644 $(BIN).conf.example $(DESTDIR)$(PREFIX)/etc/$(BIN).conf
	install -d $(DESTDIR)/etc/systemd/system
	install -m644 $(BIN).service $(DESTDIR)/etc/systemd/system/$(BIN).service
	@echo
	@echo "已安装。下一步:"
	@echo "  sudo nano $(PREFIX)/etc/$(BIN).conf       # 改端口/默认画质档"
	@echo "  sudo systemctl daemon-reload"
	@echo "  sudo systemctl enable --now $(BIN)"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)
	rm -f $(DESTDIR)/etc/systemd/system/$(BIN).service

clean:
	rm -f $(BIN)

.PHONY: install uninstall clean
