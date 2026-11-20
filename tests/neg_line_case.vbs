' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.

' an error in a Case expression reports the Case line, inside a class method
Class K
    Sub Run(n)
        Select Case 1
            Case 2
            Case 1 / n
        End Select
    End Sub
End Class
Dim k
Set k = New K
k.Run 0
