' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.
Option Explicit

' Tests for WinHttp.WinHttpRequest.5.1 against tests/httpserver.py (127.0.0.1:18099)
Const BASE = "http://127.0.0.1:18099"

Dim w, b

' --- basic GET, Option property, WaitForResponse ---
Set w = CreateObject("WinHttp.WinHttpRequest.5.1")
WScript.Echo "default-ua=" & w.Option(0)
WScript.Echo "default-redir=" & w.Option(6)
w.Option(0) = "mscript-test/1.0"
WScript.Echo "ua-set=" & w.Option(0)
w.SetTimeouts 5000, 5000, 5000, 5000
w.Open "GET", BASE & "/ok", False
w.Send
WScript.Echo "wait=" & w.WaitForResponse(5)
WScript.Echo "readyState=" & w.readyState
WScript.Echo "status=" & w.Status & " " & w.StatusText
WScript.Echo "text=" & w.ResponseText
WScript.Echo "hdr=" & w.GetResponseHeader("X-Custom")
WScript.Echo "url-opt=" & w.Option(1)
b = w.ResponseBody
WScript.Echo "bodylen=" & (UBound(b) + 1)
WScript.Echo "has-hdr=" & (InStr(w.GetAllResponseHeaders, "X-Custom: yes") > 0)

' --- POST with custom headers + custom user agent ---
Set w = CreateObject("WinHttp.WinHttpRequest.5.1")
w.Option(0) = "agent-x"
w.Open "POST", BASE & "/echo", False
w.SetRequestHeader "Content-Type", "application/x-test"
w.SetRequestHeader "X-Test", "zz"
w.Send "hello=1"
WScript.Echo "echo=" & w.ResponseText

' --- redirects: default on, Option(6)=False disables ---
Set w = CreateObject("WinHttp.WinHttpRequest.5.1")
w.Open "GET", BASE & "/redir", False
w.Send
WScript.Echo "redir=" & w.Status & "|" & w.ResponseText
Set w = CreateObject("WinHttp.WinHttpRequest.5.1")
w.Option(6) = False
w.Open "GET", BASE & "/redir", False
w.Send
WScript.Echo "noredir=" & w.Status

' --- 404 ---
Set w = CreateObject("WinHttp.WinHttpRequest.5.1")
w.Open "GET", BASE & "/missing", False
w.Send
WScript.Echo "missing=" & w.Status & "|" & w.ResponseText

' --- SetCredentials / SetProxy / SetClientCertificate accepted ---
w.SetCredentials "u", "p", 0
w.SetProxy 1, "", ""
w.SetClientCertificate "cert"
WScript.Echo "noop-ok"

' --- abort ---
w.Open "GET", BASE & "/ok", False
w.Abort
WScript.Echo "aborted=" & w.readyState
