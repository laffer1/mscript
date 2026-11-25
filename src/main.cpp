// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Lucas Holt. All rights reserved.

// main.cpp — mscript command line entry point.
// usage: mscript [options] <script.vbs> [args...]
//        mscript -e "code"
#include "interpreter.h"
#include "lexer.h"
#include "objects.h"
#include "parser.h"
#include "asp.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <new>
#include <sstream>
#include <string>

static void usage() {
    std::fputs(
        "mscript — VBScript-compatible interpreter (C++23)\n"
        "usage:\n"
        "  mscript <script.vbs> [args...]   run a script file ('-' reads stdin)\n"
        "  mscript -e \"<code>\"              run code from the command line\n"
        "  mscript --version                 print version\n"
        "  mscript --help                    this help\n",
        stderr);
}

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) { usage(); return 2; }

    std::string code;
    std::string srcName = "<command line>";
    std::vector<std::string> scriptArgs;

    if (args[0] == "--version" || args[0] == "-v") {
        std::printf("mscript 1.0 (VBScript-compatible interpreter, C++23/clang)\n");
        return 0;
    }
    if (args[0] == "--help" || args[0] == "-h" || args[0] == "/?") {
        usage();
        return 0;
    }
    if (args[0] == "-e" || args[0] == "--eval") {
        if (args.size() < 2) { std::fputs("mscript: -e requires an argument\n", stderr); return 2; }
        code = args[1];
        scriptArgs.assign(args.begin() + 2, args.end());
        srcName = "-e";
    } else {
        srcName = args[0];
        scriptArgs.assign(args.begin() + 1, args.end());
    }

    // Everything from here on (file read, lex/parse, interpreter setup, run)
    // is inside the guarded region so allocation failures anywhere report a
    // clean error instead of terminating the process.
    try {
        if (srcName != "-e") {
            std::ifstream in(srcName == "-" ? "/dev/stdin" : srcName, std::ios::binary);
            if (!in) {
                std::fprintf(stderr, "mscript: cannot open '%s'\n", srcName.c_str());
                return 2;
            }
            std::ostringstream ss;
            ss << in.rdbuf();
            code = ss.str();
            // extension: a "#!" first line (shebang, e.g. #!/usr/local/bin/mscript)
            // is ignored; blank it out but keep the newline so line numbers
            // still match the file
            if (code.compare(0, 2, "#!") == 0) {
                size_t nl = code.find('\n');
                if (nl == std::string::npos) code.clear();
                else code.replace(0, nl, nl, ' ');
            }
        }

        // extension: .asp files are classic-ASP templates (mixed HTML and
        // <% %> code blocks); transform to plain VBScript before lexing
        bool aspMode = srcName.size() >= 4 &&
                       lower(srcName.substr(srcName.size() - 4)) == ".asp";
        if (aspMode) code = aspToVbs(code);

        Program prog = Parser(lex(code)).parseProgram();

        // ASP extension: a global.asa next to the script contributes its
        // declarations (Application_OnStart / Session_OnStart / ...) ahead
        // of the page's own statements
        if (aspMode) {
            std::error_code ec;
            // global.asa lives at the application root, so pages in
            // subdirectories get the same hooks
            std::filesystem::path gasa =
                std::filesystem::path(aspAppRootOf(srcName)) / "global.asa";
            if (std::filesystem::exists(gasa, ec)) {
                std::ifstream gin(gasa, std::ios::binary);
                std::ostringstream gss;
                gss << gin.rdbuf();
                Program g = Parser(lex(aspToVbs(gss.str(), true))).parseProgram();
                std::vector<StmtPtr> merged;
                for (auto& st : g.stmts) merged.push_back(std::move(st));
                for (auto& st : prog.stmts) merged.push_back(std::move(st));
                prog.stmts = std::move(merged);
            }
        }

        Interp ip;
        ip.scriptFullName = srcName;
        initWScript(ip, srcName, srcName, scriptArgs);
        if (aspMode) ip.aspMode = true;
        try {
            // ASP setup (session/state db) and the global.asa hooks run inside
            // the VBSError handler so their errors get the normal ASP error
            // report + response flush instead of no CGI response at all
            if (aspMode) {
                registerAspObjects(ip, std::getenv("REQUEST_METHOD") != nullptr);
                // arm the Server.ScriptTimeout wall-clock check (read live in execStmtList)
                ip.aspStart = (double)std::time(nullptr);
            }
            ip.load(prog, srcName);
            bool endedInHook = false;
            if (aspMode) {
                try {
                    aspGlobalAsaHooks(ip);
                } catch (EndSig&) {
                    // Response.End / Redirect / Server.Transfer in
                    // Application_OnStart or Session_OnStart ends the request
                    endedInHook = true;
                }
            }
            if (!endedInHook) ip.run();
        } catch (VBSError& e) {
            // small positive codes are VB errors shown as &H800Axxxx; a full
            // HRESULT (negative int32) is displayed as-is
            unsigned hex = (e.number >= 0 && e.number <= 0xFFFF)
                               ? (unsigned)(e.number | 0x800A0000)
                               : (unsigned)e.number;
            std::fprintf(stderr, "mscript: %s(%d): mscript runtime error '%08x': %s\n",
                         srcName.c_str(), e.line, hex, e.what());
            std::fflush(stderr);
            if (aspMode) flushAspResponse(ip); // still emit the CGI response
            ip.shutdown(); // objects still alive get Class_Terminate after the error
            return 1;
        }
        if (aspMode) flushAspResponse(ip);
        ip.shutdown(); // Class_Terminate for objects still alive at script end
        return ip.exitCode;
    } catch (ParseError& e) {
        std::fprintf(stderr, "mscript: %s(%d): syntax error: %s\n", srcName.c_str(), e.line, e.msg.c_str());
        return 1;
    } catch (const std::bad_alloc&) {
        std::fprintf(stderr, "mscript: %s: out of memory\n", srcName.c_str());
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "mscript: %s: internal error: %s\n", srcName.c_str(), e.what());
        return 1;
    }
}
