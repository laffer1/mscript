' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.

' Arrays are values: assignment, ByVal, function results, fields and
' collections get a copy; ByRef parameters still share the caller's array.

Dim a, b
a = Array(1, 2, 3)
b = a
b(0) = 99
WScript.Echo "assign: " & a(0) & " " & b(0)

Sub ByValSub(ByVal x)
    x(1) = "byval"
End Sub
Sub ByRefSub(ByRef x)
    x(1) = "byref"
End Sub
Sub DefaultSub(x)
    x(2) = "default"
End Sub
ByValSub a
WScript.Echo "byval: " & a(1)
ByRefSub a
WScript.Echo "byref: " & a(1)
DefaultSub a
WScript.Echo "default byref: " & a(2)
a(2) = 3
DefaultSub (a)
WScript.Echo "parenthesized arg: " & a(2)
Call DefaultSub((a))
WScript.Echo "call with (a): " & a(2)

Function Echoed(ByRef x)
    Echoed = x
End Function
Dim c
c = Echoed(a)
c(0) = "copy"
WScript.Echo "function result: " & a(0) & " " & c(0)

Function Fresh()
    Fresh = Array("f0", "f1")
End Function
c = Fresh()
c(1) = "changed"
WScript.Echo "fresh result: " & Fresh()(1) & " " & c(1)

' nested arrays are copied deeply
Dim n, m
n = Array(Array(1, 2), "x")
m = n
m(0)(0) = 100
WScript.Echo "nested: " & n(0)(0) & " " & m(0)(0)

Dim inner, outer
inner = Array(5, 6)
outer = Array(inner, 0)
inner(0) = 50
WScript.Echo "stored element: " & outer(0)(0) & " " & inner(0)

Dim holder(1)
holder(0) = inner
inner(1) = 60
WScript.Echo "element assign: " & holder(0)(1) & " " & inner(1)

Sub NestedByVal(ByVal x)
    x(0)(1) = "inner byval"
End Sub
NestedByVal n
WScript.Echo "nested byval: " & n(0)(1)

' ReDim Preserve and Erase only touch their own copy
Dim fx(2), g
fx(0) = "f"
g = fx
ReDim Preserve g(4)
g(0) = "g"
WScript.Echo "redim copy: " & fx(0) & " " & UBound(fx) & " " & g(0) & " " & UBound(g)
Dim h
h = a
Erase h
WScript.Echo "erase copy: " & UBound(a) & " " & a(0)

Sub GrowByRef(ByRef x)
    ReDim Preserve x(5)
    x(5) = "grown"
End Sub
Dim dyn
dyn = Array(1)
GrowByRef dyn
WScript.Echo "byref redim: " & UBound(dyn) & " " & dyn(5)

Sub EraseByVal(ByVal x)
    Erase x
End Sub
Sub EraseByRef(ByRef x)
    Erase x
End Sub
Dim ev(1)
ev(0) = "kept"
EraseByVal ev
WScript.Echo "byval erase: " & ev(0)
EraseByRef ev
WScript.Echo "byref erase: " & IsEmpty(ev(0)) & " " & UBound(ev)

' For Each hands out copies of nested arrays
Dim rows, row
rows = Array(Array(1), Array(2))
For Each row In rows
    row(0) = 0
Next
WScript.Echo "for each: " & rows(0)(0) & " " & rows(1)(0)

' class fields and Dictionary items keep their own copy
Class Holder
    Public Items
End Class
Dim o
Set o = New Holder
o.Items = a
a(0) = "after"
WScript.Echo "class field: " & o.Items(0)

Dim d
Set d = CreateObject("Scripting.Dictionary")
d.Add "k", a
d("j") = a
a(0) = "later"
WScript.Echo "dictionary: " & d("k")(0) & " " & d("j")(0)

' parentheses around an argument pass a copy, for scalars too
Sub Bump(x)
    x = x + 1
End Sub
Dim cnt
cnt = 1
Bump (cnt)
Bump cnt
Call Bump((cnt))
Call Bump(cnt)
WScript.Echo "paren scalar: " & cnt
