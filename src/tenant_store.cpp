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
    // SQLITE_OPEN_FULLMUTEX forces the connection into serialized
    // threading mode regardless of the underlying library's build
    // defaults.  Without this, the system SQLite on macOS is
    // typically built in multi-thread mode (SQLITE_THREADSAFE=2),
    // which forbids sharing one `sqlite3*` across threads — and the
    // API server's accept loop spawns one thread per connection, all
    // calling into this TenantStore.  Sharing a non-serialized
    // connection would race the parser and surface as garbage error
    // messages ("no such table: <…>") followed by SIGSEGV inside
    // sqlite3RunParser.  FULLMUTEX makes every API call take an
    // internal mutex on the connection, making concurrent calls safe
    // at the cost of one mutex per SQL op (negligible for our scale).
    int rc = sqlite3_open_v2(
        path.c_str(), &db_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
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

    // ── Conversations + messages ────────────────────────────────────────
    // Added in v3.  Conversation threads are tenant-scoped; messages are
    // append-only and FK-linked back to their conversation.  ON DELETE
    // CASCADE so DELETE /v1/conversations/:id cleans up messages without
    // an explicit transaction in the caller.
    exec_sql(db_, R"SQL(
        CREATE TABLE IF NOT EXISTS conversations (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            tenant_id       INTEGER NOT NULL,
            title           TEXT    NOT NULL DEFAULT '',
            agent_id        TEXT    NOT NULL,
            agent_def_json  TEXT    NOT NULL DEFAULT '',
            created_at      INTEGER NOT NULL,
            updated_at      INTEGER NOT NULL,
            message_count   INTEGER NOT NULL DEFAULT 0,
            archived        INTEGER NOT NULL DEFAULT 0,
            FOREIGN KEY (tenant_id) REFERENCES tenants(id)
        );
    )SQL");
    exec_sql(db_, R"SQL(
        CREATE INDEX IF NOT EXISTS conversations_tenant_updated
            ON conversations(tenant_id, updated_at DESC);
    )SQL");
    exec_sql(db_, R"SQL(
        CREATE TABLE IF NOT EXISTS messages (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            conversation_id INTEGER NOT NULL,
            role            TEXT    NOT NULL,
            content         TEXT    NOT NULL,
            input_tokens    INTEGER NOT NULL DEFAULT 0,
            output_tokens   INTEGER NOT NULL DEFAULT 0,
            billed_uc       INTEGER NOT NULL DEFAULT 0,
            created_at      INTEGER NOT NULL,
            request_id      TEXT,
            FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE
        );
    )SQL");
    exec_sql(db_, R"SQL(
        CREATE INDEX IF NOT EXISTS messages_conversation_id
            ON messages(conversation_id, id);
    )SQL");

    // ── Structured memory: entries + relations ─────────────────────────
    // Added in v4.  Backs the frontend graph UI — typed nodes with
    // free-form content, plus directed labeled edges between them.
    // Distinct storage from the legacy file scratchpads under
    // ~/.arbiter/memory/t<id>/<agent_id>.md; the two surfaces don't share
    // data and writes here don't go through agents.
    exec_sql(db_, R"SQL(
        CREATE TABLE IF NOT EXISTS memory_entries (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            tenant_id   INTEGER NOT NULL,
            type        TEXT    NOT NULL,
            title       TEXT    NOT NULL,
            content     TEXT    NOT NULL DEFAULT '',
            source      TEXT    NOT NULL DEFAULT '',
            tags        TEXT    NOT NULL DEFAULT '[]',
            created_at  INTEGER NOT NULL,
            updated_at  INTEGER NOT NULL,
            FOREIGN KEY (tenant_id) REFERENCES tenants(id)
        );
    )SQL");
    exec_sql(db_, R"SQL(
        CREATE INDEX IF NOT EXISTS memory_entries_tenant_type
            ON memory_entries(tenant_id, type);
    )SQL");
    exec_sql(db_, R"SQL(
        CREATE INDEX IF NOT EXISTS memory_entries_tenant_updated
            ON memory_entries(tenant_id, updated_at DESC);
    )SQL");
    exec_sql(db_, R"SQL(
        CREATE TABLE IF NOT EXISTS memory_relations (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            tenant_id   INTEGER NOT NULL,
            source_id   INTEGER NOT NULL,
            target_id   INTEGER NOT NULL,
            relation    TEXT    NOT NULL,
            created_at  INTEGER NOT NULL,
            FOREIGN KEY (tenant_id) REFERENCES tenants(id),
            FOREIGN KEY (source_id) REFERENCES memory_entries(id) ON DELETE CASCADE,
            FOREIGN KEY (target_id) REFERENCES memory_entries(id) ON DELETE CASCADE
        );
    )SQL");
    exec_sql(db_, R"SQL(
        CREATE INDEX IF NOT EXISTS memory_relations_tenant
            ON memory_relations(tenant_id);
    )SQL");
    exec_sql(db_, R"SQL(
        CREATE INDEX IF NOT EXISTS memory_relations_source
            ON memory_relations(source_id);
    )SQL");
    exec_sql(db_, R"SQL(
        CREATE INDEX IF NOT EXISTS memory_relations_target
            ON memory_relations(target_id);
    )SQL");
    exec_sql(db_, R"SQL(
        CREATE UNIQUE INDEX IF NOT EXISTS memory_relations_unique
            ON memory_relations(tenant_id, source_id, target_id, relation);
    )SQL");

    // Proposal-queue status column (added in v5).  'accepted' rows are
    // the curated graph; 'proposed' rows are agent-contributed entries
    // awaiting human review and are hidden from the normal list/get/graph
    // reads.  ALTER TABLE ADD COLUMN is idempotent only when the column
    // doesn't already exist — reuse the column_exists/add_column helpers
    // pattern from usage_log above.
    auto entries_col_exists = [this](const char* col) -> bool {
        Stmt q(db_, "PRAGMA table_info(memory_entries);");
        while (q.step() == SQLITE_ROW) {
            if (q.column_text(1) == col) return true;
        }
        return false;
    };
    auto add_entries_col = [this, &entries_col_exists](const char* col, const char* defn) {
        if (entries_col_exists(col)) return;
        std::string sql = std::string("ALTER TABLE memory_entries ADD COLUMN ") +
                          col + " " + defn + ";";
        exec_sql(db_, sql.c_str());
    };
    add_entries_col("status", "TEXT NOT NULL DEFAULT 'accepted'");

    auto relations_col_exists = [this](const char* col) -> bool {
        Stmt q(db_, "PRAGMA table_info(memory_relations);");
        while (q.step() == SQLITE_ROW) {
            if (q.column_text(1) == col) return true;
        }
        return false;
    };
    auto add_relations_col = [this, &relations_col_exists](const char* col, const char* defn) {
        if (relations_col_exists(col)) return;
        std::string sql = std::string("ALTER TABLE memory_relations ADD COLUMN ") +
                          col + " " + defn + ";";
        exec_sql(db_, sql.c_str());
    };
    add_relations_col("status", "TEXT NOT NULL DEFAULT 'accepted'");

    exec_sql(db_, R"SQL(
        CREATE INDEX IF NOT EXISTS memory_entries_tenant_status
            ON memory_entries(tenant_id, status);
    )SQL");
    exec_sql(db_, R"SQL(
        CREATE INDEX IF NOT EXISTS memory_relations_tenant_status
            ON memory_relations(tenant_id, status);
    )SQL");

    // ── Per-tenant agent catalog (added in v6) ─────────────────────────
    // Stores agent_def blobs sent from the front-end so callers can
    // reference agents by `agent_id` across requests instead of re-
    // sending the full constitution every turn.  `agent_id` is caller-
    // chosen; the unique index keeps a tenant from registering the same
    // id twice but lets two tenants independently use overlapping ids.
    // The denormalised name/role/model columns exist only to keep list
    // responses cheap — `agent_def_json` is the canonical source.
    exec_sql(db_, R"SQL(
        CREATE TABLE IF NOT EXISTS tenant_agents (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            tenant_id       INTEGER NOT NULL,
            agent_id        TEXT    NOT NULL,
            name            TEXT    NOT NULL DEFAULT '',
            role            TEXT    NOT NULL DEFAULT '',
            model           TEXT    NOT NULL DEFAULT '',
            agent_def_json  TEXT    NOT NULL,
            created_at      INTEGER NOT NULL,
            updated_at      INTEGER NOT NULL,
            FOREIGN KEY (tenant_id) REFERENCES tenants(id) ON DELETE CASCADE
        );
    )SQL");
    exec_sql(db_, R"SQL(
        CREATE UNIQUE INDEX IF NOT EXISTS tenant_agents_tenant_agent_id
            ON tenant_agents(tenant_id, agent_id);
    )SQL");
    exec_sql(db_, R"SQL(
        CREATE INDEX IF NOT EXISTS tenant_agents_tenant_updated
            ON tenant_agents(tenant_id, updated_at DESC);
    )SQL");

    // ── Agent file-scratchpad storage (added in v7) ────────────────────
    // Replaces the filesystem scratchpad at ~/.arbiter/memory/t<tid>/.
    // One row per (tenant, scope_key); scope_key == "" is the shared
    // pipeline scratchpad, any other value is an agent_id.  Single
    // cumulative `content` blob per row — appends rewrite the column.
    exec_sql(db_, R"SQL(
        CREATE TABLE IF NOT EXISTS agent_scratchpad (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            tenant_id   INTEGER NOT NULL,
            scope_key   TEXT    NOT NULL,
            content     TEXT    NOT NULL DEFAULT '',
            updated_at  INTEGER NOT NULL,
            FOREIGN KEY (tenant_id) REFERENCES tenants(id) ON DELETE CASCADE
        );
    )SQL");
    exec_sql(db_, R"SQL(
        CREATE UNIQUE INDEX IF NOT EXISTS agent_scratchpad_tenant_scope
            ON agent_scratchpad(tenant_id, scope_key);
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

// ─── Conversations ──────────────────────────────────────────────────────────

namespace {

constexpr const char* kConvCols =
    "id, tenant_id, title, agent_id, agent_def_json, "
    "created_at, updated_at, message_count, archived";

Conversation row_to_conversation(Stmt& q) {
    Conversation c;
    c.id              = q.column_int64(0);
    c.tenant_id       = q.column_int64(1);
    c.title           = q.column_text(2);
    c.agent_id        = q.column_text(3);
    c.agent_def_json  = q.column_text(4);
    c.created_at      = q.column_int64(5);
    c.updated_at      = q.column_int64(6);
    c.message_count   = q.column_int64(7);
    c.archived        = q.column_int64(8) != 0;
    return c;
}

constexpr const char* kMsgCols =
    "id, conversation_id, role, content, "
    "input_tokens, output_tokens, billed_uc, created_at, request_id";

ConversationMessage row_to_message(Stmt& q) {
    ConversationMessage m;
    m.id              = q.column_int64(0);
    m.conversation_id = q.column_int64(1);
    m.role            = q.column_text(2);
    m.content         = q.column_text(3);
    m.input_tokens    = q.column_int64(4);
    m.output_tokens   = q.column_int64(5);
    m.billed_uc       = q.column_int64(6);
    m.created_at      = q.column_int64(7);
    m.request_id      = q.column_text(8);
    return m;
}

} // namespace

Conversation TenantStore::create_conversation(int64_t tenant_id,
                                               const std::string& title,
                                               const std::string& agent_id,
                                               const std::string& agent_def_json) {
    if (!db_) throw std::runtime_error("TenantStore not opened");

    const int64_t now = now_epoch();
    Stmt q(db_,
        "INSERT INTO conversations "
        "(tenant_id, title, agent_id, agent_def_json, created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?);");
    q.bind(1, tenant_id);
    q.bind(2, title);
    q.bind(3, agent_id);
    q.bind(4, agent_def_json);
    q.bind(5, now);
    q.bind(6, now);
    int rc = q.step();
    if (rc != SQLITE_DONE) check_sqlite(db_, rc, "insert conversation");

    Conversation c;
    c.id              = sqlite3_last_insert_rowid(db_);
    c.tenant_id       = tenant_id;
    c.title           = title;
    c.agent_id        = agent_id;
    c.agent_def_json  = agent_def_json;
    c.created_at      = now;
    c.updated_at      = now;
    c.message_count   = 0;
    c.archived        = false;
    return c;
}

std::vector<Conversation>
TenantStore::list_conversations(int64_t tenant_id, int64_t before_updated_at,
                                 int limit) const {
    std::vector<Conversation> out;
    if (!db_) return out;

    const int cap = (limit > 0 && limit <= 200) ? limit : 50;

    std::string sql = std::string("SELECT ") + kConvCols +
                       " FROM conversations WHERE tenant_id = ?";
    if (before_updated_at > 0) sql += " AND updated_at < ?";
    sql += " ORDER BY updated_at DESC LIMIT ?;";

    Stmt q(db_, sql.c_str());
    int idx = 1;
    q.bind(idx++, tenant_id);
    if (before_updated_at > 0) q.bind(idx++, before_updated_at);
    q.bind(idx, static_cast<int64_t>(cap));

    while (q.step() == SQLITE_ROW) out.push_back(row_to_conversation(q));
    return out;
}

std::optional<Conversation>
TenantStore::get_conversation(int64_t tenant_id, int64_t id) const {
    if (!db_) return std::nullopt;
    Stmt q(db_, (std::string("SELECT ") + kConvCols +
                 " FROM conversations WHERE tenant_id = ? AND id = ?;").c_str());
    q.bind(1, tenant_id);
    q.bind(2, id);
    if (q.step() != SQLITE_ROW) return std::nullopt;
    return row_to_conversation(q);
}

bool TenantStore::update_conversation(int64_t tenant_id, int64_t id,
                                       const std::string& new_title,
                                       int set_archived) {
    if (!db_) return false;

    // Build dynamic UPDATE so we only touch fields the caller actually
    // wanted to change.  No-op (both args sentinel) returns true if the
    // conversation exists, false otherwise — same as a normal PATCH.
    std::vector<std::string> sets;
    if (!new_title.empty()) sets.push_back("title = ?");
    if (set_archived >= 0)  sets.push_back("archived = ?");
    if (sets.empty()) {
        return get_conversation(tenant_id, id).has_value();
    }
    sets.push_back("updated_at = ?");

    std::string sql = "UPDATE conversations SET ";
    for (size_t i = 0; i < sets.size(); ++i) {
        if (i) sql += ", ";
        sql += sets[i];
    }
    sql += " WHERE tenant_id = ? AND id = ?;";

    Stmt q(db_, sql.c_str());
    int idx = 1;
    if (!new_title.empty()) q.bind(idx++, new_title);
    if (set_archived >= 0)  q.bind(idx++, static_cast<int64_t>(set_archived));
    q.bind(idx++, now_epoch());
    q.bind(idx++, tenant_id);
    q.bind(idx, id);
    q.step();
    return sqlite3_changes(db_) > 0;
}

bool TenantStore::delete_conversation(int64_t tenant_id, int64_t id) {
    if (!db_) return false;
    Stmt q(db_, "DELETE FROM conversations WHERE tenant_id = ? AND id = ?;");
    q.bind(1, tenant_id);
    q.bind(2, id);
    q.step();
    // ON DELETE CASCADE on messages.conversation_id handles the message rows.
    return sqlite3_changes(db_) > 0;
}

ConversationMessage TenantStore::append_message(int64_t tenant_id,
                                                 int64_t conversation_id,
                                                 const std::string& role,
                                                 const std::string& content,
                                                 int64_t input_tokens,
                                                 int64_t output_tokens,
                                                 int64_t billed_uc,
                                                 const std::string& request_id) {
    if (!db_) throw std::runtime_error("TenantStore not opened");

    // Verify the conversation belongs to this tenant before inserting.
    // Without this a leaked conversation_id would let any tenant write
    // into someone else's thread.
    auto conv = get_conversation(tenant_id, conversation_id);
    if (!conv)
        throw std::runtime_error("conversation not found for tenant");

    const int64_t now = now_epoch();
    exec_sql(db_, "BEGIN IMMEDIATE;");
    try {
        {
            Stmt ins(db_,
                std::string("INSERT INTO messages (").append(kMsgCols)
                    .append(") VALUES (NULL, ?, ?, ?, ?, ?, ?, ?, ?);").c_str());
            ins.bind(1, conversation_id);
            ins.bind(2, role);
            ins.bind(3, content);
            ins.bind(4, input_tokens);
            ins.bind(5, output_tokens);
            ins.bind(6, billed_uc);
            ins.bind(7, now);
            if (request_id.empty()) ins.bind(8, nullptr);
            else                    ins.bind(8, request_id);
            ins.step();
        }
        const int64_t mid = sqlite3_last_insert_rowid(db_);
        {
            Stmt bump(db_,
                "UPDATE conversations "
                "   SET updated_at    = ?, "
                "       message_count = message_count + 1 "
                " WHERE id = ?;");
            bump.bind(1, now);
            bump.bind(2, conversation_id);
            bump.step();
        }
        exec_sql(db_, "COMMIT;");

        ConversationMessage m;
        m.id              = mid;
        m.conversation_id = conversation_id;
        m.role            = role;
        m.content         = content;
        m.input_tokens    = input_tokens;
        m.output_tokens   = output_tokens;
        m.billed_uc       = billed_uc;
        m.created_at      = now;
        m.request_id      = request_id;
        return m;
    } catch (...) {
        exec_sql(db_, "ROLLBACK;");
        throw;
    }
}

std::vector<ConversationMessage>
TenantStore::list_messages(int64_t tenant_id, int64_t conversation_id,
                            int64_t after_id, int limit) const {
    std::vector<ConversationMessage> out;
    if (!db_) return out;

    // Same tenant-scoping check as append_message.
    if (!get_conversation(tenant_id, conversation_id)) return out;

    const int cap = (limit > 0 && limit <= 500) ? limit : 200;
    std::string sql = std::string("SELECT ") + kMsgCols +
                       " FROM messages WHERE conversation_id = ?";
    if (after_id > 0) sql += " AND id > ?";
    sql += " ORDER BY id ASC LIMIT ?;";

    Stmt q(db_, sql.c_str());
    int idx = 1;
    q.bind(idx++, conversation_id);
    if (after_id > 0) q.bind(idx++, after_id);
    q.bind(idx, static_cast<int64_t>(cap));

    while (q.step() == SQLITE_ROW) out.push_back(row_to_message(q));
    return out;
}

bool TenantStore::reload_tenant(int64_t id, Tenant& t) const {
    auto r = get_tenant(id);
    if (!r) return false;
    t = *r;
    return true;
}

// ─── Memory entries + relations ────────────────────────────────────────────

namespace {

constexpr const char* kEntryCols =
    "id, tenant_id, type, title, content, source, tags, status, created_at, updated_at";

MemoryEntry row_to_entry(Stmt& q) {
    MemoryEntry e;
    e.id         = q.column_int64(0);
    e.tenant_id  = q.column_int64(1);
    e.type       = q.column_text(2);
    e.title      = q.column_text(3);
    e.content    = q.column_text(4);
    e.source     = q.column_text(5);
    e.tags_json  = q.column_text(6);
    e.status     = q.column_text(7);
    e.created_at = q.column_int64(8);
    e.updated_at = q.column_int64(9);
    return e;
}

constexpr const char* kRelationCols =
    "id, tenant_id, source_id, target_id, relation, status, created_at";

MemoryRelation row_to_relation(Stmt& q) {
    MemoryRelation r;
    r.id         = q.column_int64(0);
    r.tenant_id  = q.column_int64(1);
    r.source_id  = q.column_int64(2);
    r.target_id  = q.column_int64(3);
    r.relation   = q.column_text(4);
    r.status     = q.column_text(5);
    r.created_at = q.column_int64(6);
    return r;
}

} // namespace

MemoryEntry TenantStore::create_entry(int64_t tenant_id,
                                       const std::string& type,
                                       const std::string& title,
                                       const std::string& content,
                                       const std::string& source,
                                       const std::string& tags_json,
                                       const std::string& status) {
    if (!db_) throw std::runtime_error("TenantStore not opened");

    const int64_t now = now_epoch();
    Stmt q(db_,
        "INSERT INTO memory_entries "
        "(tenant_id, type, title, content, source, tags, status, created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);");
    q.bind(1, tenant_id);
    q.bind(2, type);
    q.bind(3, title);
    q.bind(4, content);
    q.bind(5, source);
    q.bind(6, tags_json.empty() ? std::string("[]") : tags_json);
    q.bind(7, status.empty() ? std::string("accepted") : status);
    q.bind(8, now);
    q.bind(9, now);
    int rc = q.step();
    if (rc != SQLITE_DONE) check_sqlite(db_, rc, "insert memory_entry");

    MemoryEntry e;
    e.id         = sqlite3_last_insert_rowid(db_);
    e.tenant_id  = tenant_id;
    e.type       = type;
    e.title      = title;
    e.content    = content;
    e.source     = source;
    e.tags_json  = tags_json.empty() ? "[]" : tags_json;
    e.status     = status.empty() ? "accepted" : status;
    e.created_at = now;
    e.updated_at = now;
    return e;
}

std::optional<MemoryEntry>
TenantStore::get_entry(int64_t tenant_id, int64_t id) const {
    if (!db_) return std::nullopt;
    Stmt q(db_, (std::string("SELECT ") + kEntryCols +
                 " FROM memory_entries WHERE tenant_id = ? AND id = ?;").c_str());
    q.bind(1, tenant_id);
    q.bind(2, id);
    if (q.step() != SQLITE_ROW) return std::nullopt;
    return row_to_entry(q);
}

std::vector<MemoryEntry>
TenantStore::list_entries(int64_t tenant_id, const EntryFilter& f) const {
    std::vector<MemoryEntry> out;
    if (!db_) return out;

    // Build WHERE incrementally so each filter is optional.  Placeholders
    // are appended in the same order as the binds below, so the index walk
    // is mechanical.
    std::string sql = std::string("SELECT ") + kEntryCols +
                       " FROM memory_entries WHERE tenant_id = ?";
    if (!f.types.empty()) {
        sql += " AND type IN (";
        for (size_t i = 0; i < f.types.size(); ++i) {
            if (i) sql += ",";
            sql += "?";
        }
        sql += ")";
    }
    if (!f.tag.empty())          sql += " AND tags LIKE ?";
    if (!f.q.empty())            sql += " AND (title LIKE ? OR content LIKE ?)";
    if (f.since > 0)             sql += " AND created_at >= ?";
    if (f.before_updated_at > 0) sql += " AND updated_at < ?";
    if (!f.status.empty())       sql += " AND status = ?";

    const int cap = (f.limit > 0 && f.limit <= 200) ? f.limit : 50;
    sql += " ORDER BY updated_at DESC LIMIT ?;";

    Stmt q(db_, sql.c_str());
    int idx = 1;
    q.bind(idx++, tenant_id);
    for (auto& t : f.types) q.bind(idx++, t);
    if (!f.tag.empty()) q.bind(idx++, std::string("%\"" + f.tag + "\"%"));
    if (!f.q.empty()) {
        // Bind a fresh string at each idx — sqlite3_bind_text with
        // SQLITE_TRANSIENT will copy, but the underlying std::string must
        // outlive the bind call until step().  Using two locals here.
        const std::string pat = "%" + f.q + "%";
        q.bind(idx++, pat);
        q.bind(idx++, pat);
    }
    if (f.since > 0)             q.bind(idx++, f.since);
    if (f.before_updated_at > 0) q.bind(idx++, f.before_updated_at);
    if (!f.status.empty())       q.bind(idx++, f.status);
    q.bind(idx, static_cast<int64_t>(cap));

    while (q.step() == SQLITE_ROW) out.push_back(row_to_entry(q));
    return out;
}

bool TenantStore::update_entry(int64_t tenant_id, int64_t id,
                                const std::optional<std::string>& title,
                                const std::optional<std::string>& content,
                                const std::optional<std::string>& source,
                                const std::optional<std::string>& tags_json,
                                const std::optional<std::string>& type) {
    if (!db_) return false;

    std::vector<std::string> sets;
    if (title)     sets.push_back("title = ?");
    if (content)   sets.push_back("content = ?");
    if (source)    sets.push_back("source = ?");
    if (tags_json) sets.push_back("tags = ?");
    if (type)      sets.push_back("type = ?");
    if (sets.empty()) {
        // Nothing to change — match update_conversation's PATCH shape and
        // return true if the row exists.
        return get_entry(tenant_id, id).has_value();
    }
    sets.push_back("updated_at = ?");

    std::string sql = "UPDATE memory_entries SET ";
    for (size_t i = 0; i < sets.size(); ++i) {
        if (i) sql += ", ";
        sql += sets[i];
    }
    sql += " WHERE tenant_id = ? AND id = ?;";

    Stmt q(db_, sql.c_str());
    int idx = 1;
    if (title)     q.bind(idx++, *title);
    if (content)   q.bind(idx++, *content);
    if (source)    q.bind(idx++, *source);
    if (tags_json) q.bind(idx++, *tags_json);
    if (type)      q.bind(idx++, *type);
    q.bind(idx++, now_epoch());
    q.bind(idx++, tenant_id);
    q.bind(idx, id);
    q.step();
    return sqlite3_changes(db_) > 0;
}

bool TenantStore::delete_entry(int64_t tenant_id, int64_t id) {
    if (!db_) return false;
    // ON DELETE CASCADE on memory_relations.{source_id, target_id} drops
    // any dangling edges automatically.
    Stmt q(db_, "DELETE FROM memory_entries WHERE tenant_id = ? AND id = ?;");
    q.bind(1, tenant_id);
    q.bind(2, id);
    q.step();
    return sqlite3_changes(db_) > 0;
}

std::optional<MemoryRelation>
TenantStore::create_relation(int64_t tenant_id,
                              int64_t source_id, int64_t target_id,
                              const std::string& relation,
                              const std::string& status) {
    if (!db_) return std::nullopt;
    const int64_t now = now_epoch();
    const std::string s = status.empty() ? std::string("accepted") : status;
    Stmt q(db_,
        "INSERT INTO memory_relations "
        "(tenant_id, source_id, target_id, relation, status, created_at) "
        "VALUES (?, ?, ?, ?, ?, ?);");
    q.bind(1, tenant_id);
    q.bind(2, source_id);
    q.bind(3, target_id);
    q.bind(4, relation);
    q.bind(5, s);
    q.bind(6, now);
    int rc = q.step();
    if (rc == SQLITE_CONSTRAINT) return std::nullopt;
    if (rc != SQLITE_DONE) check_sqlite(db_, rc, "insert memory_relation");

    MemoryRelation r;
    r.id         = sqlite3_last_insert_rowid(db_);
    r.tenant_id  = tenant_id;
    r.source_id  = source_id;
    r.target_id  = target_id;
    r.relation   = relation;
    r.status     = s;
    r.created_at = now;
    return r;
}

std::optional<MemoryRelation>
TenantStore::find_relation(int64_t tenant_id,
                            int64_t source_id, int64_t target_id,
                            const std::string& relation) const {
    if (!db_) return std::nullopt;
    Stmt q(db_, (std::string("SELECT ") + kRelationCols +
                 " FROM memory_relations WHERE tenant_id = ? "
                 "  AND source_id = ? AND target_id = ? AND relation = ?;").c_str());
    q.bind(1, tenant_id);
    q.bind(2, source_id);
    q.bind(3, target_id);
    q.bind(4, relation);
    if (q.step() != SQLITE_ROW) return std::nullopt;
    return row_to_relation(q);
}

std::vector<MemoryRelation>
TenantStore::list_relations(int64_t tenant_id,
                             int64_t source_id, int64_t target_id,
                             const std::string& relation,
                             int limit,
                             const std::string& status) const {
    std::vector<MemoryRelation> out;
    if (!db_) return out;

    std::string sql = std::string("SELECT ") + kRelationCols +
                       " FROM memory_relations WHERE tenant_id = ?";
    if (source_id > 0)     sql += " AND source_id = ?";
    if (target_id > 0)     sql += " AND target_id = ?";
    if (!relation.empty()) sql += " AND relation = ?";
    if (!status.empty())   sql += " AND status = ?";
    const int cap = (limit > 0 && limit <= 1000) ? limit : 200;
    sql += " ORDER BY id DESC LIMIT ?;";

    Stmt q(db_, sql.c_str());
    int idx = 1;
    q.bind(idx++, tenant_id);
    if (source_id > 0)     q.bind(idx++, source_id);
    if (target_id > 0)     q.bind(idx++, target_id);
    if (!relation.empty()) q.bind(idx++, relation);
    if (!status.empty())   q.bind(idx++, status);
    q.bind(idx, static_cast<int64_t>(cap));

    while (q.step() == SQLITE_ROW) out.push_back(row_to_relation(q));
    return out;
}

bool TenantStore::delete_relation(int64_t tenant_id, int64_t id) {
    if (!db_) return false;
    Stmt q(db_, "DELETE FROM memory_relations WHERE tenant_id = ? AND id = ?;");
    q.bind(1, tenant_id);
    q.bind(2, id);
    q.step();
    return sqlite3_changes(db_) > 0;
}

bool TenantStore::accept_entry(int64_t tenant_id, int64_t id) {
    if (!db_) return false;
    // Conditional update — only succeeds if the row is currently 'proposed'.
    // Rows that don't exist or are already accepted leave sqlite3_changes() at 0.
    Stmt q(db_,
        "UPDATE memory_entries "
        "   SET status = 'accepted', updated_at = ? "
        " WHERE tenant_id = ? AND id = ? AND status = 'proposed';");
    q.bind(1, now_epoch());
    q.bind(2, tenant_id);
    q.bind(3, id);
    q.step();
    return sqlite3_changes(db_) > 0;
}

bool TenantStore::accept_relation(int64_t tenant_id, int64_t id) {
    if (!db_) return false;
    // Read the row first so we can validate the both-endpoints-accepted
    // invariant before flipping status.  A relation pointing at a still-
    // proposed entry would surface as a dangling edge in the curated graph.
    Stmt s(db_, (std::string("SELECT ") + kRelationCols +
                 " FROM memory_relations WHERE tenant_id = ? AND id = ?;").c_str());
    s.bind(1, tenant_id);
    s.bind(2, id);
    if (s.step() != SQLITE_ROW) return false;
    MemoryRelation rel = row_to_relation(s);
    if (rel.status != "proposed") return false;

    // Both endpoint entries must already be in the curated graph.
    auto src = get_entry(tenant_id, rel.source_id);
    auto dst = get_entry(tenant_id, rel.target_id);
    if (!src || !dst) return false;
    if (src->status != "accepted" || dst->status != "accepted") return false;

    Stmt q(db_,
        "UPDATE memory_relations "
        "   SET status = 'accepted' "
        " WHERE tenant_id = ? AND id = ? AND status = 'proposed';");
    q.bind(1, tenant_id);
    q.bind(2, id);
    q.step();
    return sqlite3_changes(db_) > 0;
}

// ── Tenant agent catalog ───────────────────────────────────────────────

namespace {

constexpr const char* kAgentCols =
    "id, tenant_id, agent_id, name, role, model, agent_def_json, "
    "created_at, updated_at";

AgentRecord row_to_agent(Stmt& q) {
    AgentRecord a;
    a.id              = q.column_int64(0);
    a.tenant_id       = q.column_int64(1);
    a.agent_id        = q.column_text(2);
    a.name            = q.column_text(3);
    a.role            = q.column_text(4);
    a.model           = q.column_text(5);
    a.agent_def_json  = q.column_text(6);
    a.created_at      = q.column_int64(7);
    a.updated_at      = q.column_int64(8);
    return a;
}

} // namespace

std::optional<AgentRecord>
TenantStore::create_agent_record(int64_t tenant_id,
                                  const std::string& agent_id,
                                  const std::string& name,
                                  const std::string& role,
                                  const std::string& model,
                                  const std::string& agent_def_json) {
    if (!db_ || tenant_id <= 0 || agent_id.empty()) return std::nullopt;
    const int64_t now = now_epoch();
    Stmt q(db_,
        "INSERT INTO tenant_agents "
        "(tenant_id, agent_id, name, role, model, agent_def_json, "
        " created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?);");
    q.bind(1, tenant_id);
    q.bind(2, agent_id);
    q.bind(3, name);
    q.bind(4, role);
    q.bind(5, model);
    q.bind(6, agent_def_json);
    q.bind(7, now);
    q.bind(8, now);
    int rc = q.step();
    if (rc == SQLITE_CONSTRAINT) return std::nullopt;
    if (rc != SQLITE_DONE) check_sqlite(db_, rc, "insert tenant_agent");

    AgentRecord a;
    a.id              = sqlite3_last_insert_rowid(db_);
    a.tenant_id       = tenant_id;
    a.agent_id        = agent_id;
    a.name            = name;
    a.role            = role;
    a.model           = model;
    a.agent_def_json  = agent_def_json;
    a.created_at      = now;
    a.updated_at      = now;
    return a;
}

std::optional<AgentRecord>
TenantStore::get_agent_record(int64_t tenant_id,
                               const std::string& agent_id) const {
    if (!db_) return std::nullopt;
    Stmt q(db_, (std::string("SELECT ") + kAgentCols +
                 " FROM tenant_agents WHERE tenant_id = ? AND agent_id = ?;").c_str());
    q.bind(1, tenant_id);
    q.bind(2, agent_id);
    if (q.step() != SQLITE_ROW) return std::nullopt;
    return row_to_agent(q);
}

std::vector<AgentRecord>
TenantStore::list_agent_records(int64_t tenant_id, int limit) const {
    std::vector<AgentRecord> out;
    if (!db_) return out;
    const int cap = (limit > 0 && limit <= 200) ? limit : 50;
    Stmt q(db_, (std::string("SELECT ") + kAgentCols +
                 " FROM tenant_agents WHERE tenant_id = ? "
                 " ORDER BY updated_at DESC LIMIT ?;").c_str());
    q.bind(1, tenant_id);
    q.bind(2, static_cast<int64_t>(cap));
    while (q.step() == SQLITE_ROW) out.push_back(row_to_agent(q));
    return out;
}

bool TenantStore::update_agent_record(int64_t tenant_id,
                                       const std::string& agent_id,
                                       const std::string& name,
                                       const std::string& role,
                                       const std::string& model,
                                       const std::string& agent_def_json) {
    if (!db_) return false;
    Stmt q(db_,
        "UPDATE tenant_agents "
        "   SET name = ?, role = ?, model = ?, agent_def_json = ?, "
        "       updated_at = ? "
        " WHERE tenant_id = ? AND agent_id = ?;");
    q.bind(1, name);
    q.bind(2, role);
    q.bind(3, model);
    q.bind(4, agent_def_json);
    q.bind(5, now_epoch());
    q.bind(6, tenant_id);
    q.bind(7, agent_id);
    q.step();
    return sqlite3_changes(db_) > 0;
}

bool TenantStore::delete_agent_record(int64_t tenant_id,
                                       const std::string& agent_id) {
    if (!db_) return false;
    Stmt q(db_, "DELETE FROM tenant_agents WHERE tenant_id = ? AND agent_id = ?;");
    q.bind(1, tenant_id);
    q.bind(2, agent_id);
    q.step();
    return sqlite3_changes(db_) > 0;
}

// ── Agent file-scratchpad ──────────────────────────────────────────────

std::string TenantStore::read_scratchpad(int64_t tenant_id,
                                          const std::string& scope_key) const {
    if (!db_) return "";
    Stmt q(db_, "SELECT content FROM agent_scratchpad "
                 "WHERE tenant_id = ? AND scope_key = ?;");
    q.bind(1, tenant_id);
    q.bind(2, scope_key);
    if (q.step() != SQLITE_ROW) return "";
    return q.column_text(0);
}

namespace {
std::string scratchpad_block(const std::string& text) {
    // Match the file-based format so existing prompts and consumers
    // still see `<!-- YYYY-MM-DD HH:MM:SS --> ...` between entries.
    std::time_t now = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    std::string out;
    out.reserve(text.size() + 64);
    out += "\n<!-- ";
    out += ts;
    out += " -->\n";
    out += text;
    out += "\n";
    return out;
}
} // namespace

int64_t TenantStore::append_scratchpad(int64_t tenant_id,
                                        const std::string& scope_key,
                                        const std::string& text) {
    if (!db_) return 0;
    const std::string block = scratchpad_block(text);
    const int64_t now = now_epoch();
    // Read-modify-write under the shared connection: SQLite serialises
    // writers via the busy-timeout pragma, so concurrent appends from
    // parallel orchestrator threads will queue rather than corrupt.
    std::string current;
    {
        Stmt sel(db_, "SELECT content FROM agent_scratchpad "
                       "WHERE tenant_id = ? AND scope_key = ?;");
        sel.bind(1, tenant_id);
        sel.bind(2, scope_key);
        if (sel.step() == SQLITE_ROW) current = sel.column_text(0);
    }
    const std::string new_content = current + block;

    if (current.empty()) {
        // First write — try INSERT; on UNIQUE conflict (a parallel
        // appender beat us to it) fall through to UPDATE.
        Stmt ins(db_,
            "INSERT OR IGNORE INTO agent_scratchpad "
            "(tenant_id, scope_key, content, updated_at) "
            "VALUES (?, ?, ?, ?);");
        ins.bind(1, tenant_id);
        ins.bind(2, scope_key);
        ins.bind(3, new_content);
        ins.bind(4, now);
        ins.step();
        if (sqlite3_changes(db_) > 0) return static_cast<int64_t>(new_content.size());
    }

    Stmt upd(db_,
        "UPDATE agent_scratchpad SET content = content || ?, updated_at = ? "
        " WHERE tenant_id = ? AND scope_key = ?;");
    upd.bind(1, block);
    upd.bind(2, now);
    upd.bind(3, tenant_id);
    upd.bind(4, scope_key);
    upd.step();
    // Fetch the post-update size so the caller can surface a useful
    // "OK: wrote N bytes" line that matches the file path's behaviour.
    Stmt sz(db_, "SELECT length(content) FROM agent_scratchpad "
                  "WHERE tenant_id = ? AND scope_key = ?;");
    sz.bind(1, tenant_id);
    sz.bind(2, scope_key);
    if (sz.step() == SQLITE_ROW) return sz.column_int64(0);
    return 0;
}

bool TenantStore::clear_scratchpad(int64_t tenant_id,
                                    const std::string& scope_key) {
    if (!db_) return false;
    Stmt q(db_, "DELETE FROM agent_scratchpad "
                 "WHERE tenant_id = ? AND scope_key = ?;");
    q.bind(1, tenant_id);
    q.bind(2, scope_key);
    q.step();
    return sqlite3_changes(db_) > 0;
}

std::vector<std::string>
TenantStore::list_scratchpad_scopes(int64_t tenant_id) const {
    std::vector<std::string> out;
    if (!db_) return out;
    Stmt q(db_, "SELECT scope_key FROM agent_scratchpad "
                 "WHERE tenant_id = ? AND length(content) > 0 "
                 " ORDER BY scope_key;");
    q.bind(1, tenant_id);
    while (q.step() == SQLITE_ROW) out.push_back(q.column_text(0));
    return out;
}

} // namespace index_ai
