' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.

' ADODB on SQLite3: TINYINT, MONEY and BLOB columns map to Byte, Currency and
' Byte() values, and Byte, Currency and Single parameters bind as numbers.

Dim cn, rs
Set cn = CreateObject("ADODB.Connection")
cn.Open "Provider=SQLite3;Data Source=:memory:"
cn.Execute "CREATE TABLE t (b TINYINT, m MONEY, r REAL, x BLOB)"
cn.Execute "INSERT INTO t VALUES (200, 12.3456, 1.5, X'0102FF')"
Set rs = cn.Execute("SELECT b, m, r, x FROM t")
Dim f
For Each f In rs.Fields
    If f.Name <> "x" Then WScript.Echo f.Name & "=" & f.Value & " " & TypeName(f.Value) & " type=" & f.Type
Next
Dim blob : blob = rs("x")
WScript.Echo TypeName(blob) & " " & VarType(blob) & " " & UBound(blob) & " " & blob(2) & " " & TypeName(blob(2))
cn.Execute "CREATE TABLE u (v)"
Sub Ins(v)
    Dim cmd : Set cmd = CreateObject("ADODB.Command")
    cmd.ActiveConnection = cn
    cmd.CommandText = "INSERT INTO u VALUES (?)"
    cmd.CreateParameter "v", 12, 1, 0, v
    cmd.Execute
End Sub
Ins CCur(2.5)
Ins CCur(3)
Ins CByte(7)
Ins CSng(0.25)
Set rs = cn.Execute("SELECT v, typeof(v) FROM u")
Do Until rs.EOF
  WScript.Echo rs(0) & " " & rs(1)
  rs.MoveNext
Loop
