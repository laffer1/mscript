// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Lucas Holt. All rights reserved.

// regexp.h — VBScript.RegExp-compatible object built on the libc POSIX regex
// (regcomp/regexec, <regex.h>).
#pragma once

#include "variant.h"
#include <memory>

struct Interp;

std::shared_ptr<VBObject> makeRegExp(Interp& interp);
