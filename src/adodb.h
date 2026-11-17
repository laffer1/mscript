// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Lucas Holt. All rights reserved.

// adodb.h — ADODB.Connection / ADODB.Recordset / ADODB.Command on SQLite3
// (the sqlite3 library shipped in the MidnightBSD base system, -lsqlite3).
#pragma once

#include "variant.h"
#include <memory>

struct Interp;

std::shared_ptr<VBObject> makeAdoConnection(Interp& interp);
std::shared_ptr<VBObject> makeAdoRecordset(Interp& interp);
std::shared_ptr<VBObject> makeAdoCommand(Interp& interp);
