' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.

' MSXML2.DOMDocument
Dim doc
Set doc = CreateObject("MSXML2.DOMDocument.6.0")
doc.async = False

Dim xml
xml = "<catalog>" & _
      "<book id=""b1"" lang=""en""><title>Great Expectations</title><author>Dickens</author><price>9.99</price></book>" & _
      "<book id=""b2"" lang=""fr""><title>Les Miserables</title><author>Hugo</author><price>12.50</price></book>" & _
      "<book id=""b3"" lang=""en""><title>Emma</title><author>Austen</author><price>7.25</price></book>" & _
      "</catalog>"

WScript.Echo doc.loadXML(xml)
WScript.Echo "parseError: " & doc.parseError.errorCode

Dim root
Set root = doc.documentElement
WScript.Echo root.nodeName, root.nodeType, root.childNodes.length

' iterate children
Dim n
For Each n In root.childNodes
    WScript.Echo "book id=" & n.getAttribute("id") & " lang=" & n.attributes.getNamedItem("lang").nodeValue
Next

' getElementsByTagName
WScript.Echo doc.getElementsByTagName("title").length
WScript.Echo doc.getElementsByTagName("title").item(1).text

' XPath
WScript.Echo root.selectNodes("book").length
WScript.Echo root.selectNodes("book[@lang='en']").length
WScript.Echo root.selectSingleNode("book[@id='b2']/title").text
WScript.Echo doc.selectNodes("//price").length
WScript.Echo doc.selectSingleNode("//book[3]/author").text
WScript.Echo root.selectNodes("book[contains(title,'Miserables')]").length

' node navigation
Dim b1
Set b1 = root.firstChild
WScript.Echo b1.nodeName, b1.nextSibling.nodeName
WScript.Echo b1.firstChild.nodeName, b1.firstChild.text
WScript.Echo b1.childNodes.item(2).nodeValue

' build a document from scratch and serialize
Dim d2
Set d2 = CreateObject("MSXML2.DOMDocument")
Dim el
Set el = d2.createElement("greeting")
el.setAttribute "lang", "en"
Dim t
Set t = d2.createTextNode("Hello & <world>")
el.appendChild t
Set d2.documentElement = el
WScript.Echo d2.xml
WScript.Echo el.text

' modify: append + remove
Dim c
Set c = d2.createElement("extra")
c.appendChild d2.createTextNode("more")
el.appendChild c
WScript.Echo el.childNodes.length
el.removeChild c
WScript.Echo el.childNodes.length, el.getAttribute("lang")

' parse error reporting
Dim bad
Set bad = CreateObject("MSXML2.DOMDocument")
WScript.Echo bad.loadXML("<root><unclosed></root>")
WScript.Echo bad.parseError.errorCode <> 0, bad.parseError.line, Len(bad.parseError.reason) > 0

' load/save round trip via file
Dim fso
Set fso = CreateObject("Scripting.FileSystemObject")
Dim path
path = fso.BuildPath(fso.GetSpecialFolder(2).Path, "mscript_xml_test.xml")
doc.save path
Dim d3
Set d3 = CreateObject("MSXML2.DOMDocument.3.0")
WScript.Echo d3.load(path)
WScript.Echo d3.documentElement.nodeName, d3.getElementsByTagName("book").length
WScript.Echo d3.selectSingleNode("//book[@id=""b3""]/@id").nodeValue
fso.DeleteFile path
