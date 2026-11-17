// builtins.h — registration of VBScript built-in functions and constants.
#pragma once

struct Interp;

void registerBuiltins(Interp& interp);
void registerConstants(Interp& interp);
