<%@ LANGUAGE=VBScript %>
<%
' ASP features used by real pages: combined Request(), collection Count/Item,
' Response header properties, Response.Cookies attributes, Contents, LCID
Response.CacheControl = "no-cache"
Response.Charset = "utf-8"
Response.PICS = "(PICS-1.1 mscript)"
Response.ExpiresAbsolute = CDate("2030-01-01 00:00:00")
Response.Cookies("theme") = "dark"
Response.Cookies("theme")("path") = "/app"
Response.Cookies("theme")("expires") = CDate("2031-06-01 12:00:00")
Response.Cookies("pref") = "x=1"
Response.Cookies("pref")("httponly") = True
Response.Write "req-name=" & Request("name") & vbCrLf
Response.Write "req-cookie=" & Request("theme") & vbCrLf
Response.Write "req-sv=" & Request("QUERY_STRING") & vbCrLf
Response.Write "qs-count=" & Request.QueryString.Count & vbCrLf
Response.Write "qs-item=" & Request.QueryString.Item("n") & vbCrLf
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
%>
