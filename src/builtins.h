// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Lucas Holt. All rights reserved.

// builtins.h — registration of VBScript built-in functions and constants.
#pragma once

struct Interp;

void registerBuiltins(Interp& interp);
void registerConstants(Interp& interp);
