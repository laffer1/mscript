' classes
Class Point
    Public x, y

    Private Sub Class_Initialize()
    End Sub

    Public Sub Initialize(px, py)
        x = px
        y = py
    End Sub

    Public Function Sum()
        Sum = x + y
    End Function

    Public Property Get Label
        Label = "(" & x & "," & y & ")"
    End Property

    Public Property Let Label(v)
        ' ignore
    End Property

    Public Function Moved(dx, dy)
        Moved = x + dx + y + dy
    End Function
End Class

Dim p
Set p = New Point
p.Initialize 3, 4
WScript.Echo p.x, p.y, p.Sum(), p.Label

Dim p2
Set p2 = New Point
p2.x = 10
p2.y = 20
WScript.Echo p2.Sum(), p2.Label, p2.Moved(1, 2)

' With block
With p2
    .x = 100
    WScript.Echo .Sum()
End With
WScript.Echo p2.x

' Me reference
Class Counter
    Public n
    Public Sub Inc()
        n = n + 1
    End Sub
    Public Function GetN()
        GetN = Me.n
    End Function
End Class
Dim c
Set c = New Counter
c.Inc
c.Inc
c.Inc
WScript.Echo c.n, c.GetN()

' class with default init via Initialize event
Class Greeter
    Public name
    Private Sub Class_Initialize()
        name = "anon"
    End Sub
    Public Function Greet()
        Greet = "hi " & name
    End Function
End Class
Dim g
Set g = New Greeter
WScript.Echo g.Greet()
g.name = "bob"
WScript.Echo g.Greet()

' Is / Nothing
Dim obj
WScript.Echo IsObject(p), IsObject(obj)
WScript.Echo obj Is Nothing
Set obj = p
WScript.Echo obj Is p, obj Is Nothing
