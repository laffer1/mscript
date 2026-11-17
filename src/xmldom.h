// xmldom.h — MSXML2.DOMDocument-compatible DOM built on bsdxml (expat).
#pragma once

#include "variant.h"
#include <memory>
#include <string>
#include <vector>

struct Interp;

// Creates a new MSXML2.DOMDocument-compatible object.
std::shared_ptr<VBObject> makeDomDocument(Interp& interp);
