<% b = Request.BinaryRead(Request.TotalBytes) : Response.Write TypeName(b) & ":" & LenB(b) & ":" & AscB(MidB(b, 2, 1)) & ":" & InStrB(b, ChrB(255)) %>
