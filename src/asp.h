// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Lucas Holt. All rights reserved.

// asp.h — classic-ASP extension (not part of VBScript): .asp template files
// with mixed HTML and <% %> code blocks, plus the CGI object model
// (Request / Response / Server / Session / Application). Session and
// Application state is stored in SQLite under /var/db/mscript.
#pragma once

#include "variant.h"

#include <memory>
#include <string>

struct Interp;

// CGI only: move the request headers that the HTTP client stack (libfetch and
// our XMLHTTP) would read as its own configuration out of the environment.
// A client sending "Proxy: http://evil" otherwise sets HTTP_PROXY and every
// outgoing request the page makes goes through the attacker ("httpoxy").
// Request.ServerVariables still reports the original header values.
void cgiSanitizeEnv();

// Transform an .asp template into plain VBScript: HTML text becomes
// Response.Write statements, <%= expr %> becomes Response.Write (expr),
// <% code %> is passed through, <%@ directives %> are dropped, and
// <script runat="server"> blocks are treated as code. dropText keeps only
// the code blocks (used for global.asa, whose markup is never rendered).
// Newlines are preserved so error line numbers still match the .asp file.
std::string aspToVbs(const std::string& src, bool dropText = false);

// The ASP application root for a script: the nearest ancestor directory
// containing a global.asa, else the script's own directory.
std::string aspAppRootOf(const std::string& scriptFullName);

// Register the ASP globals (request/response/server/session/application).
// cgi=true when running under a web server (REQUEST_METHOD is set).
void registerAspObjects(Interp& ip, bool cgi);

// Run global.asa hooks after the program is loaded: Application_OnStart
// (once per application, guarded in the state db) and Session_OnStart for
// newly issued sessions. Call only in ASP mode.
void aspGlobalAsaHooks(Interp& ip);

// Route WScript.Echo output into the Response body in ASP mode.
// Returns true if the output was consumed by the Response object.
bool aspResponseWrite(Interp& ip, const std::string& s);

// Emit the CGI response (Status/Content-Type/cookies/headers + body) when
// running as CGI; otherwise just the body. Called once at script end.
void flushAspResponse(Interp& ip);
