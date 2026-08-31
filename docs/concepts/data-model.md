# Data model

Reference shapes for the rows arbiter persists. Every endpoint that returns one of these types matches this schema; lists are arrays of these objects under named keys.

## Tenant

| Field          | Type    | Notes |
|----------------|---------|-------|
| `id`           | integer | Assigned at creation. Stable. |
| `name`         | string  | Display-only; no uniqueness constraint. |
| `disabled`     | boolean | `true` → all `/v1/orchestrate` calls return 401. |
| `created_at`   | integer | Epoch seconds. |
| `last_used_at` | integer | Epoch seconds. 0 if the tenant has never made a call. |

## Conversation

| Field            | Type    | Notes |
|------------------|---------|-------|
| `id`             | integer | Stable per tenant. |
| `tenant_id`      | integer | FK into `tenants`. Always equals the caller's tenant. |
| `title`          | string  | Display title. May be empty. |
| `agent_id`       | string  | Which agent this thread talks to. |
| `agent_def`      | object? | Snapshot of the inline agent definition (if the conversation was created with one). Absent for stored / `index` agents. |
| `created_at`     | integer | Epoch seconds. |
| `updated_at`     | integer | Epoch seconds. Bumped on every message append. |
| `message_count`  | integer | Total messages in the thread. |
| `archived`       | boolean | Hidden from default UI views; not deleted. |
| `origin`         | string  | `"api"` (HTTP) or `"tui"` (interactive sidebar). Defaults to `"api"`. |
| `cwd`            | string  | Working directory at creation (TUI); empty for typical API threads. |
| `deleted_at`     | integer | Soft-delete epoch; `0` = visible. Soft-deleted rows are omitted from list endpoints. |
| `total_tokens`   | integer | Cumulative billed tokens (TUI sidebar); `0` for API-only threads unless set. |
| `titled`         | boolean | Title locked against auto-titling (TUI). |
| `folder_id`      | integer? | FK into `conversation_folders`, or `null` when unfiled. |

TUI sessions also store a multi-agent `session_json` blob on the row (not exposed on the HTTP conversation resource today). HTTP turns continue to use the `messages` table.

## ConversationFolder

Named grouping for conversations (TUI sidebar tree and HTTP `folder_id` filter).

| Field         | Type    | Notes |
|---------------|---------|-------|
| `id`          | integer | Stable per tenant. |
| `tenant_id`   | integer | FK into `tenants`. |
| `name`        | string  | Display name. |
| `position`    | integer | Sort order among folders (ascending). |
| `created_at`  | integer | Epoch seconds. |
| `updated_at`  | integer | Epoch seconds. |

Deleting a folder unfiles its conversations (`folder_id` cleared) rather than cascading deletes. TUI collapse state for folder headers lives in `tui_prefs.folder_collapse_json` (JSON array of folder ids), not on the folder row.
## ConversationMessage

| Field             | Type    | Notes |
|-------------------|---------|-------|
| `id`              | integer | Append-only, ordered. |
| `conversation_id` | integer | FK into `conversations`. |
| `role`            | string  | `"user"` or `"assistant"`. |
| `content`         | string  | Full message text. |
| `input_tokens`    | integer | 0 for user messages; full request total for assistant messages. |
| `output_tokens`   | integer | Same. |
| `created_at`      | integer | Epoch seconds. |
| `request_id`      | string? | Correlates to the SSE stream that produced it. |

## Agent (catalog row)

| Field         | Type    | Notes |
|---------------|---------|-------|
| `id`          | string  | Caller-chosen identifier (typically a UUID), `[A-Za-z0-9_-]{1,64}`. |
| `name`        | string  | Display name. |
| `role`        | string  | Short role descriptor. |
| `model`       | string  | Routed by arbiter's provider prefix table. |
| `goal`        | string  | What this agent is trying to accomplish. |
| `brevity`     | string  | `"lite"` \| `"full"` \| `"ultra"`. |
| `max_tokens`  | integer | Response cap per turn. |
| `temperature` | number  | 0.0–2.0. |
| `rules`       | array<string> | Behavioral constraints. |
| `capabilities`| array<string> | Tools this agent uses (used by master for routing). |
| `mode`        | string? | `""` / `"standard"` (compressed specialist), `"conversational"` (index-style), `"spoken"` (TTS / Intercom), `"writer"`, or `"planner"`. |
| `channel`     | string? | `"voice"` when this agent's replies are read aloud. Request `channel` can set the same overlay for one turn. See [Voice](voice.md). |
| `advisor`     | object? | Structured advisor config: `{model, prompt?, mode?, max_redirects?, malformed_halts?}`. `mode: "consult"` (default) makes `/advise` available; `mode: "gate"` additionally enforces a runtime gate at the executor's terminating turn. See [advisor](advisor.md). |
| `intent`      | object? | Ingress classify/route: `{mode?, min_confidence?, apply_routing?, model?}`. Distinct from `memory.intent_routing`. See [intent](intent.md). |
| `presence`    | object? | Always-on residency: `{mode?, watch?, interject?, model?, prompt?, max_notes_per_turn?}`. `mode: "always_on"` reviews matching peers after each tool batch and may inject a `[PRESENCE: …]` note. See [presence](presence.md). |
| `advisor_model` | string? | **Legacy** shorthand for `advisor.model` with `mode: "consult"`. New configs should use `advisor`. |
| `personality` | string? | Free-form personality overlay. |
| `created_at`  | integer | Epoch seconds. Stored agents only; absent for the built-in `index`. |
| `updated_at`  | integer | Epoch seconds. Stored agents only. |

## MemoryEntry

| Field             | Type    | Notes |
|-------------------|---------|-------|
| `id`              | integer | Stable. |
| `tenant_id`       | integer | FK. |
| `type`            | string  | `user` \| `feedback` \| `project` \| `reference` \| `learning` \| `context`. |
| `title`           | string  | Non-empty, ≤ 200 chars. |
| `content`         | string  | ≤ 64 KiB. |
| `source`          | string  | Free-form provenance, ≤ 200 chars. |
| `tags`            | array<string> | 0..32 tags, each 1–64 chars. |
| `artifact_id`     | int?    | Optional FK to `tenant_artifacts`. `null` when unlinked. |
| `conversation_id` | int?    | Optional scope. `null` = unscoped (visible from every conversation). Positive = pinned to one conversation; conversation-local search ranks it above tenant-wide hits. |
| `valid_from`      | integer | Epoch seconds. When the fact became true. Set automatically on insert (= `created_at`); not editable. |
| `valid_to`        | int?    | Epoch seconds when the entry was invalidated. `null` while active. Stamped by `POST /v1/memory/entries/:id/invalidate`; once set, the row is hidden from default reads but reachable via `?as_of=<epoch>`. See [Structured memory → Temporal model](structured-memory.md#temporal-model). |
| `created_at`      | integer | Epoch seconds. |
| `updated_at`      | integer | Epoch seconds. |

## MemoryRelation

| Field        | Type    | Notes |
|--------------|---------|-------|
| `id`         | integer | Stable. |
| `tenant_id`  | integer | FK. |
| `source_id`  | integer | FK to `memory_entries.id`. |
| `target_id`  | integer | FK to `memory_entries.id`. Different from `source_id` (no self-loops). |
| `relation`   | string  | `relates_to` \| `refines` \| `contradicts` \| `supersedes` \| `supports`. |
| `created_at` | integer | Epoch seconds. |

## ArtifactRecord

| Field             | Type    | Notes |
|-------------------|---------|-------|
| `id`              | integer | Stable. |
| `tenant_id`       | integer | FK. |
| `conversation_id` | integer | FK. |
| `path`            | string  | Sanitised, ≤ 256 chars total. |
| `sha256`          | string  | Hex digest of `content`. |
| `mime_type`       | string  | Free-form; default `application/octet-stream`. |
| `size`            | integer | Bytes. |
| `created_at`      | integer | Epoch seconds. |
| `updated_at`      | integer | Epoch seconds. |

## ReconcileRun

One `POST /v1/reconcile` job. Tenant-scoped. Distinct from `request_status` (SSE log) even when they share `request_id`.

| Field | Type | Notes |
|-------|------|-------|
| `request_id` | string | Same id as the SSE log row. |
| `tenant_id` | integer | FK. |
| `status` | string | `running` \| `satisfied` \| `failed` \| `rolled_back` \| `canceled`. |
| `reason` | string | `ok`, `delta_unresolved`, `verification_missing`, `unknown_invariant`, … |
| `target_state` | object | Desired end state. |
| `invariants` | array\<string\> | Expr + named catalog. |
| `contract` | object | Compiled clauses. |
| `workspace.kind` | string | `sandbox` \| `path`. |
| `rollback_on_failure` | boolean | Snapshot/restore. |
| `result` | object? | Terminal `reconcile.done` payload. |
| `created_at` / `updated_at` | integer | Epoch seconds. |

## See also

- [Tenants](tenants.md), [Structured memory](structured-memory.md), [Artifacts](artifacts.md).
