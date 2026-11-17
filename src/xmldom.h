// xmldom.h — MSXML2.DOMDocument-compatible DOM built on bsdxml (expat).
#pragma once

#include "variant.h"
#include <memory>
#include <string>
#include <vector>

struct Interp;

// Creates a new MSXML2.DOMDocument-compatible object.
std::shared_ptr<VBObject> makeDomDocument(Interp& interp);

// Creates a DOMDocument and parses xml into it; ok is set to the parse result.
std::shared_ptr<VBObject> makeDomDocumentFromXML(Interp& interp, const std::string& xml, bool& ok);
