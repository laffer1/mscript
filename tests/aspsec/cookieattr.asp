<% On Error Resume Next : Response.Buffer = False : Response.Cookies("a").Path = Request.QueryString("p") : Response.Write "path-err=" & Err.Number & "|" & Request.QueryString("x") %>
