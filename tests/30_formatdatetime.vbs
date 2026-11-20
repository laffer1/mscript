' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.

' FormatDateTime formats (en-US defaults); fixed dates only
Dim d
d = DateSerial(2024, 1, 5) + TimeSerial(13, 4, 9)
WScript.Echo vbGeneralDate, vbLongDate, vbShortDate, vbLongTime, vbShortTime
WScript.Echo FormatDateTime(d)
WScript.Echo FormatDateTime(d, vbGeneralDate)
WScript.Echo FormatDateTime(d, vbLongDate)
WScript.Echo FormatDateTime(d, vbShortDate)
WScript.Echo FormatDateTime(d, vbLongTime)
WScript.Echo FormatDateTime(d, vbShortTime)

' vbGeneralDate shows only the parts that are non-zero
WScript.Echo FormatDateTime(DateSerial(2024, 12, 31), vbGeneralDate)
WScript.Echo FormatDateTime(TimeSerial(9, 5, 0), vbGeneralDate)
WScript.Echo FormatDateTime(0, vbGeneralDate)

' vbShortTime is 24-hour hh:mm
WScript.Echo FormatDateTime(TimeSerial(9, 5, 0), vbShortTime)
WScript.Echo FormatDateTime(TimeSerial(0, 0, 0), vbShortTime)
WScript.Echo FormatDateTime(TimeSerial(23, 59, 59), vbShortTime)

' vbLongTime keeps the 12-hour clock
WScript.Echo FormatDateTime(TimeSerial(0, 0, 0), vbLongTime)
WScript.Echo FormatDateTime(TimeSerial(12, 30, 0), vbLongTime)

' strings are converted to dates first
WScript.Echo FormatDateTime("2024-03-15", vbLongDate)

' an invalid format is error 5, Null is error 94
On Error Resume Next
x = FormatDateTime(d, 5)
WScript.Echo "format 5: " & Err.Number
Err.Clear
x = FormatDateTime(d, -1)
WScript.Echo "format -1: " & Err.Number
Err.Clear
x = FormatDateTime(Null)
WScript.Echo "Null: " & Err.Number
Err.Clear
