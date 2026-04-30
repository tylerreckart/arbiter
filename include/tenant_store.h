#pragma once
// index/include/tenant_store.h
//
// SQLite-backed tenant + usage accounting for the HTTP API.
//
// Each tenant carries:
//   • An opaque plaintext API token (shown to the user once at creation,
//     stored in the DB only as a SHA-256 hex digest).
//   • A display name for CLI reporting.
//   • An optional monthly usage cap in micro-cents (0 = unlimited).
//   • A rolling month-to-date usage total, reset at the start of each
//     billing month (computed from wall-clock UTC on record_usage).
//   • A disabled flag for admin kill-switches.
//
// Usage is logged per LLM turn with provider cost + our markup, both in
// micro-cents.  The API server increments month_to_date inline; the
// usage_log table stays append-only for invoicing.
//
// "Micro-cents" (µ¢): 1 USD = 1_000_000 µ¢.  Keeps everything integer
// while faithfully representing fractional-cent LLM costs.  Display
// conversions live at the bottom of this header.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace index_ai {

struct Tenant {
    int64_t     id                 = 0;
    std::string api_key_hash;            // SHA-256 hex of the plaintext token
    std::string name;
    int64_t     monthly_cap_uc     = 0;  // 0 = unlimited
    std::string month_yyyymm;            // e.g. "2026-04" — the MTD period
    int64_t     month_to_date_uc   = 0;
    bool        disabled           = false;
    int64_t     created_at         = 0;  // epoch seconds
    int64_t     last_used_at       = 0;  // epoch seconds (0 if never)
};

// One row from the conversations table.  Each conversation is a thread of
// messages between a tenant's user and one agent (master or sub-agent).
// Per-message billing is the existing usage_log; this table just owns the
// thread-level metadata so the frontend can show a sidebar.
struct Conversation {
    int64_t     id              = 0;
    int64_t     tenant_id       = 0;
    std::string title;                  // human-set or auto-generated; "" until first turn
    std::string agent_id;               // which agent this conversation talks to
    std::string agent_def_json;         // empty for preloaded agents; the full agent_def
                                        // JSON for inline-defined agents (so the
                                        // conversation continues working even if the
                                        // caller's DB-side definition is offline)
    int64_t     created_at      = 0;
    int64_t     updated_at      = 0;    // bumped on every message append
    int64_t     message_count   = 0;
    bool        archived        = false;
};

// One row from the messages table.  Append-only; rows are never edited.
struct ConversationMessage {
    int64_t     id              = 0;
    int64_t     conversation_id = 0;
    std::string role;                   // "user" | "assistant"
    std::string content;
    int64_t     input_tokens    = 0;
    int64_t     output_tokens   = 0;
    int64_t     billed_uc       = 0;
    int64_t     created_at      = 0;
    std::string request_id;             // correlates with usage_log + cancel
};

// One row from the tenant_artifacts table.  Persistent server-side
// artifact storage for agent-generated files.  Distinct from the
// ephemeral SSE `file` events emitted by /write — artifacts here
// outlive the request, are tenant + conversation scoped, and are
// addressable through GET /v1/conversations/:id/artifacts/:aid.
//
// Lookups never load `content` by default — list / get_meta calls
// return everything else; the blob is fetched separately via
// get_artifact_content so the list path stays cheap.  `path` is
// stored already sanitized; entry-point validation happens in
// sanitize_artifact_path before insert.
struct ArtifactRecord {
    int64_t     id              = 0;
    int64_t     tenant_id       = 0;
    int64_t     conversation_id = 0;
    std::string path;
    std::string sha256;                 // hex digest of content
    std::string mime_type;
    int64_t     size            = 0;    // bytes
    int64_t     created_at      = 0;
    int64_t     updated_at      = 0;
};

// Outcome of put_artifact.  Distinct rejection codes so HTTP and
// agent callers can map cleanly: PathRejected → 400, QuotaExceeded
// → 413, Created → 201, Updated → 200.  `tenant_used_bytes` and
// `conversation_used_bytes` are POST-write totals — let the caller
// surface "you have N bytes left" tool-result feedback.
struct PutArtifactResult {
    enum class Status { Created, Updated, QuotaExceeded, PathRejected };
    Status                          status                  = Status::PathRejected;
    std::optional<ArtifactRecord>   record;
    std::string                     error_msg;
    int64_t                         tenant_used_bytes       = 0;
    int64_t                         conversation_used_bytes = 0;
};

// Validate + canonicalise an untrusted artifact path.  Returns the
// canonical form on success (forward-slash separators, no trailing
// slash, components trimmed).  On rejection returns nullopt and
// populates `err` with a caller-shippable message.  Rules:
//   • not empty, ≤ 256 chars total
//   • each component 1..128 chars
//   • not absolute (no leading /, no Windows drive letter)
//   • no traversal (`.`, `..`, hidden `.foo`)
//   • no null bytes or control chars
//   • not a Windows-reserved name (CON, PRN, AUX, NUL, COM1-9, LPT1-9)
// Backslashes are normalised to forward slashes before validation.
// Caller passes the user-supplied path; the canonical form is what
// goes into the unique index.
std::optional<std::string>
sanitize_artifact_path(const std::string& raw, std::string& err);

// Hard quota ceilings.  Enforced inside put_artifact; the per-tenant
// number is the upper bound for the SQLite-blob backing — tenants
// approaching it should migrate to the (future) object-storage tier.
constexpr int64_t kArtifactPerFileMaxBytes         = 1ll  * 1024 * 1024;
constexpr int64_t kArtifactPerConversationMaxBytes = 50ll * 1024 * 1024;
constexpr int64_t kArtifactPerTenantMaxBytes       = 500ll * 1024 * 1024;

// One row from the agent_scratchpad table.  The legacy file scratchpad
// at `~/.arbiter/memory/t<tid>/<agent_id>.md` is replaced by per-tenant
// rows here when the API is the consumer — keeping notes inside the
// same DB the conversation history lives in (no orphan files when a
// tenant is deleted; no per-machine filesystem state to back up).
//
// `scope_key` is the agent_id for per-agent scratchpads or "" (the
// empty string) for the pipeline-shared scratchpad that every agent in
// a turn can read/write.  `content` is the cumulative markdown text
// — appends modify the row in place, keeping the read path a single
// SELECT.  `updated_at` is bumped on every write.
struct AgentScratchpad {
    int64_t     id          = 0;
    int64_t     tenant_id   = 0;
    std::string scope_key;              // agent_id or "" for shared
    std::string content;
    int64_t     updated_at  = 0;
};

// One row from the tenant_agents table.  Persists per-tenant agent
// definitions sent from the front-end so callers can reference them
// across requests by `agent_id` instead of re-sending the full
// `agent_def` JSON on every turn.  The HTTP API is authoritative for
// the catalog: the front-end is the source of truth and the server
// stores whatever it sends, replacing the blob wholesale on PATCH.
//
// `agent_id` is caller-chosen (typically a UUID owned by the sibling
// service) and is what `/agent`, `/parallel`, and the orchestrate
// request body all reference.  Unique per-tenant — two tenants can
// independently use the id "researcher" without collision.
//
// `name`, `role`, `model` are denormalised from the canonical
// `agent_def_json` blob solely for cheap list-display rendering;
// reads that need anything else parse the blob.
struct AgentRecord {
    int64_t     id              = 0;
    int64_t     tenant_id       = 0;
    std::string agent_id;               // caller-chosen identifier
    std::string name;
    std::string role;
    std::string model;
    std::string agent_def_json;         // raw canonical JSON blob
    int64_t     created_at      = 0;
    int64_t     updated_at      = 0;
};

// One row from the memory_entries table.  These are the structured-memory
// nodes the frontend graph UI renders — each entry is a typed note with
// free-form content, an optional list of tags, and a free-form provenance
// string.  Distinct from the legacy file-scratchpad memory at
// `~/.arbiter/memory/t<id>/<agent_id>.md`: those endpoints stay read-only
// and store unstructured per-agent markdown.  An entry is *not* a parsed
// agent scratchpad and an agent's `/mem write` does not create entries.
struct MemoryEntry {
    int64_t     id          = 0;
    int64_t     tenant_id   = 0;
    std::string type;                   // closed enum, validated server-side
    std::string title;
    std::string content;
    std::string source;                 // free-form provenance string
    std::string tags_json;              // raw JSON array of strings; serialize on output
    // Optional reference to a tenant_artifacts row.  0 = no artifact.
    // FK ON DELETE SET NULL — if the linked artifact's conversation is
    // dropped (cascade) the link nulls out but the memory entry stays.
    // The linked artifact may live in a different conversation than the
    // reader; cross-conversation reads require the agent to cite the
    // memory entry (`/read #<aid> via=mem:<mid>`) so access flows through
    // the curated graph rather than around it.
    int64_t     artifact_id = 0;
    int64_t     created_at  = 0;
    int64_t     updated_at  = 0;
};

// One row from the memory_relations table.  Relations are directed and
// per-type — the unique index on (tenant_id, source_id, target_id, relation)
// allows both `A→B refines` and `B→A refines` to coexist.  Symmetric
// relations like `contradicts` are still stored directed; clients dedupe
// for display.
struct MemoryRelation {
    int64_t     id          = 0;
    int64_t     tenant_id   = 0;
    int64_t     source_id   = 0;
    int64_t     target_id   = 0;
    std::string relation;               // closed enum, validated server-side
    int64_t     created_at  = 0;
};

// One row from the append-only usage_log table.  All monetary fields
// in micro-cents.  Returned by TenantStore::list_usage for the admin
// API's billing-ledger read path.
//
// Cost breakdown rationale: we capture per-token-type cost at write time
// rather than recomputing from tokens × pricing on read.  Pricing tables
// drift (vendors raise/lower rates, we update kPricingEntries); historical
// rows must reflect the rate that was actually billed at the time of the
// call.  provider_uc is the denormalized sum of the four component costs.
struct UsageEntry {
    int64_t     id                  = 0;
    int64_t     tenant_id           = 0;
    int64_t     timestamp           = 0;  // epoch seconds
    std::string model;
    int64_t     input_tokens        = 0;  // total input (incl. cached)
    int64_t     output_tokens       = 0;
    int64_t     cache_read_tokens   = 0;  // subset of input that hit a cache
    int64_t     cache_create_tokens = 0;  // tokens written to cache (Anthropic only)
    int64_t     input_uc            = 0;  // cost for plain (non-cached) input
    int64_t     output_uc           = 0;  // cost for output tokens
    int64_t     cache_read_uc       = 0;  // cost for cache-read tokens (cheaper rate)
    int64_t     cache_create_uc     = 0;  // cost for cache-write tokens (premium rate)
    int64_t     provider_uc         = 0;  // sum of the four above
    int64_t     markup_uc           = 0;  // 20% over provider_uc, rounded up
    std::string request_id;               // empty if unset
};

class TenantStore {
public:
    TenantStore() = default;
    ~TenantStore();

    TenantStore(const TenantStore&)            = delete;
    TenantStore& operator=(const TenantStore&) = delete;

    // Open (or create) the SQLite file at `path`.  Runs migrations on
    // every open — safe to re-run.
    void open(const std::string& path);

    // Create a tenant.  Returns the resulting Tenant record plus the
    // plaintext token — the only time the plaintext is ever visible;
    // subsequent startups only hold the hash.
    struct CreatedTenant { Tenant tenant; std::string token; };
    CreatedTenant create_tenant(const std::string& name, int64_t monthly_cap_uc);

    // Disable or re-enable a tenant.  `key` matches either the numeric id
    // or the display name (first hit wins).  Returns true on success.
    bool set_disabled(const std::string& key, bool disabled);

    // Update the monthly cap (µ¢; 0 = unlimited).  Admin-only; no effect on
    // in-flight requests that have already passed the pre-flight check.
    // Returns true if a tenant with this id exists.
    bool set_cap(int64_t tenant_id, int64_t cap_uc);

    // Look up by plaintext token.  Returns nullopt if the token isn't
    // valid, the tenant is disabled, or the DB is closed.  Updates
    // last_used_at in the process.
    std::optional<Tenant> find_by_token(const std::string& token);

    // Per-token-type cost breakdown captured at write time.  All values in
    // micro-cents.  The four component fields must sum to provider_uc — the
    // record_usage caller is responsible for that math (CostTracker::
    // compute_cost_breakdown does it).
    struct CostParts {
        int64_t input_uc        = 0;
        int64_t output_uc       = 0;
        int64_t cache_read_uc   = 0;
        int64_t cache_create_uc = 0;
    };

    // Record one LLM turn's usage.  Handles month-rollover by resetting
    // month_to_date_uc when the current UTC month differs from the
    // tenant's stored month_yyyymm.  Returns the post-update MTD in
    // micro-cents — callers use this to check against monthly_cap_uc
    // and abort mid-stream if needed.
    int64_t record_usage(int64_t tenant_id,
                         const std::string& model,
                         int input_tokens,
                         int output_tokens,
                         int cache_read_tokens,
                         int cache_create_tokens,
                         const CostParts& parts,
                         int64_t markup_uc,
                         const std::string& request_id = "");

    std::vector<Tenant> list_tenants() const;
    std::optional<Tenant> get_tenant(int64_t id) const;

    // Read rows from usage_log, newest first.  Any filter argument set
    // to 0 (or negative, for `limit`) is ignored:
    //   tenant_id == 0  → all tenants
    //   since     == 0  → no lower bound
    //   until     == 0  → no upper bound
    //   limit    <= 0   → default cap (1000)
    // Used by the admin /v1/admin/usage endpoint so a sibling billing
    // service can paginate through the ledger without touching SQLite
    // directly.
    std::vector<UsageEntry> list_usage(int64_t tenant_id,
                                       int64_t since_ts,
                                       int64_t until_ts,
                                       int     limit) const;

    // Aggregated rollup for analytics: one bucket per distinct value of
    // `group_by` (model | day | tenant), summing tokens + costs over the
    // filtered window.  Saves a sibling service from pulling N raw rows
    // just to render a chart.  group_by:
    //   "model"  → key = the model string                    ("claude-sonnet-4-6")
    //   "day"    → key = "YYYY-MM-DD" (UTC)                  ("2026-04-23")
    //   "tenant" → key = tenant id as string                  ("3")
    // Any unrecognized group_by falls back to "model".
    struct UsageBucket {
        std::string key;
        int64_t     calls               = 0;
        int64_t     input_tokens        = 0;
        int64_t     output_tokens       = 0;
        int64_t     cache_read_tokens   = 0;
        int64_t     cache_create_tokens = 0;
        int64_t     input_uc            = 0;
        int64_t     output_uc           = 0;
        int64_t     cache_read_uc       = 0;
        int64_t     cache_create_uc     = 0;
        int64_t     provider_uc         = 0;
        int64_t     markup_uc           = 0;
    };
    std::vector<UsageBucket> usage_summary(int64_t tenant_id,
                                            int64_t since_ts,
                                            int64_t until_ts,
                                            const std::string& group_by) const;

    // ── Conversations ─────────────────────────────────────────────────────
    //
    // Conversations are tenant-scoped threads of messages.  Every method
    // here takes the tenant_id so an integer id leak from one tenant to
    // another can't surface someone else's conversation.  All times are
    // epoch seconds.

    Conversation create_conversation(int64_t tenant_id,
                                      const std::string& title,
                                      const std::string& agent_id,
                                      const std::string& agent_def_json = "");

    // List newest first.  `before_updated_at == 0` means "from the latest";
    // pass the previous page's last `updated_at` to paginate backward.
    // `limit` is hard-capped at 200.
    std::vector<Conversation> list_conversations(int64_t tenant_id,
                                                  int64_t before_updated_at,
                                                  int     limit) const;

    std::optional<Conversation> get_conversation(int64_t tenant_id, int64_t id) const;

    // PATCH-style: any non-empty field replaces.  `archived` flag uses the
    // tri-state encoding (-1 = no change, 0 = false, 1 = true) since bool
    // can't represent absence.
    bool update_conversation(int64_t tenant_id, int64_t id,
                              const std::string& new_title,    // "" = no change
                              int                set_archived);// -1 = no change

    bool delete_conversation(int64_t tenant_id, int64_t id);

    // Append a message; bumps the parent conversation's updated_at + count.
    // Caller computes billing fields elsewhere (or passes 0s).
    ConversationMessage append_message(int64_t tenant_id, int64_t conversation_id,
                                        const std::string& role,
                                        const std::string& content,
                                        int64_t input_tokens,
                                        int64_t output_tokens,
                                        int64_t billed_uc,
                                        const std::string& request_id);

    // List messages in a conversation, oldest first (chat order).  Caller
    // can pass `after_id` for forward pagination; 0 = from the start.
    std::vector<ConversationMessage>
    list_messages(int64_t tenant_id, int64_t conversation_id,
                  int64_t after_id, int limit) const;

    // ── Tenant-stored agent definitions ────────────────────────────────
    //
    // Per-tenant catalog of agent constitutions sent from the front-end.
    // Lets callers POST an agent once and reference it by `agent_id` on
    // every subsequent /v1/orchestrate, /agent, or /parallel call instead
    // of re-sending the full blob.  All methods are tenant-scoped — a
    // leaked id never surfaces another tenant's row.
    //
    // `agent_def_json` is the canonical blob; `name`/`role`/`model` are
    // denormalised for list-display ergonomics.  PATCH replaces the blob
    // wholesale (no field-level merge) since the front-end owns the
    // canonical representation.

    // Returns nullopt on unique-index conflict on (tenant_id, agent_id) —
    // caller surfaces 409 with the existing row.
    std::optional<AgentRecord> create_agent_record(int64_t tenant_id,
                                                    const std::string& agent_id,
                                                    const std::string& name,
                                                    const std::string& role,
                                                    const std::string& model,
                                                    const std::string& agent_def_json);

    std::optional<AgentRecord> get_agent_record(int64_t tenant_id,
                                                 const std::string& agent_id) const;

    // Newest `updated_at` first.  Hard-capped at 200 per page.
    std::vector<AgentRecord> list_agent_records(int64_t tenant_id,
                                                 int limit) const;

    // Wholesale replace.  Bumps updated_at.  Returns false if the row
    // doesn't exist for this tenant.
    bool update_agent_record(int64_t tenant_id,
                              const std::string& agent_id,
                              const std::string& name,
                              const std::string& role,
                              const std::string& model,
                              const std::string& agent_def_json);

    bool delete_agent_record(int64_t tenant_id, const std::string& agent_id);

    // ── Artifact store (per-conversation persistent files) ──────────────
    //
    // Tenant + conversation scoped, addressed by `path` within a single
    // conversation's "working directory".  Content is stored as a BLOB
    // in SQLite — fine to single-digit-GB scale per tenant; beyond that,
    // the same interface can be re-implemented against S3/MinIO without
    // the agents or HTTP callers noticing.
    //
    // Path validation (sanitize_artifact_path) is the caller's job.  The
    // store treats `sanitized_path` as already-trusted and only enforces
    // the unique index, quota, and per-file ceiling.

    // PUT-style on path conflict — replaces content, sha256, size,
    // mime_type and bumps updated_at.  Returns Status + the post-write
    // record on success.  Quota checks subtract any pre-existing entry's
    // size before testing the cap, so an in-place overwrite of a 100 KB
    // file with 200 KB only "costs" 100 KB against the conversation
    // quota.
    PutArtifactResult put_artifact(int64_t tenant_id,
                                    int64_t conversation_id,
                                    const std::string& sanitized_path,
                                    const std::string& content,
                                    const std::string& mime_type);

    // Metadata-only fetch — does NOT load the BLOB.  Use this for list
    // pages, the JSON metadata endpoint, agent /list, etc.
    std::optional<ArtifactRecord>
    get_artifact_meta(int64_t tenant_id, int64_t id) const;

    // BLOB fetch — separate so list paths don't pull megabytes.  Returns
    // nullopt if the row doesn't exist for this tenant.
    std::optional<std::string>
    get_artifact_content(int64_t tenant_id, int64_t id) const;

    // Lookup by (tenant, conversation, path) — used by the agent
    // /read slash command to address artifacts the way they were
    // written.  Returns the metadata row.
    std::optional<ArtifactRecord>
    get_artifact_meta_by_path(int64_t tenant_id, int64_t conversation_id,
                                const std::string& sanitized_path) const;

    // Newest `updated_at` first.  Hard-capped at 200 per page.
    std::vector<ArtifactRecord>
    list_artifacts_conversation(int64_t tenant_id, int64_t conversation_id,
                                  int limit) const;

    // Cross-conversation discovery for this tenant.  Same ordering and
    // page cap.
    std::vector<ArtifactRecord>
    list_artifacts_tenant(int64_t tenant_id, int limit) const;

    bool delete_artifact(int64_t tenant_id, int64_t id);

    // SUM(size) — used by put_artifact for quota math and by HTTP
    // callers exposing "you have used X of Y" surfaces.
    int64_t bytes_used_tenant(int64_t tenant_id) const;
    int64_t bytes_used_conversation(int64_t tenant_id,
                                     int64_t conversation_id) const;

    // ── Agent file-scratchpad (DB-backed) ───────────────────────────────
    //
    // Replaces the per-tenant filesystem scratchpad at
    // `~/.arbiter/memory/t<tid>/<agent_id>.md`.  Pass `scope_key = ""` for
    // the pipeline-shared scratchpad; any other value is treated as an
    // agent_id.  Empty content is returned for missing rows (read is
    // non-fatal).  append_scratchpad inserts the row on first write and
    // appends a timestamped block on subsequent writes — same semantics
    // as the file-based version.

    std::string read_scratchpad(int64_t tenant_id,
                                 const std::string& scope_key) const;

    // Appends a `\n<!-- <ts> -->\n<text>\n` block to the existing content
    // (or starts the content with that block on first write).  Returns
    // the new total content size in bytes (callers usually ignore).
    int64_t append_scratchpad(int64_t tenant_id,
                               const std::string& scope_key,
                               const std::string& text);

    // Returns true if a row was deleted.  Idempotent.
    bool clear_scratchpad(int64_t tenant_id, const std::string& scope_key);

    // List every scope_key that has a non-empty scratchpad for this
    // tenant.  Used by `GET /v1/memory` to enumerate available agent
    // notebooks without reading the filesystem.
    std::vector<std::string> list_scratchpad_scopes(int64_t tenant_id) const;

    // ── Structured memory entries + relations ───────────────────────────
    //
    // Tenant-scoped graph storage backing the frontend's force-graph view.
    // Every method takes `tenant_id` and includes it in WHERE clauses so a
    // leaked integer id from one tenant never surfaces another tenant's
    // entry — cross-tenant lookups return as 404 (not 403), matching the
    // Conversation pattern above.  All times are epoch seconds.

    // `tags_json` is the raw JSON array of strings — caller validates the
    // shape (this layer trusts it).  `source` is a free-form provenance
    // string ("planning", "ingest", a URL, etc.).  `artifact_id` is an
    // optional FK to a tenant_artifacts row — caller is responsible for
    // verifying the artifact belongs to this tenant (use get_artifact_meta)
    // before passing the id in.  Pass 0 for "no artifact".
    MemoryEntry create_entry(int64_t tenant_id,
                              const std::string& type,
                              const std::string& title,
                              const std::string& content,
                              const std::string& source,
                              const std::string& tags_json,
                              int64_t artifact_id = 0);

    std::optional<MemoryEntry> get_entry(int64_t tenant_id, int64_t id) const;

    struct EntryFilter {
        std::vector<std::string> types;             // OR-filter; empty = all
        std::string              tag;               // single-tag substring match
        std::string              q;                 // LIKE on title + content
        int64_t                  since                 = 0;  // created_at >= since
        int64_t                  before_updated_at     = 0;  // cursor; 0 = latest
        int                      limit                 = 50;
    };
    std::vector<MemoryEntry> list_entries(int64_t tenant_id,
                                           const EntryFilter& f) const;

    // PATCH-style: any std::nullopt argument leaves the field untouched.
    // Bumps updated_at on a successful change.  Returns false if the entry
    // doesn't belong to this tenant.  `artifact_id` uses the same nullopt
    // semantics; pass `std::optional(0)` to explicitly clear the link.
    bool update_entry(int64_t tenant_id, int64_t id,
                      const std::optional<std::string>& title,
                      const std::optional<std::string>& content,
                      const std::optional<std::string>& source,
                      const std::optional<std::string>& tags_json,
                      const std::optional<std::string>& type,
                      const std::optional<int64_t>& artifact_id = std::nullopt);

    bool delete_entry(int64_t tenant_id, int64_t id);

    // Returns nullopt on unique-index conflict — caller pairs that with
    // find_relation() to surface the existing row in a 409 response.
    std::optional<MemoryRelation> create_relation(int64_t tenant_id,
                                                   int64_t source_id,
                                                   int64_t target_id,
                                                   const std::string& relation);

    std::optional<MemoryRelation> find_relation(int64_t tenant_id,
                                                 int64_t source_id,
                                                 int64_t target_id,
                                                 const std::string& relation) const;

    // Filter args: 0/empty = no filter on that dimension.  Hard-capped at 1000.
    std::vector<MemoryRelation> list_relations(int64_t tenant_id,
                                                int64_t source_id,
                                                int64_t target_id,
                                                const std::string& relation,
                                                int limit) const;

    bool delete_relation(int64_t tenant_id, int64_t id);

private:
    sqlite3* db_ = nullptr;

    // Re-read a tenant row into `t`.  Used internally after mutations.
    bool reload_tenant(int64_t id, Tenant& t) const;
};

// ─── Unit conversions ──────────────────────────────────────────────────────

inline int64_t usd_to_uc(double usd) {
    // Round half-away-from-zero so a 0.0000049 USD call still registers.
    return usd >= 0 ? static_cast<int64_t>(usd * 1'000'000.0 + 0.5)
                    : -static_cast<int64_t>(-usd * 1'000'000.0 + 0.5);
}
inline double uc_to_usd(int64_t uc) {
    return static_cast<double>(uc) / 1'000'000.0;
}
// Round up to the nearest whole cent — always billed in our favor so
// fractional-cent accumulations round toward the charge.
inline int64_t uc_to_cents_ceil(int64_t uc) {
    return (uc + 9999) / 10000;
}

// Markup: 20% over provider cost, rounded up to the nearest µ¢ so we
// never undercharge due to integer truncation.
inline int64_t markup_uc(int64_t provider_uc) {
    return (provider_uc * 20 + 99) / 100;
}

} // namespace index_ai
