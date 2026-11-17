' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.

' FileSystemObject
Dim fso
Set fso = CreateObject("Scripting.FileSystemObject")

Dim path
path = fso.BuildPath(fso.GetSpecialFolder(2).Path, "mscript_test.txt")
WScript.Echo path

WScript.Echo "exists before: " & fso.FileExists(path)

Dim ts
Set ts = fso.CreateTextFile(path, True)
ts.WriteLine "line one"
ts.Write "part "
ts.WriteLine "two"
ts.WriteBlankLines 1
ts.WriteLine "line three"
ts.Close

WScript.Echo "exists after: " & fso.FileExists(path)

Set ts = fso.OpenTextFile(path, ForReading)
WScript.Echo "atend before: " & ts.AtEndOfStream
WScript.Echo ts.ReadLine
WScript.Echo ts.ReadLine
WScript.Echo "line: " & ts.Line
WScript.Echo ts.ReadAll
WScript.Echo "atend after: " & ts.AtEndOfStream
ts.Close

Dim f
Set f = fso.GetFile(path)
WScript.Echo f.Name, f.Size > 0, f.Type
WScript.Echo fso.GetFileName(path), fso.GetBaseName(path), fso.GetExtensionName(path)
WScript.Echo fso.GetParentFolderName(path) <> ""

fso.DeleteFile path
WScript.Echo "exists deleted: " & fso.FileExists(path)

' OpenTextFile append
Set ts = fso.OpenTextFile(path, ForAppending, True)
ts.WriteLine "appended"
ts.Close
Set ts = fso.OpenTextFile(path, ForReading)
WScript.Echo ts.ReadAll
ts.Close
fso.DeleteFile path

' folder ops
Dim tmpd
tmpd = fso.BuildPath(fso.GetSpecialFolder(2).Path, "mscript_dir")
fso.CreateFolder tmpd
WScript.Echo fso.FolderExists(tmpd)
fso.DeleteFolder tmpd
WScript.Echo fso.FolderExists(tmpd)

' WScript.Shell
Dim sh
Set sh = CreateObject("WScript.Shell")
WScript.Echo sh.ExpandEnvironmentStrings("%HOME%") <> ""
