' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.
Option Explicit

' Tests for ADODB.Connection / Recordset / Command on SQLite3 (base libsqlite3)
Dim cn, rs, f, g, cmd, p, fso

Set fso = CreateObject("Scripting.FileSystemObject")
On Error Resume Next
fso.DeleteFile "tests/21_adodb.db", True
On Error Goto 0

' --- connection open / execute DDL / empty recordset ---
Set cn = CreateObject("ADODB.Connection")
cn.Open "Provider=SQLite3;Data Source=:memory:"
WScript.Echo "state=" & cn.State & " provider=" & cn.Provider & " ver-prefix=" & (Left(cn.Version, 7) = "SQLite ")
cn.Execute "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, score REAL, active BOOLEAN)"
Set rs = cn.Execute("SELECT * FROM t")
WScript.Echo "empty-eof=" & rs.EOF & " count=" & rs.RecordCount & " state=" & rs.State

' --- AddNew / Update ---
rs.AddNew
rs("name") = "alice"
rs("score") = 9.5
rs("active") = True
rs.Update
rs.AddNew
rs("name") = "bob"
rs("score") = 3.25
rs("active") = False
rs.Update

' --- navigation ---
Set rs = cn.Execute("SELECT id, name, score, active FROM t ORDER BY id")
WScript.Echo "count=" & rs.RecordCount & " bof=" & rs.BOF & " eof=" & rs.EOF
Do While Not rs.EOF
    WScript.Echo rs("id") & "|" & rs("name") & "|" & rs("score") & "|" & rs("active")
    rs.MoveNext
Loop
WScript.Echo "eof=" & rs.EOF
rs.MoveLast
WScript.Echo "last=" & rs("name") & " abspos=" & rs.AbsolutePosition
rs.MovePrevious
WScript.Echo "prev=" & rs("name")
rs.MoveFirst
WScript.Echo "first=" & rs("name") & " bof=" & rs.BOF
rs.Move 1
WScript.Echo "move1=" & rs("name")
rs.AbsolutePosition = 1
WScript.Echo "abs1=" & rs("name")

' --- fields ---
WScript.Echo "fields=" & rs.Fields.Count
For Each f In rs.Fields
    WScript.Echo "  f=" & f.Name & " type=" & f.Type & " val=" & f.Value
Next
WScript.Echo "item-name=" & rs.Fields.Item("name").Value & " item-idx=" & rs.Fields.Item(1).Name

' --- GetRows: (field, record) ---
g = rs.GetRows
WScript.Echo "g=" & UBound(g, 1) & "x" & UBound(g, 2) & " g(1,0)=" & g(1, 0) & " g(1,1)=" & g(1, 1)

' --- update existing row ---
rs.MoveFirst
rs("score") = 1.25
rs.Update
Set rs = cn.Execute("SELECT score FROM t WHERE id=1")
WScript.Echo "upd=" & rs("score")

' --- delete ---
Set rs = cn.Execute("SELECT * FROM t WHERE id=2")
rs.Delete
WScript.Echo "after-del=" & cn.Execute("SELECT COUNT(*) FROM t")(0)

' --- transactions (savepoint nesting) ---
cn.BeginTrans
cn.Execute "INSERT INTO t (name) VALUES ('tx')"
cn.RollbackTrans
WScript.Echo "after-rb=" & cn.Execute("SELECT COUNT(*) FROM t")(0)
cn.BeginTrans
cn.Execute "INSERT INTO t (name) VALUES ('tx')"
cn.CommitTrans
WScript.Echo "after-commit=" & cn.Execute("SELECT COUNT(*) FROM t")(0)

' --- command with positional and named parameters ---
Set cmd = CreateObject("ADODB.Command")
cmd.ActiveConnection = cn
cmd.CommandText = "SELECT name FROM t WHERE id = ?"
cmd.CommandType = adCmdText
Set p = cmd.CreateParameter("id", adInteger, adParamInput, 0, 1)
Set rs = cmd.Execute
WScript.Echo "cmd=" & rs("name") & " params=" & cmd.Parameters.Count
cmd.CommandText = "SELECT name FROM t WHERE name = :nm"
cmd.Parameters.Delete 0
cmd.CreateParameter "nm", adVarChar, adParamInput, 0, "tx"
Set rs = cmd.Execute
WScript.Echo "cmd-named=" & rs("name")
cmd.CommandType = adCmdTable
cmd.CommandText = "t"
Set rs = cmd.Execute
WScript.Echo "cmd-table=" & rs.RecordCount

' --- file-backed database: persist, close, reopen ---
Set cn = CreateObject("ADODB.Connection")
cn.Open "Data Source=tests/21_adodb.db"
cn.Execute "CREATE TABLE f (k TEXT PRIMARY KEY, v INTEGER)"
cn.Execute "INSERT INTO f (k, v) VALUES ('one', 1)"
cn.Close
WScript.Echo "closed-state=" & cn.State
Set cn = CreateObject("ADODB.Connection")
cn.Open "Provider=SQLite3;Data Source=tests/21_adodb.db"
Set rs = cn.Execute("SELECT k, v FROM f")
WScript.Echo "file=" & rs("k") & "=" & rs("v") & " count=" & rs.RecordCount
cn.Close
fso.DeleteFile "tests/21_adodb.db", True

' --- read-only mode on a missing file errors ---
Set cn = CreateObject("ADODB.Connection")
cn.Mode = adModeRead
On Error Resume Next
cn.Open "Data Source=tests/21_does_not_exist.db"
WScript.Echo "ro-missing-err=" & Err.Number
On Error Goto 0
