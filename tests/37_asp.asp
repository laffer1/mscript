<%@ LANGUAGE=VBScript %>
<html><body>
<%
Dim name
name = Request.QueryString("name")
Session("visits") = Session("visits") + 1
Application("hits") = Application("hits") + 1
%>
<h1>Hello, <%= Server.HTMLEncode(name) %>!</h1>
<p>n = <%= Request.QueryString("n") %></p>
<p>visits = <%= Session("visits") %>, hits = <%= Application("hits") %></p>
<p>encoded = <%= Server.URLEncode("a b&c=") %></p>
<% Response.Write "<p>direct write</p>" %>
</body></html>
