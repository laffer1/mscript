' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.
'
' shim.vbs — test harness shim for running Wine's vbscript conformance .vbs
' files (dlls/vbscript/tests/*.vbs from the wine port, LGPL — NOT copied
' into this repo; tests/wine/run.sh reads them from the port workdir) under
' mscript. run.sh concatenates this BEFORE the wine test file (with the wine
' file's "Option Explicit" hoisted to line 1): mscript registers Dim at
' execution time, so the harness globals must be declared before the wine
' file's top-level statements run. Procedures hoist, so the wine file can
' call ok/is/trace/getVT defined here.
'
' Provides the globals the wine C harness (run.c) injects: ok, is, trace,
' getVT, todo_wine_ok, throwInt, throwWithDesc, collectionObj, reportSuccess.
' run.sh appends a footer after the wine file that prints the "1..N" summary.
Dim g_no, g_fail
g_no = 0
g_fail = 0

Sub todo_wine_ok(cond, msg)
    ' wine's todo_wine_ok: a failure is expected (TAP "# TODO"), not counted
    g_no = g_no + 1
    If CBool(cond) Then
        WScript.Echo "ok " & g_no & " - " & msg
    Else
        WScript.Echo "not ok " & g_no & " - " & msg & " # TODO"
    End If
End Sub

Sub throwInt(n)
    ' wine's harness returns the raw HRESULT: success codes (bit 31 clear)
    ' are not errors and leave Err.Number untouched
    If n < 0 Then Err.Raise n
End Sub

Sub throwWithDesc()
    Err.Raise &hdeadbeef&, "", "test", "test.chm", 10
End Sub

' wine's collectionObj is an enumerable object with a reset method
Class ShimColl
    Sub reset()
    End Sub
    Function _NewEnum()
        _NewEnum = Array(1, 2, 3)
    End Function
End Class
Dim collectionObj
Set collectionObj = New ShimColl

Sub reportSuccess()
    ' wine prints a success marker here; the TAP summary is printed by the footer
End Sub

' wine's harness exposes the locale as a global; assume English
Const isEnglishLang = True

' wine's testObj is a dynamic-dispatch object; a class with keyword-named
' fields covers the lang.vbs "keywords as member names" tests
Class ShimTestObj
    Dim and, byref, byval, call, case, class, const, dim, do, each, else, elseif, empty, end, eqv, exit, false, for, function, get, goto, if, imp, in, is, let, loop, me, mod, new, next, not, nothing, null, on, option, or, preserve, private, property, public, redim, rem, resume, select, set, stop, sub, then, to, true, until, wend, while, with, xor
End Class
Dim testObj
Set testObj = New ShimTestObj
' wine's C testObj answers 10 for any keyword-named property get
testObj.and = 10 : testObj.byref = 10 : testObj.byval = 10 : testObj.call = 10
testObj.case = 10 : testObj.class = 10 : testObj.const = 10 : testObj.dim = 10
testObj.do = 10 : testObj.each = 10 : testObj.else = 10 : testObj.elseif = 10
testObj.empty = 10 : testObj.end = 10 : testObj.eqv = 10 : testObj.exit = 10
testObj.false = 10 : testObj.for = 10 : testObj.function = 10 : testObj.get = 10
testObj.goto = 10 : testObj.if = 10 : testObj.imp = 10 : testObj.in = 10
testObj.is = 10 : testObj.let = 10 : testObj.loop = 10 : testObj.me = 10
testObj.mod = 10 : testObj.new = 10 : testObj.next = 10 : testObj.not = 10
testObj.nothing = 10 : testObj.null = 10 : testObj.on = 10 : testObj.option = 10
testObj.or = 10 : testObj.preserve = 10 : testObj.private = 10 : testObj.property = 10
testObj.public = 10 : testObj.redim = 10 : testObj.rem = 10 : testObj.resume = 10
testObj.select = 10 : testObj.set = 10 : testObj.stop = 10 : testObj.sub = 10
testObj.then = 10 : testObj.to = 10 : testObj.true = 10 : testObj.until = 10
testObj.wend = 10 : testObj.while = 10 : testObj.with = 10 : testObj.xor = 10

' wine's unkObj is an IUnknown; any object keeps `obj is unkObj` working
Dim unkObj
Set unkObj = New ShimTestObj

' wine's harness registers a global named "test" (the script global object);
' at global scope Me is that same object
Dim test
Set test = Me

Sub testDisp(o)
    ' wine's C harness exercises IDispatch on the object; nothing to check here
End Sub

Sub testArray(n, a)
    ' wine's harness checks the array has the expected number of dimensions
    Dim good, i
    good = IsArray(a)
    If good And n >= 0 Then
        For i = 1 To n
            On Error Resume Next
            good = good And (UBound(a, i) >= 0)
            On Error Goto 0
        Next
    End If
    Call ok(good, "testArray dims=" & n)
End Sub

Function isArrayFixed(a)
    ' a fixed-size array (Dim a(n)) rejects ReDim with error 10; probe it
    On Error Resume Next
    Err.Clear
    ReDim Preserve a(UBound(a))
    isArrayFixed = (Err.Number = 10)
    On Error Goto 0
End Function

Sub ok(cond, msg)
    g_no = g_no + 1
    Dim c
    If IsNull(cond) Then c = False Else c = CBool(cond)
    If c Then
        WScript.Echo "ok " & g_no & " - " & msg
    Else
        g_fail = g_fail + 1
        WScript.Echo "not ok " & g_no & " - " & msg
    End If
End Sub

Sub is(got, expected, msg)
    g_no = g_no + 1
    If CStr(got) = CStr(expected) Then
        WScript.Echo "ok " & g_no & " - " & msg
    Else
        g_fail = g_fail + 1
        WScript.Echo "not ok " & g_no & " - " & msg & " (got """ & CStr(got) & """, expected """ & CStr(expected) & """)"
    End If
End Sub

Sub trace(msg)
    ' wine only prints trace when tracing is enabled; keep it silent
End Sub

Function isNullDisp(v)
    ' wine's harness helper: true when v is a NULL IDispatch (Nothing)
    isNullDisp = (VarType(v) = 9)
End Function

Function getVT(v)
    Dim t
    t = VarType(v)
    If t >= 8192 Then
        getVT = "VT_ARRAY"
    Else
        Select Case t
            Case 0: getVT = "VT_EMPTY"
            Case 1: getVT = "VT_NULL"
            Case 2: getVT = "VT_I2"
            Case 3: getVT = "VT_I4"
            Case 4: getVT = "VT_R4"
            Case 5: getVT = "VT_R8"
            Case 6: getVT = "VT_CY"
            Case 7: getVT = "VT_DATE"
            Case 8: getVT = "VT_BSTR"
            Case 9: getVT = "VT_DISPATCH"
            Case 10: getVT = "VT_ERROR"
            Case 11: getVT = "VT_BOOL"
            Case 12: getVT = "VT_VARIANT"
            Case 13: getVT = "VT_UNKNOWN"
            Case 17: getVT = "VT_UI1"
            Case Else: getVT = "VT_" & t
        End Select
    End If
End Function


