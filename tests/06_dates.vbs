' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.

' dates (fixed dates only, deterministic)
Dim d
d = DateSerial(2024, 2, 29)
WScript.Echo d
WScript.Echo Year(d), Month(d), Day(d), Weekday(d)
WScript.Echo MonthName(Month(d)), WeekdayName(Weekday(d))
WScript.Echo DateAdd("d", 1, d), DateAdd("m", 1, DateSerial(2024, 1, 31))
WScript.Echo DateAdd("yyyy", 1, d)
WScript.Echo DateDiff("d", DateSerial(2024, 1, 1), DateSerial(2024, 12, 31))
WScript.Echo DateDiff("m", DateSerial(2024, 1, 1), DateSerial(2024, 12, 31))
WScript.Echo DateDiff("yyyy", DateSerial(2020, 6, 15), DateSerial(2024, 6, 15))
WScript.Echo DatePart("q", d), DatePart("y", d), DatePart("w", d)
WScript.Echo DateSerial(2024, 13, 1)
WScript.Echo TimeSerial(13, 30, 15)
WScript.Echo Hour(TimeSerial(13, 30, 15)), Minute(TimeSerial(13, 30, 15)), Second(TimeSerial(13, 30, 15))
WScript.Echo DateValue("2024-03-15"), TimeValue("2:30 PM")
WScript.Echo CDate("3/15/2024"), IsDate("2024-03-15"), IsDate("not a date"), IsDate(42)
WScript.Echo FormatDateTime(DateSerial(2024, 3, 15), 2)
WScript.Echo FormatDateTime(DateSerial(2024, 3, 15) + TimeSerial(14, 5, 9), 0)
WScript.Echo Weekday(DateSerial(2024, 1, 1)), WeekdayName(1)
WScript.Echo Timer > 0
WScript.Echo IsDate(Now), IsDate(Date), IsDate(Time)
