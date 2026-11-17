' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.

' procedures: Sub, Function, ByRef, ByVal, recursion, Optional, Call
Sub Say(msg)
    WScript.Echo "say: " & msg
End Sub

Say "hello"
Call Say("via call")

Function Add(a, b)
    Add = a + b
End Function
WScript.Echo Add(2, 3)

Sub Swap(ByRef x, ByRef y)
    Dim t
    t = x
    x = y
    y = t
End Sub

Dim p, q
p = 1 : q = 2
Swap p, q
WScript.Echo p, q

Sub NoChange(ByVal x)
    x = 99
End Sub
Dim v
v = 5
NoChange v
WScript.Echo "byval " & v

Function Fib(n)
    If n < 2 Then
        Fib = n
    Else
        Fib = Fib(n - 1) + Fib(n - 2)
    End If
End Function
WScript.Echo "fib(10)=" & Fib(10)

Function Fact(n)
    If n <= 1 Then
        Fact = 1
        Exit Function
    End If
    Fact = n * Fact(n - 1)
End Function
WScript.Echo "fact(5)=" & Fact(5)

Function Greet(name, Optional greeting)
    If IsEmpty(greeting) Then greeting = "Hello"
    Greet = greeting & ", " & name & "!"
End Function
WScript.Echo Greet("Ann")
WScript.Echo Greet("Bob", "Hi")

' procedures can see globals
Dim g
g = 100
Function ReadG()
    ReadG = g * 2
End Function
WScript.Echo ReadG()
g = 200
WScript.Echo ReadG()

' local shadows global
Function Shadow()
    Dim g
    g = 1
    Shadow = g
End Function
WScript.Echo Shadow(), g

' calling with expression args
WScript.Echo Add(1 + 2, Add(3, 4))
