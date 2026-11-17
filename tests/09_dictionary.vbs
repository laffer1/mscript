' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.

' Scripting.Dictionary
Dim d
Set d = CreateObject("Scripting.Dictionary")
d.Add "apple", 1
d.Add "banana", 2
d("cherry") = 3
WScript.Echo d.Count
WScript.Echo d("apple"), d("banana"), d("cherry")
WScript.Echo d.Exists("banana"), d.Exists("date")
d("apple") = 10
WScript.Echo d.Item("apple")
WScript.Echo Join(d.Keys, ",")
WScript.Echo Join(d.Items, ",")

Dim k
For Each k In d
    WScript.Echo k & "=" & d(k)
Next

d.Remove "banana"
WScript.Echo d.Count, d.Exists("banana")

d.RemoveAll
WScript.Echo d.Count

' compare mode
Dim d2
Set d2 = CreateObject("Scripting.Dictionary")
d2.CompareMode = 1
d2.Add "Key", "v"
WScript.Echo d2.Exists("KEY"), d2("key")
