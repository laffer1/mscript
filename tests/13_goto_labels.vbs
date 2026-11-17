' labels, GoTo, On Error Goto <label>
Dim i
i = 0
Again:
i = i + 1
If i < 3 Then GoTo Again
WScript.Echo "goto loop i=" & i

On Error GoTo ErrHandler
Dim x
x = 1 / 0
WScript.Echo "not reached"
GoTo Done
ErrHandler:
WScript.Echo "caught " & Err.Number
Done:
WScript.Echo "done"

' numeric line labels
GoTo 100
WScript.Echo "skipped"
100
WScript.Echo "at 100"
