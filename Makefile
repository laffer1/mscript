# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Lucas Holt. All rights reserved.

# mscript — build with the system clang (BSD make syntax)
# (plain '=' so the ambient CXX/CXXFLAGS env vars don't override; command line still can)
CXX      = clang++
CXXFLAGS = -std=c++23 -O2 -Wall -Wextra -Wno-unused-parameter
SRC      != ls src/*.cpp
HDR      != ls src/*.h
LDLIBS   = -lbsdxml

mscript: $(SRC) $(HDR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC) $(LDLIBS)

test: mscript
	sh tests/run.sh

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man

install: mscript
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)/man1
	install -m 755 mscript $(DESTDIR)$(BINDIR)/mscript
	install -m 644 man/mscript.1 $(DESTDIR)$(MANDIR)/man1/mscript.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/mscript $(DESTDIR)$(MANDIR)/man1/mscript.1

clean:
	rm -f mscript

.PHONY: test install uninstall clean
