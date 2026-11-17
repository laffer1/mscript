' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.

' demo.vbs — a realistic mscript program: word-frequency report over a text file.
Option Explicit

Class Report
    Public Title
    Private counts
    Private total

    Private Sub Class_Initialize()
        Title = "report"
        Set counts = CreateObject("Scripting.Dictionary")
        total = 0
    End Sub

    Public Sub Add(word)
        word = LCase(Trim(word))
        If Len(word) = 0 Then Exit Sub
        If counts.Exists(word) Then
            counts(word) = counts(word) + 1
        Else
            counts.Add word, 1
        End If
        total = total + 1
    End Sub

    Public Function Total()
        Total = total
    End Function

    Public Sub Dump(topN)
        Dim keys, i, w, n
        keys = counts.Keys
        ' simple insertion sort by count desc (small N)
        For i = 0 To UBound(keys)
            For n = i + 1 To UBound(keys)
                If counts(keys(n)) > counts(keys(i)) Then
                    w = keys(i) : keys(i) = keys(n) : keys(n) = w
                End If
            Next
        Next
        WScript.Echo "== " & Title & " =="
        WScript.Echo "total words: " & total & ", distinct: " & counts.Count
        Dim lim
        lim = topN
        If lim > UBound(keys) Then lim = UBound(keys)
        For i = 0 To lim
            WScript.Echo Right(Space(8) & counts(keys(i)), 8) & "  " & keys(i)
        Next
    End Sub
End Class

Sub Main()
    Dim fso, path, ts, line, words, i, rep
    Set fso = CreateObject("Scripting.FileSystemObject")
    path = fso.BuildPath(fso.GetSpecialFolder(2).Path, "mscript_demo_input.txt")

    ' write a sample input file
    Set ts = fso.CreateTextFile(path, True)
    ts.WriteLine "the quick brown fox jumps over the lazy dog"
    ts.WriteLine "the dog barked at the fox, and the fox ran"
    ts.WriteLine "the end"
    ts.Close

    Set rep = New Report
    rep.Title = "word frequency"

    Set ts = fso.OpenTextFile(path, ForReading)
    Do While Not ts.AtEndOfStream
        line = ts.ReadLine
        words = Split(line, " ")
        For i = 0 To UBound(words)
            ' strip punctuation
            rep.Add Replace(Replace(words(i), ",", ""), ".", "")
        Next
    Loop
    ts.Close
    fso.DeleteFile path

    rep.Dump 5

    ' demonstrate error handling
    On Error Resume Next
    Dim bad
    bad = 1 / 0
    If Err.Number <> 0 Then WScript.Echo "caught error " & Err.Number & ": " & Err.Description
    On Error Goto 0

    ' demonstrate Eval
    WScript.Echo "eval: " & Eval("Sqr(144) + Len(""demo"")")
End Sub

Main
WScript.Echo "demo finished"
