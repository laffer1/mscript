<%
Dim n, names
Response.Write "secret=[" & Request.ServerVariables("SECRET_TOKEN") & "][" & Request("SECRET_TOKEN") & "]"
Response.Write "|statedb=[" & Request.ServerVariables("MScript_ASP_STATE_DB") & "]|path=[" & Request("PATH") & "]"
Response.Write "|hdr=" & Request.ServerVariables("HTTP_X_TEST") & ":" & Request.ServerVariables("http_x_test")
Response.Write "|auth=" & Request.ServerVariables("AUTH_USER") & "|url=" & Request.ServerVariables("URL")
Response.Write "|secure=" & Request.ServerVariables("SERVER_PORT_SECURE")
Response.Write "|raw=" & Replace(Request.ServerVariables("ALL_RAW"), vbCrLf, ";")
For Each n In Request.ServerVariables : names = names & n & "," : Next
Response.Write "|names=" & names
%>
