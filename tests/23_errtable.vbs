' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.

' Error descriptions and HRESULT -> Err.Number translation (src/errtable.cpp)
On Error Resume Next
Dim n
For Each n In Array(5, 6, 9, 11, 13, 53, 70, 76, 424, 429, 438, 445, 450, 500, 5017, 999)
  Err.Clear
  Err.Raise n
  WScript.Echo Err.Number & ": " & Err.Description
Next
For Each n In Array(&H800A000D, &H800A01B6, &H80004001, &H80020005, &H8002000B, _
                    &H8002000E, &H80030002, &H80040154, &H80070005, &H80070057, _
                    &H80004005)
  Err.Clear
  Err.Raise n
  WScript.Echo Hex(n) & " -> " & Err.Number & ": " & Err.Description
Next
