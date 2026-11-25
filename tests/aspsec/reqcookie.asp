<%
Dim c, k, keys
Set c = Request.Cookies("user")
Response.Write "name=" & c("name") & "|lang=" & Request.Cookies("user")("lang")
Response.Write "|haskeys=" & c.HasKeys & ":" & Request.Cookies("theme").HasKeys
Response.Write "|count=" & c.Count
Response.Write "|whole=" & Request.Cookies("user")
Response.Write "|theme=" & Request.Cookies("theme") & "|eq=" & (Request.Cookies("theme") = "dark=x")
Response.Write "|plain=" & Request.Cookies("plain") & "|missing=[" & Request.Cookies("nope") & "]"
Response.Write "|req=" & Request("theme")
For Each k In c : keys = keys & k & "," : Next
Response.Write "|keys=" & keys
%>
