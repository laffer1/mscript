' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.

' command-line arguments
Dim i
WScript.Echo "argc=" & WScript.Arguments.Count
For i = 0 To WScript.Arguments.Count - 1
    WScript.Echo "arg(" & i & ")=" & WScript.Arguments(i)
Next
WScript.Echo WScript.ScriptName
