' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.

' a runtime error after line continuations reports its own line
Dim x
x = "a" & _
    "b" & _
    "c"

' comment
Dim a(1) : x = 1 : a(5) = x
