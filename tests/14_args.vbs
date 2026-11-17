' command-line arguments
Dim i
WScript.Echo "argc=" & (UBound(WScript.Arguments) + 1)
For i = 0 To UBound(WScript.Arguments)
    WScript.Echo "arg(" & i & ")=" & WScript.Arguments(i)
Next
WScript.Echo WScript.ScriptName
