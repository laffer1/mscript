' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.

' Property Get/Let/Set with parameters, default members with arguments, and
' unqualified calls to a class's own members
Class Grid
    Private cells(2, 2)
    Private objs(2)
    Private d
    Public arr(3)

    Private Sub Class_Initialize()
        Set d = CreateObject("Scripting.Dictionary")
    End Sub

    Public Property Get Cell(r, c)
        Cell = cells(r, c)
    End Property
    Public Property Let Cell(r, c, v)
        cells(r, c) = v
    End Property

    Public Property Get Obj(i)
        Set Obj = objs(i)
    End Property
    Public Property Set Obj(i, o)
        Set objs(i) = o
    End Property

    Public Property Get Items()
        Set Items = d
    End Property

    Public Sub Fill(prefix)
        Dim r, c
        For r = 0 To 2
            For c = 0 To 2
                Cell(r, c) = prefix & r & c
            Next
        Next
        Me.Cell(1, 1) = "centre"
    End Sub

    Public Function Fact(n)
        If n <= 1 Then Fact = 1 Else Fact = n * Fact(n - 1)
    End Function

    Public Function Corner()
        Corner = Cell(2, 2) & "/" & Fact(4)
    End Function

    Public Sub Bump(ByRef x)
        x = x + 1
    End Sub
End Class

Dim g
Set g = New Grid
g.Fill "c"
WScript.Echo g.Cell(0, 2), g.Cell(1, 1), g.Cell(2, 0)
Dim v : v = g.Cell(0, 0)
WScript.Echo v
WScript.Echo g.Corner()

Set g.Obj(0) = Nothing
Set g.Obj(1) = New Grid
WScript.Echo g.Obj(0) Is Nothing, g.Obj(1).Cell(0, 0) = Empty

With g
    .Cell(2, 2) = "with"
    WScript.Echo .Cell(2, 2)
End With

g.Items("k") = "dict"
WScript.Echo g.Items("k"), g.Items.Count
g.arr(1) = "field"
WScript.Echo g.arr(1)

Dim n
n = 1 : g.Bump n : WScript.Echo n
n = 1 : Call g.Bump(n) : WScript.Echo n

' default property with arguments
Class Bag
    Private m(4)
    Public Default Property Get Item(i)
        Item = m(i)
    End Property
    Public Property Let Item(i, v)
        m(i) = v
    End Property
End Class
Dim b
Set b = New Bag
b(1) = "one"
b.Item(4) = 44
WScript.Echo b(1), b.Item(1), b(4)

' default function
Class Doubler
    Public Default Function Twice(x)
        Twice = x * 2
    End Function
End Class
Dim dd
Set dd = New Doubler
WScript.Echo dd(21), dd.Twice(5)

' unqualified Sub, Function and Property calls inside a class
Class Counter
    Private v
    Property Get Value
        Value = v
    End Property
    Property Let Value(x)
        v = x
    End Property
    Sub Reset
        Value = 0
    End Sub
    Sub Inc
        Value = Value + 1
    End Sub
    Function Twice()
        Reset
        Inc
        Inc
        Twice = Value
    End Function
End Class
Dim k
Set k = New Counter
WScript.Echo k.Twice()

On Error Resume Next
Dim x
x = g.Cell(1) : WScript.Echo "too few:", Err.Number : Err.Clear
g.Cell(1, 2, 3) = 4 : WScript.Echo "too many:", Err.Number : Err.Clear
x = g.Nope(1) : WScript.Echo "missing:", Err.Number : Err.Clear
Set g.Cell(0, 0) = New Grid : WScript.Echo "Set on Let-only:", Err.Number : Err.Clear
g.Obj(0) = 5 : WScript.Echo "Let on Set-only:", Err.Number : Err.Clear
