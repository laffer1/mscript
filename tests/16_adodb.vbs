' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.

' ADODB.Stream — in-memory text stream
Dim stm
Set stm = CreateObject("ADODB.Stream")
WScript.Echo stm.State          ' adStateClosed = 0
stm.Type = adTypeText
stm.Mode = adModeReadWrite
stm.Charset = "utf-8"
stm.Open
WScript.Echo stm.State, stm.EOS, stm.Size

stm.WriteText "line one" & vbCrLf & "line two" & vbCrLf & "line three"
WScript.Echo "size=" & stm.Size, "eos=" & stm.EOS

stm.Position = 0
WScript.Echo stm.ReadText(4)    ' "line"
WScript.Echo stm.ReadText(adReadLine)
WScript.Echo stm.ReadText(adReadLine)
WScript.Echo stm.ReadText(adReadAll)
WScript.Echo "eos=" & stm.EOS

' full rewind and read everything
stm.Position = 0
WScript.Echo "[" & stm.ReadText & "]"
stm.Close

' binary stream: write bytes, read them back as an array
Dim bin
Set bin = CreateObject("ADODB.Stream")
bin.Type = adTypeBinary
bin.Open
Dim bytes
bytes = Array(72, 101, 108, 108, 111)   ' "Hello"
bin.Write bytes
WScript.Echo "binsize=" & bin.Size
bin.Position = 0
Dim back
back = bin.Read
WScript.Echo UBound(back), back(0), back(4), Chr(back(0)) & Chr(back(1))
bin.Position = 0
WScript.Echo bin.Read(2)(0)     ' index into the returned array directly
bin.Position = 0
WScript.Echo UBound(bin.Read(2)) + 1
bin.Close

' SaveToFile / LoadFromFile round trip (fresh stream)
Dim fso
Set fso = CreateObject("Scripting.FileSystemObject")
Dim path
path = fso.BuildPath(fso.GetSpecialFolder(2).Path, "mscript_adodb.txt")

Dim stmS
Set stmS = CreateObject("ADODB.Stream")
stmS.Type = adTypeText
stmS.Open
stmS.WriteText "saved content" & vbCrLf & "second line"
stmS.SaveToFile path, adSaveCreateOverWrite
stmS.Close
WScript.Echo "file exists: " & fso.FileExists(path)

Dim stm2
Set stm2 = CreateObject("ADODB.Stream")
stm2.Type = adTypeText
stm2.LoadFromFile path
WScript.Echo "loaded size=" & stm2.Size
WScript.Echo stm2.ReadText
stm2.Close
fso.DeleteFile path

' CopyTo and SetEOS
Dim a, b
Set a = CreateObject("ADODB.Stream")
Set b = CreateObject("ADODB.Stream")
a.Type = adTypeText : a.Open
b.Type = adTypeText : b.Open
a.WriteText "abcdef"
a.Position = 2
b.WriteText "XY"
b.Position = 1
a.CopyTo b          ' overwrites at b's pos 1 with "cdef" -> "Xcdef"
b.Position = 0
WScript.Echo "[" & b.ReadText & "]"   ' Xcdef
a.Position = 3
a.SetEOS            ' truncate a at pos 3 -> "abc"
a.Position = 0
WScript.Echo "[" & a.ReadText & "]"
a.Close : b.Close
