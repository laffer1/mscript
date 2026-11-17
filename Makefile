# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Lucas Holt. All rights reserved.

# mscript — build with the system clang (BSD make syntax)
# (plain '=' so the ambient CXX/CXXFLAGS env vars don't override; command line still can)
CXX      = clang++
CXXFLAGS = -std=c++23 -O2 -Wall -Wextra -Wno-unused-parameter
SRC      != ls src/*.cpp
HDR      != ls src/*.h

mscript: $(SRC) $(HDR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

test: mscript
	sh tests/run.sh

clean:
	rm -f mscript

.PHONY: test clean
