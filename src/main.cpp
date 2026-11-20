// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Lucas Holt. All rights reserved.

// main.cpp — mscript command line entry point.
// usage: mscript [options] <script.vbs> [args...]
//        mscript -e "code"
#include "interpreter.h"
#include "lexer.h"
#include "objects.h"
#include "parser.h"

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
        }

        Program prog = Parser(lex(code)).parseProgram();

        Interp ip;
        initWScript(ip, srcName, srcName, scriptArgs);
        ip.load(prog, srcName);
        try {
            ip.run();
        } catch (VBSError& e) {
            // small positive codes are VB errors shown as &H800Axxxx; a full
            // HRESULT (negative int32) is displayed as-is
            unsigned hex = (e.number >= 0 && e.number <= 0xFFFF)
                               ? (unsigned)(e.number | 0x800A0000)
                               : (unsigned)e.number;
            std::fprintf(stderr, "mscript: %s(%d): mscript runtime error '%08x': %s\n",
                         srcName.c_str(), e.line, hex, e.what());
            return 1;
        }
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
