// arbiter/src/tenant_store.cpp — see tenant_store.h
//
// SQLite is linked in as the only new dependency (system sqlite3, linked
// via find_package(SQLite3)).  The embedded DB file is single-writer
// friendly and sufficient for the single-binary deploy profile; when we
// outgrow it (multi-node, shared state), swap the connection layer for
// Postgres without rewriting the schema.

#include "tenant_store.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <openssl/rand.h>
#include <openssl/sha.h>
#include <sqlite3.h>

namespace arbiter {

namespace {

// ─── Helpers ────────────────────────────────────────────────────────────────

int64_t now_epoch() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// English stopword list — common articles, pronouns, auxiliary verbs,
// prepositions, question words, and meta-conversational fillers ("tell",
// "ask", "say", "want", "know") that appear in nearly every natural-
// language question without carrying retrieval signal.  Hand-curated:
// errs toward removing too few rather than too many, since over-
// pruning a content word would silently drop recall on queries that
// happen to centre on it.
//
// Why hand-curate instead of using a published list (NLTK, MySQL,
// etc.): published stopword lists are tuned for general document
// retrieval, not question-style conversational queries.  They tend to
// keep "what / when / how" (which we want to drop because they're
// ubiquitous in our query distribution) and drop "now / very" (which
// we want to keep because they carry signal in conversational
// memory).
const std::unordered_set<std::string>& fts5_stopwords() {
    static const std::unordered_set<std::string> kSet = {
        // Articles
        "a", "an", "the",
        // Pronouns
        "i", "me", "my", "mine", "myself",
        "you", "your", "yours", "yourself",
        "he", "him", "his", "himself",
        "she", "her", "hers", "herself",
        "it", "its", "itself",
        "we", "us", "our", "ours", "ourselves",
        "they", "them", "their", "theirs", "themselves",
        // Auxiliary / common verbs
        "is", "was", "were", "are", "am", "be", "been", "being",
        "do", "did", "does", "done", "doing",
        "have", "has", "had", "having",
        "will", "would", "could", "should", "shall",
        "may", "might", "must", "can",
        // Question words
        "what", "when", "where", "who", "whom", "whose",
        "why", "how", "which",
        // Prepositions / conjunctions
        "of", "in", "on", "at", "to", "for", "with", "without",
        "from", "by", "as", "into", "onto", "about", "over", "under",
        "and", "or", "but", "if", "so", "than", "then", "because",
        "while", "during", "before", "after",
        // Determiners
        "this", "that", "these", "those",
        "some", "any", "each", "every",
        // Conversational fillers — meta-verbs about the conversation
        // itself.  "Tell me about X" → drop "tell", "me", "about".
        "tell", "told", "say", "said", "saying",
        "ask", "asked", "asking",
        "talk", "talked", "talking",
        "mention", "mentioned",
        "remember", "recall", "know", "knew",
        // Generic verbs that rarely disambiguate
        "get", "got", "getting",
        "make", "made", "making",
        "take", "took", "taking",
        "go", "went", "going",
        // "not" intentionally kept — negation can flip meaning.
    };
    return kSet;
}

bool fts5_is_stopword(const std::string& tok) {
    std::string lower;
    lower.reserve(tok.size());
    for (char c : tok) {
        lower += static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));
    }
    return fts5_stopwords().count(lower) > 0;
}

// Quote a single token for FTS5, escaping internal quotes by doubling.
// FTS5 treats `"foo"` as a single-token query (still goes through the
// Porter tokenizer at match time so stems still apply).
void fts5_quote_token(std::string& out, const std::string& tok) {
    out += '"';
    for (char c : tok) {
        if (c == '"') out += "\"\"";
        else          out += c;
    }
    out += '"';
}

// Convert a free-form user query into a safe FTS5 expression.
//
// Pipeline:
//   1. Tokenise on whitespace.
//   2. Strip stopwords (a/the/what/did/i/you/...).  Common conversational
//      filler dilutes BM25 scoring across hundreds of thousands of
//      irrelevant rows; removing it concentrates the score on
//      content-bearing tokens and dramatically tightens the
//      candidate pool.
//   3. If 2+ content tokens remain, emit a phrase clause first
//      (`"tok1 tok2 ..."`) so rows containing the tokens adjacent
//      score above rows containing them separately.  Phrase
//      proximity is a strong signal in factual recall queries
//      ("software engineer", "machine learning", "Tucker Carlson")
//      where the meaningful concept is multi-word.
//   4. Append the bag-of-words OR fallback (`tok1 OR tok2 OR ...`)
//      so we still match rows that have the words separately —
//      OR-rather-than-AND keeps recall on queries where no single
//      row contains every content token.
//
// All tokens are quoted to neutralise FTS5 operator characters
// (-, +, *, ^, :, parens).  Quoting still goes through Porter at match
// time, so `"deploys"` still matches a document containing
// "deployment".
//
// Degenerate cases:
//   • Empty input → empty string (caller treats as "no filter").
//   • Every token a stopword (e.g. "what is the?") → fall back to the
//     original tokens.  Better to keep a low-quality query working
//     than fail silently on an edge case.
std::string fts5_escape(const std::string& q) {
    // Split on whitespace.
    std::vector<std::string> tokens;
    {
        size_t i = 0;
        while (i < q.size()) {
            while (i < q.size() &&
                   std::isspace(static_cast<unsigned char>(q[i]))) ++i;
            if (i >= q.size()) break;
            size_t j = i;
            while (j < q.size() &&
                   !std::isspace(static_cast<unsigned char>(q[j]))) ++j;
            tokens.emplace_back(q.substr(i, j - i));
            i = j;
        }
    }
    if (tokens.empty()) return {};

    // Strip stopwords; if every token was a stopword, fall back to the
    // original list so degenerate queries still produce a result.
    std::vector<std::string> kept;
    kept.reserve(tokens.size());
    for (auto& t : tokens) {
        if (!fts5_is_stopword(t)) kept.push_back(t);
    }
    const std::vector<std::string>& final_toks =
        kept.empty() ? tokens : kept;

    std::string out;
    out.reserve(q.size() * 3);

    // Phrase clause for 2+ content tokens.  FTS5 treats a quoted string
    // with internal whitespace as a phrase query (tokens must appear
    // adjacent).  A row matching the phrase scores it as one term plus
    // each constituent term separately (because we OR them below), so
    // phrase-matching rows dominate the ranking automatically — no
    // extra weight tuning required.
    if (final_toks.size() >= 2) {
        out += '"';
        for (size_t k = 0; k < final_toks.size(); ++k) {
            if (k > 0) out += ' ';
            for (char c : final_toks[k]) {
                if (c == '"') out += "\"\"";
                else          out += c;
            }
        }
        out += '"';
    }

    // NEAR proximity clause — bridges the gap between strict phrase
    // (high precision, low recall) and bag-of-words OR (high recall,
    // low precision).  FTS5's NEAR(t1 t2 ... tn, K) matches rows where
    // every token appears within K word positions of any other.  K=8
    // is a comfortable default — wide enough to span "<token> the
    // other day before <token>" but narrow enough to filter out rows
    // that just happen to mention the tokens in different paragraphs.
    //
    // Only emitted for 2..6 tokens.  Single-token queries don't need
    // proximity (NEAR needs ≥2 phrases anyway), and very long queries
    // either devolve into runtime-expensive cross-products or lose
    // their conceptual cohesion to the point where adjacency stops
    // being a useful signal.
    if (final_toks.size() >= 2 && final_toks.size() <= 6) {
        if (!out.empty()) out += " OR ";
        out += "NEAR(";
        for (size_t k = 0; k < final_toks.size(); ++k) {
            if (k > 0) out += ' ';
            fts5_quote_token(out, final_toks[k]);
        }
        out += ", 8)";
    }

    // Bag-of-words OR fallback.
    for (auto& t : final_toks) {
        if (!out.empty()) out += " OR ";
        fts5_quote_token(out, t);
    }
    return out;
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
    t.id           = q.column_int64(0);
    t.api_key_hash = q.column_text(1);
    t.name         = q.column_text(2);
    t.disabled     = q.column_int64(3) != 0;
    t.created_at   = q.column_int64(4);
    t.last_used_at = q.column_int64(5);
    return t;
}

constexpr const char* kTenantCols =
    "id, api_key_hash, name, disabled, created_at, last_used_at";

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

    // WAL gives us concurrent readers while the writer appends.
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
            disabled             INTEGER NOT NULL DEFAULT 0,
            created_at           INTEGER NOT NULL,
            last_used_at         INTEGER NOT NULL DEFAULT 0
        );
    )SQL");

    // Billing-cleanup migration.  Older DBs carry a usage_log table plus
    // monthly_cap_uc/month_yyyymm/month_to_date_uc columns on `tenants` —
    // billing is now external, so drop the dead schema on first open of
    // an upgraded DB.  ALTER TABLE DROP COLUMN needs SQLite ≥ 3.35
    // (Mar 2021); both macOS and modern Linux ship newer builds.
    // IF EXISTS / column-existence guards keep the migration idempotent
    // across reopens.
    exec_sql(db_, "DROP TABLE IF EXISTS usage_log;");
    auto tenant_col_exists = [this](const char* col) -> bool {
        Stmt q(db_, "PRAGMA table_info(tenants);");
        while (q.step() == SQLITE_ROW) {
            if (q.column_text(1) == col) return true;
        }
        return false;
    };
    auto drop_tenant_col = [this, &tenant_col_exists](const char* col) {
        if (!tenant_col_exists(col)) return;
        std::string sql = std::string("ALTER TABLE tenants DROP COLUMN ") + col + ";";
        exec_sql(db_, sql.c_str());
    };
    drop_tenant_col("monthly_cap_uc");
    drop_tenant_col("month_yyyymm");
    drop_tenant_col("month_to_date_uc");

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
            created_at      INTEGER NOT NULL,
            request_id      TEXT,
            FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE
        );
    )SQL");
    exec_sql(db_, R"SQL(
        CREATE INDEX IF NOT EXISTS messages_conversation_id
            ON messages(conversation_id, id);
    )SQL");
    // Billing-cleanup migration: drop the legacy `billed_uc` column on
    // pre-billing-extraction DBs.  Same idempotent pattern as tenants
    // above.
    {
        auto msg_col_exists = [this](const char* col) -> bool {
            Stmt q(db_, "PRAGMA table_info(messages);");
            while (q.step() == SQLITE_ROW) {
                if (q.column_text(1) == col) return true;
            }
            return false;
        };
        if (msg_col_exists("billed_uc"))
            exec_sql(db_, "ALTER TABLE messages DROP COLUMN billed_uc;");
    }

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
    // doesn't already exist — guard with PRAGMA table_info.
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
    // Optional artifact reference (added in v9).  Nullable → 0 in our
    // row mapping when unset.  No FK declared inline because ALTER TABLE
    // ADD COLUMN can't add a FK in SQLite — the soft reference is fine
    // since cleanup happens via an explicit nullify in delete_artifact()
    // rather than relying on the engine's cascade.
    add_entries_col("artifact_id", "INTEGER");
    // Temporal validity windows.  `valid_from` is when the fact became
    // true (defaults to created_at on insert); `valid_to` is when it
    // stopped being true.  NULL valid_to means "currently active" — the
    // common case.  Soft-delete via invalidate_entry() sets valid_to.
    // Default reads filter to active rows; EntryFilter::as_of selects a
    // historical timestamp.  Hard delete still cascades through
    // delete_entry() — the temporal window doesn't replace it, it adds
    // a fact-no-longer-true semantic alongside.
    add_entries_col("valid_from", "INTEGER");
    add_entries_col("valid_to",   "INTEGER");
    // Optional conversation scope.  NULL means "unscoped — available from
    // any conversation" so old rows that pre-date the column stay
    // discoverable across all reads.  Positive values pin the entry to
    // a specific conversation, e.g. a /mem add entry made during turn N
    // gets that turn's conversation_id; reads run conversation-scoped
    // first via search_entries_graduated and broaden if not enough hits
    // come back.
    add_entries_col("conversation_id", "INTEGER");
    // Backfill `valid_from` for rows that pre-date the migration.  No-op
    // on subsequent opens (create_entry sets the column explicitly).
    exec_sql(db_,
        "UPDATE memory_entries "
        "   SET valid_from = created_at "
        " WHERE valid_from IS NULL;");
    // Partial index makes the default "active rows only" filter cheap on
    // tenants with deep history.  WHERE valid_to IS NULL keeps the index
    // small — invalidated rows aren't indexed at all here.
    exec_sql(db_, R"SQL(
        CREATE INDEX IF NOT EXISTS memory_entries_tenant_active
            ON memory_entries(tenant_id, updated_at DESC)
            WHERE valid_to IS NULL;
    )SQL");
    // Conversation-scoped reads use this index.  Same partial filter as
    // above so the index stays small — invalidated rows aren't indexed.
    exec_sql(db_, R"SQL(
        CREATE INDEX IF NOT EXISTS memory_entries_tenant_conv_active
            ON memory_entries(tenant_id, conversation_id, updated_at DESC)
            WHERE valid_to IS NULL;
    )SQL");

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

    // ── Memory-entry full-text search (FTS5) ───────────────────────────
    // External-content table mirroring memory_entries(title, content,
    // tags, source) so `/mem search` ranks by Okapi-BM25 instead of
    // returning anything containing a substring, newest-first.
    //
    // External-content mode (`content='memory_entries'`) means the FTS
    // table stores only the inverted index; the original text is fetched
    // through `content_rowid='id'` from the source table.  Three triggers
    // keep the index in sync; a one-shot rebuild guard below populates
    // it for DBs that existed before this migration landed.
    //
    // Tokenizer:
    //   • porter — English stemming so "deploys" matches "deployment"
    //   • unicode61 — case-folded Unicode word breaks
    //   • remove_diacritics 2 — accent-insensitive (NFKD pass)
    exec_sql(db_, R"SQL(
        CREATE VIRTUAL TABLE IF NOT EXISTS memory_entries_fts
        USING fts5(
            title, content, tags, source,
            content='memory_entries',
            content_rowid='id',
            tokenize='porter unicode61 remove_diacritics 2'
        );
    )SQL");
    exec_sql(db_, R"SQL(
        CREATE TRIGGER IF NOT EXISTS memory_entries_fts_ai
        AFTER INSERT ON memory_entries
        BEGIN
            INSERT INTO memory_entries_fts(rowid, title, content, tags, source)
            VALUES (new.id, new.title, new.content, new.tags, new.source);
        END;
    )SQL");
    exec_sql(db_, R"SQL(
        CREATE TRIGGER IF NOT EXISTS memory_entries_fts_ad
        AFTER DELETE ON memory_entries
        BEGIN
            INSERT INTO memory_entries_fts(memory_entries_fts, rowid,
                                            title, content, tags, source)
            VALUES('delete', old.id, old.title, old.content,
                   old.tags, old.source);
        END;
    )SQL");
    exec_sql(db_, R"SQL(
        CREATE TRIGGER IF NOT EXISTS memory_entries_fts_au
        AFTER UPDATE ON memory_entries
        BEGIN
            INSERT INTO memory_entries_fts(memory_entries_fts, rowid,
                                            title, content, tags, source)
            VALUES('delete', old.id, old.title, old.content,
                   old.tags, old.source);
            INSERT INTO memory_entries_fts(rowid, title, content, tags, source)
            VALUES (new.id, new.title, new.content, new.tags, new.source);
        END;
    )SQL");

    // One-shot rebuild for DBs that pre-date the FTS table.  Triggers
    // catch every future write; this fills the index for rows that
    // already existed.
    //
    // Detection uses PRAGMA user_version rather than a COUNT(*) probe:
    // an external-content FTS5 table delegates COUNT(*) to the source
    // table, so it always reports the source row count, never zero,
    // even when the inverted index is empty.  user_version defaults to
    // 0 on every fresh-or-pre-migration DB; we bump to 1 once after a
    // successful rebuild and skip on subsequent opens.
    {
        Stmt q(db_, "PRAGMA user_version;");
        int64_t v = (q.step() == SQLITE_ROW) ? q.column_int64(0) : 0;
        if (v < 1) {
            exec_sql(db_,
                "INSERT INTO memory_entries_fts(memory_entries_fts) "
                "VALUES('rebuild');");
            exec_sql(db_, "PRAGMA user_version = 1;");
        }
    }

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

    // ── Artifact store (added in v8) ───────────────────────────────────
    // Per-(tenant, conversation, path) blobs for /write --persist.  The
    // unique index gates PUT-on-conflict semantics in put_artifact; the
    // CASCADE FKs on conversation_id + tenant_id mean conversation /
    // tenant deletion drops artifacts automatically.
    exec_sql(db_, R"SQL(
        CREATE TABLE IF NOT EXISTS tenant_artifacts (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            tenant_id       INTEGER NOT NULL,
            conversation_id INTEGER NOT NULL,
            path            TEXT    NOT NULL,
            content         BLOB    NOT NULL,
            sha256          TEXT    NOT NULL,
            mime_type       TEXT    NOT NULL DEFAULT 'application/octet-stream',
            size            INTEGER NOT NULL,
            created_at      INTEGER NOT NULL,
            updated_at      INTEGER NOT NULL,
            FOREIGN KEY (tenant_id)       REFERENCES tenants(id)       ON DELETE CASCADE,
            FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE
        );
    )SQL");
    exec_sql(db_, R"SQL(
        CREATE UNIQUE INDEX IF NOT EXISTS tenant_artifacts_unique
            ON tenant_artifacts(tenant_id, conversation_id, path);
    )SQL");
    exec_sql(db_, R"SQL(
        CREATE INDEX IF NOT EXISTS tenant_artifacts_tenant_updated
            ON tenant_artifacts(tenant_id, updated_at DESC);
    )SQL");
    exec_sql(db_, R"SQL(
        CREATE INDEX IF NOT EXISTS tenant_artifacts_conv_updated
            ON tenant_artifacts(conversation_id, updated_at DESC);
    )SQL");

    // Nullify dangling memory_entries.artifact_id references when an
    // artifact is deleted by ANY path — including the FK CASCADE that
    // fires when a conversation is dropped (which our delete_artifact
    // helper never sees).  Without this trigger, the next read of the
    // affected memory entry would surface a stale id pointing at
    // nothing.
    exec_sql(db_, R"SQL(
        CREATE TRIGGER IF NOT EXISTS memory_entries_artifact_id_clear
        AFTER DELETE ON tenant_artifacts
        BEGIN
            UPDATE memory_entries
               SET artifact_id = NULL
             WHERE artifact_id = OLD.id;
        END;
    )SQL");

    // ── A2A task store (added in v9 for /v1/a2a/agents/:id) ────────────
    // One row per A2A message/send or message/stream invocation.  task_id
    // is the same value as the arbiter request_id stamped into the
    // InFlightRegistry, so /v1/requests/:id/cancel and tasks/cancel
    // both resolve through the same handle.  context_id is opaque from
    // arbiter's perspective — it threads through the protocol verbatim
    // and is not foreign-keyed against conversations (PR-4).
    exec_sql(db_, R"SQL(
        CREATE TABLE IF NOT EXISTS a2a_tasks (
            task_id            TEXT    PRIMARY KEY,
            tenant_id          INTEGER NOT NULL,
            agent_id           TEXT    NOT NULL,
            context_id         TEXT    NOT NULL DEFAULT '',
            state              TEXT    NOT NULL,
            created_at         INTEGER NOT NULL,
            updated_at         INTEGER NOT NULL,
            final_message_json TEXT    NOT NULL DEFAULT '',
            error_message      TEXT    NOT NULL DEFAULT '',
            FOREIGN KEY (tenant_id) REFERENCES tenants(id) ON DELETE CASCADE
        );
    )SQL");
    exec_sql(db_, R"SQL(
        CREATE INDEX IF NOT EXISTS a2a_tasks_tenant_updated
            ON a2a_tasks(tenant_id, updated_at DESC);
    )SQL");
    exec_sql(db_, R"SQL(
        CREATE INDEX IF NOT EXISTS a2a_tasks_context
            ON a2a_tasks(tenant_id, context_id);
    )SQL");

    // Scheduled tasks: persistent agent-scheduled background work, fired
    // by the Scheduler tick thread.  Status-indexed for the tick query
    // (which scans `status='active' AND next_fire_at <= now`); tenant-
    // indexed for /v1/schedules listing.
    exec_sql(db_, R"SQL(
        CREATE TABLE IF NOT EXISTS scheduled_tasks (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            tenant_id       INTEGER NOT NULL,
            agent_id        TEXT    NOT NULL,
            conversation_id INTEGER NOT NULL DEFAULT 0,
            message         TEXT    NOT NULL,
            schedule_phrase TEXT    NOT NULL,
            schedule_kind   TEXT    NOT NULL,
            fire_at         INTEGER NOT NULL DEFAULT 0,
            recur_json      TEXT    NOT NULL DEFAULT '',
            next_fire_at    INTEGER NOT NULL,
            status          TEXT    NOT NULL,
            created_at      INTEGER NOT NULL,
            updated_at      INTEGER NOT NULL,
            last_run_at     INTEGER NOT NULL DEFAULT 0,
            last_run_id     INTEGER NOT NULL DEFAULT 0,
            run_count       INTEGER NOT NULL DEFAULT 0,
            FOREIGN KEY (tenant_id) REFERENCES tenants(id) ON DELETE CASCADE
        );
    )SQL");
    exec_sql(db_, R"SQL(
        CREATE INDEX IF NOT EXISTS scheduled_tasks_due
            ON scheduled_tasks(status, next_fire_at);
    )SQL");
    exec_sql(db_, R"SQL(
        CREATE INDEX IF NOT EXISTS scheduled_tasks_tenant
            ON scheduled_tasks(tenant_id, status, updated_at DESC);
    )SQL");

    // Task runs: one row per fired execution.  Tenant + task indexed for
    // listing; cascade-delete with the parent task row and the tenant.
    exec_sql(db_, R"SQL(
        CREATE TABLE IF NOT EXISTS task_runs (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            tenant_id       INTEGER NOT NULL,
            task_id         INTEGER NOT NULL,
            status          TEXT    NOT NULL,
            started_at      INTEGER NOT NULL,
            completed_at    INTEGER NOT NULL DEFAULT 0,
            request_id      TEXT    NOT NULL DEFAULT '',
            result_summary  TEXT    NOT NULL DEFAULT '',
            error_message   TEXT    NOT NULL DEFAULT '',
            input_tokens    INTEGER NOT NULL DEFAULT 0,
            output_tokens   INTEGER NOT NULL DEFAULT 0,
            notified        INTEGER NOT NULL DEFAULT 0,
            FOREIGN KEY (tenant_id) REFERENCES tenants(id) ON DELETE CASCADE,
            FOREIGN KEY (task_id)   REFERENCES scheduled_tasks(id) ON DELETE CASCADE
        );
    )SQL");
    exec_sql(db_, R"SQL(
        CREATE INDEX IF NOT EXISTS task_runs_task
            ON task_runs(task_id, started_at DESC);
    )SQL");
    exec_sql(db_, R"SQL(
        CREATE INDEX IF NOT EXISTS task_runs_tenant
            ON task_runs(tenant_id, started_at DESC);
    )SQL");
}

TenantStore::CreatedTenant
TenantStore::create_tenant(const std::string& name) {
    if (!db_) throw std::runtime_error("TenantStore not opened");

    std::string token = generate_token();
    std::string hash  = sha256_hex(token);
    int64_t     ts    = now_epoch();

    Stmt q(db_,
           "INSERT INTO tenants (api_key_hash, name, disabled, created_at, last_used_at) "
           "VALUES (?, ?, 0, ?, 0);");
    q.bind(1, hash);
    q.bind(2, name);
    q.bind(3, ts);

    int rc = q.step();
    if (rc != SQLITE_DONE) {
        check_sqlite(db_, rc, "insert tenant");
    }

    Tenant t;
    t.id           = sqlite3_last_insert_rowid(db_);
    t.api_key_hash = hash;
    t.name         = name;
    t.disabled     = false;
    t.created_at   = ts;
    t.last_used_at = 0;
    return {t, token};
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
    "input_tokens, output_tokens, created_at, request_id";

ConversationMessage row_to_message(Stmt& q) {
    ConversationMessage m;
    m.id              = q.column_int64(0);
    m.conversation_id = q.column_int64(1);
    m.role            = q.column_text(2);
    m.content         = q.column_text(3);
    m.input_tokens    = q.column_int64(4);
    m.output_tokens   = q.column_int64(5);
    m.created_at      = q.column_int64(6);
    m.request_id      = q.column_text(7);
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
                    .append(") VALUES (NULL, ?, ?, ?, ?, ?, ?, ?);").c_str());
            ins.bind(1, conversation_id);
            ins.bind(2, role);
            ins.bind(3, content);
            ins.bind(4, input_tokens);
            ins.bind(5, output_tokens);
            ins.bind(6, now);
            if (request_id.empty()) ins.bind(7, nullptr);
            else                    ins.bind(7, request_id);
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

// The `status` column is preserved on disk for back-compat with rows
// written under the old proposal-queue model — those rows now appear
// alongside everything else, since agents write directly without a
// review step.  We just stop selecting / filtering on it.
constexpr const char* kEntryCols =
    "id, tenant_id, type, title, content, source, tags, "
    "artifact_id, created_at, updated_at, valid_from, valid_to, "
    "conversation_id";

MemoryEntry row_to_entry(Stmt& q) {
    MemoryEntry e;
    e.id          = q.column_int64(0);
    e.tenant_id   = q.column_int64(1);
    e.type        = q.column_text(2);
    e.title       = q.column_text(3);
    e.content     = q.column_text(4);
    e.source      = q.column_text(5);
    e.tags_json   = q.column_text(6);
    // SQLite NULL → 0 via column_int64; matches our "0 = no artifact"
    // sentinel in the public struct.
    e.artifact_id = q.column_int64(7);
    e.created_at  = q.column_int64(8);
    e.updated_at  = q.column_int64(9);
    // valid_from is always set on insert (no NULL).  valid_to is NULL
    // for active rows; column_int64 maps NULL → 0, matching the
    // "0 = active" convention in MemoryEntry.
    e.valid_from       = q.column_int64(10);
    e.valid_to         = q.column_int64(11);
    // conversation_id NULL → 0 (unscoped).
    e.conversation_id  = q.column_int64(12);
    return e;
}

constexpr const char* kRelationCols =
    "id, tenant_id, source_id, target_id, relation, created_at";

MemoryRelation row_to_relation(Stmt& q) {
    MemoryRelation r;
    r.id         = q.column_int64(0);
    r.tenant_id  = q.column_int64(1);
    r.source_id  = q.column_int64(2);
    r.target_id  = q.column_int64(3);
    r.relation   = q.column_text(4);
    r.created_at = q.column_int64(5);
    return r;
}

} // namespace

MemoryEntry TenantStore::create_entry(int64_t tenant_id,
                                       const std::string& type,
                                       const std::string& title,
                                       const std::string& content,
                                       const std::string& source,
                                       const std::string& tags_json,
                                       int64_t artifact_id,
                                       int64_t conversation_id,
                                       int64_t created_at_override) {
    if (!db_) throw std::runtime_error("TenantStore not opened");

    const int64_t wall_now = now_epoch();
    // Override applies to created_at, updated_at, valid_from together —
    // the entry should look as if it was authored at the override time.
    // Caller is trusted to pass a sane epoch; we don't sanity-check.
    const int64_t now = (created_at_override > 0) ? created_at_override
                                                   : wall_now;
    // Don't write the status column explicitly — its SQL default is
    // 'accepted' from the legacy schema, which is what we want now that
    // proposals are gone.  `valid_from` is set explicitly to make the
    // column non-NULL on insert; `valid_to` stays NULL (active row).
    // `conversation_id` is NULL when the caller passes 0 — keeps the
    // column sparse for entries created outside a conversation context
    // (HTTP admin imports, scripted seeds, the historical case where
    // the column didn't exist).
    Stmt q(db_,
        "INSERT INTO memory_entries "
        "(tenant_id, type, title, content, source, tags, "
        " artifact_id, created_at, updated_at, valid_from, "
        " conversation_id) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
    q.bind(1, tenant_id);
    q.bind(2, type);
    q.bind(3, title);
    q.bind(4, content);
    q.bind(5, source);
    q.bind(6, tags_json.empty() ? std::string("[]") : tags_json);
    // 0 ⇒ NULL — keeps the column sparse rather than seeding zero
    // pseudo-references that would all collide on join.
    if (artifact_id > 0) q.bind(7, artifact_id);
    else                  sqlite3_bind_null(q.raw(), 7);
    q.bind(8, now);
    q.bind(9, now);
    q.bind(10, now);
    if (conversation_id > 0) q.bind(11, conversation_id);
    else                      sqlite3_bind_null(q.raw(), 11);
    int rc = q.step();
    if (rc != SQLITE_DONE) check_sqlite(db_, rc, "insert memory_entry");

    MemoryEntry e;
    e.id              = sqlite3_last_insert_rowid(db_);
    e.tenant_id       = tenant_id;
    e.type            = type;
    e.title           = title;
    e.content         = content;
    e.source          = source;
    e.tags_json       = tags_json.empty() ? "[]" : tags_json;
    e.artifact_id     = artifact_id;
    e.created_at      = now;
    e.updated_at      = now;
    e.valid_from      = now;
    e.valid_to        = 0;          // active
    e.conversation_id = conversation_id;
    return e;
}

std::optional<MemoryEntry>
TenantStore::get_entry(int64_t tenant_id, int64_t id) const {
    if (!db_) return std::nullopt;
    // Filters to active rows only — invalidated entries surface via
    // list_entries with EntryFilter::as_of, not through point-lookup.
    // A direct id read of an invalidated entry returning the row would
    // make it easy for callers to skip the temporal-window check by
    // accident; the path of least resistance should respect validity.
    Stmt q(db_, (std::string("SELECT ") + kEntryCols +
                 " FROM memory_entries "
                 "WHERE tenant_id = ? AND id = ? AND valid_to IS NULL;").c_str());
    q.bind(1, tenant_id);
    q.bind(2, id);
    if (q.step() != SQLITE_ROW) return std::nullopt;
    return row_to_entry(q);
}

std::vector<MemoryEntry>
TenantStore::list_entries(int64_t tenant_id, const EntryFilter& f) const {
    std::vector<MemoryEntry> out;
    if (!db_) return out;

    const int cap = (f.limit > 0 && f.limit <= 200) ? f.limit : 50;
    const std::string fts_query = f.q.empty() ? std::string()
                                              : fts5_escape(f.q);

    // ── Search path (q is non-empty) ───────────────────────────────────
    // FTS5 MATCH + Okapi-BM25 ranking, with type / tag treated as score
    // *boosts* rather than hard filters — see EntryFilter doc-comment.
    //
    // Per-field weights in the bm25() call (title, content, tags, source).
    // Title and tags are short, editorial, and high-information per token;
    // content is long and noisier; source is provenance flavour.
    //
    // SQLite's bm25() returns a value where smaller (more-negative) is a
    // stronger match.  Multiplying by a fraction < 1 makes a more-negative
    // score *more* negative — i.e. boosts it up the ranking.  Boost
    // factors below were tuned by hand: roughly 30% for type match, 20%
    // for tag match.  Tunable; document if changed.
    if (!fts_query.empty()) {
        // SQLite's bm25() returns *negative* numbers where smaller (more
        // negative) is a stronger match.  To boost a row we want to make
        // its score *more* negative — which means multiplying by a number
        // greater than 1.  Suppression would use a factor < 1, but we
        // don't currently use that direction.
        //
        // Boost magnitudes were tuned by hand on small fixtures: ~30% for
        // a type match, ~20% for a tag match.  Tunable; document if
        // changed.
        constexpr double kTypeBoost = 1.3;
        constexpr double kTagBoost  = 1.2;
        // Prefixed select-list — same column order as kEntryCols so
        // row_to_entry's positional reads still line up.  Required because
        // memory_entries_fts has columns of the same name (title, content,
        // tags, source) on the join.
        constexpr const char* kEntryColsPrefixed =
            "e.id, e.tenant_id, e.type, e.title, e.content, e.source, "
            "e.tags, e.artifact_id, e.created_at, e.updated_at, "
            "e.valid_from, e.valid_to, e.conversation_id";

        std::string sql =
            std::string("SELECT ") + kEntryColsPrefixed + ", "
            "(bm25(memory_entries_fts, 10.0, 4.0, 8.0, 2.0)";
        if (!f.types.empty()) {
            sql += " * CASE WHEN e.type IN (";
            for (size_t i = 0; i < f.types.size(); ++i) {
                if (i) sql += ",";
                sql += "?";
            }
            sql += ") THEN " + std::to_string(kTypeBoost) +
                   " ELSE 1.0 END";
        }
        if (!f.tag.empty()) {
            sql += " * CASE WHEN e.tags LIKE ? THEN " +
                   std::to_string(kTagBoost) + " ELSE 1.0 END";
        }
        sql += ") AS score "
               "FROM memory_entries e "
               "JOIN memory_entries_fts fts ON fts.rowid = e.id "
               "WHERE e.tenant_id = ? "
               "  AND memory_entries_fts MATCH ?";
        if (f.as_of > 0) {
            sql += " AND e.valid_from <= ? "
                   " AND (e.valid_to IS NULL OR e.valid_to > ?)";
        } else {
            sql += " AND e.valid_to IS NULL";
        }
        if (f.conversation_id > 0) {
            // OR-NULL fallback: rows pinned to this conversation OR rows
            // that are unscoped stay reachable.  Pre-migration entries
            // (NULL conversation_id) are visible from every conversation.
            sql += " AND (e.conversation_id = ? "
                   "      OR e.conversation_id IS NULL)";
        }
        if (f.since > 0)             sql += " AND e.created_at >= ?";
        if (f.before_updated_at > 0) sql += " AND e.updated_at < ?";
        sql += " ORDER BY score ASC LIMIT ?;";

        Stmt q(db_, sql.c_str());
        int idx = 1;
        // Aliased columns first by virtue of the SELECT order ⇒ binds
        // come in CASE / WHERE / cap order.
        for (auto& t : f.types) q.bind(idx++, t);
        std::string tag_pat;
        if (!f.tag.empty()) {
            tag_pat = "%\"" + f.tag + "\"%";
            q.bind(idx++, tag_pat);
        }
        q.bind(idx++, tenant_id);
        q.bind(idx++, fts_query);
        if (f.as_of > 0) {
            q.bind(idx++, f.as_of);
            q.bind(idx++, f.as_of);
        }
        if (f.conversation_id > 0) q.bind(idx++, f.conversation_id);
        if (f.since > 0)             q.bind(idx++, f.since);
        if (f.before_updated_at > 0) q.bind(idx++, f.before_updated_at);
        q.bind(idx, static_cast<int64_t>(cap));

        while (q.step() == SQLITE_ROW) out.push_back(row_to_entry(q));
        return out;
    }

    // ── Browse path (q is empty) ───────────────────────────────────────
    // Hard-filter on type / tag and order by recency.  Same shape as
    // before the FTS migration — agents that just want "the latest N
    // project entries" still get a deterministic chronological list.
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
    if (f.as_of > 0) {
        sql += " AND valid_from <= ? "
               " AND (valid_to IS NULL OR valid_to > ?)";
    } else {
        sql += " AND valid_to IS NULL";
    }
    if (f.conversation_id > 0) {
        sql += " AND (conversation_id = ? OR conversation_id IS NULL)";
    }
    if (f.since > 0)             sql += " AND created_at >= ?";
    if (f.before_updated_at > 0) sql += " AND updated_at < ?";
    sql += " ORDER BY updated_at DESC LIMIT ?;";

    Stmt q(db_, sql.c_str());
    int idx = 1;
    q.bind(idx++, tenant_id);
    for (auto& t : f.types) q.bind(idx++, t);
    std::string tag_pat;
    if (!f.tag.empty()) {
        tag_pat = "%\"" + f.tag + "\"%";
        q.bind(idx++, tag_pat);
    }
    if (f.as_of > 0) {
        q.bind(idx++, f.as_of);
        q.bind(idx++, f.as_of);
    }
    if (f.conversation_id > 0) q.bind(idx++, f.conversation_id);
    if (f.since > 0)             q.bind(idx++, f.since);
    if (f.before_updated_at > 0) q.bind(idx++, f.before_updated_at);
    q.bind(idx, static_cast<int64_t>(cap));

    while (q.step() == SQLITE_ROW) out.push_back(row_to_entry(q));
    return out;
}

std::vector<MemoryEntry>
TenantStore::search_entries_graduated(int64_t tenant_id,
                                       const EntryFilter& f) const {
    // Empty FTS query: nothing to rank, return empty.  Caller wanting a
    // browse should use list_entries directly.
    if (f.q.empty()) return {};

    const int cap = (f.limit > 0 && f.limit <= 200) ? f.limit : 50;

    // No conversation context ⇒ collapse to a single tenant-wide query.
    if (f.conversation_id <= 0) {
        return list_entries(tenant_id, f);
    }

    // Two-pass reciprocal-rank fusion.  The previous behavior was
    // "conversation hits first, tenant-wide appended only if pass 1
    // came up short."  That sandbagged a strong tenant-wide hit behind
    // a weak conversation-local one whenever pass 1 returned enough
    // rows to skip pass 2 — exactly the multi-session question case
    // where the answer lives in another conversation.
    //
    // RRF treats each pass as a ranked list and scores an entry by
    //     Σ_p  weight_p / (k + rank_p)
    // where rank is 1-based and missing-from-pass contributes nothing.
    // k=60 is the canonical RRF constant; weight_p tilts the fusion
    // toward one ranker.  We weight the conversation pass at 1.5 and
    // tenant-wide at 1.0 to keep locality bias on close calls — an
    // entry that ranks well in *this* conversation should outrank a
    // tenant-wide entry of the same lexical strength — without letting
    // weak conversation hits crowd out clearly stronger wide ones.
    constexpr double kRrfK              = 60.0;
    constexpr double kConversationWeight = 1.5;
    constexpr double kTenantWideWeight   = 1.0;

    // Each pass pulls a wider candidate pool than `cap` so RRF has
    // enough overlap to fuse.  At cap=10, fusing two top-10 lists with
    // little overlap collapses to "concatenate," which defeats the
    // point.  3x cap (with a 50-row floor) gives enough rank depth for
    // genuine consensus signal without ballooning SQL cost — both
    // passes are indexed FTS5 reads with the same MATCH expression.
    const int pool_cap = std::max(cap * 3, 50);

    EntryFilter conv_f = f;
    conv_f.limit = pool_cap;
    std::vector<MemoryEntry> conv_hits = list_entries(tenant_id, conv_f);

    EntryFilter wide_f = f;
    wide_f.conversation_id = 0;
    wide_f.limit = pool_cap;
    std::vector<MemoryEntry> wide_hits = list_entries(tenant_id, wide_f);

    // Fast path: tenant-wide pass returned nothing meaningful (rare,
    // but the FTS query could be effectively conversation-private).
    // Skip the fusion bookkeeping entirely.
    if (wide_hits.empty()) {
        if (static_cast<int>(conv_hits.size()) > cap) conv_hits.resize(cap);
        return conv_hits;
    }
    if (conv_hits.empty()) {
        if (static_cast<int>(wide_hits.size()) > cap) wide_hits.resize(cap);
        return wide_hits;
    }

    // Build id → (entry pointer, fused score).  The entry is taken
    // from whichever pass surfaced it first; both passes return the
    // same row, so identity follows id rather than which list owns it.
    struct FusedRow {
        const MemoryEntry* entry;
        double             score;
    };
    std::unordered_map<int64_t, FusedRow> fused;
    fused.reserve(conv_hits.size() + wide_hits.size());

    for (size_t i = 0; i < conv_hits.size(); ++i) {
        const auto& e = conv_hits[i];
        double contrib = kConversationWeight /
                         (kRrfK + static_cast<double>(i + 1));
        auto it = fused.find(e.id);
        if (it == fused.end()) fused.emplace(e.id, FusedRow{&e, contrib});
        else                   it->second.score += contrib;
    }
    for (size_t i = 0; i < wide_hits.size(); ++i) {
        const auto& e = wide_hits[i];
        double contrib = kTenantWideWeight /
                         (kRrfK + static_cast<double>(i + 1));
        auto it = fused.find(e.id);
        if (it == fused.end()) fused.emplace(e.id, FusedRow{&e, contrib});
        else                   it->second.score += contrib;
    }

    // Sort by fused score descending; ties broken by id ascending so
    // results are deterministic across runs.
    std::vector<std::pair<int64_t, double>> ranked;
    ranked.reserve(fused.size());
    for (auto& [id, row] : fused) ranked.emplace_back(id, row.score);
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) {
                  if (a.second != b.second) return a.second > b.second;
                  return a.first < b.first;
              });

    std::vector<MemoryEntry> merged;
    merged.reserve(static_cast<size_t>(cap));
    for (auto& [id, _] : ranked) {
        if (static_cast<int>(merged.size()) >= cap) break;
        merged.push_back(*fused[id].entry);
    }
    return merged;
}

bool TenantStore::update_entry(int64_t tenant_id, int64_t id,
                                const std::optional<std::string>& title,
                                const std::optional<std::string>& content,
                                const std::optional<std::string>& source,
                                const std::optional<std::string>& tags_json,
                                const std::optional<std::string>& type,
                                const std::optional<int64_t>& artifact_id) {
    if (!db_) return false;

    std::vector<std::string> sets;
    if (title)       sets.push_back("title = ?");
    if (content)     sets.push_back("content = ?");
    if (source)      sets.push_back("source = ?");
    if (tags_json)   sets.push_back("tags = ?");
    if (type)        sets.push_back("type = ?");
    if (artifact_id) sets.push_back("artifact_id = ?");
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
    // Active-rows-only filter mirrors get_entry: an invalidated row can't
    // be edited.  If the caller needs to correct a historical record they
    // hard-delete and re-create — keeps "what was true at time T" honest.
    sql += " WHERE tenant_id = ? AND id = ? AND valid_to IS NULL;";

    Stmt q(db_, sql.c_str());
    int idx = 1;
    if (title)     q.bind(idx++, *title);
    if (content)   q.bind(idx++, *content);
    if (source)    q.bind(idx++, *source);
    if (tags_json) q.bind(idx++, *tags_json);
    if (type)      q.bind(idx++, *type);
    if (artifact_id) {
        // Caller passes 0 to clear the link (NULL); positive id sets it.
        if (*artifact_id > 0) q.bind(idx++, *artifact_id);
        else                   sqlite3_bind_null(q.raw(), idx++);
    }
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

bool TenantStore::invalidate_entry(int64_t tenant_id, int64_t id, int64_t when) {
    if (!db_) return false;
    const int64_t ts = (when > 0) ? when : now_epoch();
    // Idempotent rejection: only mark valid_to on rows where it's still
    // NULL.  A second call returns false rather than silently moving the
    // invalidation timestamp around — callers that want to change a
    // window should hard-delete and re-create instead.
    Stmt q(db_,
        "UPDATE memory_entries "
        "   SET valid_to = ? "
        " WHERE tenant_id = ? AND id = ? AND valid_to IS NULL;");
    q.bind(1, ts);
    q.bind(2, tenant_id);
    q.bind(3, id);
    q.step();
    return sqlite3_changes(db_) > 0;
}

std::optional<MemoryRelation>
TenantStore::create_relation(int64_t tenant_id,
                              int64_t source_id, int64_t target_id,
                              const std::string& relation) {
    if (!db_) return std::nullopt;
    const int64_t now = now_epoch();
    // Don't write the legacy `status` column; SQL default ('accepted')
    // applies for back-compat with existing rows that still have it.
    Stmt q(db_,
        "INSERT INTO memory_relations "
        "(tenant_id, source_id, target_id, relation, created_at) "
        "VALUES (?, ?, ?, ?, ?);");
    q.bind(1, tenant_id);
    q.bind(2, source_id);
    q.bind(3, target_id);
    q.bind(4, relation);
    q.bind(5, now);
    int rc = q.step();
    if (rc == SQLITE_CONSTRAINT) return std::nullopt;
    if (rc != SQLITE_DONE) check_sqlite(db_, rc, "insert memory_relation");

    MemoryRelation r;
    r.id         = sqlite3_last_insert_rowid(db_);
    r.tenant_id  = tenant_id;
    r.source_id  = source_id;
    r.target_id  = target_id;
    r.relation   = relation;
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
                             int limit) const {
    std::vector<MemoryRelation> out;
    if (!db_) return out;

    std::string sql = std::string("SELECT ") + kRelationCols +
                       " FROM memory_relations WHERE tenant_id = ?";
    if (source_id > 0)     sql += " AND source_id = ?";
    if (target_id > 0)     sql += " AND target_id = ?";
    if (!relation.empty()) sql += " AND relation = ?";
    const int cap = (limit > 0 && limit <= 1000) ? limit : 200;
    sql += " ORDER BY id DESC LIMIT ?;";

    Stmt q(db_, sql.c_str());
    int idx = 1;
    q.bind(idx++, tenant_id);
    if (source_id > 0)     q.bind(idx++, source_id);
    if (target_id > 0)     q.bind(idx++, target_id);
    if (!relation.empty()) q.bind(idx++, relation);
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

// ── A2A task store ─────────────────────────────────────────────────────

void TenantStore::create_a2a_task(int64_t tenant_id,
                                    const std::string& task_id,
                                    const std::string& agent_id,
                                    const std::string& context_id,
                                    const std::string& state) {
    if (!db_) throw std::runtime_error("TenantStore not opened");
    const int64_t ts = now_epoch();
    Stmt q(db_,
        "INSERT INTO a2a_tasks "
        "(task_id, tenant_id, agent_id, context_id, state, "
        " created_at, updated_at, final_message_json, error_message) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, '', '');");
    q.bind(1, task_id);
    q.bind(2, tenant_id);
    q.bind(3, agent_id);
    q.bind(4, context_id);
    q.bind(5, state);
    q.bind(6, ts);
    q.bind(7, ts);
    int rc = q.step();
    if (rc != SQLITE_DONE) check_sqlite(db_, rc, "insert a2a_task");
}

bool TenantStore::update_a2a_task(int64_t tenant_id,
                                    const std::string& task_id,
                                    const std::string& state,
                                    const std::string& final_message_json,
                                    const std::string& error_message) {
    if (!db_) return false;
    const int64_t ts = now_epoch();
    Stmt q(db_,
        "UPDATE a2a_tasks "
        "   SET state = ?, updated_at = ?, "
        "       final_message_json = ?, error_message = ? "
        " WHERE tenant_id = ? AND task_id = ?;");
    q.bind(1, state);
    q.bind(2, ts);
    q.bind(3, final_message_json);
    q.bind(4, error_message);
    q.bind(5, tenant_id);
    q.bind(6, task_id);
    q.step();
    return sqlite3_changes(db_) > 0;
}

std::optional<TenantStore::A2aTaskRecord>
TenantStore::get_a2a_task(int64_t tenant_id,
                            const std::string& task_id) const {
    if (!db_) return std::nullopt;
    Stmt q(db_,
        "SELECT task_id, tenant_id, agent_id, context_id, state, "
        "       created_at, updated_at, final_message_json, error_message "
        "  FROM a2a_tasks "
        " WHERE tenant_id = ? AND task_id = ?;");
    q.bind(1, tenant_id);
    q.bind(2, task_id);
    if (q.step() != SQLITE_ROW) return std::nullopt;
    A2aTaskRecord r;
    r.task_id            = q.column_text(0);
    r.tenant_id          = q.column_int64(1);
    r.agent_id           = q.column_text(2);
    r.context_id         = q.column_text(3);
    r.state              = q.column_text(4);
    r.created_at         = q.column_int64(5);
    r.updated_at         = q.column_int64(6);
    r.final_message_json = q.column_text(7);
    r.error_message      = q.column_text(8);
    return r;
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

// ── Artifact path sanitizer ────────────────────────────────────────────

namespace {

bool ci_equals(const std::string& a, const char* b) {
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return i == a.size() && b[i] == '\0';
}

// Strip an extension before checking Windows-reserved names — `CON.txt`
// is just as broken as `CON` on Windows.
std::string component_stem(const std::string& comp) {
    auto dot = comp.find('.');
    if (dot == std::string::npos) return comp;
    return comp.substr(0, dot);
}

bool is_windows_reserved_stem(const std::string& stem) {
    static const char* reserved[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5",
        "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5",
        "LPT6", "LPT7", "LPT8", "LPT9",
    };
    for (auto* r : reserved) if (ci_equals(stem, r)) return true;
    return false;
}

} // namespace

std::optional<std::string>
sanitize_artifact_path(const std::string& raw, std::string& err) {
    err.clear();
    if (raw.empty()) {
        err = "empty path";
        return std::nullopt;
    }
    if (raw.size() > 256) {
        err = "path > 256 chars";
        return std::nullopt;
    }

    // Normalise backslashes.  Then a Windows drive letter (`C:\foo`) shows
    // up as `C:/foo` and the `:` in the first component triggers the
    // colon check below.
    std::string s = raw;
    for (auto& c : s) if (c == '\\') c = '/';

    if (s.front() == '/') {
        err = "absolute paths not allowed";
        return std::nullopt;
    }

    // Walk components, rejecting anything dodgy.  We rebuild the canonical
    // form as we go — collapses runs of `/` and trims trailing slashes.
    std::ostringstream canon;
    size_t start = 0;
    bool first_component = true;
    while (start <= s.size()) {
        size_t slash = s.find('/', start);
        const size_t end = (slash == std::string::npos) ? s.size() : slash;
        std::string comp = s.substr(start, end - start);

        if (comp.empty()) {
            // `foo//bar` collapses to `foo/bar`; trailing `/` is dropped
            // by the loop terminating at end-of-string.  But a leading
            // `/` already errored above, so an empty FIRST component
            // can't reach here.
            if (slash == std::string::npos) break;
            start = slash + 1;
            continue;
        }
        if (comp.size() > 128) {
            err = "path component '" + comp.substr(0, 16) + "...' > 128 chars";
            return std::nullopt;
        }
        if (comp == "." || comp == "..") {
            err = "traversal component '" + comp + "' not allowed";
            return std::nullopt;
        }
        if (comp.front() == '.') {
            err = "hidden / dotfile components not allowed: '" + comp + "'";
            return std::nullopt;
        }
        for (unsigned char ch : comp) {
            if (ch == 0) {
                err = "null byte in path";
                return std::nullopt;
            }
            if (ch < 0x20 || ch == 0x7f) {
                err = "control character in path";
                return std::nullopt;
            }
            if (ch == ':') {
                err = "':' not allowed in path components (Windows alt-stream)";
                return std::nullopt;
            }
        }
        if (is_windows_reserved_stem(component_stem(comp))) {
            err = "'" + comp + "' is a Windows-reserved name";
            return std::nullopt;
        }

        if (!first_component) canon << '/';
        canon << comp;
        first_component = false;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }

    std::string out = canon.str();
    if (out.empty()) {
        err = "path resolved to empty after canonicalisation";
        return std::nullopt;
    }
    return out;
}

// ── Artifact CRUD ──────────────────────────────────────────────────────

namespace {

constexpr const char* kArtifactMetaCols =
    "id, tenant_id, conversation_id, path, sha256, mime_type, size, "
    "created_at, updated_at";

ArtifactRecord row_to_artifact(Stmt& q) {
    ArtifactRecord r;
    r.id              = q.column_int64(0);
    r.tenant_id       = q.column_int64(1);
    r.conversation_id = q.column_int64(2);
    r.path            = q.column_text(3);
    r.sha256          = q.column_text(4);
    r.mime_type       = q.column_text(5);
    r.size            = q.column_int64(6);
    r.created_at      = q.column_int64(7);
    r.updated_at      = q.column_int64(8);
    return r;
}

} // namespace

PutArtifactResult
TenantStore::put_artifact(int64_t tenant_id, int64_t conversation_id,
                           const std::string& sanitized_path,
                           const std::string& content,
                           const std::string& mime_type) {
    PutArtifactResult out;
    if (!db_) {
        out.status    = PutArtifactResult::Status::PathRejected;
        out.error_msg = "store not opened";
        return out;
    }

    const int64_t size = static_cast<int64_t>(content.size());
    if (size > kArtifactPerFileMaxBytes) {
        out.status    = PutArtifactResult::Status::QuotaExceeded;
        out.error_msg = "file size " + std::to_string(size) +
                         " exceeds per-file cap " +
                         std::to_string(kArtifactPerFileMaxBytes) + " bytes";
        out.tenant_used_bytes       = bytes_used_tenant(tenant_id);
        out.conversation_used_bytes =
            bytes_used_conversation(tenant_id, conversation_id);
        return out;
    }

    // Determine if this is an in-place update (same path → subtract its
    // size from quota math) or a fresh insert.
    int64_t existing_size = 0;
    bool    is_update     = false;
    {
        Stmt sel(db_, "SELECT size FROM tenant_artifacts "
                       "WHERE tenant_id = ? AND conversation_id = ? AND path = ?;");
        sel.bind(1, tenant_id);
        sel.bind(2, conversation_id);
        sel.bind(3, sanitized_path);
        if (sel.step() == SQLITE_ROW) {
            existing_size = sel.column_int64(0);
            is_update     = true;
        }
    }

    const int64_t conv_used   = bytes_used_conversation(tenant_id, conversation_id);
    const int64_t tenant_used = bytes_used_tenant(tenant_id);
    const int64_t conv_after  = conv_used   - existing_size + size;
    const int64_t tnt_after   = tenant_used - existing_size + size;

    if (conv_after > kArtifactPerConversationMaxBytes) {
        out.status    = PutArtifactResult::Status::QuotaExceeded;
        out.error_msg = "conversation quota exhausted: " +
                         std::to_string(conv_after) + " > " +
                         std::to_string(kArtifactPerConversationMaxBytes) + " bytes";
        out.tenant_used_bytes       = tenant_used;
        out.conversation_used_bytes = conv_used;
        return out;
    }
    if (tnt_after > kArtifactPerTenantMaxBytes) {
        out.status    = PutArtifactResult::Status::QuotaExceeded;
        out.error_msg = "tenant quota exhausted: " +
                         std::to_string(tnt_after) + " > " +
                         std::to_string(kArtifactPerTenantMaxBytes) + " bytes";
        out.tenant_used_bytes       = tenant_used;
        out.conversation_used_bytes = conv_used;
        return out;
    }

    const std::string digest    = sha256_hex(content);
    const std::string mime_safe = mime_type.empty() ? "application/octet-stream"
                                                     : mime_type;
    const int64_t now = now_epoch();

    if (is_update) {
        Stmt upd(db_,
            "UPDATE tenant_artifacts "
            "   SET content = ?, sha256 = ?, mime_type = ?, "
            "       size = ?, updated_at = ? "
            " WHERE tenant_id = ? AND conversation_id = ? AND path = ?;");
        sqlite3_bind_blob(upd.raw(), 1, content.data(),
                          static_cast<int>(content.size()), SQLITE_TRANSIENT);
        upd.bind(2, digest);
        upd.bind(3, mime_safe);
        upd.bind(4, size);
        upd.bind(5, now);
        upd.bind(6, tenant_id);
        upd.bind(7, conversation_id);
        upd.bind(8, sanitized_path);
        upd.step();
        out.status = PutArtifactResult::Status::Updated;
    } else {
        Stmt ins(db_,
            "INSERT INTO tenant_artifacts "
            "(tenant_id, conversation_id, path, content, sha256, "
            " mime_type, size, created_at, updated_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);");
        ins.bind(1, tenant_id);
        ins.bind(2, conversation_id);
        ins.bind(3, sanitized_path);
        sqlite3_bind_blob(ins.raw(), 4, content.data(),
                          static_cast<int>(content.size()), SQLITE_TRANSIENT);
        ins.bind(5, digest);
        ins.bind(6, mime_safe);
        ins.bind(7, size);
        ins.bind(8, now);
        ins.bind(9, now);
        int rc = ins.step();
        if (rc == SQLITE_CONSTRAINT) {
            // Race: a parallel writer beat us between the SELECT and the
            // INSERT.  Surface a clean error rather than silently
            // overwriting; the caller can retry as a PUT.
            out.status    = PutArtifactResult::Status::PathRejected;
            out.error_msg = "path collision (concurrent write); retry";
            out.tenant_used_bytes       = tenant_used;
            out.conversation_used_bytes = conv_used;
            return out;
        }
        out.status = PutArtifactResult::Status::Created;
    }

    // Reload the canonical row so the caller's response shape is
    // identical for create and update.
    Stmt sel(db_, (std::string("SELECT ") + kArtifactMetaCols +
                   " FROM tenant_artifacts "
                   "WHERE tenant_id = ? AND conversation_id = ? AND path = ?;").c_str());
    sel.bind(1, tenant_id);
    sel.bind(2, conversation_id);
    sel.bind(3, sanitized_path);
    if (sel.step() == SQLITE_ROW) {
        out.record = row_to_artifact(sel);
    }
    out.tenant_used_bytes       = tnt_after;
    out.conversation_used_bytes = conv_after;
    return out;
}

std::optional<ArtifactRecord>
TenantStore::get_artifact_meta(int64_t tenant_id, int64_t id) const {
    if (!db_) return std::nullopt;
    Stmt q(db_, (std::string("SELECT ") + kArtifactMetaCols +
                 " FROM tenant_artifacts WHERE tenant_id = ? AND id = ?;").c_str());
    q.bind(1, tenant_id);
    q.bind(2, id);
    if (q.step() != SQLITE_ROW) return std::nullopt;
    return row_to_artifact(q);
}

std::optional<std::string>
TenantStore::get_artifact_content(int64_t tenant_id, int64_t id) const {
    if (!db_) return std::nullopt;
    Stmt q(db_, "SELECT content FROM tenant_artifacts "
                 "WHERE tenant_id = ? AND id = ?;");
    q.bind(1, tenant_id);
    q.bind(2, id);
    if (q.step() != SQLITE_ROW) return std::nullopt;
    const void* blob = sqlite3_column_blob(q.raw(), 0);
    int n = sqlite3_column_bytes(q.raw(), 0);
    if (!blob || n <= 0) return std::string{};
    return std::string(static_cast<const char*>(blob), static_cast<size_t>(n));
}

std::optional<ArtifactRecord>
TenantStore::get_artifact_meta_by_path(int64_t tenant_id,
                                        int64_t conversation_id,
                                        const std::string& sanitized_path) const {
    if (!db_) return std::nullopt;
    Stmt q(db_, (std::string("SELECT ") + kArtifactMetaCols +
                 " FROM tenant_artifacts "
                 "WHERE tenant_id = ? AND conversation_id = ? AND path = ?;").c_str());
    q.bind(1, tenant_id);
    q.bind(2, conversation_id);
    q.bind(3, sanitized_path);
    if (q.step() != SQLITE_ROW) return std::nullopt;
    return row_to_artifact(q);
}

std::vector<ArtifactRecord>
TenantStore::list_artifacts_conversation(int64_t tenant_id,
                                          int64_t conversation_id,
                                          int limit) const {
    std::vector<ArtifactRecord> out;
    if (!db_) return out;
    const int cap = (limit > 0 && limit <= 200) ? limit : 50;
    Stmt q(db_, (std::string("SELECT ") + kArtifactMetaCols +
                 " FROM tenant_artifacts "
                 "WHERE tenant_id = ? AND conversation_id = ? "
                 " ORDER BY updated_at DESC LIMIT ?;").c_str());
    q.bind(1, tenant_id);
    q.bind(2, conversation_id);
    q.bind(3, static_cast<int64_t>(cap));
    while (q.step() == SQLITE_ROW) out.push_back(row_to_artifact(q));
    return out;
}

std::vector<ArtifactRecord>
TenantStore::list_artifacts_tenant(int64_t tenant_id, int limit) const {
    std::vector<ArtifactRecord> out;
    if (!db_) return out;
    const int cap = (limit > 0 && limit <= 200) ? limit : 50;
    Stmt q(db_, (std::string("SELECT ") + kArtifactMetaCols +
                 " FROM tenant_artifacts "
                 "WHERE tenant_id = ? "
                 " ORDER BY updated_at DESC LIMIT ?;").c_str());
    q.bind(1, tenant_id);
    q.bind(2, static_cast<int64_t>(cap));
    while (q.step() == SQLITE_ROW) out.push_back(row_to_artifact(q));
    return out;
}

bool TenantStore::delete_artifact(int64_t tenant_id, int64_t id) {
    if (!db_) return false;
    // Soft cascade: nullify memory_entries.artifact_id referencing this
    // row before deleting it.  The schema-level FK couldn't be added by
    // ALTER TABLE (SQLite limitation), so we do the SET NULL ourselves.
    // Tenant scope is doubly enforced — the WHERE clauses on both
    // statements include tenant_id.
    {
        Stmt clr(db_, "UPDATE memory_entries SET artifact_id = NULL "
                       "WHERE tenant_id = ? AND artifact_id = ?;");
        clr.bind(1, tenant_id);
        clr.bind(2, id);
        clr.step();
    }
    Stmt q(db_, "DELETE FROM tenant_artifacts WHERE tenant_id = ? AND id = ?;");
    q.bind(1, tenant_id);
    q.bind(2, id);
    q.step();
    return sqlite3_changes(db_) > 0;
}

int64_t TenantStore::bytes_used_tenant(int64_t tenant_id) const {
    if (!db_) return 0;
    Stmt q(db_, "SELECT COALESCE(SUM(size), 0) FROM tenant_artifacts "
                 "WHERE tenant_id = ?;");
    q.bind(1, tenant_id);
    if (q.step() != SQLITE_ROW) return 0;
    return q.column_int64(0);
}

int64_t TenantStore::bytes_used_conversation(int64_t tenant_id,
                                              int64_t conversation_id) const {
    if (!db_) return 0;
    Stmt q(db_, "SELECT COALESCE(SUM(size), 0) FROM tenant_artifacts "
                 "WHERE tenant_id = ? AND conversation_id = ?;");
    q.bind(1, tenant_id);
    q.bind(2, conversation_id);
    if (q.step() != SQLITE_ROW) return 0;
    return q.column_int64(0);
}

// ── Scheduled tasks ────────────────────────────────────────────────────

namespace {

constexpr const char* kScheduledTaskCols =
    "id, tenant_id, agent_id, conversation_id, message, schedule_phrase, "
    "schedule_kind, fire_at, recur_json, next_fire_at, status, "
    "created_at, updated_at, last_run_at, last_run_id, run_count";

TenantStore::ScheduledTask row_to_scheduled_task(Stmt& q) {
    TenantStore::ScheduledTask t;
    t.id              = q.column_int64(0);
    t.tenant_id       = q.column_int64(1);
    t.agent_id        = q.column_text(2);
    t.conversation_id = q.column_int64(3);
    t.message         = q.column_text(4);
    t.schedule_phrase = q.column_text(5);
    t.schedule_kind   = q.column_text(6);
    t.fire_at         = q.column_int64(7);
    t.recur_json      = q.column_text(8);
    t.next_fire_at    = q.column_int64(9);
    t.status          = q.column_text(10);
    t.created_at      = q.column_int64(11);
    t.updated_at      = q.column_int64(12);
    t.last_run_at     = q.column_int64(13);
    t.last_run_id     = q.column_int64(14);
    t.run_count       = q.column_int64(15);
    return t;
}

constexpr const char* kTaskRunCols =
    "id, tenant_id, task_id, status, started_at, completed_at, "
    "request_id, result_summary, error_message, "
    "input_tokens, output_tokens, notified";

TenantStore::TaskRun row_to_task_run(Stmt& q) {
    TenantStore::TaskRun r;
    r.id             = q.column_int64(0);
    r.tenant_id      = q.column_int64(1);
    r.task_id        = q.column_int64(2);
    r.status         = q.column_text(3);
    r.started_at     = q.column_int64(4);
    r.completed_at   = q.column_int64(5);
    r.request_id     = q.column_text(6);
    r.result_summary = q.column_text(7);
    r.error_message  = q.column_text(8);
    r.input_tokens   = q.column_int64(9);
    r.output_tokens  = q.column_int64(10);
    r.notified       = q.column_int64(11) != 0;
    return r;
}

} // namespace

TenantStore::ScheduledTask
TenantStore::create_scheduled_task(int64_t tenant_id,
                                    const std::string& agent_id,
                                    int64_t conversation_id,
                                    const std::string& message,
                                    const std::string& schedule_phrase,
                                    const std::string& schedule_kind,
                                    int64_t fire_at,
                                    const std::string& recur_json,
                                    int64_t next_fire_at) {
    if (!db_) throw std::runtime_error("TenantStore not opened");
    const int64_t ts = now_epoch();
    Stmt q(db_,
        "INSERT INTO scheduled_tasks "
        "(tenant_id, agent_id, conversation_id, message, schedule_phrase, "
        " schedule_kind, fire_at, recur_json, next_fire_at, status, "
        " created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 'active', ?, ?);");
    q.bind(1, tenant_id);
    q.bind(2, agent_id);
    q.bind(3, conversation_id);
    q.bind(4, message);
    q.bind(5, schedule_phrase);
    q.bind(6, schedule_kind);
    q.bind(7, fire_at);
    q.bind(8, recur_json);
    q.bind(9, next_fire_at);
    q.bind(10, ts);
    q.bind(11, ts);
    int rc = q.step();
    if (rc != SQLITE_DONE) check_sqlite(db_, rc, "insert scheduled_task");

    ScheduledTask t;
    t.id              = sqlite3_last_insert_rowid(db_);
    t.tenant_id       = tenant_id;
    t.agent_id        = agent_id;
    t.conversation_id = conversation_id;
    t.message         = message;
    t.schedule_phrase = schedule_phrase;
    t.schedule_kind   = schedule_kind;
    t.fire_at         = fire_at;
    t.recur_json      = recur_json;
    t.next_fire_at    = next_fire_at;
    t.status          = "active";
    t.created_at      = ts;
    t.updated_at      = ts;
    return t;
}

std::optional<TenantStore::ScheduledTask>
TenantStore::get_scheduled_task(int64_t tenant_id, int64_t id) const {
    if (!db_) return std::nullopt;
    std::string sql = std::string("SELECT ") + kScheduledTaskCols +
        " FROM scheduled_tasks WHERE tenant_id = ? AND id = ?;";
    Stmt q(db_, sql.c_str());
    q.bind(1, tenant_id);
    q.bind(2, id);
    if (q.step() != SQLITE_ROW) return std::nullopt;
    return row_to_scheduled_task(q);
}

std::vector<TenantStore::ScheduledTask>
TenantStore::list_scheduled_tasks(int64_t tenant_id,
                                    const std::string& status_filter,
                                    int limit) const {
    std::vector<ScheduledTask> out;
    if (!db_) return out;
    if (limit <= 0 || limit > 200) limit = 200;
    std::string sql = std::string("SELECT ") + kScheduledTaskCols +
        " FROM scheduled_tasks WHERE tenant_id = ?";
    if (!status_filter.empty()) sql += " AND status = ?";
    sql += " ORDER BY updated_at DESC LIMIT ?;";
    Stmt q(db_, sql.c_str());
    int idx = 1;
    q.bind(idx++, tenant_id);
    if (!status_filter.empty()) q.bind(idx++, status_filter);
    q.bind(idx, static_cast<int64_t>(limit));
    while (q.step() == SQLITE_ROW) out.push_back(row_to_scheduled_task(q));
    return out;
}

std::vector<TenantStore::ScheduledTask>
TenantStore::list_due_scheduled_tasks(int64_t cutoff_epoch, int limit) const {
    std::vector<ScheduledTask> out;
    if (!db_) return out;
    if (limit <= 0 || limit > 200) limit = 200;
    std::string sql = std::string("SELECT ") + kScheduledTaskCols +
        " FROM scheduled_tasks "
        " WHERE status = 'active' AND next_fire_at <= ?"
        " ORDER BY next_fire_at ASC LIMIT ?;";
    Stmt q(db_, sql.c_str());
    q.bind(1, cutoff_epoch);
    q.bind(2, static_cast<int64_t>(limit));
    while (q.step() == SQLITE_ROW) out.push_back(row_to_scheduled_task(q));
    return out;
}

bool TenantStore::update_scheduled_task(
        int64_t tenant_id, int64_t id,
        const std::optional<std::string>& status,
        const std::optional<int64_t>& next_fire_at,
        const std::optional<int64_t>& last_run_at,
        const std::optional<int64_t>& last_run_id,
        const std::optional<int64_t>& run_count_delta) {
    if (!db_) return false;
    std::string sql = "UPDATE scheduled_tasks SET updated_at = ?";
    if (status)        sql += ", status = ?";
    if (next_fire_at)  sql += ", next_fire_at = ?";
    if (last_run_at)   sql += ", last_run_at = ?";
    if (last_run_id)   sql += ", last_run_id = ?";
    if (run_count_delta) sql += ", run_count = run_count + ?";
    sql += " WHERE tenant_id = ? AND id = ?;";

    Stmt q(db_, sql.c_str());
    int idx = 1;
    q.bind(idx++, now_epoch());
    if (status)          q.bind(idx++, *status);
    if (next_fire_at)    q.bind(idx++, *next_fire_at);
    if (last_run_at)     q.bind(idx++, *last_run_at);
    if (last_run_id)     q.bind(idx++, *last_run_id);
    if (run_count_delta) q.bind(idx++, *run_count_delta);
    q.bind(idx++, tenant_id);
    q.bind(idx,   id);
    q.step();
    return sqlite3_changes(db_) > 0;
}

bool TenantStore::delete_scheduled_task(int64_t tenant_id, int64_t id) {
    if (!db_) return false;
    Stmt q(db_, "DELETE FROM scheduled_tasks WHERE tenant_id = ? AND id = ?;");
    q.bind(1, tenant_id);
    q.bind(2, id);
    q.step();
    return sqlite3_changes(db_) > 0;
}

TenantStore::TaskRun
TenantStore::create_task_run(int64_t tenant_id, int64_t task_id,
                              const std::string& status,
                              int64_t started_at,
                              const std::string& request_id) {
    if (!db_) throw std::runtime_error("TenantStore not opened");
    Stmt q(db_,
        "INSERT INTO task_runs "
        "(tenant_id, task_id, status, started_at, request_id) "
        "VALUES (?, ?, ?, ?, ?);");
    q.bind(1, tenant_id);
    q.bind(2, task_id);
    q.bind(3, status);
    q.bind(4, started_at);
    q.bind(5, request_id);
    int rc = q.step();
    if (rc != SQLITE_DONE) check_sqlite(db_, rc, "insert task_run");

    TaskRun r;
    r.id         = sqlite3_last_insert_rowid(db_);
    r.tenant_id  = tenant_id;
    r.task_id    = task_id;
    r.status     = status;
    r.started_at = started_at;
    r.request_id = request_id;
    return r;
}

bool TenantStore::update_task_run(
        int64_t tenant_id, int64_t id,
        const std::optional<std::string>& status,
        const std::optional<int64_t>& completed_at,
        const std::optional<std::string>& result_summary,
        const std::optional<std::string>& error_message,
        const std::optional<int64_t>& input_tokens,
        const std::optional<int64_t>& output_tokens,
        const std::optional<bool>& notified) {
    if (!db_) return false;
    std::string sql = "UPDATE task_runs SET ";
    bool any = false;
    auto comma = [&]() { if (any) sql += ", "; any = true; };
    if (status)         { comma(); sql += "status = ?"; }
    if (completed_at)   { comma(); sql += "completed_at = ?"; }
    if (result_summary) { comma(); sql += "result_summary = ?"; }
    if (error_message)  { comma(); sql += "error_message = ?"; }
    if (input_tokens)   { comma(); sql += "input_tokens = ?"; }
    if (output_tokens)  { comma(); sql += "output_tokens = ?"; }
    if (notified)       { comma(); sql += "notified = ?"; }
    if (!any) return false;
    sql += " WHERE tenant_id = ? AND id = ?;";

    Stmt q(db_, sql.c_str());
    int idx = 1;
    if (status)         q.bind(idx++, *status);
    if (completed_at)   q.bind(idx++, *completed_at);
    if (result_summary) q.bind(idx++, *result_summary);
    if (error_message)  q.bind(idx++, *error_message);
    if (input_tokens)   q.bind(idx++, *input_tokens);
    if (output_tokens)  q.bind(idx++, *output_tokens);
    if (notified)       q.bind(idx++, static_cast<int64_t>(*notified ? 1 : 0));
    q.bind(idx++, tenant_id);
    q.bind(idx,   id);
    q.step();
    return sqlite3_changes(db_) > 0;
}

std::optional<TenantStore::TaskRun>
TenantStore::get_task_run(int64_t tenant_id, int64_t id) const {
    if (!db_) return std::nullopt;
    std::string sql = std::string("SELECT ") + kTaskRunCols +
        " FROM task_runs WHERE tenant_id = ? AND id = ?;";
    Stmt q(db_, sql.c_str());
    q.bind(1, tenant_id);
    q.bind(2, id);
    if (q.step() != SQLITE_ROW) return std::nullopt;
    return row_to_task_run(q);
}

std::vector<TenantStore::TaskRun>
TenantStore::list_task_runs(int64_t tenant_id, int64_t task_id,
                              int64_t since_epoch, int limit) const {
    std::vector<TaskRun> out;
    if (!db_) return out;
    if (limit <= 0 || limit > 200) limit = 200;
    std::string sql = std::string("SELECT ") + kTaskRunCols +
        " FROM task_runs WHERE tenant_id = ?";
    if (task_id    > 0) sql += " AND task_id = ?";
    if (since_epoch > 0) sql += " AND started_at >= ?";
    sql += " ORDER BY started_at DESC LIMIT ?;";
    Stmt q(db_, sql.c_str());
    int idx = 1;
    q.bind(idx++, tenant_id);
    if (task_id     > 0) q.bind(idx++, task_id);
    if (since_epoch > 0) q.bind(idx++, since_epoch);
    q.bind(idx, static_cast<int64_t>(limit));
    while (q.step() == SQLITE_ROW) out.push_back(row_to_task_run(q));
    return out;
}

} // namespace arbiter
