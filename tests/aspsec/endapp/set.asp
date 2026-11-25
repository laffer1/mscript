<% Session("name") = Request.QueryString("n") : If Request.QueryString("t") <> "" Then Session.Timeout = CLng(Request.QueryString("t")) %><%= Session.SessionID %>
