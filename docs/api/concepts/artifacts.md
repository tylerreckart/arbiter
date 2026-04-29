# Artifacts

Persistent server-side file storage scoped to **(tenant, conversation)**. Replaces the file-on-disk model with a sandboxed blob store that lives in the same SQLite database as conversations and structured memory — no host filesystem access, no path-traversal attack surface, automatic cleanup when a conversation is deleted.

The default `/write` slash command stays **ephemeral** — content is streamed as an SSE `file` event the frontend renders, with no server-side persistence. Adding `--persist` saves the same content into the artifact store. Two slash commands let agents read it back: `/read <path>` and `/list`.

## Storage model

| Property | Value |
|----------|-------|
| Backing store | SQLite BLOB column on `tenant_artifacts` |
| Primary key | `(tenant_id, conversation_id, path)` unique |
| Concurrency | Serialised by `SQLITE_OPEN_FULLMUTEX` (see [Operational notes](operations.md)) |
| Cascade delete | FK `ON DELETE CASCADE` on `tenants(id)` and `conversations(id)` — drop a conversation, its artifacts go with it |

## Path safety

Paths are validated by a single canonical `sanitize_artifact_path` helper used by every entry point. Rules:

| Rejected | Reason |
|----------|--------|
| Empty path or > 256 chars | length |
| Component > 128 chars | length |
| Absolute (`/foo`, leading `\`) | path traversal protection |
| Drive letter (`C:\`, anything with `:`) | Windows safety |
| `..`, `.` | path traversal |
| Hidden (`.env`, `.git`, any `.foo`) | accidental dotfile leakage |
| Null bytes or control chars (< 0x20 or 0x7f) | injection / display safety |
| Windows-reserved names (`CON`, `PRN`, `AUX`, `NUL`, `COM1`-9, `LPT1`-9, case-insensitive, with or without extension) | cross-platform safety |

Backslashes are normalised to forward slashes before validation; repeated separators collapse; trailing slash is dropped. The canonical form is what goes into the unique index.

## Quotas

Hard ceilings, enforced inside `put_artifact` for every entry point:

| Scope            | Default |
|------------------|---------|
| Per file         | **1 MB** |
| Per conversation | **50 MB** |
| Per tenant       | **500 MB** |

PUT-on-conflict semantics: writing to an existing path **replaces** the row (same `id`, bumped `updated_at`), and quota math subtracts the existing size before checking the cap. Overwriting a 100 KB file with 200 KB only "costs" 100 KB against the conversation quota.

Responses surface the post-write totals in `tenant_used_bytes` and `conversation_used_bytes` so callers (and the agent's own tool result) know how close to the cap they are.

## Agent slash commands

| Command | Effect |
|---------|--------|
| `/write <path>` | Ephemeral SSE `file` event only. The default; matches the prior behaviour exactly. |
| `/write --persist <path>` | SSE event AND artifact-store row. Returns `OK: persisted N bytes (artifact #ID, K of LIMIT bytes used)` so the agent can self-throttle on quota. |
| `/read <path>` | Reads a previously persisted artifact in this conversation. ERR if path is invalid or not present. |
| `/read #<aid>` | Same-conversation read by artifact id. |
| `/read #<aid> via=mem:<entry_id>` | **Cross-conversation** read, capability granted by a memory entry that links the artifact. See "Memory ↔ artifact" below. |
| `/list` | Lists this conversation's artifacts, one per line: `<path>  (<size> bytes, mime=<type>, id=<id>)`. |

The `--persist` write goes through the same path validator as the HTTP endpoint — the agent can't smuggle in `..` or absolute paths even if it tries. CLI/REPL contexts (no conversation, no tenant) leave the artifact callbacks null; `/write --persist` falls back to ephemeral with a `WARN: --persist requested but artifact store is unavailable` line, and `/read`/`/list` return ERR.

## Memory ↔ artifact linkage

Memory entries can carry an optional `artifact_id` field referencing a row in the artifact store. The link is set via:

- **HTTP**: [`POST /v1/memory/entries`](../memory/entries/create.md) or [`PATCH /v1/memory/entries/:id`](../memory/entries/patch.md) with `artifact_id` in the body. Cross-tenant ids return 400.
- **Agent slash**: `/mem propose entry <type> <title> --artifact #<id>` — the agent that just `/write --persist`-ed a file can file it directly into a memory proposal in the same turn.

When the linked artifact is deleted (directly via [`DELETE /v1/artifacts/:aid`](../artifacts/delete.md), or indirectly via its conversation being deleted), the database trigger `memory_entries_artifact_id_clear` nullifies the link automatically. The memory entry survives; future reads see `artifact_id: null` (or `(link expired ...)` in the agent-facing format).

### The cross-conversation read rule

Memories are tenant-scoped; artifacts are conversation-scoped. When an agent reads a memory whose linked artifact lives in a *different* conversation, fetching the blob requires explicit citation of the memory entry as a capability:

```
/read #<aid>                     # SAME-conversation: allowed unconditionally
/read <path>                     # SAME-conversation by path: same as today
/read #<aid> via=mem:<entry_id>  # CROSS-conversation: required citation form
```

Server-side, the artifact reader validates that:

1. The artifact exists for this tenant.
2. If the artifact's home conversation matches the active conversation, the read is allowed.
3. Otherwise, `via=mem:<entry_id>` MUST resolve to an `accepted` memory entry in this tenant whose `artifact_id` equals the requested id.

A missing or mismatched `via=` clause returns ERR. The agent never sees a cross-conversation artifact unless a curated memory grants access — the human accepting the memory proposal is the access-control boundary. Same-conversation reads (the common path) need no citation.

The `/mem entry <id>` agent response prints the literal `/read` line to copy:

```
[/mem entry 42]
#42 [reference] Findings report
linked artifact:
  #88  output/report.md  (1832 bytes, mime=text/markdown)
  cross-conversation — fetch with: /read #88 via=mem:42
[END MEMORY]
```

## Frontend safety

The path string lands on the client as a UTF-8 display field — it's untrusted. **The frontend must NOT pass it directly to `fs.writeFile` or any other path-sensitive API** without its own client-side sanitizer (same rules as the server's, plus your platform's specifics). If you let the user save the artifact to disk, build the destination path from the basename and a vetted directory of your choosing — never from the agent-supplied path.

## Non-goals (v1)

- No object-storage backend yet (S3/MinIO). The same interface fronts a future tier when tenants push past the SQLite ceiling.
- No artifact versioning beyond PUT-overwrites. `git`-style history is a separate feature.
- No public / cross-tenant sharing.
- No mime sniffing; agents declare or accept the default. Frontends should validate the type field they trust before rendering inline.

## See also

- [`POST /v1/conversations/:id/artifacts`](../artifacts/conversations-create.md), [`GET /v1/conversations/:id/artifacts`](../artifacts/conversations-list.md), [`GET /v1/conversations/:id/artifacts/:aid`](../artifacts/conversations-get.md), [`GET /v1/conversations/:id/artifacts/:aid/raw`](../artifacts/conversations-raw.md), [`DELETE /v1/conversations/:id/artifacts/:aid`](../artifacts/conversations-delete.md)
- [`GET /v1/artifacts`](../artifacts/list.md), [`GET /v1/artifacts/:aid`](../artifacts/get.md), [`GET /v1/artifacts/:aid/raw`](../artifacts/raw.md), [`DELETE /v1/artifacts/:aid`](../artifacts/delete.md)
- [Structured memory](structured-memory.md) — for the memory↔artifact link
