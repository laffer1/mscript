<%
Response.Write "x"
Response.Flush
On Error Resume Next
Response.AddHeader "X-Late", "1"
Response.Write "|add=" & Hex(Err.Number) : Err.Clear
Response.Cookies("late") = "1"
Response.Write "|cookie=" & Hex(Err.Number) : Err.Clear
Response.Status = "404 Not Found"
Response.Write "|status=" & Hex(Err.Number) : Err.Clear
Response.Redirect "/elsewhere"
Response.Write "|redirect=" & Hex(Err.Number) : Err.Clear
%>
