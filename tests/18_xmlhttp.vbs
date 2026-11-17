' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.
Option Explicit

' Tests for MSXML2.XMLHTTP against the local test server (tests/httpserver.py on 127.0.0.1:18099)
Const BASE = "http://127.0.0.1:18099"

Dim x, body, i, s, doc, node

' --- basic GET ---
Set x = CreateObject("MSXML2.XMLHTTP")
x.Open "GET", BASE & "/ok", False
WScript.Echo "after-open=" & x.readyState
x.timeout = 5000
x.Send
WScript.Echo "readyState=" & x.readyState
WScript.Echo "status=" & x.status
WScript.Echo "statusText=" & x.statusText
WScript.Echo "text=" & x.responseText
WScript.Echo "custom=" & x.getResponseHeader("X-Custom")
WScript.Echo "missing-hdr=" & x.getResponseHeader("X-Nope")
WScript.Echo "has-custom=" & (InStr(x.getAllResponseHeaders, "X-Custom: yes") > 0)
body = x.responseBody
s = 0
For i = 0 To UBound(body)
    s = s + body(i)
Next
WScript.Echo "bodylen=" & (UBound(body) + 1)
WScript.Echo "sum=" & s

' --- POST with custom headers (MSXML2.XMLHTTP.6.0 progId) ---
Dim x2
Set x2 = CreateObject("MSXML2.XMLHTTP.6.0")
x2.Open "POST", BASE & "/echo", True ' async flag accepted, still synchronous
x2.setRequestHeader "Content-Type", "application/x-test"
x2.setRequestHeader "X-Test", "abc"
x2.Send "payload=1"
WScript.Echo "echo=" & x2.responseText

' --- 404 ---
Dim x3
Set x3 = CreateObject("Microsoft.XMLHTTP")
x3.Open "GET", BASE & "/missing", False
x3.Send
WScript.Echo "missing=" & x3.status & "|" & x3.responseText

' --- redirect ---
Dim x4
Set x4 = CreateObject("MSXML2.ServerXMLHTTP")
x4.Open "GET", BASE & "/redir", False
x4.Send
WScript.Echo "redir=" & x4.status & "|" & x4.responseText

' --- chunked transfer encoding ---
Dim x5
Set x5 = CreateObject("MSXML2.XMLHTTP")
x5.Open "GET", BASE & "/chunked", False
x5.Send
WScript.Echo "chunked=" & x5.responseText

' --- responseXML ---
Dim x6
Set x6 = CreateObject("MSXML2.XMLHTTP")
x6.Open "GET", BASE & "/xml", False
x6.Send
Set doc = x6.responseXML
WScript.Echo "xmlroot=" & doc.documentElement.nodeName
Set node = doc.selectSingleNode("//b")
WScript.Echo "xmlb=" & node.text

' --- charset: iso-8859-1 body decodes to the e-acute character ---
Dim x7
Set x7 = CreateObject("MSXML2.XMLHTTP")
x7.Open "GET", BASE & "/latin1", False
x7.Send
WScript.Echo "latin=" & x7.responseText & "|" & (x7.responseText = "caf" & ChrW(&H00E9))

' --- file:// via libfetch ---
Dim x8, fso
Set fso = CreateObject("Scripting.FileSystemObject")
Set x8 = CreateObject("MSXML2.XMLHTTP")
x8.Open "GET", "file://" & fso.GetAbsolutePathName(WScript.ScriptFullName), False
x8.Send
WScript.Echo "file=" & x8.status & "|" & (InStr(x8.responseText, "Option Explicit") > 0)

' --- abort ---
x8.Open "GET", BASE & "/ok", False
x8.abort
WScript.Echo "aborted=" & x8.readyState
