' Eval / Execute / ExecuteGlobal
WScript.Echo Eval("1 + 2 * 3")
WScript.Echo Eval("""hello"" & "" world""")

Execute "Dim exVar" & vbCrLf & "exVar = 41 + 1"
WScript.Echo exVar

ExecuteGlobal "Dim gVar" & vbCrLf & "gVar = 7"
Sub UseG()
    WScript.Echo "in sub gVar=" & gVar
End Sub
UseG

Execute "WScript.Echo ""from execute"""

Dim code
code = "Function ExecFn(n)" & vbCrLf & "  ExecFn = n * 10" & vbCrLf & "End Function"
ExecuteGlobal code
WScript.Echo ExecFn(4)
