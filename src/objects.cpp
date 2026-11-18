// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Lucas Holt. All rights reserved.

#include "objects.h"
#include "adodb.h"
#include "interpreter.h"
#include "regexp.h"
#include "xmldom.h"
#include "xmlhttp.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

// ---------- Err ----------

static std::shared_ptr<MapObject> makeErr(Interp& ip) {
    auto err = std::make_shared<MapObject>();
    err->kind = "Err";
    // Err.Number / Err.HelpContext are VT_I4 (Long) in VBScript, even when 0
    err->props["number"] = Variant::mkLong(0);
    err->props["description"] = Variant::mkStr("");
    err->props["source"] = Variant::mkStr("");
    err->props["helpfile"] = Variant::mkStr("");
    err->props["helpcontext"] = Variant::mkLong(0);
    err->methods["clear"] = [](MapObject* m, std::vector<Variant>&) -> Variant {
        m->props["number"] = Variant::mkLong(0);
        m->props["description"] = Variant::mkStr("");
        m->props["source"] = Variant::mkStr("");
        m->props["helpfile"] = Variant::mkStr("");
        m->props["helpcontext"] = Variant::mkLong(0);
        return Variant();
    };
    err->methods["raise"] = [](MapObject* m, std::vector<Variant>& a) -> Variant {
        int code = (int)toInt64(a.size() > 0 ? a[0] : Variant::mkInt(0));
        // vbscript runtime: Err.Raise accepts 1..65535 or a full HRESULT
        if (code == 0 || code > 0xFFFF) throwErr(5, errDesc(5));
        std::string src = a.size() > 1 ? toStr(a[1]) : "";
        std::string desc = a.size() > 2 ? toStr(a[2]) : "";
        std::string hf = a.size() > 3 ? toStr(a[3]) : "";
        int hc = a.size() > 4 ? (int)toInt64(a[4]) : 0;
        // Err.Number shows the VB-mapped value (FACILITY_VBS -> low word,
        // known OLE HRESULTs -> VBSE code, anything else -> full 32-bit)
        int num = mapHRESULTToVB((uint32_t)code);
        if (desc.empty()) desc = errDesc(num);
        throw VBSError(num, desc, src, hf, hc);
    };
    return err;
}

// ---------- stream object (StdOut/StdIn/StdErr) ----------

static std::shared_ptr<MapObject> makeStream(FILE* f, bool input) {
    auto s = std::make_shared<MapObject>();
    s->kind = "TextStream";
    s->methods["write"] = [f](MapObject*, std::vector<Variant>& a) -> Variant {
        for (auto& v : a) { std::string t = toStrLenient(v); std::fwrite(t.data(), 1, t.size(), f); }
        std::fflush(f);
        return Variant();
    };
    s->methods["writeline"] = [f](MapObject*, std::vector<Variant>& a) -> Variant {
        for (auto& v : a) { std::string t = toStrLenient(v); std::fwrite(t.data(), 1, t.size(), f); }
        std::fputc('\n', f);
        std::fflush(f);
        return Variant();
    };
    s->methods["writeblanklines"] = [f](MapObject*, std::vector<Variant>& a) -> Variant {
        long long n = a.empty() ? 1 : toInt64(a[0]);
        for (long long i = 0; i < n; i++) std::fputc('\n', f);
        std::fflush(f);
        return Variant();
    };
    if (input) {
        s->methods["read"] = [f](MapObject*, std::vector<Variant>& a) -> Variant {
            long long n = a.empty() ? 1 : toInt64(a[0]);
            std::string out;
            for (long long i = 0; i < n; i++) { int c = std::fgetc(f); if (c == EOF) break; out += (char)c; }
            return Variant::mkStr(out);
        };
        s->methods["readline"] = [f](MapObject*, std::vector<Variant>&) -> Variant {
            std::string line;
            int c;
            while ((c = std::fgetc(f)) != EOF) {
                if (c == '\n') break;
                line += (char)c;
            }
            if (c == EOF && line.empty()) throwErr(62, "Input past end of file");
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return Variant::mkStr(line);
        };
        s->methods["readall"] = [f](MapObject*, std::vector<Variant>&) -> Variant {
            std::string out;
            int c;
            while ((c = std::fgetc(f)) != EOF) out += (char)c;
            return Variant::mkStr(out);
        };
        s->props["atendofstream"] = Variant::mkBool(false);
    }
    return s;
}

// ---------- WScript ----------

void initWScript(Interp& ip, const std::string& scriptName, const std::string& fullName,
                 const std::vector<std::string>& args) {
    auto ws = std::make_shared<MapObject>();
    ws->kind = "WScript";
    ws->props["name"] = Variant::mkStr("WScript");
    ws->props["fullname"] = Variant::mkStr(fullName.empty() ? scriptName : fullName);
    ws->props["path"] = Variant::mkStr([] {
        if (char* p = std::getenv("WSCRIPT_PATH")) return std::string(p);
        return std::string("/");
    }());
    ws->props["scriptname"] = Variant::mkStr(scriptName);
    ws->props["scriptfullname"] = Variant::mkStr(fullName.empty() ? scriptName : fullName);
    ws->props["version"] = Variant::mkStr("5.8");
    {
        auto arr = std::make_shared<VArray>();
        arr->base = 0;
        arr->dims = {(long long)args.size()};
        for (auto& s : args) arr->data.push_back(Variant::mkStr(s));
        arr->allocated = true;
        ws->props["arguments"] = Variant::mkArr(arr);
    }
    ws->props["stdout"] = Variant::mkObj(makeStream(stdout, false));
    ws->props["stderr"] = Variant::mkObj(makeStream(stderr, false));
    ws->props["stdin"] = Variant::mkObj(makeStream(stdin, true));
    ws->methods["echo"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        std::string out;
        for (size_t i = 0; i < a.size(); i++) {
            if (i) out += ' ';
            out += toStrLenient(a[i]);
        }
        out += '\n';
        std::fwrite(out.data(), 1, out.size(), stdout);
        std::fflush(stdout);
        return Variant();
    };
    ws->methods["quit"] = [&ip](MapObject*, std::vector<Variant>& a) -> Variant {
        ip.exitCode = a.empty() ? 0 : (int)toInt64(a[0]);
        ip.quitCalled = true;
        throw EndSig{};
    };
    ws->methods["sleep"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        long long ms = a.empty() ? 0 : toInt64(a[0]);
        if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return Variant();
    };
    ip.wscriptObj = ws;
    ip.global.vars["wscript"] = Variant::mkObj(ws);
}

// ---------- Scripting.Dictionary ----------

struct DictData {
    std::vector<std::string> keys;                 // original keys
    std::vector<Variant> vals;
    std::map<std::string, size_t> index;           // lookup key (maybe lowercased)
    bool textCompare = false;
    std::string mapKey(const std::string& k) const { return textCompare ? lower(k) : k; }
};

struct DictObject : MapObject {
    std::shared_ptr<DictData> data;
    explicit DictObject(std::shared_ptr<DictData> dd) : data(std::move(dd)) { kind = "Dictionary"; }
    void setMember(const std::string& n, const Variant& v) override {
        if (n == "comparemode") {
            data->textCompare = (toInt64(v) == 1);
            // rebuild index under new mode
            data->index.clear();
            for (size_t i = 0; i < data->keys.size(); i++) data->index[data->mapKey(data->keys[i])] = i;
            props["comparemode"] = v;
            return;
        }
        MapObject::setMember(n, v);
    }
};

static std::shared_ptr<MapObject> makeDictionary() {
    auto data = std::make_shared<DictData>();
    auto d = std::make_shared<DictObject>(data);
    d->props["count"] = Variant::mkInt(0);
    d->props["comparemode"] = Variant::mkInt(0);

    auto refresh = [d, data]() { d->props["count"] = Variant::mkLong((long long)data->keys.size()); };
    auto find = [data](const std::string& k) -> long long {
        auto it = data->index.find(data->mapKey(k));
        return it == data->index.end() ? -1 : (long long)it->second;
    };
    d->methods["add"] = [data, refresh](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.size() < 2) throwErr(450, errDesc(450));
        std::string k = toStr(a[0]);
        if (data->index.count(data->mapKey(k))) throwErr(457, "This key is already associated with an element of this collection");
        data->index[data->mapKey(k)] = data->keys.size();
        data->keys.push_back(k);
        data->vals.push_back(a[1]);
        refresh();
        return Variant();
    };
    d->methods["exists"] = [find](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.empty()) throwErr(450, errDesc(450));
        return Variant::mkBool(find(toStr(a[0])) >= 0);
    };
    d->methods["remove"] = [data, refresh, find](MapObject*, std::vector<Variant>& a) -> Variant {
        long long i = find(toStr(a[0]));
        if (i < 0) throwErr(32811, "Element not found");
        data->index.erase(data->mapKey(toStr(a[0])));
        data->keys.erase(data->keys.begin() + i);
        data->vals.erase(data->vals.begin() + i);
        // rebuild index
        data->index.clear();
        for (size_t j = 0; j < data->keys.size(); j++) data->index[data->mapKey(data->keys[j])] = j;
        refresh();
        return Variant();
    };
    d->methods["removeall"] = [data, refresh](MapObject*, std::vector<Variant>&) -> Variant {
        data->keys.clear(); data->vals.clear(); data->index.clear();
        refresh();
        return Variant();
    };
    d->methods["keys"] = [data](MapObject*, std::vector<Variant>&) -> Variant {
        auto arr = std::make_shared<VArray>();
        arr->base = 0; arr->dims = {(long long)data->keys.size()};
        for (auto& k : data->keys) arr->data.push_back(Variant::mkStr(k));
        arr->allocated = true;
        return Variant::mkArr(arr);
    };
    d->methods["items"] = [data](MapObject*, std::vector<Variant>&) -> Variant {
        auto arr = std::make_shared<VArray>();
        arr->base = 0; arr->dims = {(long long)data->vals.size()};
        arr->data.assign(data->vals.begin(), data->vals.end());
        arr->allocated = true;
        return Variant::mkArr(arr);
    };
    // Item: method form dict.Item(k) and default form dict(k)
    auto itemGet = [find](MapObject*, std::vector<Variant>& a, DictData* dd) -> Variant {
        long long i = find(toStr(a[0]));
        if (i < 0) {
            // VB auto-adds on read? No: reading missing key via Item returns Empty; via dict("k") = v adds
            return Variant();
        }
        return dd->vals[(size_t)i];
    };
    d->methods["item"] = [data, itemGet](MapObject* m, std::vector<Variant>& a) -> Variant {
        if (a.empty()) throwErr(450, errDesc(450));
        return itemGet(m, a, data.get());
    };
    d->onInvoke = [data, itemGet](MapObject* m, std::vector<Variant>& a) -> Variant {
        if (a.empty()) throwErr(450, errDesc(450));
        return itemGet(m, a, data.get());
    };
    d->onAssignInvoke = [data, refresh](MapObject*, std::vector<Variant>& a, const Variant& v) -> void {
        std::string k = toStr(a[0]);
        auto it = data->index.find(data->mapKey(k));
        if (it != data->index.end()) { data->vals[it->second] = v; return; }
        data->index[data->mapKey(k)] = data->keys.size();
        data->keys.push_back(k);
        data->vals.push_back(v);
        refresh();
    };
    d->onEnum = [data](MapObject*) -> std::vector<Variant> {
        std::vector<Variant> out;
        for (auto& k : data->keys) out.push_back(Variant::mkStr(k));
        return out;
    };
    // CompareMode property set
    // (MapObject::setMember writes props directly; intercept via custom object would be needed — acceptable)
    return d;
}

// ---------- TextStream (file) ----------

struct TsData {
    std::string content;   // for reading: whole file; for writing: buffer
    size_t pos = 0;
    bool writable = false;
    std::fstream fs;
    std::string path;
    bool open = false;
};

// TextStream with live AtEndOfStream / AtEndOfLine / Line / Column properties.
struct LiveTextStream : MapObject {
    std::shared_ptr<TsData> d;
    explicit LiveTextStream(std::shared_ptr<TsData> data) : d(std::move(data)) { kind = "TextStream"; }
    Variant getMember(const std::string& n) override {
        if (n == "atendofstream") return Variant::mkBool(d->pos >= d->content.size());
        if (n == "atendofline") {
            if (d->pos >= d->content.size()) return Variant::mkBool(true);
            size_t nl = d->content.find('\n', d->pos);
            return Variant::mkBool(nl == d->pos);
        }
        if (n == "line" || n == "column") {
            long long line = 1, col = 1;
            for (size_t i = 0; i < d->pos && i < d->content.size(); i++) {
                if (d->content[i] == '\n') { line++; col = 1; } else col++;
            }
            return Variant::mkLong(n == "line" ? line : col);
        }
        return MapObject::getMember(n);
    }
};

static std::shared_ptr<VBObject> makeTextStream(const std::string& path, int iomode, bool create) {
    auto d = std::make_shared<TsData>();
    d->path = path;
    std::ios::openmode mode = std::ios::binary;
    if (iomode == 1) mode |= std::ios::in;
    else if (iomode == 2) mode |= std::ios::out | std::ios::trunc;
    else if (iomode == 8) mode |= std::ios::out | std::ios::app;
    else throwErr(5, errDesc(5));
    if (iomode == 1) {
        if (!fs::exists(path)) throwErr(53, "File not found");
        std::ifstream in(path, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        d->content = ss.str();
    } else {
        d->writable = true;
        if (iomode == 8) {
            std::ifstream in(path, std::ios::binary);
            std::ostringstream ss; ss << in.rdbuf();
            d->content = ss.str();
            d->pos = d->content.size();
        }
        d->fs.open(path, mode);
        if (!d->fs) throwErr(70, "Permission denied");
        d->open = true;
    }
    auto t = std::make_shared<LiveTextStream>(d);
    t->methods["read"] = [d](MapObject*, std::vector<Variant>& a) -> Variant {
        long long n = a.empty() ? 1 : toInt64(a[0]);
        if (n < 0) throwErr(5, errDesc(5));
        std::string out = d->content.substr(d->pos, (size_t)n);
        d->pos += out.size();
        return Variant::mkStr(out);
    };
    t->methods["readall"] = [d](MapObject*, std::vector<Variant>&) -> Variant {
        std::string out = d->content.substr(d->pos);
        d->pos = d->content.size();
        return Variant::mkStr(out);
    };
    t->methods["readline"] = [d](MapObject*, std::vector<Variant>&) -> Variant {
        if (d->pos >= d->content.size()) throwErr(62, "Input past end of file");
        size_t nl = d->content.find('\n', d->pos);
        std::string line = nl == std::string::npos ? d->content.substr(d->pos) : d->content.substr(d->pos, nl - d->pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        d->pos = nl == std::string::npos ? d->content.size() : nl + 1;
        return Variant::mkStr(line);
    };
    t->methods["write"] = [d](MapObject*, std::vector<Variant>& a) -> Variant {
        if (!d->writable) throwErr(70, "Permission denied");
        for (auto& v : a) {
            std::string s = toStr(v);
            d->content.insert(d->pos, s);
            d->pos += s.size();
        }
        return Variant();
    };
    t->methods["writeline"] = [d](MapObject*, std::vector<Variant>& a) -> Variant {
        if (!d->writable) throwErr(70, "Permission denied");
        for (auto& v : a) {
            std::string s = toStr(v);
            d->content.insert(d->pos, s + "\n");
            d->pos += s.size() + 1;
        }
        return Variant();
    };
    t->methods["writeblanklines"] = [d](MapObject*, std::vector<Variant>& a) -> Variant {
        if (!d->writable) throwErr(70, "Permission denied");
        long long n = a.empty() ? 1 : toInt64(a[0]);
        std::string s((size_t)n, '\n');
        d->content.insert(d->pos, s);
        d->pos += s.size();
        return Variant();
    };
    t->methods["skip"] = [d](MapObject*, std::vector<Variant>& a) -> Variant {
        long long n = a.empty() ? 1 : toInt64(a[0]);
        d->pos = std::min(d->content.size(), d->pos + (size_t)n);
        return Variant();
    };
    t->methods["skipline"] = [d](MapObject*, std::vector<Variant>&) -> Variant {
        size_t nl = d->content.find('\n', d->pos);
        d->pos = nl == std::string::npos ? d->content.size() : nl + 1;
        return Variant();
    };
    t->methods["close"] = [d](MapObject*, std::vector<Variant>&) -> Variant {
        if (d->writable && d->open) {
            d->fs.write(d->content.data(), (std::streamsize)d->content.size());
            d->fs.close();
            d->open = false;
        }
        return Variant();
    };
    return t;
}

// ---------- File / Folder ----------

static double fileTimeToSerial(fs::file_time_type ft) {
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    std::time_t tt = std::chrono::system_clock::to_time_t(sctp);
    std::tm tmv; gmtime_r(&tt, &tmv);
    return makeSerial(tmv.tm_year + 1900, (unsigned)tmv.tm_mon + 1, (unsigned)tmv.tm_mday,
                      tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

static std::shared_ptr<MapObject> makeFile(const fs::path& p);
static std::shared_ptr<MapObject> makeFolder(const fs::path& p);

// Folder with lazily-built Files / SubFolders collections (avoids deep recursion)
struct FolderObject : MapObject {
    fs::path p;
    explicit FolderObject(const fs::path& path) : p(path) { kind = "Folder"; }
    static std::shared_ptr<VArray> listDir(const fs::path& path, bool wantDirs) {
        auto arr = std::make_shared<VArray>();
        arr->base = 0;
        std::vector<Variant> items;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(path, ec)) {
            auto st = e.symlink_status(ec); // do not follow symlinks (avoids loops)
            bool isDir = fs::is_directory(st);
            if (isDir == wantDirs) {
                if (isDir) items.push_back(Variant::mkObj(makeFolder(e.path())));
                else if (fs::is_regular_file(st)) items.push_back(Variant::mkObj(makeFile(e.path())));
            }
        }
        arr->dims = {(long long)items.size()};
        arr->data.assign(items.begin(), items.end());
        arr->allocated = true;
        return arr;
    }
    Variant getMember(const std::string& n) override {
        if (n == "files") return Variant::mkArr(listDir(p, false));
        if (n == "subfolders") return Variant::mkArr(listDir(p, true));
        return MapObject::getMember(n);
    }
};

static std::shared_ptr<MapObject> makeFile(const fs::path& p) {
    if (!fs::exists(p) || !fs::is_regular_file(p)) throwErr(53, "File not found");
    auto f = std::make_shared<MapObject>();
    f->kind = "File";
    f->props["name"] = Variant::mkStr(p.filename().string());
    f->props["path"] = Variant::mkStr(fs::absolute(p).string());
    f->props["shortname"] = Variant::mkStr(p.filename().string());
    f->props["shortpath"] = f->props["path"];
    f->props["size"] = Variant::mkLong((long long)fs::file_size(p));
    f->props["type"] = Variant::mkStr("File");
    f->props["attributes"] = Variant::mkInt(0);
    f->props["drive"] = Variant::mkStr("/");
    f->props["datecreated"] = Variant::mkDate(fileTimeToSerial(fs::last_write_time(p)));
    f->props["datelastmodified"] = Variant::mkDate(fileTimeToSerial(fs::last_write_time(p)));
    f->props["datelastaccessed"] = Variant::mkDate(fileTimeToSerial(fs::last_write_time(p)));
    {
        fs::path pp = p.parent_path();
        if (pp.empty() || pp == p) f->props["parentfolder"] = Variant::mkObj(nullptr);
        else f->props["parentfolder"] = Variant::mkObj(makeFolder(pp));
    }
    f->methods["openastextstream"] = [p](MapObject*, std::vector<Variant>& a) -> Variant {
        long long mode = a.size() >= 1 ? toInt64(a[0]) : 1;
        return Variant::mkObj(makeTextStream(p.string(), (int)mode, true));
    };
    // capture path for delete/move/copy
    std::string ps = p.string();
    f->methods["delete"] = [ps](MapObject*, std::vector<Variant>&) -> Variant {
        std::error_code ec;
        fs::remove(ps, ec);
        if (ec) throwErr(70, "Permission denied");
        return Variant();
    };
    f->methods["move"] = [ps](MapObject*, std::vector<Variant>& a) -> Variant {
        std::string dst = toStr(a[0]);
        std::error_code ec;
        fs::rename(ps, dst, ec);
        if (ec) throwErr(58, "File already exists");
        return Variant();
    };
    f->methods["copy"] = [ps](MapObject*, std::vector<Variant>& a) -> Variant {
        std::string dst = toStr(a[0]);
        bool overwrite = a.size() < 2 || toBool(a[1]);
        std::error_code ec;
        fs::copy_file(ps, dst, overwrite ? fs::copy_options::overwrite_existing : fs::copy_options::none, ec);
        if (ec) throwErr(70, "Permission denied");
        return Variant();
    };
    return f;
}

static std::shared_ptr<MapObject> makeFolder(const fs::path& p) {
    if (!fs::exists(p) || !fs::is_directory(p)) throwErr(76, "Path not found");
    auto f = std::make_shared<FolderObject>(p);
    f->props["name"] = Variant::mkStr(p.filename().string().empty() ? p.string() : p.filename().string());
    f->props["path"] = Variant::mkStr(fs::absolute(p).string());
    f->props["shortname"] = f->props["name"];
    f->props["shortpath"] = f->props["path"];
    f->props["size"] = Variant::mkLong(0);
    f->props["type"] = Variant::mkStr("Folder");
    f->props["attributes"] = Variant::mkInt(16); // vbDirectory
    f->props["drive"] = Variant::mkStr("/");
    f->props["datelastmodified"] = Variant::mkDate(fileTimeToSerial(fs::last_write_time(p)));
    {
        fs::path pp = p.parent_path();
        if (pp.empty() || pp == p) f->props["parentfolder"] = Variant::mkObj(nullptr);
        else f->props["parentfolder"] = Variant::mkObj(makeFolder(pp));
    }
    // Files / SubFolders are built lazily by FolderObject::getMember
    std::string ps = p.string();
    f->methods["delete"] = [ps](MapObject*, std::vector<Variant>&) -> Variant {
        std::error_code ec;
        fs::remove_all(ps, ec);
        if (ec) throwErr(70, "Permission denied");
        return Variant();
    };
    f->methods["move"] = [ps](MapObject*, std::vector<Variant>& a) -> Variant {
        std::error_code ec;
        fs::rename(ps, toStr(a[0]), ec);
        if (ec) throwErr(58, "File already exists");
        return Variant();
    };
    f->methods["copy"] = [ps](MapObject*, std::vector<Variant>& a) -> Variant {
        std::error_code ec;
        fs::copy(ps, toStr(a[0]), fs::copy_options::recursive, ec);
        if (ec) throwErr(70, "Permission denied");
        return Variant();
    };
    f->methods["createtextfile"] = [ps](MapObject*, std::vector<Variant>& a) -> Variant {
        std::string name = toStr(a[0]);
        bool overwrite = a.size() < 2 || toBool(a[1]);
        fs::path fp = fs::path(ps) / name;
        if (fs::exists(fp) && !overwrite) throwErr(58, "File already exists");
        return Variant::mkObj(makeTextStream(fp.string(), 2, true));
    };
    return f;
}

// ---------- FileSystemObject ----------

static std::shared_ptr<MapObject> makeFSO(Interp& ip) {
    (void)ip;
    auto f = std::make_shared<MapObject>();
    f->kind = "FileSystemObject";
    f->methods["fileexists"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        std::error_code ec;
        return Variant::mkBool(fs::is_regular_file(toStr(a[0]), ec));
    };
    f->methods["folderexists"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        std::error_code ec;
        return Variant::mkBool(fs::exists(toStr(a[0]), ec) && fs::is_directory(toStr(a[0]), ec));
    };
    f->methods["createtextfile"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        std::string p = toStr(a[0]);
        bool overwrite = a.size() < 2 || toBool(a[1]);
        if (fs::exists(p) && !overwrite) throwErr(58, "File already exists");
        return Variant::mkObj(makeTextStream(p, 2, true));
    };
    f->methods["opentextfile"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        std::string p = toStr(a[0]);
        long long iomode = a.size() >= 2 ? toInt64(a[1]) : 1;
        bool create = a.size() >= 3 ? toBool(a[2]) : false;
        if (iomode == 1 && !fs::exists(p)) {
            if (!create) throwErr(53, "File not found");
            std::ofstream out(p, std::ios::binary);
        }
        return Variant::mkObj(makeTextStream(p, (int)iomode, create));
    };
    f->methods["createfolder"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        std::string p = toStr(a[0]);
        if (fs::exists(p)) throwErr(58, "File already exists");
        std::error_code ec;
        if (!fs::create_directory(p, ec)) throwErr(76, "Path not found");
        return Variant::mkObj(makeFolder(p));
    };
    f->methods["deletefile"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        std::string p = toStr(a[0]);
        std::error_code ec;
        if (!fs::exists(p, ec)) throwErr(76, "Path not found");
        fs::remove(p, ec);
        if (ec) throwErr(70, "Permission denied");
        return Variant();
    };
    f->methods["deletefolder"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        std::string p = toStr(a[0]);
        std::error_code ec;
        if (!fs::exists(p, ec)) throwErr(76, "Path not found");
        fs::remove_all(p, ec);
        if (ec) throwErr(70, "Permission denied");
        return Variant();
    };
    f->methods["copyfile"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        std::string src = toStr(a[0]), dst = toStr(a[1]);
        bool overwrite = a.size() < 3 || toBool(a[2]);
        if (!fs::exists(src)) throwErr(53, "File not found");
        std::error_code ec;
        fs::copy_file(src, dst, overwrite ? fs::copy_options::overwrite_existing : fs::copy_options::none, ec);
        if (ec) throwErr(70, "Permission denied");
        return Variant();
    };
    f->methods["movefile"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        std::error_code ec;
        fs::rename(toStr(a[0]), toStr(a[1]), ec);
        if (ec) throwErr(70, "Permission denied");
        return Variant();
    };
    f->methods["copyfolder"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        std::error_code ec;
        fs::copy(toStr(a[0]), toStr(a[1]), fs::copy_options::recursive, ec);
        if (ec) throwErr(70, "Permission denied");
        return Variant();
    };
    f->methods["movefolder"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        std::error_code ec;
        fs::rename(toStr(a[0]), toStr(a[1]), ec);
        if (ec) throwErr(70, "Permission denied");
        return Variant();
    };
    f->methods["getfile"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        return Variant::mkObj(makeFile(toStr(a[0])));
    };
    f->methods["getfolder"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        return Variant::mkObj(makeFolder(toStr(a[0])));
    };
    f->methods["getabsolutepathname"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        std::error_code ec;
        return Variant::mkStr(fs::absolute(toStr(a[0]), ec).string());
    };
    f->methods["getfilename"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        return Variant::mkStr(fs::path(toStr(a[0])).filename().string());
    };
    f->methods["getbasename"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        return Variant::mkStr(fs::path(toStr(a[0])).stem().string());
    };
    f->methods["getextensionname"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        return Variant::mkStr(fs::path(toStr(a[0])).extension().string());
    };
    f->methods["getparentfoldername"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        auto p = fs::path(toStr(a[0])).parent_path();
        return Variant::mkStr(p.empty() ? std::string() : p.string());
    };
    f->methods["getdrivename"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        return Variant::mkStr("/");
    };
    f->methods["buildpath"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        std::string x = toStr(a[0]), y = toStr(a[1]);
        if (!x.empty() && x.back() != '/' && !y.empty() && y.front() != '/') return Variant::mkStr(x + "/" + y);
        return Variant::mkStr(x + y);
    };
    f->methods["getspecialfolder"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        long long which = toInt64(a[0]);
        std::string p = which == 0 ? "/" : (which == 1 ? "/usr" : "/tmp");
        return Variant::mkObj(makeFolder(p));
    };
    f->methods["gettempname"] = [](MapObject*, std::vector<Variant>&) -> Variant {
        char buf[] = "/tmp/mscriptXXXXXX";
        int fd = mkstemp(buf);
        if (fd >= 0) close(fd);
        return Variant::mkStr(fs::path(buf).filename().string());
    };
    f->methods["getfileversion"] = [](MapObject*, std::vector<Variant>&) -> Variant {
        return Variant::mkStr("");
    };
    return f;
}

// ---------- ADODB.Stream ----------

struct AdoData {
    int type = 2;            // adTypeText (1 = adTypeBinary)
    int mode = 3;            // adModeReadWrite
    std::string charset = "utf-8";
    std::string buf;         // raw bytes
    size_t pos = 0;          // byte offset
    int lineSep = -1;        // adCRLF
    bool open = false;
};

static std::string charsetEncode(const std::string& s, const std::string& cs) {
    if (cs == "utf-8" || cs == "utf8") return s;
    // ascii / latin-1 family: one byte per char, '?' for unencodable
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = s[i];
        unsigned cp; size_t adv;
        if (c < 0x80) { cp = c; adv = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; adv = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; adv = 3; }
        else { cp = c & 0x07; adv = 4; }
        for (size_t k = 1; k < adv && i + k < s.size(); k++) cp = (cp << 6) | (s[i + k] & 0x3F);
        i += adv;
        out += (cp <= 0xFF) ? (char)cp : '?';
    }
    return out;
}

static std::string charsetDecode(const std::string& bytes, const std::string& cs) {
    if (cs == "utf-8" || cs == "utf8") return bytes;
    std::string out;
    for (unsigned char c : bytes) {
        if (c < 0x80) out += (char)c;
        else { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
    }
    return out;
}

static std::string normCharset(std::string cs) {
    cs = lower(trim(cs));
    if (cs == "utf8") return "utf-8";
    if (cs == "ascii" || cs == "us-ascii") return "us-ascii";
    if (cs == "latin1" || cs == "iso-8859-1" || cs == "windows-1252" || cs == "iso8859-1") return "iso-8859-1";
    return cs; // unknown charsets pass through as utf-8
}

struct AdoStream : MapObject {
    std::shared_ptr<AdoData> d;
    explicit AdoStream(std::shared_ptr<AdoData> data) : d(std::move(data)) { kind = "ADODB.Stream"; }

    Variant getMember(const std::string& n) override {
        if (n == "type") return Variant::mkInt(d->type);
        if (n == "mode") return Variant::mkInt(d->mode);
        if (n == "charset") return Variant::mkStr(d->charset);
        if (n == "position") return Variant::mkLong((long long)d->pos);
        if (n == "size") return Variant::mkLong((long long)d->buf.size());
        if (n == "eos") return Variant::mkBool(d->pos >= d->buf.size());
        if (n == "lineseparator") return Variant::mkInt(d->lineSep);
        if (n == "state") return Variant::mkInt(d->open ? 1 : 0); // adStateOpen / adStateClosed
        return MapObject::getMember(n);
    }
    void setMember(const std::string& n, const Variant& v) override {
        if (n == "type") { long long t = toInt64(v); if (t != 1 && t != 2) throwErr(5, errDesc(5)); d->type = (int)t; return; }
        if (n == "mode") { d->mode = (int)toInt64(v); return; }
        if (n == "charset") { d->charset = normCharset(toStr(v)); return; }
        if (n == "position") {
            long long p = toInt64(v);
            if (p < 0) p = 0;
            if ((size_t)p > d->buf.size()) p = (long long)d->buf.size();
            d->pos = (size_t)p;
            return;
        }
        if (n == "lineseparator") { long long ls = toInt64(v); if (ls != -1 && ls != 10 && ls != 13) throwErr(5, errDesc(5)); d->lineSep = (int)ls; return; }
        MapObject::setMember(n, v);
    }
};

static std::shared_ptr<MapObject> makeAdoStream() {
    auto data = std::make_shared<AdoData>();
    auto s = std::make_shared<AdoStream>(data);

    auto requireOpen = [data]() {
        if (!data->open) throwErr(3001, "Operation is not allowed when the object is closed");
    };
    s->methods["open"] = [data](MapObject*, std::vector<Variant>&) -> Variant {
        data->open = true;
        data->pos = 0;
        return Variant();
    };
    s->methods["close"] = [data](MapObject*, std::vector<Variant>&) -> Variant {
        data->open = false;
        return Variant();
    };
    s->methods["cancel"] = [](MapObject*, std::vector<Variant>&) -> Variant { return Variant(); };
    s->methods["flush"] = [requireOpen](MapObject*, std::vector<Variant>&) -> Variant {
        requireOpen();
        return Variant();
    };
    s->methods["writetext"] = [data, requireOpen](MapObject*, std::vector<Variant>& a) -> Variant {
        requireOpen();
        if (a.empty()) throwErr(450, errDesc(450));
        std::string text = toStr(a[0]);
        std::string bytes = charsetEncode(text, data->charset);
        // ADODB overwrites at the current position (extending if needed)
        data->buf.replace(data->pos, bytes.size(), bytes);
        data->pos += bytes.size();
        return Variant();
    };
    s->methods["write"] = [data, requireOpen](MapObject*, std::vector<Variant>& a) -> Variant {
        requireOpen();
        if (a.empty()) throwErr(450, errDesc(450));
        const Variant& v = a[0];
        std::string bytes;
        if (v.t == VT::Array && v.arr) {
            for (auto& e : v.arr->data) {
                long long b = toInt64(e);
                if (b < 0 || b > 255) throwErr(5, errDesc(5));
                bytes += (char)b;
            }
        } else {
            bytes = toStr(v); // raw bytes of the string
        }
        data->buf.replace(data->pos, bytes.size(), bytes);
        data->pos += bytes.size();
        return Variant();
    };
    s->methods["read"] = [data, requireOpen](MapObject*, std::vector<Variant>& a) -> Variant {
        requireOpen();
        long long n = a.empty() ? -1 : toInt64(a[0]); // -1 = adReadAll
        size_t avail = data->buf.size() - data->pos;
        size_t take = (n < 0) ? avail : std::min(avail, (size_t)n);
        auto arr = std::make_shared<VArray>();
        arr->base = 0;
        arr->dims = {(long long)take};
        for (size_t i = 0; i < take; i++) arr->data.push_back(Variant::mkInt((unsigned char)data->buf[data->pos + i]));
        arr->allocated = true;
        data->pos += take;
        return Variant::mkArr(arr);
    };
    s->methods["readtext"] = [data, requireOpen](MapObject*, std::vector<Variant>& a) -> Variant {
        requireOpen();
        long long n = a.empty() ? -1 : toInt64(a[0]); // -1 adReadAll, -2 adReadLine
        if (n == -2) {
            // read one line (terminated by LF, CR, or CRLF), without the terminator
            size_t start = data->pos;
            size_t nl = data->buf.find('\n', start);
            size_t cr = data->buf.find('\r', start);
            size_t end, next;
            if (nl == std::string::npos && cr == std::string::npos) { end = data->buf.size(); next = end; }
            else if (cr != std::string::npos && (nl == std::string::npos || cr < nl)) {
                end = cr;
                next = (cr + 1 < data->buf.size() && data->buf[cr + 1] == '\n') ? cr + 2 : cr + 1;
            } else { end = nl; next = nl + 1; }
            std::string line = charsetDecode(data->buf.substr(start, end - start), data->charset);
            data->pos = next;
            return Variant::mkStr(line);
        }
        size_t avail = data->buf.size() - data->pos;
        if (n < 0) {
            std::string s = charsetDecode(data->buf.substr(data->pos), data->charset);
            data->pos = data->buf.size();
            return Variant::mkStr(s);
        }
        // n characters (code points) for utf-8, bytes otherwise
        size_t take = std::min(avail, (size_t)n);
        if (data->charset == "utf-8" || data->charset == "utf8") {
            // count n code points
            size_t i = data->pos, cnt = 0;
            while (i < data->buf.size() && cnt < (size_t)n) {
                unsigned char c = data->buf[i];
                size_t adv = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 : ((c & 0xF0) == 0xE0) ? 3 : 4;
                i += adv; cnt++;
            }
            take = i - data->pos;
        }
        std::string s = charsetDecode(data->buf.substr(data->pos, take), data->charset);
        data->pos += take;
        return Variant::mkStr(s);
    };
    s->methods["savetofile"] = [data, requireOpen](MapObject*, std::vector<Variant>& a) -> Variant {
        requireOpen();
        if (a.empty()) throwErr(450, errDesc(450));
        std::string path = toStr(a[0]);
        long long opt = a.size() >= 2 ? toInt64(a[1]) : 2; // default adSaveCreateOverWrite
        std::error_code ec;
        if (opt == 1 && fs::exists(path, ec)) throwErr(58, "File already exists");
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) throwErr(70, "Permission denied");
        out.write(data->buf.data(), (std::streamsize)data->buf.size());
        return Variant();
    };
    s->methods["loadfromfile"] = [data](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.empty()) throwErr(450, errDesc(450));
        std::string path = toStr(a[0]);
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) throwErr(53, "File not found");
        std::ifstream in(path, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        data->buf = ss.str();
        data->pos = 0;
        data->open = true;
        return Variant();
    };
    s->methods["copyto"] = [data](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.empty()) throwErr(450, errDesc(450));
        const Variant& dst = a[0];
        if (dst.t != VT::Object || !dst.obj) throwErr(424, errDesc(424));
        auto* other = dynamic_cast<AdoStream*>(dst.obj.get());
        if (!other) throwErr(13, errDesc(13));
        long long n = a.size() >= 2 ? toInt64(a[1]) : -1; // -1 = all remaining
        size_t avail = data->buf.size() - data->pos;
        size_t take = (n < 0) ? avail : std::min(avail, (size_t)n);
        std::string chunk = data->buf.substr(data->pos, take);
        other->d->buf.replace(other->d->pos, chunk.size(), chunk); // overwrite like ADODB
        other->d->pos += take;
        data->pos += take;
        return Variant();
    };
    s->methods["seteos"] = [data, requireOpen](MapObject*, std::vector<Variant>&) -> Variant {
        requireOpen();
        data->buf.resize(data->pos);
        return Variant();
    };
    return s;
}

// ---------- WScript.Shell / WScript.Network ----------

static std::shared_ptr<MapObject> makeShell() {
    auto s = std::make_shared<MapObject>();
    s->kind = "WshShell";
    s->methods["run"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        std::string cmd = toStr(a[0]);
        bool wait = a.size() < 3 || toBool(a[2]);
        int rc = 0;
        if (wait) rc = std::system(cmd.c_str());
        else rc = std::system((cmd + " &").c_str());
        return Variant::mkInt(rc);
    };
    s->methods["popup"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        std::string msg = a.empty() ? "" : toStrLenient(a[0]);
        std::fwrite(msg.data(), 1, msg.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
        return Variant::mkInt(1); // vbOK
    };
    s->methods["expandenvironmentstrings"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        std::string s = toStr(a[0]);
        std::string out;
        size_t i = 0;
        while (i < s.size()) {
            if (s[i] == '%') {
                size_t j = s.find('%', i + 1);
                if (j != std::string::npos) {
                    std::string name = s.substr(i + 1, j - i - 1);
                    const char* v = std::getenv(name.c_str());
                    out += v ? v : "";
                    i = j + 1;
                    continue;
                }
            }
            out += s[i++];
        }
        return Variant::mkStr(out);
    };
    s->methods["regread"] = [](MapObject*, std::vector<Variant>&) -> Variant {
        throwErr(450, "Registry access is not supported");
    };
    s->methods["regwrite"] = [](MapObject*, std::vector<Variant>&) -> Variant {
        throwErr(450, "Registry access is not supported");
    };
    return s;
}

static std::shared_ptr<MapObject> makeNetwork() {
    auto n = std::make_shared<MapObject>();
    n->kind = "WshNetwork";
    char host[256] = {0};
    gethostname(host, sizeof host - 1);
    n->props["computername"] = Variant::mkStr(host);
    const char* u = std::getenv("USER");
    n->props["username"] = Variant::mkStr(u ? u : "user");
    const char* d = std::getenv("USERDOMAIN");
    n->props["userdomain"] = Variant::mkStr(d ? d : "WORKGROUP");
    return n;
}

// ---------- registration ----------

void registerHostObjects(Interp& ip) {
    ip.errObj = makeErr(ip);
    ip.global.vars["err"] = Variant::mkObj(ip.errObj);
    // WScript created by initWScript (called from main)
}

std::shared_ptr<VBObject> createHostObject(Interp& ip, const std::string& progIdIn) {
    std::string progId = lower(trim(progIdIn));
    if (progId == "scripting.dictionary") return makeDictionary();
    if (progId == "scripting.filesystemobject") return makeFSO(ip);
    if (progId == "wscript.shell") return makeShell();
    if (progId == "wscript.network") return makeNetwork();
    if (progId == "adodb.stream" || progId.rfind("adodb.stream.", 0) == 0) return makeAdoStream();
    // ADODB database objects on SQLite3 (base system): ADODB.Connection / Recordset / Command
    if (progId == "adodb.connection" || progId.rfind("adodb.connection.", 0) == 0) return makeAdoConnection(ip);
    if (progId == "adodb.recordset" || progId.rfind("adodb.recordset.", 0) == 0) return makeAdoRecordset(ip);
    if (progId == "adodb.command" || progId.rfind("adodb.command.", 0) == 0) return makeAdoCommand(ip);
    // MSXML DOM: MSXML2.DOMDocument[.x.y], MSXML3/4/6.DOMDocument, Microsoft.XMLDOM, ...
    if (progId == "microsoft.xmldom" ||
        (progId.find("domdocument") != std::string::npos && progId.rfind("msxml", 0) == 0))
        return makeDomDocument(ip);
    // MSXML HTTP: MSXML2.XMLHTTP[.x], MSXML2.ServerXMLHTTP, MSXML3/6.XMLHTTP, Microsoft.XMLHTTP
    if (progId.find("xmlhttp") != std::string::npos &&
        (progId.rfind("msxml", 0) == 0 || progId.rfind("microsoft", 0) == 0))
        return makeXmlHttp(ip);
    // WinHTTP: WinHttp.WinHttpRequest[.5.1]
    if (progId.find("winhttprequest") != std::string::npos && progId.rfind("winhttp", 0) == 0)
        return makeWinHttpRequest(ip);
    // VBScript.RegExp (also usable as the intrinsic class "RegExp" via New)
    if (progId == "vbscript.regexp" || progId == "regexp") return makeRegExp(ip);
    return nullptr;
}
