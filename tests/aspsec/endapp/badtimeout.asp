<% On Error Resume Next : Session.Timeout = 0 : Response.Write "t0=" & Err.Number : Err.Clear : Session.Timeout = 1441 : Response.Write "|t1441=" & Err.Number %>
