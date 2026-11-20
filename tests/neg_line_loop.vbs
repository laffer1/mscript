' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.

' an error in Loop Until reports the Loop line
Dim i, z
z = 0
Do
    i = i + 1
Loop Until i / z > 1
