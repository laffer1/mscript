<% On Error Resume Next : x = Request.Form("k1") : Response.Write "form-err=" & Err.Number %>[<%= Request.Form.Count %>]
