' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.
Option Explicit

' Tests for VBScript.RegExp (libc POSIX regex backend)
Dim r, r2, ms, m, t

Set r = CreateObject("VBScript.RegExp")
WScript.Echo "defaults=" & r.Pattern & "|" & r.Global & "|" & r.IgnoreCase & "|" & r.Multiline

' Test + Execute with groups, Global
r.Pattern = "(\d+)-(\w+)"
r.Global = True
WScript.Echo "test=" & r.Test("abc 12-xy zz 34-q")
WScript.Echo "noglobal-test=" & r.Test("no digits here")
Set ms = r.Execute("abc 12-xy zz 34-q")
WScript.Echo "count=" & ms.Count
For Each m In ms
    WScript.Echo "m=" & m.Value & " @" & m.FirstIndex & " len=" & m.Length & _
        " subs=" & m.SubMatches.Count & " s0=" & m.SubMatches(0) & " s1=" & m.SubMatches(1)
Next
WScript.Echo "item1=" & ms.Item(1).Value
WScript.Echo "item-missing-isnothing=" & (ms.Item(9) Is Nothing)

' non-participating group -> Empty submatch
r.Pattern = "(a)(b)?"
Set ms = r.Execute("xa")
WScript.Echo "empty-sub=" & (ms.Item(0).SubMatches(1) = Empty) & "|" & ms.Item(0).SubMatches(0)

' Replace: first only vs Global, $&, $$, $`, $', $n
r.Pattern = "a+"
WScript.Echo "rep1=" & r.Replace("baa baa!", "X")
r.Global = True
WScript.Echo "repg=" & r.Replace("baa baa!", "[$&]")
Set r2 = CreateObject("VBScript.RegExp")
r2.Pattern = "(\w+)@(\w+)"
WScript.Echo "repl-groups=" & r2.Replace("mail a@b end", "<$2@$1>")
r2.Pattern = "b"
WScript.Echo "repl-dollar=" & r2.Replace("abc", "[$`|$&|$'']")
r2.Pattern = "(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)"
WScript.Echo "repl-2digit=" & r2.Replace("abcdefghijk", "<$11|$$|$9>")

' character classes \d \w \s and escapes
r2.Pattern = "^\s*\S+$"
WScript.Echo "s-class=" & r2.Test("  abc") & "|" & r2.Test(" ")
r2.Pattern = "[\d]+"
WScript.Echo "class-d=" & r2.Test("a99") & "|" & r2.Test("abc")
r2.Pattern = "\d{2,3}"
WScript.Echo "d-range=" & r2.Test("x1234")
r2.Pattern = "x(?:y)z(\d)"
WScript.Echo "noncap-shift=" & r2.Replace("xyz1!", "[$1]")

' dot never matches newline; Multiline anchors
r2.Pattern = "."
WScript.Echo "dot-nl=" & r2.Test(vbLf)
r2.Multiline = True
r2.Pattern = "^.$"
WScript.Echo "ml=" & r2.Test("a" & vbLf & "b")
r2.Multiline = False
WScript.Echo "noml=" & r2.Test("a" & vbLf & "b")

' IgnoreCase
r2.IgnoreCase = True
r2.Pattern = "abc"
WScript.Echo "icase=" & r2.Test("ABC")
r2.IgnoreCase = False
WScript.Echo "case=" & r2.Test("ABC")

' empty global matches
r2.Global = True
r2.Pattern = "x*"
Set ms = r2.Execute("abc")
WScript.Echo "empty-count=" & ms.Count

' unsupported constructs raise error 5 with a clear message
On Error Resume Next
r2.Pattern = "\bword"
t = r2.Test("word")
WScript.Echo "err-b=" & Err.Number & " " & Err.Description
Err.Clear
r2.Pattern = "(a)\1"
t = r2.Test("aa")
WScript.Echo "err-backref=" & Err.Number
Err.Clear
r2.Pattern = "a*?"
t = r2.Test("aaa")
WScript.Echo "err-nongreedy=" & Err.Number
Err.Clear
r2.Pattern = "(?!x)y"
t = r2.Test("y")
WScript.Echo "err-look=" & Err.Number
On Error Goto 0
