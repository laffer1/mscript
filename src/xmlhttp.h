// xmlhttp.h — MSXML2.XMLHTTP-compatible object.
// http://  -> built-in HTTP/1.1 client over POSIX sockets (full status/headers)
// https:// -> libfetch (TLS), file:// -> libfetch
#pragma once

#include "variant.h"
#include <memory>
#include <string>

struct Interp;

std::shared_ptr<VBObject> makeXmlHttp(Interp& interp);

// WinHttp.WinHttpRequest[.5.1] — same transport as XMLHTTP, plus the
// WinHTTP-specific Option property, SetTimeouts, WaitForResponse, SetCredentials.
std::shared_ptr<VBObject> makeWinHttpRequest(Interp& interp);
