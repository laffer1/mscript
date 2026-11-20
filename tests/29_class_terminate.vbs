' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.

' Class_Terminate runs once, when the last reference to an object goes away.
Class Tracked
    Public Name
    Public Other
    Private Sub Class_Terminate
        WScript.Echo "terminate " & Name
    End Sub
End Class

Function Make(n)
    Set Make = New Tracked
    Make.Name = n
End Function

Dim a, b, arr(1), d, g

' Set x = Nothing
Set a = Make("nothing")
Set a = Nothing
WScript.Echo "after Set Nothing"

' reassignment releases the old object
Set a = Make("first")
Set a = Make("second")
WScript.Echo "after reassign"

' a second reference keeps the object alive
Set b = a
Set a = Nothing
WScript.Echo "still alive: " & b.Name
Set b = Nothing

' locals are released when the procedure returns, before the caller goes on
Sub UseLocal
    Dim o
    Set o = Make("sub local")
    WScript.Echo "in sub"
End Sub
UseLocal
Function LocalResult
    Dim o
    Set o = Make("function local")
    LocalResult = "function result"
End Function
WScript.Echo LocalResult()

' array elements and Erase
Set arr(0) = Make("element 0")
Set arr(1) = Make("element 1")
Set arr(0) = Nothing
Erase arr
WScript.Echo "after Erase"

' Dictionary items
Set d = CreateObject("Scripting.Dictionary")
d.Add "k", Make("dictionary item")
d.Remove "k"
WScript.Echo "after Remove"

' a temporary object
Call (Make("temporary")).Name
WScript.Echo "after temporary"

' calling Class_Terminate yourself does not count
Set a = Make("explicit")
a.Name = "explicit call"
Call a.Class_Terminate
a.Name = "release"
Set a = Nothing

' a reference cycle keeps both objects alive (VBScript leaks them too)
Set a = Make("cycle 1")
Set b = Make("cycle 2")
Set a.Other = b
Set b.Other = a
Set a = Nothing
Set b = Nothing
WScript.Echo "after cycle"

' errors in Class_Terminate are discarded and leave Err alone
Class Failing
    Private Sub Class_Terminate
        Err.Raise 5
        WScript.Echo "not reached"
    End Sub
End Class
On Error Resume Next
Err.Raise 11
Set a = New Failing
Set a = Nothing
WScript.Echo "Err.Number after failing terminator: " & Err.Number
On Error GoTo 0

' Class_Terminate can keep the object alive; it does not run a second time
Class Keeper
    Private Sub Class_Terminate
        WScript.Echo "keeper terminate"
        Set g = Me
    End Sub
End Class
Set a = New Keeper
Set a = Nothing
WScript.Echo "kept: " & IsObject(g)
Set g = Nothing
WScript.Echo "after keeper"

' a long chain is released without overflowing the stack
Class Node
    Public NextNode
    Private Sub Class_Terminate
        count = count + 1
    End Sub
End Class
Dim count, head, n, i
count = 0
Set head = Nothing
For i = 1 To 5000
    Set n = New Node
    Set n.NextNode = head
    Set head = n
Next
Set n = Nothing
Set head = Nothing
WScript.Echo "chain terminated: " & count

' objects still alive at the end of the script are terminated after it
Set a = Make("script end")
WScript.Echo "last statement"
