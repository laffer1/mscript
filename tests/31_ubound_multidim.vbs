' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.

' UBound/LBound on multi-dimensional arrays, and ReDim
Dim a(2, 4, 6)
WScript.Echo UBound(a), UBound(a, 1), UBound(a, 2), UBound(a, 3)
WScript.Echo LBound(a), LBound(a, 2), LBound(a, 3)

Dim i, s
s = ""
For i = 1 To 3
    s = s & UBound(a, i) & " "
Next
WScript.Echo s

' ReDim declares a name it doesn't know, and replaces a non-array value
ReDim b(1, 3)
WScript.Echo UBound(b), UBound(b, 2)
Dim c : c = 5
ReDim c(2)
WScript.Echo IsArray(c), UBound(c)

' ReDim Preserve may change only the last dimension
b(1, 3) = "kept"
ReDim Preserve b(1, 5)
WScript.Echo UBound(b, 1), UBound(b, 2), b(1, 3)
ReDim Preserve b(1, 2)
WScript.Echo UBound(b, 2), IsEmpty(b(1, 2))

' ReDim x(-1) is an empty array
ReDim e(-1)
WScript.Echo LBound(e), UBound(e)
WScript.Echo UBound(Array())

' a dynamic array with no bounds yet, then ReDim Preserve on it
Dim d()
ReDim Preserve d(3)
d(0) = "x"
ReDim Preserve d(5)
WScript.Echo UBound(d), d(0)

On Error Resume Next
Dim r
r = UBound(a, 4) : WScript.Echo "dim 4:", Err.Number : Err.Clear
r = UBound(a, 0) : WScript.Echo "dim 0:", Err.Number : Err.Clear
r = LBound(a, 4) : WScript.Echo "lbound dim 4:", Err.Number : Err.Clear
r = UBound(5) : WScript.Echo "number:", Err.Number : Err.Clear
r = LBound("s") : WScript.Echo "string:", Err.Number : Err.Clear
r = UBound(Empty) : WScript.Echo "empty:", Err.Number : Err.Clear
Dim u()
r = UBound(u) : WScript.Echo "no bounds:", Err.Number : Err.Clear
ReDim Preserve b(2, 5) : WScript.Echo "preserve first dim:", Err.Number : Err.Clear
ReDim Preserve b(1) : WScript.Echo "preserve dim count:", Err.Number : Err.Clear
ReDim a(1) : WScript.Echo "fixed:", Err.Number : Err.Clear
WScript.Echo UBound(b, 1), UBound(b, 2)
