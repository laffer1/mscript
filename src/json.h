// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Lucas Holt. All rights reserved.

// json.h — JSON extension object (not part of VBScript): a global JSON object
// with JavaScript-style Parse/Stringify, parsing via libucl (base system).
#pragma once

#include "variant.h"

#include <memory>

struct Interp;

std::shared_ptr<VBObject> makeJson(Interp& ip);
