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

// Transform an .asp template into plain VBScript: HTML text becomes
// Response.Write statements, <%= expr %> becomes Response.Write (expr),
// <% code %> is passed through, <%@ directives %> are dropped.
// Newlines are preserved so error line numbers still match the .asp file.
std::string aspToVbs(const std::string& src);

// Register the ASP globals (request/response/server/session/application).
// cgi=true when running under a web server (REQUEST_METHOD is set).
void registerAspObjects(Interp& ip, bool cgi);

// Route WScript.Echo output into the Response body in ASP mode.
// Returns true if the output was consumed by the Response object.
bool aspResponseWrite(Interp& ip, const std::string& s);

// Emit the CGI response (Status/Content-Type/cookies/headers + body) when
// running as CGI; otherwise just the body. Called once at script end.
void flushAspResponse(Interp& ip);
