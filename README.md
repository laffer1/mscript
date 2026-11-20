# mscript

A **VBScript-compatible interpreter** written in **C++23**, built with the system
Clang compiler. Run `.vbs` scripts directly on any platform with a C++23 toolchain —
no WSH/cscript required.

**Build requirements:** XML support (`MSXML2.DOMDocument`) is parsed with
bsdxml, the MidnightBSD/FreeBSD base-system copy of
[libexpat](https://github.com/libexpat/libexpat) (expat 2, MIT licence) — on MidnightBSD/FreeBSD
that's the system `libbsdxml` (`-lbsdxml`); HTTPS fetching (`MSXML2.XMLHTTP`) uses the system
`libfetch` (`-lfetch`); database access (`ADODB.*`) uses the base-system SQLite3
(`-lsqlite3`). All are already linked by the Makefile.

```
$ make                # builds ./mscript with clang++ -std=c++23 -lbsdxml -lfetch -lsqlite3
$ ./mscript hello.vbs
Hello, World!
$ make test           # runs the test suite (24 scripts)
passed: 24  failed: 0
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
  `/a/b`, `//tag`, `*`, `.`, `..`, `text()`, `@attr`, `[n]`, `[@a='v']`, `[tag]`, `[contains(@a,'x')]` and `[contains(tag,'x')]`),
  and `MSXML2.XMLHTTP` (any `MSXML*.XMLHTTP[.x]`, `MSXML2.ServerXMLHTTP`, `Microsoft.XMLHTTP`
  progID: `open`/`send`/`setRequestHeader`/`abort`/`getAllResponseHeaders`/`getResponseHeader`,
  properties `readyState`/`status`/`statusText`/`responseText`/`responseBody`/`responseXML`/`timeout`;
  `http://` uses a built-in HTTP/1.1 client with full status, response headers, chunked
  decoding and redirect following, `https://` and `file://` go through libfetch),
  `WinHttp.WinHttpRequest.5.1` (same transport, plus the WinHTTP-specific
  `Option` property — 0 UserAgentString, 1 URL (read-only), 6 EnableRedirects,
  14 MaxAutomaticRedirects — `SetTimeouts`, `WaitForResponse`, `SetCredentials`;
  `SetProxy`/`SetClientCertificate` are accepted no-ops),
  `VBScript.RegExp` (on the system libc POSIX regex: `Pattern`/`Global`/`IgnoreCase`/`Multiline`,
  `Test`/`Execute`/`Replace` with `Match` (`FirstIndex`/`Length`/`Value`/`SubMatches`)
  and match collections supporting `Count`/`Item(i)`/For Each),
  and `ADODB.Connection` / `ADODB.Recordset` / `ADODB.Command` on SQLite3 (base system):
  connection strings `Provider=SQLite3;Data Source=file.db` (bare path and `:memory:`
  work too; `Mode` `adModeRead` opens read-only), `Execute`/`BeginTrans`/`CommitTrans`/
  `RollbackTrans` (savepoint-nested), recordset navigation (`MoveFirst/Last/Next/Previous/Move`,
  `AbsolutePosition`, `RecordCount`, `BOF`/`EOF`), `Fields` (`Count`/`Item(i)`/For Each;
  `Field` `Name`/`Value`/`Type`/`ActualSize`), `GetRows`/`GetString`, `AddNew`/`Update`/`Delete`/
  `CancelUpdate` (writes need a simple single-table SELECT — the SQL is rewritten to
  prepend `rowid`; anything else is read-only), and `Command` with `CommandText`/
  `CommandType` (`adCmdText`/`adCmdTable`), `CreateParameter` and positional (`?`) or
  named (`:name`) parameter binding
- **XML parsing** uses the system bsdxml (expat 2) library, linked via `-lbsdxml`;
  **HTTP fetching** uses the system libfetch, linked via `-lfetch`;
  **regular expressions** use the libc POSIX regex (`regcomp`/`regexec`, no extra library);
  **database access** uses the base-system SQLite3, linked via `-lsqlite3`
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
├── src/xmlhttp.{h,cpp} MSXML2.XMLHTTP + WinHttp.WinHttpRequest.5.1 client (raw HTTP/1.1 + libfetch)
├── src/regexp.{h,cpp}  VBScript.RegExp on the libc POSIX regex
├── src/adodb.{h,cpp}   ADODB Connection/Recordset/Command on SQLite3
├── tests/              24 .vbs tests + .expected outputs + run.sh (make test);
│                       18_xmlhttp.vbs / 20_winhttp.vbs run against tests/httpserver.py
│                       (python3, 127.0.0.1:18099)
├── man/mscript.1       mdoc manual page (installed by make install)
└── examples/demo.vbs   word-frequency report demo
```

## Security notes / resource limits

Malicious or buggy input fails with a clean VBScript error, not a crash:
- Array dimensions are validated and the element count is capped (2^24 =
  16,777,216 elements, and never beyond what `size_t` can represent);
  overflow/negative/oversized `Dim`/`ReDim` raises error 7 ("Out of memory") —
  previously a dimension product overflow could corrupt the heap.
- Parser expression/block nesting is capped at 256 (syntax error), and
  procedure recursion at 1000 frames (error 28 "Out of stack space") instead
  of overflowing the C++ stack.
- `Space`/`String` are capped at 2^28 characters (error 14 "Out of string
  space"); `std::bad_alloc` anywhere is caught and reported as out of memory.
- XML parsing caps element nesting at 512 elements — enforced for parsed XML
  and DOM API mutations (`appendChild` etc.) alike, since recursive tree
  operations would overflow the stack (`parseError.reason` reports it);
  expat 2.10's billion-laughs protection is active and external entities are
  not resolved (no XXE). Documents over 2 GB are rejected.
- HTTP responses are capped at 64 MB; request methods/headers/URLs are
  validated against CRLF injection; credentials are stripped on cross-origin
  redirects; `connect()` honors the configured timeout (non-blocking + poll).
- `VBScript.RegExp` uses the libc backtracking regex, which can in principle
  backtrack catastrophically (ReDoS) on patterns like `(a+)+$` fed with long
  hostile strings — avoid applying complex patterns to untrusted input.
- ADODB-generated INSERT/UPDATE/DELETE quote identifiers with `"`-doubling.
- Paths containing NUL bytes truncate at the OS call (no sandbox exists to
  bypass; noted for awareness).

## Known limitations

- `CreateObject` supports only the progIDs listed above (`Scripting.Dictionary`,
  `Scripting.FileSystemObject`, `WScript.Shell`, `WScript.Network`, `ADODB.Stream`,
  `MSXML*.DOMDocument`, `MSXML*.XMLHTTP`/`Microsoft.XMLHTTP`, `WinHttp.WinHttpRequest[.5.1]`,
  `VBScript.RegExp`, `ADODB.Connection`/`ADODB.Recordset`/`ADODB.Command`); other COM objects
  raise error 429 ("ActiveX component can't create object").
  In `ADODB.Stream`, `Position` is a byte offset and `Write`/`WriteText`/`CopyTo`
  overwrite at the current position, as in ADODB. The DOM's XPath support is the
  subset listed above, not full XPath 1.0; XML names are case-sensitive.
- `XMLHTTP`/`WinHttpRequest` requests are always synchronous (the `async` flag of `open`
  is accepted but ignored, `onreadystatechange` is not fired — there is no `GetRef`;
  `WaitForResponse` returns `True` immediately). Over `https://`/`file://` (libfetch)
  response headers are not exposed, so `getResponseHeader` returns `""` and `status` is
  200 on success or mapped from the error reason (e.g. 404); binary request bodies
  containing NUL over `https://` are rejected because libfetch takes a C string.
  Responses are limited to 64 MiB.
  `WinHttpRequest` honors `Option(6)` (EnableRedirects) only on the built-in `http://`
  path — libfetch always follows redirects. `SetProxy`/`SetClientCertificate` are no-ops
  (proxies come from `http_proxy`/`HTTP_PROXY`).
- `VBScript.RegExp` translates patterns to POSIX ERE: `\d \D \w \W \s \S`, `\n \r \t \f \v`,
  `(?:...)` (group indices are remapped, so `$1`/`SubMatches` count only capturing groups),
  `.` never matches `\n`, and `Multiline` uses `REG_NEWLINE`. Not supported (error 5 with a
  clear message): backreferences (`\1`), non-greedy quantifiers (`*? +? ??`), lookaround
  (`?= (?! (?<= (?<!`), named groups, and `\b` — the BSD libc regex has no word boundary.
  With `Multiline` on, negated classes like `[^a]` do not match `\n` (REG_NEWLINE semantics).
- `ADODB` is SQLite3-only. Recordsets materialize the full result in memory at
  `Open`/`Execute`. `recordsAffected` out-parameters of `Execute` are not supported (host
  method calls don't pass `ByRef`); use a `SELECT COUNT(*)`. Binary columns come back as
  byte arrays; dates bind as their serial double; `OpenSchema` and stored procedures are
  unsupported; `Errors` is empty.
- `MsgBox` prints to stdout and returns `vbOK` (no GUI); `InputBox` reads stdin.
- Case folding (`UCase`/`LCase`/text compare) is ASCII-only; strings are UTF-8.
- No named arguments (`:=`), no `GetRef`.
- `Option Explicit` is enforced at statement-execution time (mscript interprets
  rather than compiles), so the error surfaces when the statement runs — same
  error number, message, and line as WSH.

## License

BSD 2-Clause — see [LICENSE](LICENSE). Copyright (c) 2026 Lucas Holt.

XML parsing uses expat, which is under the MIT licence. On MidnightBSD/FreeBSD it comes
from the base system as `libbsdxml`. If you ship a binary statically linked against
[libexpat](https://github.com/libexpat/libexpat) (for example a static Linux build),
include libexpat's MIT copyright and permission notice (its `COPYING` file) with it.

VBScript, Windows Script Host, MSXML, ADODB and WinHTTP are trademarks of Microsoft Corporation; mscript is not affiliated with or endorsed by Microsoft.
