// index/src/tenant_store.cpp — see tenant_store.h
//
// SQLite is linked in as the only new dependency (system sqlite3, linked
// via find_package(SQLite3)).  The embedded DB file is single-writer
// friendly and sufficient for the single-binary deploy profile; when we
// outgrow it (multi-node, shared state), swap the connection layer for
// Postgres without rewriting the schema.

#include "tenant_store.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include <openssl/rand.h>
#include <openssl/sha.h>
#include <sqlite3.h>

namespace index_ai {

namespace {

// ─── Helpers ────────────────────────────────────────────────────────────────

int64_t now_epoch() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Current UTC month as "YYYY-MM".  Used to detect rollover.
std::string current_month_utc() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d", tm.tm_year + 1900, tm.tm_mon + 1);
    return buf;
}

std::string bytes_to_hex(const unsigned char* data, size_t len) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i) ss << std::setw(2) << static_cast<int>(data[i]);
    return ss.str();
}

std::string sha256_hex(const std::string& input) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.data()),
           input.size(), digest);
    return bytes_to_hex(digest, SHA256_DIGEST_LENGTH);
}

std::string generate_token() {
    // 32 random bytes → 64 hex chars.  Prefixed "atr_" (arbiter) so it's
    // recognizable in logs without shape ambiguity with other keys.
    unsigned char buf[32];
    if (RAND_bytes(buf, sizeof(buf)) != 1)
        throw std::runtime_error("CSPRNG failure generating tenant token");
    return "atr_" + bytes_to_hex(buf, sizeof(buf));
}

void check_sqlite(sqlite3* db, int rc, const std::string& ctx) {
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
        std::string msg = ctx + ": ";
        msg += sqlite3_errmsg(db);
        throw std::runtime_error(msg);
    }
}

// Thin RAII wrapper for prepared statements.
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) : db_(db) {
        int rc = sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr);
        check_sqlite(db, rc, std::string("prepare: ") + sql);
    }
    ~Stmt() { if (stmt_) sqlite3_finalize(stmt_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    void bind(int idx, int64_t v)              { sqlite3_bind_int64(stmt_, idx, v); }
    void bind(int idx, const std::string& v)   {
        sqlite3_bind_text(stmt_, idx, v.c_str(), -1, SQLITE_TRANSIENT);
    }
    void bind(int idx, std::nullptr_t)         { sqlite3_bind_null(stmt_, idx); }

    int step() { return sqlite3_step(stmt_); }

    int64_t column_int64(int idx) const { return sqlite3_column_int64(stmt_, idx); }
    std::string column_text(int idx) const {
        auto* p = sqlite3_column_text(stmt_, idx);
        return p ? std::string(reinterpret_cast<const char*>(p)) : std::string();
    }

    sqlite3_stmt* raw() { return stmt_; }
private:
    sqlite3*      db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

void exec_sql(sqlite3* db, const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = std::string("exec: ") + (err ? err : "(unknown)");
        if (err) sqlite3_free(err);
        throw std::runtime_error(msg);
    }
}

Tenant row_to_tenant(Stmt& q) {
    // Column order must match the SELECT lists below.
    Tenant t;
    t.id                = q.column_int64(0);
    t.api_key_hash      = q.column_text(1);
    t.name              = q.column_text(2);
    t.monthly_cap_uc    = q.column_int64(3);
    t.month_yyyymm      = q.column_text(4);
    t.month_to_date_uc  = q.column_int64(5);
    t.disabled          = q.column_int64(6) != 0;
    t.created_at        = q.column_int64(7);
    t.last_used_at      = q.column_int64(8);
    return t;
}

constexpr const char* kTenantCols =
    "id, api_key_hash, name, monthly_cap_uc, month_yyyymm, "
    "month_to_date_uc, disabled, created_at, last_used_at";

} // namespace

// ─── TenantStore ────────────────────────────────────────────────────────────

TenantStore::~TenantStore() {
    if (db_) sqlite3_close(db_);
}

void TenantStore::open(const std::string& path) {
    if (db_) return;   // idempotent; caller re-opening the same instance is a no-op
    int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::string msg = "sqlite3_open: " + std::string(sqlite3_errmsg(db_));
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error(msg);
    }

    // WAL gives us concurrent readers while the writer appends.  foreign_keys
    // is off by default in SQLite — turn it on so usage_log's FK is enforced.
    exec_sql(db_, "PRAGMA journal_mode = WAL;");
    exec_sql(db_, "PRAGMA foreign_keys = ON;");
    exec_sql(db_, "PRAGMA busy_timeout = 5000;");

    // Schema migrations.  Additive only — each CREATE IF NOT EXISTS is
    // safe on an existing DB; new columns need their own ALTER.
    exec_sql(db_, R"SQL(
        CREATE TABLE IF NOT EXISTS tenants (
            id                   INTEGER PRIMARY KEY AUTOINCREMENT,
            api_key_hash         TEXT    UNIQUE NOT NULL,
            name                 TEXT    NOT NULL,
            monthly_cap_uc       INTEGER NOT NULL DEFAULT 0,
            month_yyyymm         TEXT    NOT NULL DEFAULT '',
            month_to_date_uc     INTEGER NOT NULL DEFAULT 0,
            disabled             INTEGER NOT NULL DEFAULT 0,
            created_at           INTEGER NOT NULL,
            last_used_at         INTEGER NOT NULL DEFAULT 0
        );
    )SQL");
    exec_sql(db_, R"SQL(
        CREATE TABLE IF NOT EXISTS usage_log (
            id                   INTEGER PRIMARY KEY AUTOINCREMENT,
            tenant_id            INTEGER NOT NULL,
            timestamp            INTEGER NOT NULL,
            model                TEXT    NOT NULL,
            input_tokens         INTEGER NOT NULL,
            output_tokens        INTEGER NOT NULL,
            cache_read_tokens    INTEGER NOT NULL DEFAULT 0,
            provider_uc          INTEGER NOT NULL,
            markup_uc            INTEGER NOT NULL,
            request_id           TEXT,
            FOREIGN KEY (tenant_id) REFERENCES tenants(id)
        );
    )SQL");

    // Per-token-type cost breakdown columns added in v2.  ALTER TABLE ADD
    // COLUMN is idempotent only if we first check existence — SQLite
    // doesn't have IF NOT EXISTS for ALTER.  Walk PRAGMA table_info() and
    // add each missing column individually so re-opens against an old DB
    // upgrade cleanly without erroring on duplicates.
    auto column_exists = [this](const char* col) -> bool {
        Stmt q(db_, "PRAGMA table_info(usage_log);");
        while (q.step() == SQLITE_ROW) {
            if (q.column_text(1) == col) return true;
        }
        return false;
    };
    auto add_column = [this, &column_exists](const char* col, const char* defn) {
        if (column_exists(col)) return;
        std::string sql = std::string("ALTER TABLE usage_log ADD COLUMN ") +
                          col + " " + defn + ";";
        exec_sql(db_, sql.c_str());
    };
    add_column("cache_create_tokens", "INTEGER NOT NULL DEFAULT 0");
    add_column("input_uc",            "INTEGER NOT NULL DEFAULT 0");
    add_column("output_uc",           "INTEGER NOT NULL DEFAULT 0");
    add_column("cache_read_uc",       "INTEGER NOT NULL DEFAULT 0");
    add_column("cache_create_uc",     "INTEGER NOT NULL DEFAULT 0");

    exec_sql(db_, R"SQL(
        CREATE INDEX IF NOT EXISTS usage_log_tenant_ts
            ON usage_log(tenant_id, timestamp);
    )SQL");
}

TenantStore::CreatedTenant
TenantStore::create_tenant(const std::string& name, int64_t monthly_cap_uc) {
    if (!db_) throw std::runtime_error("TenantStore not opened");

    std::string token = generate_token();
    std::string hash  = sha256_hex(token);
    std::string month = current_month_utc();
    int64_t     ts    = now_epoch();

    Stmt q(db_,
           "INSERT INTO tenants (api_key_hash, name, monthly_cap_uc, "
           "month_yyyymm, month_to_date_uc, disabled, created_at, last_used_at) "
           "VALUES (?, ?, ?, ?, 0, 0, ?, 0);");
    q.bind(1, hash);
    q.bind(2, name);
    q.bind(3, monthly_cap_uc);
    q.bind(4, month);
    q.bind(5, ts);

    int rc = q.step();
    if (rc != SQLITE_DONE) {
        check_sqlite(db_, rc, "insert tenant");
    }

    Tenant t;
    t.id               = sqlite3_last_insert_rowid(db_);
    t.api_key_hash     = hash;
    t.name             = name;
    t.monthly_cap_uc   = monthly_cap_uc;
    t.month_yyyymm     = month;
    t.month_to_date_uc = 0;
    t.disabled         = false;
    t.created_at       = ts;
    t.last_used_at     = 0;
    return {t, token};
}

bool TenantStore::set_cap(int64_t tenant_id, int64_t cap_uc) {
    if (!db_ || tenant_id <= 0 || cap_uc < 0) return false;
    Stmt q(db_, "UPDATE tenants SET monthly_cap_uc = ? WHERE id = ?;");
    q.bind(1, cap_uc);
    q.bind(2, tenant_id);
    q.step();
    return sqlite3_changes(db_) > 0;
}

bool TenantStore::set_disabled(const std::string& key, bool disabled) {
    if (!db_) return false;

    // Try numeric id first, fall back to name match.
    int64_t id = 0;
    try { id = std::stoll(key); } catch (...) { id = 0; }

    if (id > 0) {
        Stmt q(db_, "UPDATE tenants SET disabled = ? WHERE id = ?;");
        q.bind(1, static_cast<int64_t>(disabled ? 1 : 0));
        q.bind(2, id);
        q.step();
        return sqlite3_changes(db_) > 0;
    }
    Stmt q(db_, "UPDATE tenants SET disabled = ? WHERE name = ?;");
    q.bind(1, static_cast<int64_t>(disabled ? 1 : 0));
    q.bind(2, key);
    q.step();
    return sqlite3_changes(db_) > 0;
}

std::optional<Tenant> TenantStore::find_by_token(const std::string& token) {
    if (!db_ || token.empty()) return std::nullopt;
    std::string hash = sha256_hex(token);

    std::string cols = kTenantCols;
    Stmt q(db_, ("SELECT " + cols + " FROM tenants WHERE api_key_hash = ?;").c_str());
    q.bind(1, hash);
    if (q.step() != SQLITE_ROW) return std::nullopt;
    Tenant t = row_to_tenant(q);
    if (t.disabled) return std::nullopt;

    // Touch last_used_at.  Fire-and-forget; failure here shouldn't block auth.
    Stmt u(db_, "UPDATE tenants SET last_used_at = ? WHERE id = ?;");
    u.bind(1, now_epoch());
    u.bind(2, t.id);
    u.step();

    return t;
}

int64_t TenantStore::record_usage(int64_t tenant_id,
                                   const std::string& model,
                                   int input_tokens,
                                   int output_tokens,
                                   int cache_read_tokens,
                                   int cache_create_tokens,
                                   const CostParts& parts,
                                   int64_t markup_uc,
                                   const std::string& request_id) {
    if (!db_) return 0;

    const int64_t provider_uc = parts.input_uc + parts.output_uc +
                                parts.cache_read_uc + parts.cache_create_uc;

    exec_sql(db_, "BEGIN IMMEDIATE;");
    try {
        // Rollover detection — if the stored month differs from the wall
        // clock, zero out MTD and stamp the new month.  Done in the same
        // transaction as the increment so two concurrent calls can't
        // race each other into double-resetting.
        std::string month = current_month_utc();
        {
            Stmt reset(db_,
                "UPDATE tenants "
                "   SET month_yyyymm = ?, month_to_date_uc = 0 "
                " WHERE id = ? AND month_yyyymm != ?;");
            reset.bind(1, month);
            reset.bind(2, tenant_id);
            reset.bind(3, month);
            reset.step();
        }

        int64_t total_uc = provider_uc + markup_uc;
        {
            Stmt inc(db_,
                "UPDATE tenants "
                "   SET month_to_date_uc = month_to_date_uc + ?, "
                "       last_used_at     = ? "
                " WHERE id = ?;");
            inc.bind(1, total_uc);
            inc.bind(2, now_epoch());
            inc.bind(3, tenant_id);
            inc.step();
        }

        {
            Stmt log(db_,
                "INSERT INTO usage_log "
                "(tenant_id, timestamp, model, input_tokens, output_tokens, "
                " cache_read_tokens, cache_create_tokens, "
                " input_uc, output_uc, cache_read_uc, cache_create_uc, "
                " provider_uc, markup_uc, request_id) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
            log.bind(1,  tenant_id);
            log.bind(2,  now_epoch());
            log.bind(3,  model);
            log.bind(4,  static_cast<int64_t>(input_tokens));
            log.bind(5,  static_cast<int64_t>(output_tokens));
            log.bind(6,  static_cast<int64_t>(cache_read_tokens));
            log.bind(7,  static_cast<int64_t>(cache_create_tokens));
            log.bind(8,  parts.input_uc);
            log.bind(9,  parts.output_uc);
            log.bind(10, parts.cache_read_uc);
            log.bind(11, parts.cache_create_uc);
            log.bind(12, provider_uc);
            log.bind(13, markup_uc);
            if (request_id.empty()) log.bind(14, nullptr);
            else                    log.bind(14, request_id);
            log.step();
        }

        // Fetch the post-update MTD for the caller.
        int64_t mtd = 0;
        {
            Stmt r(db_, "SELECT month_to_date_uc FROM tenants WHERE id = ?;");
            r.bind(1, tenant_id);
            if (r.step() == SQLITE_ROW) mtd = r.column_int64(0);
        }

        exec_sql(db_, "COMMIT;");
        return mtd;
    } catch (...) {
        exec_sql(db_, "ROLLBACK;");
        throw;
    }
}

std::vector<Tenant> TenantStore::list_tenants() const {
    std::vector<Tenant> out;
    if (!db_) return out;
    Stmt q(db_, (std::string("SELECT ") + kTenantCols +
                 " FROM tenants ORDER BY id;").c_str());
    while (q.step() == SQLITE_ROW) out.push_back(row_to_tenant(q));
    return out;
}

std::optional<Tenant> TenantStore::get_tenant(int64_t id) const {
    if (!db_) return std::nullopt;
    Stmt q(db_, (std::string("SELECT ") + kTenantCols +
                 " FROM tenants WHERE id = ?;").c_str());
    q.bind(1, id);
    if (q.step() != SQLITE_ROW) return std::nullopt;
    return row_to_tenant(q);
}

std::vector<UsageEntry> TenantStore::list_usage(int64_t tenant_id,
                                                 int64_t since_ts,
                                                 int64_t until_ts,
                                                 int     limit) const {
    std::vector<UsageEntry> out;
    if (!db_) return out;

    // Build WHERE clause dynamically so each filter is optional.  Bind by
    // index in the same order we appended them so no fragile name-to-index
    // mapping is needed.
    std::string sql =
        "SELECT id, tenant_id, timestamp, model, input_tokens, output_tokens, "
        "       cache_read_tokens, cache_create_tokens, "
        "       input_uc, output_uc, cache_read_uc, cache_create_uc, "
        "       provider_uc, markup_uc, request_id "
        "  FROM usage_log";
    std::vector<int64_t> params;
    std::string where;
    auto add_filter = [&](const std::string& clause, int64_t v) {
        if (!where.empty()) where += " AND ";
        where += clause;
        params.push_back(v);
    };
    if (tenant_id > 0)  add_filter("tenant_id = ?", tenant_id);
    if (since_ts  > 0)  add_filter("timestamp >= ?", since_ts);
    if (until_ts  > 0)  add_filter("timestamp <= ?", until_ts);
    if (!where.empty()) sql += " WHERE " + where;

    const int cap = (limit > 0 && limit <= 10000) ? limit : 1000;
    sql += " ORDER BY id DESC LIMIT ?;";

    Stmt q(db_, sql.c_str());
    int idx = 1;
    for (int64_t v : params) q.bind(idx++, v);
    q.bind(idx, static_cast<int64_t>(cap));

    while (q.step() == SQLITE_ROW) {
        UsageEntry e;
        e.id                  = q.column_int64(0);
        e.tenant_id           = q.column_int64(1);
        e.timestamp           = q.column_int64(2);
        e.model               = q.column_text(3);
        e.input_tokens        = q.column_int64(4);
        e.output_tokens       = q.column_int64(5);
        e.cache_read_tokens   = q.column_int64(6);
        e.cache_create_tokens = q.column_int64(7);
        e.input_uc            = q.column_int64(8);
        e.output_uc           = q.column_int64(9);
        e.cache_read_uc       = q.column_int64(10);
        e.cache_create_uc     = q.column_int64(11);
        e.provider_uc         = q.column_int64(12);
        e.markup_uc           = q.column_int64(13);
        e.request_id          = q.column_text(14);
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<TenantStore::UsageBucket>
TenantStore::usage_summary(int64_t tenant_id, int64_t since_ts, int64_t until_ts,
                            const std::string& group_by) const {
    std::vector<UsageBucket> out;
    if (!db_) return out;

    // Build the GROUP BY expression up front.  strftime('%Y-%m-%d', timestamp,
    // 'unixepoch') gives UTC day buckets; model and tenant_id are direct columns.
    std::string group_expr;
    if      (group_by == "day")    group_expr = "strftime('%Y-%m-%d', timestamp, 'unixepoch')";
    else if (group_by == "tenant") group_expr = "CAST(tenant_id AS TEXT)";
    else                           group_expr = "model";

    std::vector<int64_t> params;
    std::string where;
    auto add_filter = [&](const std::string& clause, int64_t v) {
        if (!where.empty()) where += " AND ";
        where += clause;
        params.push_back(v);
    };
    if (tenant_id > 0) add_filter("tenant_id = ?", tenant_id);
    if (since_ts  > 0) add_filter("timestamp >= ?", since_ts);
    if (until_ts  > 0) add_filter("timestamp <= ?", until_ts);

    std::string sql =
        "SELECT " + group_expr + " AS bucket, "
        "       COUNT(*),"
        "       SUM(input_tokens), SUM(output_tokens),"
        "       SUM(cache_read_tokens), SUM(cache_create_tokens),"
        "       SUM(input_uc), SUM(output_uc),"
        "       SUM(cache_read_uc), SUM(cache_create_uc),"
        "       SUM(provider_uc), SUM(markup_uc) "
        "  FROM usage_log";
    if (!where.empty()) sql += " WHERE " + where;
    sql += " GROUP BY bucket ORDER BY SUM(provider_uc) DESC;";

    Stmt q(db_, sql.c_str());
    int idx = 1;
    for (int64_t v : params) q.bind(idx++, v);

    while (q.step() == SQLITE_ROW) {
        UsageBucket b;
        b.key                  = q.column_text(0);
        b.calls                = q.column_int64(1);
        b.input_tokens         = q.column_int64(2);
        b.output_tokens        = q.column_int64(3);
        b.cache_read_tokens    = q.column_int64(4);
        b.cache_create_tokens  = q.column_int64(5);
        b.input_uc             = q.column_int64(6);
        b.output_uc            = q.column_int64(7);
        b.cache_read_uc        = q.column_int64(8);
        b.cache_create_uc      = q.column_int64(9);
        b.provider_uc          = q.column_int64(10);
        b.markup_uc            = q.column_int64(11);
        out.push_back(std::move(b));
    }
    return out;
}

bool TenantStore::reload_tenant(int64_t id, Tenant& t) const {
    auto r = get_tenant(id);
    if (!r) return false;
    t = *r;
    return true;
}

} // namespace index_ai
