' arrays
Dim a(4)
a(0) = 10 : a(1) = 20 : a(2) = 30 : a(3) = 40 : a(4) = 50
WScript.Echo a(0), a(4), UBound(a), LBound(a)

Dim m(2, 3)
m(0, 0) = 1
m(2, 3) = 24
WScript.Echo m(0, 0), m(2, 3), UBound(m, 1), UBound(m, 2), LBound(m, 2)

Dim d()
ReDim d(2)
d(0) = "x" : d(1) = "y" : d(2) = "z"
WScript.Echo d(2), UBound(d)
ReDim Preserve d(4)
d(3) = "w" : d(4) = "v"
WScript.Echo d(0), d(2), d(3), d(4), UBound(d)

Dim sum
sum = 0
Dim i
For i = 0 To UBound(a)
    sum = sum + a(i)
Next
WScript.Echo "sum=" & sum

For Each i In a
    WScript.Echo "elem " & i
Next

' Erase zeroes fixed arrays
Erase a
WScript.Echo a(0), IsEmpty(a(0))

' Array() literal
Dim lit
lit = Array(1, "two", 3.5, True)
WScript.Echo lit(0), lit(1), lit(2), lit(3), TypeName(lit(3))

' Option Base 1
Option Base 1
Dim b(3)
WScript.Echo LBound(b), UBound(b)
b(1) = "first"
WScript.Echo b(1)
Option Base 0

' IsArray
WScript.Echo IsArray(a), IsArray(5), VarType(a) - vbArray

' multi-dim For Each order (row-major)
Dim mm(1, 1)
mm(0, 0) = "a" : mm(0, 1) = "b" : mm(1, 0) = "c" : mm(1, 1) = "d"
Dim acc
acc = ""
For Each i In mm
    acc = acc & i
Next
WScript.Echo acc
