// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Lucas Holt. All rights reserved.

// objects.h — host objects: WScript, Err, Scripting.Dictionary, FileSystemObject.
#pragma once

#include "variant.h"
#include <memory>
#include <string>
#include <vector>

struct Interp;

void registerHostObjects(Interp& interp);
void initWScript(Interp& interp, const std::string& scriptName, const std::string& fullName,
                 const std::vector<std::string>& args);
std::shared_ptr<VBObject> createHostObject(Interp& interp, const std::string& progId);
