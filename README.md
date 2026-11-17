# mscript

A **VBScript-compatible interpreter** written in **C++23**, built with the system
Clang compiler. Run `.vbs` scripts directly on any platform with a C++23 toolchain —
no WSH/cscript required.

**Build requirement:** XML support (`MSXML2.DOMDocument`) is parsed with
[bsdxml](https://github.com/bugaevc/serd) (expat 2) — on MidnightBSD/FreeBSD that's the
system `libbsdxml` (`-lbsdxml`), already linked by the Makefile.

```
$ make                # builds ./mscript with clang++ -std=c++23 -lbsdxml
$ ./mscript hello.vbs
Hello, World!
$ make test           # runs the test suite (19 scripts)
passed: 19  failed: 0
```

Install the binary and its manual page system-wide (default prefix `/usr/local`):

```
$ sudo make install   # → /usr/local/bin/mscript, /usr/local/share/man/man1/mscript.1
$ man mscript         # read the mscript(1) manual page
```

## Usage

```
mscript <script.vbs> [args...]   run a script file ('-' reads from stdin)
mscript -e "<code>"              run code from the command line
mscript --version                print version
mscript --help                   usage
```

Command-line arguments after the script name are exposed through
`WScript.Arguments`. `WScript.Echo` writes to stdout, `MsgBox` prints its prompt
and returns `vbOK`, and `InputBox` reads a line from stdin.

```vbs
Option Explicit

Dim name
name = "world"
WScript.Echo "Hello, " & UCase(name) & "!"

Dim i, total
total = 0
For i = 1 To 100
    total = total + i
Next
WScript.Echo "sum = " & total        ' sum = 5050
```

## Supported VBScript features

- **Types & coercion** — Variant semantics: `Empty`, `Null`, `Boolean` (True = -1),
  `Integer`/`Long`/`Double`, `Date`, `String`, arrays, objects; string↔number
  coercion, `Null` propagation, banker's rounding (`CInt(2.5) = 2`)
- **Operators** — full VBScript precedence (`-2^2 = 4`, `&` below `+`), `\`, `Mod`,
  `^`, comparisons, `Is`, and logical `And/Or/Xor/Eqv/Imp/Not` (bitwise on integers,
  logical on Booleans), `Option Compare Binary/Text`
- **Control flow** — `If/Then/ElseIf/Else/End If` (block and single-line),
  `Select Case` (incl. `Case Is`, `Case x To y`, `Case Else`), `For/To/Step/Next`,
  `For Each/Next`, `Do While/Until ... Loop` (pre/post), `While/Wend`,
  `Exit Do/For/Sub/Function/Property`, line labels and `GoTo`
- **Procedures** — `Sub`/`Function`, `ByVal`/`ByRef`, `Optional` default parameters,
  recursion, `Call`, `Static` locals, function-name return value
- **Classes** — `Class ... End Class`, `New`, `Me`, `Property Get/Let/Set`,
  `Class_Initialize`, `With ... End With` (incl. `.member` shorthand)
- **Arrays** — fixed and dynamic (`Dim x()`), multi-dimensional, `ReDim [Preserve]`,
  `Option Base`, `Erase`, `Array()`, `UBound`/`LBound`, `For Each`
- **Error handling** — `On Error Resume Next`, `On Error Goto 0`, `On Error Goto <label>`,
  the `Err` object (`Number`, `Description`, `Source`, `Clear`, `Raise`),
  `Option Explicit` (error 500 "Variable is undefined")
- **Built-ins** — ~70 functions: string (`Len`, `Mid`, `InStr`, `InStrRev`, `Replace`,
  `Split`, `Join`, `StrComp`, ...), math (`Sqr`, `Rnd`, `Round`, ...), date/time
  (`Now`, `DateSerial`, `DateAdd`, `DateDiff`, `DatePart`, `Weekday`, `MonthName`,
  `FormatDateTime`, ...), conversion (`CStr`, `CInt`, `CLng`, `CBool`, `CDate`, `Val`,
  `Hex`, `Oct`, `RGB`, ...), misc (`IIf`, `Choose`, `Switch`, `TypeName`, `VarType`,
  `IsNumeric`, `IsDate`, `Eval`, ...) plus all `vb*` constants (`vbCrLf`, `vbOK`,
  `vbTextCompare`, ...)
- **Host objects** — `WScript` (`Echo`, `Quit`, `Sleep`, `Arguments`, `StdIn/StdOut/StdErr`,
  `ScriptName`), `Err`, and `CreateObject` for `Scripting.Dictionary`,
  `Scripting.FileSystemObject` (files, folders, `TextStream`), `WScript.Shell`,
  `WScript.Network`, `ADODB.Stream` (in-memory text/binary streams:
  `Type`, `Mode`, `Charset`, `Position`, `Size`, `EOS`, `LineSeparator`,
  `Open`/`Close`/`Read`/`ReadText`/`Write`/`WriteText`/`Flush`/`CopyTo`/`SetEOS`/
  `LoadFromFile`/`SaveToFile`, plus the `adType*`/`adMode*`/`adSave*`/`adRead*`/`adCR`/`adLF`/`adCRLF`/`adState*` constants),
  and `MSXML2.DOMDocument` (any `MSXML*.DOMDocument[.x]` progID, parsed with bsdxml/expat:
  `load`/`loadXML`/`save`, `documentElement`, `parseError`, node properties `nodeName`/`nodeValue`/`text`/`nodeType`/`xml`,
  `childNodes`/`attributes` collections with `length`/`item(i)`/For Each, `getAttribute`/`setAttribute`,
  `appendChild`/`removeChild`/`replaceChild`/`insertBefore`/`cloneNode`, `createElement`/`createTextNode`/`createAttribute`,
  `getElementsByTagName`, and `selectNodes`/`selectSingleNode` with an XPath subset:
  `/a/b`, `//tag`, `*`, `.`, `..`, `text()`, `@attr`, `[n]`, `[@a='v']`, `[tag]`, `[contains(@a,'x')]` and `[contains(tag,'x')]`)
- **XML parsing** uses the system bsdxml (expat 2) library, linked via `-lbsdxml`
- **Dynamic code** — `Eval`, `Execute`, `ExecuteGlobal`
- **Source conveniences** — `'` and `Rem` comments, `_` line continuation, `:` statement
  separators, `&H`/`&O` literals, `""` escapes, bracketed identifiers `[my var]`,
  CRLF line endings

## Project layout

```
├── Makefile            BSD-make build: clang++ -std=c++23 -O2 -Wall -Wextra
├── src/
│   ├── main.cpp        CLI entry point
│   ├── lexer.{h,cpp}   tokenizer
│   ├── ast.h           AST node definitions
│   ├── parser.{h,cpp}  recursive-descent parser
│   ├── variant.{h,cpp} Variant value model, coercions, operators, arrays, dates
│   ├── interpreter.{h,cpp}  tree-walking evaluator: scopes, procs, classes, errors
│   ├── builtins.{h,cpp}     built-in functions and vb* constants
│   └── objects.{h,cpp}      WScript, Err, Scripting.Dictionary, FileSystemObject
├── src/xmldom.{h,cpp}  MSXML2.DOMDocument-compatible DOM on bsdxml (expat)
├── tests/              19 .vbs tests + .expected outputs + run.sh (make test)
├── man/mscript.1       mdoc manual page (installed by make install)
└── examples/demo.vbs   word-frequency report demo
```

## Known limitations

- `CreateObject` supports only the progIDs listed above (`Scripting.Dictionary`,
  `Scripting.FileSystemObject`, `WScript.Shell`, `WScript.Network`, `ADODB.Stream`,
  `MSXML*.DOMDocument`); other COM objects raise error 429 ("ActiveX component can't create object").
  In `ADODB.Stream`, `Position` is a byte offset and `Write`/`WriteText`/`CopyTo`
  overwrite at the current position, as in ADODB. The DOM's XPath support is the
  subset listed above, not full XPath 1.0; XML names are case-sensitive.
- `MsgBox` prints to stdout and returns `vbOK` (no GUI); `InputBox` reads stdin.
- Case folding (`UCase`/`LCase`/text compare) is ASCII-only; strings are UTF-8.
- No named arguments (`:=`), no `GetRef`, no `VBScript.RegExp`.
- `Option Explicit` is enforced at statement-execution time (mscript interprets
  rather than compiles), so the error surfaces when the statement runs — same
  error number, message, and line as WSH.

## License

BSD 2-Clause — see [LICENSE](LICENSE). Copyright (c) 2026 Lucas Holt.
