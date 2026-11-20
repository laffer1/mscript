' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.

' OpenTextFile iomode and create flag: ForAppending adds to the end of the
' file across several opens, ForWriting truncates, ForReading reads.
Const ForReading = 1, ForWriting = 2, ForAppending = 8
Dim fso, path, ts
Set fso = CreateObject("Scripting.FileSystemObject")
path = fso.BuildPath(fso.GetSpecialFolder(2).Path, "mscript_append_test.txt")
If fso.FileExists(path) Then fso.DeleteFile path

' create defaults to False for every mode
On Error Resume Next
Set ts = fso.OpenTextFile(path, ForAppending)
WScript.Echo "append, no create: " & Err.Number & " exists=" & fso.FileExists(path)
Err.Clear
Set ts = fso.OpenTextFile(path, ForWriting)
WScript.Echo "write, no create: " & Err.Number & " exists=" & fso.FileExists(path)
Err.Clear
Set ts = fso.OpenTextFile(path, ForReading)
WScript.Echo "read, no create: " & Err.Number
Err.Clear
On Error GoTo 0

Set ts = fso.OpenTextFile(path, ForAppending, True)
ts.WriteLine "one"
ts.Close
Set ts = fso.OpenTextFile(path, ForAppending)
ts.Write "two"
ts.WriteLine
ts.Close
Set ts = fso.OpenTextFile(path, ForAppending, False)
ts.WriteLine "three"
ts.WriteBlankLines 1
ts.Write "four"
ts.Close
WScript.Echo "size after appends: " & fso.GetFile(path).Size
Set ts = fso.OpenTextFile(path, ForReading)
Do Until ts.AtEndOfStream
    WScript.Echo "[" & ts.ReadLine & "]"
Loop
ts.Close

' a stream released without Close still reaches the file
Set ts = fso.OpenTextFile(path, ForAppending)
ts.WriteLine
ts.Write "five"
Set ts = Nothing
WScript.Echo "after release: " & Replace(fso.OpenTextFile(path).ReadAll, vbLf, "|")

' ForWriting truncates
Set ts = fso.OpenTextFile(path, ForWriting)
ts.WriteLine "fresh"
ts.Close
WScript.Echo "after ForWriting: " & Replace(fso.OpenTextFile(path, ForReading).ReadAll, vbLf, "|")

' ForReading with create=True makes an empty file
fso.DeleteFile path
Set ts = fso.OpenTextFile(path, ForReading, True)
WScript.Echo "read, create: " & ts.AtEndOfStream & " exists=" & fso.FileExists(path)
ts.Close
fso.DeleteFile path
