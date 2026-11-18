// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Lucas Holt. All rights reserved.

// adodb.cpp — ADODB.Connection / ADODB.Recordset / ADODB.Command backed by
// SQLite3 (base-system libsqlite3).
//
// Connection strings: "Provider=SQLite3;Data Source=/path/file.db[;...]",
// any key=value pairs are parsed (Data Source / Database / File Name), a bare
// path is accepted as the data source, and ":memory:" works. Only the
// SQLite3 provider is supported. Mode adModeRead opens read-only.
//
// Recordsets materialize the full result at Open/Execute (embedded DB), so
// MoveFirst/Last/Previous/Next, AbsolutePosition and RecordCount all work.
// AddNew/Update/Delete need a simple single-table SELECT source (the SQL is
// rewritten to prepend "rowid" for the WHERE clause); anything else (joins,
// UNION, aggregates without GROUP BY, ...) is read-only and update methods
// raise an error. Transactions use SAVEPOINT nesting.
//
// The recordsAffected out-parameter of Execute is not supported (host object
// method calls do not pass ByRef in mscript); read counts via a SELECT.
// Binary column values come back as byte arrays (like XMLHTTP responseBody).

#include "adodb.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ---------- connection ----------

struct DbConn {
    sqlite3* db = nullptr;
    std::string connStr;
    std::string path;
    long long mode = 3; // adModeReadWrite
    long long connTimeout = 15;
    long long cmdTimeout = 30;
    int transLevel = 0;
    bool isOpen = false;
    ~DbConn() { if (db) sqlite3_close(db); } // don't leak the handle if Close is never called
};

// Quote an SQL identifier for interpolation into generated statements
// (table/column names parsed from the recordset source): doubles embedded
// '"' so a crafted source SELECT cannot break out of the quoted identifier.
std::string quoteIdent(const std::string& ident) {
    std::string out = "\"";
    for (char c : ident) {
        out += c;
        if (c == '"') out += '"';
    }
    out += '"';
    return out;
}

[[noreturn]] void sqlErr(sqlite3* db, const std::string& prefix) {
    throwErr(5, prefix + sqlite3_errmsg(db));
}

void dbExec(DbConn& d, const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(d.db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::string m = err ? err : sqlite3_errmsg(d.db);
        sqlite3_free(err);
        throwErr(5, "SQLite error: " + m);
    }
}

void parseConnStr(const std::string& cs, std::string& path, std::string& provider) {
    size_t i = 0;
    while (i <= cs.size()) {
        size_t e = cs.find(';', i);
        if (e == std::string::npos) e = cs.size();
        std::string part = trim(cs.substr(i, e - i));
        i = e + 1;
        if (part.empty()) continue;
        size_t eq = part.find('=');
        if (eq == std::string::npos) { path = part; continue; }
        std::string k = lower(part.substr(0, eq));
        k.erase(std::remove_if(k.begin(), k.end(), [](unsigned char c) { return std::isspace(c); }), k.end());
        std::string v = trim(part.substr(eq + 1));
        if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') || (v.front() == '\'' && v.back() == '\'')))
            v = v.substr(1, v.size() - 2);
        if (k == "provider") provider = v;
        else if (k == "datasource" || k == "database" || k == "filename" || k == "file") path = v;
        // other keys (User ID, Password, ...) accepted and ignored: SQLite has no auth
    }
}

void connOpen(const std::shared_ptr<DbConn>& d, const std::string& cs) {
    if (d->isOpen) throwErr(5, "Connection is already open");
    std::string path, provider;
    parseConnStr(cs, path, provider);
    if (!provider.empty() && lower(provider).find("sqlite") == std::string::npos)
        throwErr(5, "Only the SQLite3 provider is supported (got '" + provider + "')");
    if (path.empty()) throwErr(5, "Connection string has no Data Source");
    d->connStr = cs;
    d->path = path;
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    if (d->mode == 1) flags = SQLITE_OPEN_READONLY;      // adModeRead
    else if (d->mode == 2) flags = SQLITE_OPEN_READWRITE; // adModeWrite
    if (sqlite3_open_v2(path.c_str(), &d->db, flags, nullptr) != SQLITE_OK) {
        std::string m = d->db ? sqlite3_errmsg(d->db) : "out of memory";
        if (d->db) sqlite3_close(d->db);
        d->db = nullptr;
        throwErr(5, "SQLite error: cannot open '" + path + "': " + m);
    }
    d->isOpen = true;
}

void connClose(const std::shared_ptr<DbConn>& d) {
    if (!d->isOpen) return;
    if (d->transLevel > 0) { try { dbExec(*d, "ROLLBACK"); } catch (...) {} d->transLevel = 0; }
    sqlite3_close(d->db);
    d->db = nullptr;
    d->isOpen = false;
}

// ---------- recordset ----------

// Registry so Recordset.ActiveConnection / Command.ActiveConnection can hand
// back the owning Connection object. Holds shared_ptr (no cycles: a
// Connection never references its recordsets).
std::vector<std::pair<std::shared_ptr<DbConn>, std::shared_ptr<VBObject>>>& connRegistry() {
    static std::vector<std::pair<std::shared_ptr<DbConn>, std::shared_ptr<VBObject>>> r;
    return r;
}

std::shared_ptr<VBObject> findConnObj(std::shared_ptr<DbConn> c) {
    for (auto& p : connRegistry())
        if (p.first == c) return p.second;
    return nullptr;
}

struct AdoConnection : MapObject {
    std::shared_ptr<DbConn> d;
    explicit AdoConnection(std::shared_ptr<DbConn> data) : d(std::move(data)) { kind = "ADODB.Connection"; }

    bool hasMember(const std::string& n) const override {
        static const char* ms[] = {"connectionstring","provider","state","mode","defaultdatabase",
            "connectiontimeout","commandtimeout","version","attributes","errors"};
        for (auto m : ms) if (n == m) return true;
        return MapObject::hasMember(n);
    }
    bool hasMethod(const std::string& n) const override {
        static const char* ms[] = {"open","close","execute","begintrans","committrans","rollbacktrans","cancel","openschema"};
        for (auto m : ms) if (n == m) return true;
        return MapObject::hasMethod(n);
    }
    Variant getMember(const std::string& n) override {
        if (n == "connectionstring") return Variant::mkStr(d->connStr);
        if (n == "provider") return Variant::mkStr("SQLite3");
        if (n == "state") return Variant::mkInt(d->isOpen ? 1 : 0);
        if (n == "mode") return Variant::mkLong(d->mode);
        if (n == "defaultdatabase") return Variant::mkStr(d->path);
        if (n == "connectiontimeout") return Variant::mkLong(d->connTimeout);
        if (n == "commandtimeout") return Variant::mkLong(d->cmdTimeout);
        if (n == "version") return Variant::mkStr(std::string("SQLite ") + sqlite3_libversion());
        if (n == "attributes") return Variant::mkInt(0);
        if (n == "errors") return Variant::mkObj(nullptr); // no error collection
        return MapObject::getMember(n);
    }
    void setMember(const std::string& n, const Variant& v) override {
        if (n == "connectionstring") { if (d->isOpen) throwErr(5, "Connection is open"); d->connStr = toStr(v); return; }
        if (n == "mode") { long long m = toInt64(v); if (m < 0 || m > 15) throwErr(5, errDesc(5)); d->mode = m; return; }
        if (n == "connectiontimeout") { d->connTimeout = toInt64(v); return; }
        if (n == "commandtimeout") { d->cmdTimeout = toInt64(v); return; }
        if (n == "defaultdatabase") { if (d->isOpen) throwErr(5, "Connection is open"); d->path = toStr(v); return; }
        MapObject::setMember(n, v);
    }
};

struct RsData {
    std::shared_ptr<DbConn> conn;
    std::shared_ptr<DbConn> ownConn; // when opened from a connection string
    std::string source;
    long long cursorType = 0, lockType = 1;
    std::vector<std::string> cols;
    std::vector<long long> colTypes;
    std::vector<std::vector<Variant>> rows;
    std::vector<long long> rowids; // 0 when not updateable
    bool updateable = false;
    std::string table;
    long long pos = 0; // 0-based current row; -1 = BOF
    bool isOpen = false;
    // edit state (AddNew / field assignment before Update)
    bool editing = false, adding = false;
    long long editRowId = 0;
    std::vector<Variant> editVals;
    std::vector<char> editDirty;

    bool bof() const { return rows.empty() || pos < 0; }
    bool eof() const { return rows.empty() || pos >= (long long)rows.size(); }
    bool valid() const { return !rows.empty() && pos >= 0 && pos < (long long)rows.size(); }
};

long long declTypeToAdo(const char* dt) {
    if (!dt) return -1;
    std::string t = lower(dt);
    if (t.find("bool") != std::string::npos) return 11; // adBoolean
    if (t.find("bigint") != std::string::npos) return 20; // adBigInt
    if (t.find("int") != std::string::npos) return 3;    // adInteger
    if (t.find("char") != std::string::npos || t.find("text") != std::string::npos ||
        t.find("clob") != std::string::npos) return 200;  // adVarChar
    if (t.find("blob") != std::string::npos || t.find("binary") != std::string::npos) return 205; // adLongVarBinary
    if (t.find("real") != std::string::npos || t.find("floa") != std::string::npos ||
        t.find("doub") != std::string::npos) return 5;    // adDouble
    if (t.find("num") != std::string::npos || t.find("dec") != std::string::npos) return 131; // adNumeric
    if (t.find("date") != std::string::npos || t.find("time") != std::string::npos) return 7; // adDate
    return -1;
}

Variant colValue(sqlite3_stmt* st, int i, long long adoType) {
    switch (sqlite3_column_type(st, i)) {
    case SQLITE_INTEGER: {
        long long v = sqlite3_column_int64(st, i);
        return adoType == 11 ? Variant::mkBool(v != 0) : Variant::mkLong(v);
    }
    case SQLITE_FLOAT: return Variant::mkDouble(sqlite3_column_double(st, i));
    case SQLITE_TEXT: {
        const unsigned char* p = sqlite3_column_text(st, i);
        int n = sqlite3_column_bytes(st, i);
        return Variant::mkStr(std::string((const char*)p, (size_t)n));
    }
    case SQLITE_BLOB: {
        const void* p = sqlite3_column_blob(st, i);
        int n = sqlite3_column_bytes(st, i);
        auto arr = std::make_shared<VArray>();
        arr->base = 0;
        arr->dims = { (long long)n };
        arr->data.resize((size_t)n);
        for (int k = 0; k < n; k++) arr->data[(size_t)k] = Variant::mkInt(((const unsigned char*)p)[k]);
        return Variant::mkArr(arr);
    }
    default: return Variant::mkNull();
    }
}

// Rewrite a simple single-table SELECT to prepend rowid for update support.
// Returns "" when the source is not a simple SELECT (then the recordset is
// read-only).
std::string rewriteForRowid(const std::string& sql, std::string& tableOut) {
    std::string s = trim(sql);
    // must start with SELECT
    std::string head = lower(s.substr(0, 6));
    if (head != "select") return "";
    size_t p = 6;
    // skip DISTINCT / ALL
    while (p < s.size() && std::isspace((unsigned char)s[p])) p++;
    std::string kw = lower(s.substr(p, 8));
    if (kw.rfind("distinct", 0) == 0) p += 8;
    else if (kw.rfind("all", 0) == 0 && (p + 3 >= s.size() || std::isspace((unsigned char)s[p + 3]))) p += 3;
    // find FROM at top level (no parens expected in a simple select list)
    size_t from = std::string::npos;
    for (size_t i = p; i + 4 <= s.size(); i++) {
        if (s[i] == '(') return ""; // subquery in select list -> not simple
        if (lower(s.substr(i, 4)) == "from" && (i == 0 || std::isspace((unsigned char)s[i - 1])) &&
            (i + 4 >= s.size() || std::isspace((unsigned char)s[i + 4]))) { from = i; break; }
    }
    if (from == std::string::npos) return "";
    // table token after FROM
    size_t q = from + 4;
    while (q < s.size() && std::isspace((unsigned char)s[q])) q++;
    size_t qStart = q;
    if (q < s.size() && (s[q] == '"' || s[q] == '`' || s[q] == '\'' || s[q] == '[')) {
        char open = s[q], close = (open == '[') ? ']' : open;
        q++;
        std::string tok;
        bool closed = false;
        while (q < s.size()) {
            if (s[q] == close) {
                // doubled close-quote is an escaped quote inside the identifier
                if (close != ']' && q + 1 < s.size() && s[q + 1] == close) { tok += close; q += 2; continue; }
                q++;
                closed = true;
                break;
            }
            tok += s[q++];
        }
        if (!closed) return "";
        tableOut = tok;
    } else {
        while (q < s.size() && !std::isspace((unsigned char)s[q]) && std::string(",;()").find(s[q]) == std::string::npos) q++;
        tableOut = s.substr(qStart, q - qStart);
    }
    if (tableOut.empty()) return "";
    // rest of FROM clause: reject joins / multiple tables (comma or JOIN before WHERE etc.)
    std::string rest = lower(s.substr(q));
    size_t cut = rest.find_first_of(";");
    if (cut != std::string::npos) rest = rest.substr(0, cut);
    for (size_t i = 0; i < rest.size(); i++) {
        if (rest[i] == ',') return "";
        if (rest.compare(i, 4, "join") == 0 || rest.compare(i, 5, "inner") == 0 ||
            rest.compare(i, 4, "left") == 0 || rest.compare(i, 5, "cross") == 0) return "";
        if (rest.compare(i, 5, "where") == 0 || rest.compare(i, 5, "group") == 0 ||
            rest.compare(i, 5, "order") == 0 || rest.compare(i, 6, "having") == 0 ||
            rest.compare(i, 5, "limit") == 0) break;
    }
    // rebuild: SELECT [DISTINCT] rowid AS "__mscript_rid__", <list> FROM <rest>
    std::string selKw = (kw.rfind("distinct", 0) == 0) ? "SELECT DISTINCT " : "SELECT ";
    return selKw + "rowid AS \"__mscript_rid__\", " + s.substr(p, from - p) + s.substr(from);
}

void rsRunQuery(const std::shared_ptr<RsData>& rs, const std::string& sql) {
    if (!rs->conn || !rs->conn->isOpen) throwErr(3704, "Operation is not allowed when the object is closed");
    rs->source = sql;
    rs->cols.clear(); rs->colTypes.clear(); rs->rows.clear(); rs->rowids.clear();
    rs->editing = false; rs->adding = false; rs->editVals.clear(); rs->editDirty.clear();

    std::string q = sql;
    rs->table = "";
    std::string rw = rewriteForRowid(sql, rs->table);
    if (!rw.empty()) { q = rw; rs->updateable = true; }

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(rs->conn->db, q.c_str(), -1, &st, nullptr) != SQLITE_OK)
        sqlErr(rs->conn->db, "SQLite error: ");
    int ncols = sqlite3_column_count(st);
    int ridCol = -1;
    for (int i = 0; i < ncols; i++) {
        std::string nm = sqlite3_column_name(st, i);
        if (i == 0 && nm == "__mscript_rid__") { ridCol = 0; continue; }
        rs->cols.push_back(nm);
        long long t = declTypeToAdo(sqlite3_column_decltype(st, i));
        rs->colTypes.push_back(t);
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        std::vector<Variant> row;
        long long rid = 0;
        for (int i = 0; i < ncols; i++) {
            if (i == ridCol) { rid = sqlite3_column_int64(st, i); continue; }
            int ci = i - (ridCol == 0 ? 1 : 0);
            long long ado = rs->colTypes[(size_t)ci];
            if (ado < 0) ado = sqlite3_column_type(st, i) == SQLITE_INTEGER ? 3 :
                              sqlite3_column_type(st, i) == SQLITE_FLOAT ? 5 :
                              sqlite3_column_type(st, i) == SQLITE_TEXT ? 200 :
                              sqlite3_column_type(st, i) == SQLITE_BLOB ? 205 : 0;
            row.push_back(colValue(st, i, ado));
        }
        // fill column types from first row when decltype was unknown
        if (rs->rows.empty())
            for (size_t c = 0; c < rs->cols.size(); c++)
                if (rs->colTypes[c] < 0) {
                    const Variant& v = row[c];
                    rs->colTypes[c] = v.t == VT::Long || v.t == VT::Int ? 3 :
                                       v.t == VT::Double || v.t == VT::Date ? 5 :
                                       v.t == VT::String ? 200 :
                                       v.t == VT::Array ? 205 : v.t == VT::Bool ? 11 : 0;
                }
        rs->rows.push_back(std::move(row));
        rs->rowids.push_back(rs->updateable ? rid : 0);
    }
    int rc = sqlite3_finalize(st);
    if (rc != SQLITE_OK && rc != SQLITE_DONE) sqlErr(rs->conn->db, "SQLite error: ");
    rs->pos = 0;
    rs->isOpen = true;
}

void bindVariant(sqlite3_stmt* st, int idx, const Variant& v) {
    switch (v.t) {
    case VT::Int:
    case VT::Long: sqlite3_bind_int64(st, idx, v.ll); break;
    case VT::Double: sqlite3_bind_double(st, idx, v.d); break;
    case VT::Date: sqlite3_bind_double(st, idx, v.d); break; // stored as serial double
    case VT::Bool: sqlite3_bind_int(st, idx, v.b ? 1 : 0); break;
    case VT::String: {
        // SQLITE_TRANSIENT: sqlite copies
        sqlite3_bind_text(st, idx, v.s.data(), (int)v.s.size(), SQLITE_TRANSIENT);
        break;
    }
    case VT::Array: {
        if (!v.arr) { sqlite3_bind_null(st, idx); break; }
        std::string bytes;
        for (auto& e : v.arr->data) {
            long long b = toInt64(e);
            if (b < 0 || b > 255) throwErr(5, errDesc(5));
            bytes += (char)b;
        }
        sqlite3_bind_blob(st, idx, bytes.data(), (int)bytes.size(), SQLITE_TRANSIENT);
        break;
    }
    case VT::Null:
    case VT::Empty: sqlite3_bind_null(st, idx); break;
    default: throwErr(5, "Cannot use this value as a database parameter");
    }
}

long long fieldIndex(const std::shared_ptr<RsData>& rs, const Variant& key) {
    if (key.t == VT::String) {
        std::string n = lower(key.s);
        for (size_t i = 0; i < rs->cols.size(); i++)
            if (lower(rs->cols[i]) == n) return (long long)i;
        throwErr(3265, "Item cannot be found in the collection corresponding to the requested name or ordinal");
    }
    long long i = toInt64(key);
    if (i < 0 || (size_t)i >= rs->cols.size()) throwErr(9, errDesc(9));
    return i;
}

Variant fieldValue(const std::shared_ptr<RsData>& rs, long long idx) {
    if (rs->editing) return rs->editVals[(size_t)idx];
    if (rs->valid()) return rs->rows[(size_t)rs->pos][(size_t)idx];
    return Variant();
}

void setField(const std::shared_ptr<RsData>& rs, long long idx, const Variant& v) {
    if (!rs->isOpen) throwErr(3704, "Operation is not allowed when the object is closed");
    if (!rs->valid() && !rs->adding)
        throwErr(5, "No current record — call AddNew or move to a record first");
    if (!rs->editing) {
        rs->editing = true;
        rs->editRowId = rs->valid() ? rs->rowids[(size_t)rs->pos] : 0;
        rs->editVals = rs->valid() ? rs->rows[(size_t)rs->pos] : std::vector<Variant>(rs->cols.size(), Variant::mkNull());
        rs->editDirty.assign(rs->cols.size(), 0);
    }
    rs->editVals[(size_t)idx] = v;
    rs->editDirty[(size_t)idx] = 1;
}

void rsUpdate(const std::shared_ptr<RsData>& rs) {
    if (!rs->isOpen) throwErr(3704, "Operation is not allowed when the object is closed");
    if (!rs->editing) return;
    if (!rs->updateable || rs->table.empty())
        throwErr(5, "Recordset is read-only (source is not a simple single-table SELECT)");
    DbConn& c = *rs->conn;
    if (rs->adding) {
        std::string sql = "INSERT INTO " + quoteIdent(rs->table) + " (";
        for (size_t i = 0; i < rs->cols.size(); i++) { if (i) sql += ", "; sql += quoteIdent(rs->cols[i]); }
        sql += ") VALUES (";
        for (size_t i = 0; i < rs->cols.size(); i++) { if (i) sql += ", "; sql += "?"; }
        sql += ")";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(c.db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) sqlErr(c.db, "SQLite error: ");
        for (size_t i = 0; i < rs->cols.size(); i++) bindVariant(st, (int)i + 1, rs->editVals[i]);
        int rc = sqlite3_step(st);
        if (rc != SQLITE_DONE) { sqlite3_finalize(st); sqlErr(c.db, "SQLite error: "); }
        sqlite3_finalize(st);
        long long rid = sqlite3_last_insert_rowid(c.db);
        rs->rows.push_back(rs->editVals);
        rs->rowids.push_back(rid);
        rs->pos = (long long)rs->rows.size() - 1;
    } else {
        std::string sets;
        int n = 0;
        for (size_t i = 0; i < rs->cols.size(); i++)
            if (rs->editDirty[i]) { if (n++) sets += ", "; sets += quoteIdent(rs->cols[i]) + " = ?"; }
        if (n == 0) { rs->editing = false; rs->adding = false; return; } // nothing changed
        std::string sql = "UPDATE " + quoteIdent(rs->table) + " SET " + sets + " WHERE rowid = ?";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(c.db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) sqlErr(c.db, "SQLite error: ");
        int idx = 1;
        for (size_t i = 0; i < rs->cols.size(); i++)
            if (rs->editDirty[i]) bindVariant(st, idx++, rs->editVals[i]);
        sqlite3_bind_int64(st, idx, rs->editRowId);
        int rc = sqlite3_step(st);
        if (rc != SQLITE_DONE) { sqlite3_finalize(st); sqlErr(c.db, "SQLite error: "); }
        sqlite3_finalize(st);
        if (rs->valid()) rs->rows[(size_t)rs->pos] = rs->editVals;
    }
    rs->editing = false;
    rs->adding = false;
    rs->editVals.clear();
    rs->editDirty.clear();
}

void rsDelete(const std::shared_ptr<RsData>& rs) {
    if (!rs->isOpen) throwErr(3704, "Operation is not allowed when the object is closed");
    if (!rs->valid()) throwErr(5, "No current record to delete");
    if (!rs->updateable || rs->table.empty())
        throwErr(5, "Recordset is read-only (source is not a simple single-table SELECT)");
    std::string sql = "DELETE FROM " + quoteIdent(rs->table) + " WHERE rowid = ?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(rs->conn->db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
        sqlErr(rs->conn->db, "SQLite error: ");
    sqlite3_bind_int64(st, 1, rs->rowids[(size_t)rs->pos]);
    int rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) { sqlite3_finalize(st); sqlErr(rs->conn->db, "SQLite error: "); }
    sqlite3_finalize(st);
    rs->rows.erase(rs->rows.begin() + rs->pos);
    rs->rowids.erase(rs->rowids.begin() + rs->pos);
    if (rs->pos >= (long long)rs->rows.size()) rs->pos = (long long)rs->rows.size() - 1;
    if (rs->pos < 0) rs->pos = 0;
    rs->editing = false;
    rs->adding = false;
    rs->editVals.clear();
    rs->editDirty.clear();
}

// ---------- field / fields ----------

struct AdoField : VBObject {
    std::shared_ptr<RsData> rs;
    long long idx;
    AdoField(std::shared_ptr<RsData> r, long long i) : rs(std::move(r)), idx(i) {}
    bool hasMember(const std::string& n) const override {
        return n == "name" || n == "value" || n == "type" || n == "actualsize" || n == "definedsize";
    }
    Variant getMember(const std::string& n) override {
        if (n == "name") return Variant::mkStr(rs->cols[(size_t)idx]);
        if (n == "value") return fieldValue(rs, idx);
        if (n == "type") {
            long long t = rs->colTypes[(size_t)idx];
            return Variant::mkLong(t < 0 ? 200 : t);
        }
        if (n == "actualsize") {
            Variant v = fieldValue(rs, idx);
            if (v.t == VT::String) return Variant::mkLong((long long)v.s.size());
            if (v.t == VT::Array && v.arr) return Variant::mkLong((long long)v.arr->data.size());
            if (v.t == VT::Null || v.t == VT::Empty) return Variant::mkInt(-1);
            return Variant::mkInt(0);
        }
        if (n == "definedsize") return Variant::mkInt(-1);
        throwErr(438, errDesc(438));
    }
    void setMember(const std::string& n, const Variant& v) override {
        if (n == "value") { setField(rs, idx, v); return; }
        throwErr(438, errDesc(438));
    }
    std::string objectKind() const override { return "ADODB.Field"; }
};

struct AdoFields : VBObject {
    std::shared_ptr<RsData> rs;
    explicit AdoFields(std::shared_ptr<RsData> r) : rs(std::move(r)) {}
    bool hasMember(const std::string& n) const override { return n == "count" || n == "item"; }
    bool hasMethod(const std::string& n) const override { return n == "item"; }
    Variant getMember(const std::string& n) override {
        if (n == "count") return Variant::mkLong((long long)rs->cols.size());
        throwErr(438, errDesc(438));
    }
    Variant item(const std::vector<Variant>& args) {
        long long i = fieldIndex(rs, args.empty() ? Variant::mkInt(0) : args[0]);
        return Variant::mkObj(std::make_shared<AdoField>(rs, i));
    }
    Variant callMember(const std::string& n, std::vector<Variant>& args) override {
        if (n == "item") return item(args);
        throwErr(438, errDesc(438));
    }
    Variant invoke(const std::vector<Variant>& args) override { return item(args); }
    std::vector<Variant> enumItems() override {
        std::vector<Variant> out;
        for (size_t i = 0; i < rs->cols.size(); i++) out.push_back(Variant::mkObj(std::make_shared<AdoField>(rs, (long long)i)));
        return out;
    }
    std::string objectKind() const override { return "ADODB.Fields"; }
};

// ---------- recordset ----------

struct AdoRecordset : MapObject {
    std::shared_ptr<RsData> d;
    explicit AdoRecordset(std::shared_ptr<RsData> data) : d(std::move(data)) { kind = "ADODB.Recordset"; }

    bool hasMember(const std::string& n) const override {
        static const char* ms[] = {"bof","eof","recordcount","fields","absoluteposition",
            "activeconnection","source","state","cursortype","locktype","bookmark","editmode"};
        for (auto m : ms) if (n == m) return true;
        return MapObject::hasMember(n);
    }
    bool hasMethod(const std::string& n) const override {
        static const char* ms[] = {"open","close","movenext","moveprevious","movefirst","movelast",
            "move","addnew","update","delete","cancelupdate","requery","getrows","getstring","nextrecordset","clone","supports","cancel"};
        for (auto m : ms) if (n == m) return true;
        return MapObject::hasMethod(n);
    }
    Variant getMember(const std::string& n) override {
        if (n == "bof") return Variant::mkBool(d->bof());
        if (n == "eof") return Variant::mkBool(d->eof());
        if (n == "recordcount") return Variant::mkLong(d->isOpen ? (long long)d->rows.size() : 0);
        if (n == "fields") return Variant::mkObj(std::make_shared<AdoFields>(d));
        if (n == "absoluteposition") {
            if (!d->isOpen || d->rows.empty()) return Variant::mkInt(-1); // adPosUnknown
            if (d->bof()) return Variant::mkInt(-2); // adPosBOF
            if (d->eof()) return Variant::mkInt(-3); // adPosEOF
            return Variant::mkLong(d->pos + 1);
        }
        if (n == "activeconnection") {
            if (d->conn) {
                auto o = findConnObj(d->conn);
                if (o) return Variant::mkObj(o);
            }
            return Variant::mkNull();
        }
        if (n == "source") return Variant::mkStr(d->source);
        if (n == "state") return Variant::mkInt(d->isOpen ? 1 : 0);
        if (n == "cursortype") return Variant::mkLong(d->cursorType);
        if (n == "locktype") return Variant::mkLong(d->lockType);
        if (n == "bookmark") {
            if (!d->valid()) return Variant();
            return Variant::mkLong(d->rowids[(size_t)d->pos] ? d->rowids[(size_t)d->pos] : d->pos + 1);
        }
        if (n == "editmode") return Variant::mkInt(d->adding ? 2 : (d->editing ? 1 : 0));
        return MapObject::getMember(n);
    }
    void setMember(const std::string& n, const Variant& v) override {
        if (n == "absoluteposition") {
            if (!d->isOpen) throwErr(3704, "Operation is not allowed when the object is closed");
            long long p = toInt64(v);
            if (p == -2) d->pos = -1;
            else if (p == -3) d->pos = (long long)d->rows.size();
            else {
                if (p < 1 || p > (long long)d->rows.size()) throwErr(5, errDesc(5));
                d->pos = p - 1;
            }
            d->editing = false; d->adding = false;
            return;
        }
        if (n == "activeconnection") {
            if (v.t == VT::String) {
                auto own = std::make_shared<DbConn>();
                connOpen(own, toStr(v));
                d->ownConn = own;
                d->conn = own;
            } else if (v.t == VT::Object && v.obj) {
                auto c = std::dynamic_pointer_cast<AdoConnection>(v.obj);
                if (!c) throwErr(5, "ActiveConnection must be an ADODB.Connection");
                d->conn = c->d;
            } else if (v.t == VT::Null) {
                d->conn.reset();
            } else throwErr(5, errDesc(5));
            return;
        }
        if (n == "source") { d->source = toStr(v); return; }
        MapObject::setMember(n, v);
    }
    Variant invoke(const std::vector<Variant>& args) override { // rs("col") / rs(0)
        if (args.empty()) throwErr(450, errDesc(450));
        return fieldValue(d, fieldIndex(d, args[0]));
    }
    void assignInvoke(const std::vector<Variant>& args, const Variant& v) override { // rs("col") = v
        if (args.empty()) throwErr(450, errDesc(450));
        setField(d, fieldIndex(d, args[0]), v);
    }
};

std::shared_ptr<AdoRecordset> wireRs(std::shared_ptr<RsData> data); // fwd: defined with the factory below

std::shared_ptr<AdoRecordset> rsFromExecute(std::shared_ptr<DbConn> conn, const std::string& sql) {
    auto rs = std::make_shared<RsData>();
    rs->conn = conn;
    rsRunQuery(rs, sql);
    return wireRs(rs);
}

// ---------- command / parameters ----------

struct ParamData {
    std::string name;
    long long type = 200;
    long long direction = 1;
    long long size = 0;
    Variant value;
};

struct AdoParameter : VBObject {
    std::shared_ptr<ParamData> d;
    explicit AdoParameter(std::shared_ptr<ParamData> p) : d(std::move(p)) {}
    bool hasMember(const std::string& n) const override {
        return n == "name" || n == "value" || n == "type" || n == "direction" || n == "size" || n == "attributes";
    }
    Variant getMember(const std::string& n) override {
        if (n == "name") return Variant::mkStr(d->name);
        if (n == "value") return d->value;
        if (n == "type") return Variant::mkLong(d->type);
        if (n == "direction") return Variant::mkLong(d->direction);
        if (n == "size") return Variant::mkLong(d->size);
        if (n == "attributes") return Variant::mkInt(0);
        throwErr(438, errDesc(438));
    }
    void setMember(const std::string& n, const Variant& v) override {
        if (n == "name") { d->name = toStr(v); return; }
        if (n == "value") { d->value = v; return; }
        if (n == "type") { d->type = toInt64(v); return; }
        if (n == "direction") { d->direction = toInt64(v); return; }
        if (n == "size") { d->size = toInt64(v); return; }
        throwErr(438, errDesc(438));
    }
    std::string objectKind() const override { return "ADODB.Parameter"; }
};

struct AdoParameters : VBObject {
    std::vector<std::shared_ptr<ParamData>>* items;
    explicit AdoParameters(std::vector<std::shared_ptr<ParamData>>* v) : items(v) {}
    bool hasMember(const std::string& n) const override { return n == "count" || n == "item"; }
    bool hasMethod(const std::string& n) const override { return n == "item" || n == "append" || n == "delete"; }
    Variant getMember(const std::string& n) override {
        if (n == "count") return Variant::mkLong((long long)items->size());
        throwErr(438, errDesc(438));
    }
    long long indexOf(const Variant& key) {
        if (key.t == VT::String) {
            std::string nm = lower(key.s);
            for (size_t i = 0; i < items->size(); i++)
                if (lower((*items)[i]->name) == nm) return (long long)i;
            throwErr(3265, "Item cannot be found in the collection corresponding to the requested name or ordinal");
        }
        long long i = toInt64(key);
        if (i < 0 || (size_t)i >= items->size()) throwErr(9, errDesc(9));
        return i;
    }
    Variant callMember(const std::string& n, std::vector<Variant>& args) override {
        if (n == "item") return Variant::mkObj(std::make_shared<AdoParameter>((*items)[(size_t)indexOf(args.empty() ? Variant::mkInt(0) : args[0])]));
        if (n == "append") {
            if (args.empty() || args[0].t != VT::Object || !args[0].obj) throwErr(450, errDesc(450));
            auto p = std::dynamic_pointer_cast<AdoParameter>(args[0].obj);
            if (!p) throwErr(5, errDesc(5));
            items->push_back(p->d);
            return Variant();
        }
        if (n == "delete") {
            if (args.empty()) throwErr(450, errDesc(450));
            long long i = indexOf(args[0]);
            items->erase(items->begin() + i);
            return Variant();
        }
        throwErr(438, errDesc(438));
    }
    Variant invoke(const std::vector<Variant>& args) override {
        return Variant::mkObj(std::make_shared<AdoParameter>((*items)[(size_t)indexOf(args.empty() ? Variant::mkInt(0) : args[0])]));
    }
    std::vector<Variant> enumItems() override {
        std::vector<Variant> out;
        for (auto& p : *items) out.push_back(Variant::mkObj(std::make_shared<AdoParameter>(p)));
        return out;
    }
    std::string objectKind() const override { return "ADODB.Parameters"; }
};

struct CmdData {
    std::shared_ptr<DbConn> conn;
    std::string text;
    long long cmdType = 1; // adCmdText
    long long cmdTimeout = 30;
    std::vector<std::shared_ptr<ParamData>> params;
};

struct AdoCommand : MapObject {
    std::shared_ptr<CmdData> d;
    explicit AdoCommand(std::shared_ptr<CmdData> data) : d(std::move(data)) { kind = "ADODB.Command"; }
    bool hasMember(const std::string& n) const override {
        static const char* ms[] = {"activeconnection","commandtext","commandtype","commandtimeout",
            "state","prepared","parameters","commandstream","dialect","namedparameters"};
        for (auto m : ms) if (n == m) return true;
        return MapObject::hasMember(n);
    }
    bool hasMethod(const std::string& n) const override {
        static const char* ms[] = {"execute","createparameter","cancel"};
        for (auto m : ms) if (n == m) return true;
        return MapObject::hasMethod(n);
    }
    Variant getMember(const std::string& n) override {
        if (n == "commandtext") return Variant::mkStr(d->text);
        if (n == "commandtype") return Variant::mkLong(d->cmdType);
        if (n == "commandtimeout") return Variant::mkLong(d->cmdTimeout);
        if (n == "state") return Variant::mkInt(d->conn && d->conn->isOpen ? 1 : 0);
        if (n == "prepared") return Variant::mkBool(true);
        if (n == "parameters") return Variant::mkObj(std::make_shared<AdoParameters>(&d->params));
        if (n == "namedparameters") return Variant::mkBool(true);
        if (n == "activeconnection") {
            if (d->conn) {
                auto o = findConnObj(d->conn);
                if (o) return Variant::mkObj(o);
            }
            return Variant::mkNull();
        }
        return MapObject::getMember(n);
    }
    void setMember(const std::string& n, const Variant& v) override {
        if (n == "commandtext") { d->text = toStr(v); return; }
        if (n == "commandtype") { d->cmdType = toInt64(v); return; }
        if (n == "commandtimeout") { d->cmdTimeout = toInt64(v); return; }
        if (n == "prepared") { return; }
        if (n == "activeconnection") {
            if (v.t == VT::String) {
                auto own = std::make_shared<DbConn>();
                connOpen(own, toStr(v));
                d->conn = own;
            } else if (v.t == VT::Object && v.obj) {
                auto c = std::dynamic_pointer_cast<AdoConnection>(v.obj);
                if (!c) throwErr(5, "ActiveConnection must be an ADODB.Connection");
                d->conn = c->d;
            } else throwErr(5, errDesc(5));
            return;
        }
        MapObject::setMember(n, v);
    }
};

} // namespace

std::shared_ptr<VBObject> makeAdoConnection(Interp&) {
    auto data = std::make_shared<DbConn>();
    auto o = std::make_shared<AdoConnection>(data);
    connRegistry().emplace_back(data, std::static_pointer_cast<VBObject>(o));

    o->methods["open"] = [data](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.empty()) throwErr(450, errDesc(450));
        connOpen(data, toStr(a[0])); // user/password/options accepted, ignored
        return Variant();
    };
    o->methods["close"] = [data](MapObject*, std::vector<Variant>&) -> Variant {
        connClose(data);
        return Variant();
    };
    o->methods["execute"] = [data](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.empty()) throwErr(450, errDesc(450));
        if (!data->isOpen) throwErr(3704, "Operation is not allowed when the object is closed");
        // recordsAffected out-param (a[1]) not supported: no ByRef for host methods
        return Variant::mkObj(rsFromExecute(data, toStr(a[0])));
    };
    o->methods["begintrans"] = [data](MapObject*, std::vector<Variant>&) -> Variant {
        if (!data->isOpen) throwErr(3704, "Operation is not allowed when the object is closed");
        if (data->transLevel == 0) dbExec(*data, "BEGIN");
        else dbExec(*data, "SAVEPOINT sp" + std::to_string(data->transLevel));
        data->transLevel++;
        return Variant::mkLong(data->transLevel);
    };
    o->methods["committrans"] = [data](MapObject*, std::vector<Variant>&) -> Variant {
        if (!data->isOpen || data->transLevel == 0) throwErr(5, "No transaction is active");
        if (data->transLevel == 1) dbExec(*data, "COMMIT");
        else dbExec(*data, "RELEASE sp" + std::to_string(data->transLevel - 1));
        data->transLevel--;
        return Variant();
    };
    o->methods["rollbacktrans"] = [data](MapObject*, std::vector<Variant>&) -> Variant {
        if (!data->isOpen || data->transLevel == 0) throwErr(5, "No transaction is active");
        if (data->transLevel == 1) dbExec(*data, "ROLLBACK");
        else {
            dbExec(*data, "ROLLBACK TO sp" + std::to_string(data->transLevel - 1));
            dbExec(*data, "RELEASE sp" + std::to_string(data->transLevel - 1));
        }
        data->transLevel--;
        return Variant();
    };
    o->methods["cancel"] = [](MapObject*, std::vector<Variant>&) -> Variant { return Variant(); };
    o->methods["openschema"] = [](MapObject*, std::vector<Variant>&) -> Variant {
        throwErr(5, "OpenSchema is not supported");
    };
    return o;
}

namespace {
std::shared_ptr<AdoRecordset> wireRs(std::shared_ptr<RsData> data) {
    auto o = std::make_shared<AdoRecordset>(data);

    o->methods["open"] = [data](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.empty()) throwErr(450, errDesc(450));
        std::string sql = toStr(a[0]);
        if (a.size() > 1 && a[1].t != VT::Empty && a[1].t != VT::Null) {
            if (a[1].t == VT::Object && a[1].obj) {
                auto c = std::dynamic_pointer_cast<AdoConnection>(a[1].obj);
                if (!c) throwErr(5, "ActiveConnection must be an ADODB.Connection");
                data->conn = c->d;
            } else {
                auto own = std::make_shared<DbConn>();
                connOpen(own, toStr(a[1]));
                data->ownConn = own;
                data->conn = own;
            }
        }
        if (a.size() > 2) data->cursorType = toInt64(a[2]);
        if (a.size() > 3) data->lockType = toInt64(a[3]);
        rsRunQuery(data, sql); // a[4] options ignored
        return Variant();
    };
    o->methods["close"] = [data](MapObject*, std::vector<Variant>&) -> Variant {
        data->isOpen = false;
        data->editing = false; data->adding = false;
        return Variant();
    };
    auto needOpen = [data]() {
        if (!data->isOpen) throwErr(3704, "Operation is not allowed when the object is closed");
    };
    o->methods["movenext"] = [data, needOpen](MapObject*, std::vector<Variant>&) -> Variant {
        needOpen(); data->pos++; data->editing = false; data->adding = false;
        return Variant();
    };
    o->methods["moveprevious"] = [data, needOpen](MapObject*, std::vector<Variant>&) -> Variant {
        needOpen(); data->pos--; data->editing = false; data->adding = false;
        return Variant();
    };
    o->methods["movefirst"] = [data, needOpen](MapObject*, std::vector<Variant>&) -> Variant {
        needOpen(); data->pos = 0; data->editing = false; data->adding = false;
        return Variant();
    };
    o->methods["movelast"] = [data, needOpen](MapObject*, std::vector<Variant>&) -> Variant {
        needOpen(); data->pos = (long long)data->rows.size() - 1; data->editing = false; data->adding = false;
        return Variant();
    };
    o->methods["move"] = [data, needOpen](MapObject*, std::vector<Variant>& a) -> Variant {
        needOpen();
        if (a.empty()) throwErr(450, errDesc(450));
        data->pos += toInt64(a[0]);
        if (data->pos < -1) data->pos = -1;
        if (data->pos > (long long)data->rows.size()) data->pos = (long long)data->rows.size();
        data->editing = false; data->adding = false;
        return Variant();
    };
    o->methods["addnew"] = [data, needOpen](MapObject*, std::vector<Variant>&) -> Variant {
        needOpen();
        data->adding = true;
        data->editing = true;
        data->editRowId = 0;
        data->editVals.assign(data->cols.size(), Variant::mkNull());
        data->editDirty.assign(data->cols.size(), 0);
        return Variant();
    };
    o->methods["update"] = [data](MapObject*, std::vector<Variant>&) -> Variant {
        rsUpdate(data);
        return Variant();
    };
    o->methods["delete"] = [data](MapObject*, std::vector<Variant>&) -> Variant {
        rsDelete(data);
        return Variant();
    };
    o->methods["cancelupdate"] = [data](MapObject*, std::vector<Variant>&) -> Variant {
        data->editing = false; data->adding = false;
        data->editVals.clear(); data->editDirty.clear();
        return Variant();
    };
    o->methods["requery"] = [data](MapObject*, std::vector<Variant>&) -> Variant {
        rsRunQuery(data, data->source);
        return Variant();
    };
    o->methods["getrows"] = [data, needOpen](MapObject*, std::vector<Variant>& a) -> Variant {
        needOpen();
        long long maxRows = (a.empty() || toInt64(a[0]) < 0) ? -1 : toInt64(a[0]);
        long long start = data->valid() ? data->pos : (long long)data->rows.size();
        long long n = (long long)data->rows.size() - start;
        if (maxRows >= 0 && n > maxRows) n = maxRows;
        if (n < 0) n = 0;
        size_t nf = data->cols.size();
        auto arr = std::make_shared<VArray>();
        arr->base = 0;
        arr->dims = { (long long)nf, n }; // ADO GetRows: (field, record)
        arr->data.resize(nf * (size_t)n);
        for (long long r = 0; r < n; r++)
            for (size_t f = 0; f < nf; f++)
                arr->data[f * (size_t)n + (size_t)r] = data->rows[(size_t)(start + r)][f];
        return Variant::mkArr(arr);
    };
    o->methods["getstring"] = [data, needOpen](MapObject*, std::vector<Variant>&) -> Variant {
        needOpen();
        // StringFormatEnum adClipString (2): rows joined by recordSep (\n), cols by colSep (\t)
        std::string out;
        long long start = data->valid() ? data->pos : (long long)data->rows.size();
        for (size_t r = (size_t)start; r < data->rows.size(); r++) {
            if (r > (size_t)start) out += "\n";
            for (size_t f = 0; f < data->cols.size(); f++) {
                if (f) out += "\t";
                out += toStrLenient(data->rows[r][f]);
            }
        }
        return Variant::mkStr(out);
    };
    o->methods["nextrecordset"] = [](MapObject*, std::vector<Variant>&) -> Variant { return Variant::mkObj(nullptr); };
    o->methods["clone"] = [](MapObject*, std::vector<Variant>&) -> Variant { throwErr(5, "Clone is not supported"); };
    o->methods["supports"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        // adHoldRecords 256, adMovePrevious 512? approximate: support the basics
        if (a.empty()) return Variant::mkBool(true);
        long long f = toInt64(a[0]);
        return Variant::mkBool(f == 0x100 || f == 0x200 || f == 0x400 || f == 0x1000 || f == 0x2000 || f == 0x4000 || f == 0x8000);
    };
    o->methods["cancel"] = [](MapObject*, std::vector<Variant>&) -> Variant { return Variant(); };
    return o;
}
} // namespace

std::shared_ptr<VBObject> makeAdoRecordset(Interp&) {
    return wireRs(std::make_shared<RsData>());
}

std::shared_ptr<VBObject> makeAdoCommand(Interp&) {
    auto data = std::make_shared<CmdData>();
    auto o = std::make_shared<AdoCommand>(data);

    o->methods["createparameter"] = [data](MapObject*, std::vector<Variant>& a) -> Variant {
        // (name, type, direction, size, value) — all but name optional
        auto p = std::make_shared<ParamData>();
        if (!a.empty()) p->name = toStr(a[0]);
        if (a.size() > 1) p->type = toInt64(a[1]);
        if (a.size() > 2) p->direction = toInt64(a[2]);
        if (a.size() > 3) p->size = toInt64(a[3]);
        if (a.size() > 4) p->value = a[4];
        data->params.push_back(p);
        return Variant::mkObj(std::make_shared<AdoParameter>(p));
    };
    o->methods["execute"] = [data](MapObject*, std::vector<Variant>& a) -> Variant {
        if (!data->conn || !data->conn->isOpen) throwErr(3704, "Operation is not allowed when the object is closed");
        if (data->cmdType == 4) throwErr(5, "Stored procedures are not supported by the SQLite3 provider");
        std::string sql = data->cmdType == 2 ? "SELECT * FROM " + quoteIdent(data->text) : data->text;
        // a[0] recordsAffected (not supported), a[1] parameters array, a[2] options
        const std::vector<Variant>* extra = nullptr;
        std::vector<Variant> arrCopy;
        if (a.size() > 1 && a[1].t == VT::Array && a[1].arr) {
            arrCopy.assign(a[1].arr->data.begin(), a[1].arr->data.end());
            extra = &arrCopy;
        }
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(data->conn->db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
            sqlErr(data->conn->db, "SQLite error: ");
        int np = sqlite3_bind_parameter_count(st);
        for (int i = 1; i <= np; i++) {
            std::string pname = sqlite3_bind_parameter_name(st, i) ? sqlite3_bind_parameter_name(st, i) : "";
            Variant val = Variant::mkNull();
            bool found = false;
            if (pname.empty() || pname[0] == '?') {
                // positional ? / ?NNN / anonymous: bind by ordinal
                long long ord = i;
                if (extra && (size_t)(ord - 1) < extra->size()) { val = (*extra)[(size_t)(ord - 1)]; found = true; }
                else if ((size_t)(ord - 1) < data->params.size()) { val = data->params[(size_t)(ord - 1)]->value; found = true; }
            } else if (!pname.empty()) {
                std::string nm = lower(pname.substr(1)); // strip : @ $
                if (extra) { /* array form binds positionally only */ }
                for (auto& p : data->params)
                    if (lower(p->name) == nm) { val = p->value; found = true; break; }
            }
            if (!found) {
                sqlite3_finalize(st);
                throwErr(5, "Missing value for parameter " + pname);
            }
            bindVariant(st, i, val);
        }
        if (sqlite3_column_count(st) == 0) {
            int rc = sqlite3_step(st);
            if (rc != SQLITE_DONE) { sqlite3_finalize(st); sqlErr(data->conn->db, "SQLite error: "); }
            sqlite3_finalize(st);
            auto rs = std::make_shared<RsData>();
            rs->conn = data->conn;
            rs->isOpen = true;
            return Variant::mkObj(wireRs(rs));
        }
        // materialize like a recordset query
        auto rs = std::make_shared<RsData>();
        rs->conn = data->conn;
        rs->source = sql;
        rs->updateable = false;
        int ncols = sqlite3_column_count(st);
        for (int i = 0; i < ncols; i++) {
            rs->cols.push_back(sqlite3_column_name(st, i));
            rs->colTypes.push_back(declTypeToAdo(sqlite3_column_decltype(st, i)));
        }
        while (sqlite3_step(st) == SQLITE_ROW) {
            std::vector<Variant> row;
            for (int i = 0; i < ncols; i++) {
                long long ado = rs->colTypes[(size_t)i];
                if (ado < 0) ado = sqlite3_column_type(st, i) == SQLITE_INTEGER ? 3 :
                                  sqlite3_column_type(st, i) == SQLITE_FLOAT ? 5 :
                                  sqlite3_column_type(st, i) == SQLITE_TEXT ? 200 :
                                  sqlite3_column_type(st, i) == SQLITE_BLOB ? 205 : 0;
                row.push_back(colValue(st, i, ado));
            }
            if (rs->rows.empty())
                for (size_t c = 0; c < rs->cols.size(); c++)
                    if (rs->colTypes[c] < 0) {
                        const Variant& v = row[c];
                        rs->colTypes[c] = v.t == VT::Long || v.t == VT::Int ? 3 :
                                           v.t == VT::Double || v.t == VT::Date ? 5 :
                                           v.t == VT::String ? 200 :
                                           v.t == VT::Array ? 205 : v.t == VT::Bool ? 11 : 0;
                    }
            rs->rows.push_back(std::move(row));
            rs->rowids.push_back(0);
        }
        sqlite3_finalize(st);
        rs->pos = 0;
        rs->isOpen = true;
        return Variant::mkObj(wireRs(rs));
    };
    o->methods["cancel"] = [](MapObject*, std::vector<Variant>&) -> Variant { return Variant(); };
    return o;
}
