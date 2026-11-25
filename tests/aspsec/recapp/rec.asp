<% On Error Resume Next : Server.Execute "rec.asp" : If Err.Number <> 0 Then Response.Write "depth-err=" & Err.Number : Err.Clear %>
