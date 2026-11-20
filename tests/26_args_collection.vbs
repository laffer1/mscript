' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.

' WScript.Arguments is a collection (WshArguments), run with
' tests/26_args_collection.args
Dim args, a, i, nm
Set args = WScript.Arguments
WScript.Echo "count", args.Count, args.Count(), args.Length, VarType(args.Count)
WScript.Echo args(0), args.Item(1), WScript.Arguments(2), WScript.Arguments.Item(3)
For Each a In args
    WScript.Echo "each:", a
Next
For i = 0 To args.Count - 1
    WScript.Echo i, args(i)
Next

' Unnamed: arguments that don't start with /
WScript.Echo "unnamed", args.Unnamed.Count, args.Unnamed.Length, args.Unnamed(0), args.Unnamed.Item(1)
For Each a In args.Unnamed
    WScript.Echo "unnamed each:", a
Next

' Named: /name:value and bare /name; names ignore case
Set nm = args.Named
WScript.Echo "named", nm.Count, nm.Length
WScript.Echo nm("out"), nm.Item("OUT"), WScript.Arguments.Named("Out")
WScript.Echo nm.Exists("v"), nm.Exists("V"), IsEmpty(nm("v")), VarType(nm("v"))
WScript.Echo nm("mode"), "[" & nm("empty") & "]", VarType(nm("empty")), nm.Exists("empty")
WScript.Echo nm.Exists("missing"), IsEmpty(nm("missing"))
For Each a In nm
    WScript.Echo "named each:", a
Next

' errors
On Error Resume Next
a = args(args.Count)
WScript.Echo "past end:", Err.Number, Err.Description
Err.Clear
a = args(-1)
WScript.Echo "negative:", Err.Number
Err.Clear
a = args.Unnamed(2)
WScript.Echo "unnamed past end:", Err.Number
Err.Clear
a = args("x")
WScript.Echo "non-numeric:", Err.Number
Err.Clear
On Error Goto 0
