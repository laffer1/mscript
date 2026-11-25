<%@ LANGUAGE=VBScript %>
<%
' ASP features used by real pages: combined Request(), collection Count/Item,
' Response header properties, Response.Cookies attributes, Contents, LCID
Response.CacheControl = "no-cache"
Response.Charset = "utf-8"
Response.PICS = "(PICS-1.1 mscript)"
Response.ExpiresAbsolute = CDate("2030-01-01 00:00:00")
Response.Cookies("theme") = "dark"
Response.Cookies("theme").Path = "/app"
Response.Cookies("theme").Expires = CDate("2031-06-01 12:00:00")
Response.Cookies("pref") = "x=1"
Response.Cookies("pref").HttpOnly = True
' sub-keys are a dictionary, never attributes (even when named like one)
Response.Cookies("user")("name") = "bob"
Response.Cookies("user")("domain") = "evil.example"
Response.Cookies("user")("name") = "bob b"
' setting only .Expires still emits the cookie (the delete idiom)
Response.Cookies("old").Expires = CDate("2000-01-01 00:00:00")
Response.Cookies("MixedCase") = "v"
Dim unused : unused = Response.Cookies("readOnly")   ' read must not emit Set-Cookie
On Error Resume Next
Response.IsClientConnected = False                    ' read-only: error 438
WScript.Echo "ro-err=" & Err.Number
Err.Clear
On Error Goto 0
Response.Write "req-name=" & Request("name") & vbCrLf
Response.Write "req-cookie=" & Request("theme") & vbCrLf
Response.Write "req-sv=" & Request("QUERY_STRING") & vbCrLf
Response.Write "qs-count=" & Request.QueryString.Count & vbCrLf
Response.Write "qs-item=" & Request.QueryString.Item("n") & vbCrLf
Response.Write "sv-item=" & Request.ServerVariables.Item("QUERY_STRING") & vbCrLf
Response.Write "sv-count>0=" & (Request.ServerVariables.Count > 0) & vbCrLf
Response.Write "cc=" & Response.CacheControl & vbCrLf   ' readable after a write synced it
Session.CodePage = 65001
Response.Write "cp-charset-ok" & vbCrLf
Session.CodePage = 1252
Session("user") = "alice"
Session.Contents("role") = "admin"
Response.Write "contents=" & Session.Contents.Count & ":" & Session.Contents.Item("role") & vbCrLf
Session.Contents.Remove "role"
Response.Write "after-remove=" & Session.Contents.Count & vbCrLf
Response.Write "lcid=" & Session.LCID & vbCrLf
Session.LCID = 2057
Response.Write "lcid2=" & GetLocale() & vbCrLf
Session.LCID = 1033
Response.Write "codepage=" & Session.CodePage & vbCrLf
Response.Write "connected=" & Response.IsClientConnected & vbCrLf
Response.Write "haskeys=" & Response.Cookies("user").HasKeys & ":" & Response.Cookies("theme").HasKeys & vbCrLf
Response.Write "subkey=" & Response.Cookies("user")("name") & vbCrLf
On Error Resume Next
Response.Cookies("a=b; Domain=evil") = "v"                ' invalid cookie name: error 5
Response.Write "badname-err=" & Err.Number & vbCrLf
Err.Clear
On Error Goto 0
%>
