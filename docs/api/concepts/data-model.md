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

Billing fields (caps, plan, MTD, usage entries) live in the external billing service and are not part of the runtime data model.

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
| `mode`        | string? | `"standard"` (default) or `"writer"`. |
| `advisor`     | object? | Structured advisor config: `{model, prompt?, mode?, max_redirects?, malformed_halts?}`. `mode: "consult"` (default) makes `/advise` available; `mode: "gate"` additionally enforces a runtime gate at the executor's terminating turn. See [advisor](advisor.md). |
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

## See also

- [Tenants](tenants.md), [Structured memory](structured-memory.md), [Artifacts](artifacts.md).
