# Structured memory

Two memory surfaces ship side-by-side under `/v1/memory`:

- **File scratchpads** — the legacy per-agent markdown documented at [`GET /v1/memory`](../memory/list-scratchpads.md) and [`GET /v1/memory/:agent_id`](../memory/get-scratchpad.md). Read-only over HTTP; agents write via `/mem write` during a turn.
- **Structured memory** — typed nodes (entries) and directed labeled edges (relations) in SQLite, with full CRUD over HTTP. Backs the frontend graph UI.

The two sub-systems do **not** share storage. An entry is not a parsed agent scratchpad; an agent's `/mem write` does not create entries.

Agents and HTTP callers both write directly to the curated graph. Every entry and relation is immediately visible to all read paths — there is no intermediate review step.

## Closed enums

Validated server-side and rejected with `400 {"error":"..."}` if violated. Adding values is a coordinated frontend+API change.

| Field        | Allowed values |
|--------------|----------------|
| `entry.type` | `user`, `feedback`, `project`, `reference`, `learning`, `context` |
| `relation`   | `relates_to`, `refines`, `contradicts`, `supersedes`, `supports` |

## Per-entry constraints

| Field       | Constraint |
|-------------|------------|
| `title`     | Non-empty, ≤ 200 chars |
| `content`   | ≤ 64 KiB |
| `source`    | ≤ 200 chars |
| `tags`      | JSON array of strings; each tag 1–64 chars; up to 32 tags. Always present in responses; pass `[]` (or omit) for none. |
| `artifact_id` | Optional FK to a [tenant artifact](artifacts.md). Validated against the tenant's catalogue. |

## Per-relation constraints

- `source_id != target_id` — self-loops return 400 `"self-loops not allowed"`.
- Both endpoints must belong to the calling tenant — otherwise `400 "entries belong to different tenants"`. (No 404-per-side, to avoid leaking whether the other tenant's id exists.)
- Relations are **directed and per-type**. The same pair can have multiple relations of different kinds; the same `(source, target, relation)` triple cannot exist twice — a duplicate returns `409 {"error":"...", "existing_id": N}`. Symmetric relations like `contradicts` are still stored directed; clients dedupe for display.

## Agent slash surface

Agents running inside `/v1/orchestrate` (or `/v1/conversations/:id/messages`) can read and write mid-turn. Reads surface every entry and relation; writes land directly in the graph and are visible immediately.

| Command | Effect |
|---------|--------|
| `/mem entries` | List the tenant's entries (up to 100), newest first. |
| `/mem entries project,reference` | Same, filtered to one or more types. |
| `/mem entries tag=<name>` | Same, filtered to entries carrying a specific tag. |
| `/mem entry 42` | One entry, with **neighbour titles** rendered inline on each incoming/outgoing edge. Saves a follow-up `/mem entry` per neighbour. |
| `/mem search <query>` | Relevance-ranked search across title + tags + content + source. Top 3 hits inline content excerpts; lower-ranked hits are listed as one-liners (cap 50). |
| `/mem expand 42 [depth=N]` | Subgraph around entry `42` to depth `N` (default 1, max 2; capped at 50 nodes). One round-trip for what would otherwise be N+1 chained `/mem entry` calls. |
| `/mem density 42` | Quick degree summary for an entry: in/out edge counts, distinct relation kinds, 2-hop reach. Use **before** redundant research to detect topics the graph already covers densely. |
| `/mem write entry <type> <title>` | Create a new typed node directly in the graph. |
| `/mem write entry <type> <title> --artifact #<id>` | Same, but link an artifact to the new entry in one call. |
| `/mem write link <src_id> <relation> <dst_id>` | Create a directed edge between two entries. |

Read output lands in a `[/mem entries]` / `[/mem entry]` / `[/mem search]` / `[/mem expand]` / `[/mem density]` tool-result block, framed by `[END MEMORY]`. Write output lands in a `[/mem write entry …]` or `[/mem write link …]` block — typically `OK: wrote entry #88 [project] …` so the agent can reference the new id in the same turn.

### Recommended retrieval workflow

Before doing fresh research, the agent should probe the existing graph:

```
/mem search neanderthal gene flow              → ranked hits, top-3 inlined
/mem expand 42 depth=2                         → cluster around the most-relevant hit
/mem density 42                                → "is this area already richly linked?"
```

The smarter ranker means a search rarely needs to be followed by individual `/mem entry` reads; the inlined excerpts on the top hits usually answer the agent's question. `/mem expand` replaces a chain of `/mem entry` calls when an agent needs to follow relations several hops out. `/mem density` is the "should I research more or is this enough" probe — sparse neighbourhood suggests adding entries; dense suggests synthesis.

### Tenant scoping

The reader and writer are both bound to the request's authenticated tenant — sub-agents invoked via `/agent` and parallel children spawned via `/parallel` inherit the same tenant scope. CLI/REPL contexts (`arbiter --send`, the interactive REPL) don't have a tenant; both `/mem entries…` and `/mem write…` return ERR there. This surface is API-only.

## End-to-end flow (agent writing two new related entries)

1. Agent emits `/mem write entry project Investigate cache stampede` → response includes `OK: wrote entry #88 ...`.
2. Agent emits `/mem write entry reference RFC-0042` → `OK: wrote entry #89 ...`.
3. Agent emits `/mem write link 88 relates_to 89` → `OK: wrote relation #12 ...`.
4. The next agent turn (and any HTTP caller) sees `#88`, `#89`, and the edge between them in `/mem entries`, `/mem entry 88`, and `GET /v1/memory/graph` immediately.

## See also

- [`POST /v1/memory/entries`](../memory/entries/create.md), [`GET /v1/memory/entries`](../memory/entries/list.md), [`GET /v1/memory/entries/:id`](../memory/entries/get.md), [`PATCH /v1/memory/entries/:id`](../memory/entries/patch.md), [`DELETE /v1/memory/entries/:id`](../memory/entries/delete.md)
- [`POST /v1/memory/relations`](../memory/relations/create.md), [`GET /v1/memory/relations`](../memory/relations/list.md), [`DELETE /v1/memory/relations/:id`](../memory/relations/delete.md)
- [`GET /v1/memory/graph`](../memory/graph.md)
- [Artifacts](artifacts.md) — for the memory↔artifact link
