' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.

' Currency, Byte and Single subtypes: conversions, VarType/TypeName,
' formatting, arithmetic promotion, comparisons and built-in functions.

Sub T(label, v)
    WScript.Echo label & " = " & CStr(v) & " " & TypeName(v)
End Sub

Sub E(label, expr)
    On Error Resume Next
    Dim r : r = Eval(expr)
    WScript.Echo label & " err " & Err.Number
    Err.Clear
End Sub

WScript.Echo "--- constants"
WScript.Echo vbSingle, vbCurrency, vbByte, VarType(CSng(1)), VarType(CCur(1)), VarType(CByte(1))
WScript.Echo IsNumeric(CSng(1)), IsNumeric(CCur(1)), IsNumeric(CByte(1))

WScript.Echo "--- currency"
T "CCur(1.23456)", CCur(1.23456)
T "CCur(2.5)", CCur(2.5)
T "CCur(""0.00005"")", CCur("0.00005")
T "CCur(""0.00015"")", CCur("0.00015")
T "CCur(""-0.00025"")", CCur("-0.00025")
T "CCur(-0.5)", CCur(-0.5)
T "CCur(100)", CCur(100)
T "CCur(True)", CCur(True)
T "max", CCur("922337203685477.5807")
T "min", CCur("-922337203685477.5808")
E "CCur(""922337203685477.5808"")", "CCur(""922337203685477.5808"")"
E "CCur(1E+16)", "CCur(1E+16)"
E "CCur(""abc"")", "CCur(""abc"")"
E "CCur(Null)", "CCur(Null)"
T "CCur(1.5) + 2", CCur(1.5) + 2
T "CCur(1.5) + 2.5", CCur(1.5) + 2.5
T "CCur(1.5) - CLng(1)", CCur(1.5) - CLng(1)
T "CCur(1.5) * 2", CCur(1.5) * 2
T "CCur(1.5) * 2.5", CCur(1.5) * 2.5
T "CCur(1.0001) * CCur(1.0001)", CCur(1.0001) * CCur(1.0001)
T "CCur(10) / 4", CCur(10) / 4
T "CCur(10) \ 3", CCur(10) \ 3
T "CCur(10) Mod 3", CCur(10) Mod 3
T "-CCur(5.5)", -CCur(5.5)
T "(CCur(1) + date) - date", (CCur(1) + #1/1/2000#) - #1/1/2000#
E "max + 1", "CCur(""922337203685477.5807"") + 1"
E "max * 2", "CCur(""922337203685477.5807"") * 2"
WScript.Echo CCur(0.1) + CCur(0.2) = CCur(0.3), CCur(5) = 5, CCur(0.5) < 1, CCur("0.0001") > 0
WScript.Echo CInt(CCur(2.5)), CLng(CCur(3.5)), CByte(CCur(254.5)), CDbl(CCur(1.25))
T "Int(CCur(-2.5))", Int(CCur(-2.5))
T "Fix(CCur(-2.5))", Fix(CCur(-2.5))
T "Abs(CCur(-2.5))", Abs(CCur(-2.5))
T "Round(CCur(2.345), 2)", Round(CCur(2.345), 2)
T "Round(CCur(2.355), 2)", Round(CCur(2.355), 2)
T "Round(CCur(-7.5))", Round(CCur(-7.5))
WScript.Echo Sgn(CCur(-3)), Hex(CCur(255.5)), Oct(CCur(8))

WScript.Echo "--- byte"
T "CByte(200)", CByte(200)
T "CByte(2.5)", CByte(2.5)
T "CByte(3.5)", CByte(3.5)
T "CByte(255.4)", CByte(255.4)
T "CByte(-0.4)", CByte(-0.4)
T "CByte(""17"")", CByte("17")
E "CByte(256)", "CByte(256)"
E "CByte(-1)", "CByte(-1)"
E "CByte(255.5)", "CByte(255.5)"
E "CByte(""x"")", "CByte(""x"")"
T "CByte(200) + CByte(55)", CByte(200) + CByte(55)
T "CByte(200) + CByte(56)", CByte(200) + CByte(56)
T "CByte(1) - CByte(2)", CByte(1) - CByte(2)
T "CByte(15) * CByte(17)", CByte(15) * CByte(17)
T "CByte(16) * CByte(16)", CByte(16) * CByte(16)
T "CByte(5) + 1", CByte(5) + 1
T "CByte(5) + CLng(1)", CByte(5) + CLng(1)
T "CByte(5) + Empty", CByte(5) + Empty
T "CByte(5) + True", CByte(5) + True
T "CByte(7) \ CByte(2)", CByte(7) \ CByte(2)
T "CByte(7) Mod CByte(2)", CByte(7) Mod CByte(2)
T "CByte(7) / CByte(2)", CByte(7) / CByte(2)
T "-CByte(5)", -CByte(5)
T "CByte(12) And CByte(10)", CByte(12) And CByte(10)
T "Not CByte(0)", Not CByte(0)
T "CByte(1) Or 2", CByte(1) Or 2
T "Abs(CByte(3))", Abs(CByte(3))
T "Int(CByte(3))", Int(CByte(3))
T "Round(CByte(3))", Round(CByte(3))
WScript.Echo Hex(CByte(255)), Oct(CByte(8)), CByte(3) = 3, CByte(3) < 2.5

WScript.Echo "--- single"
T "CSng(1.1)", CSng(1.1)
T "CSng(1) / 3", CSng(1) / 3
T "CSng(16777216)", CSng(16777216)
T "CSng(0.000001)", CSng(0.000001)
T "CSng(3.4E+38)", CSng(3.4E+38)
E "CSng(1E+39)", "CSng(1E+39)"
E "CSng(""x"")", "CSng(""x"")"
T "CSng(1.5) + CSng(2.25)", CSng(1.5) + CSng(2.25)
T "CSng(1.5) + 1", CSng(1.5) + 1
T "CSng(1.5) + CLng(1)", CSng(1.5) + CLng(1)
T "CSng(1.5) + 1.5", CSng(1.5) + 1.5
T "CSng(1.5) + CCur(2)", CSng(1.5) + CCur(2)
T "CSng(1.5) * CCur(2)", CSng(1.5) * CCur(2)
T "CSng(1.5) * CByte(2)", CSng(1.5) * CByte(2)
T "CSng(3E+38) * 10", CSng(3E+38) * 10
T "CSng(0.1) + CSng(0.2)", CSng(0.1) + CSng(0.2)
T "-CSng(2.5)", -CSng(2.5)
T "CSng(7) \ 2", CSng(7) \ 2
T "Int(CSng(-1.5))", Int(CSng(-1.5))
T "Round(CSng(2.5))", Round(CSng(2.5))
T "Abs(CSng(-0.25))", Abs(CSng(-0.25))
WScript.Echo CSng(1.1) = 1.1, CSng(0.1) = CDbl(0.1), CDbl(CSng(0.1)) = 0.1, CSng(0.5) = 0.5, Hex(CSng(10.5))

WScript.Echo "--- other subtypes keep VBScript's result types"
T "CLng(1) + 1", CLng(1) + 1
T "4 / 2", 4 / 2
T "2 ^ 2", 2 ^ 2
T "7 \ 2", 7 \ 2
T "-1", -1
T """1"" + 1", "1" + 1
T "Int(5)", Int(5)
T "Abs(-32768)", Abs(CInt(-32767) - 1)
T "32767 + 1", 32767 + 1
T "5.5 And 3", 5.5 And 3
T "Not 1.5", Not 1.5
T "Empty And Empty", Empty And Empty
WScript.Echo Hex(-1), Hex(-32768), Oct(-1)
