# Structured memory

Two memory surfaces ship side-by-side under `/v1/memory`:

- **File scratchpads** — the legacy per-agent markdown documented at [`GET /v1/memory`](../memory/list-scratchpads.md) and [`GET /v1/memory/:agent_id`](../memory/get-scratchpad.md). Read-only over HTTP; agents write via `/mem write` during a turn.
- **Structured memory** — typed nodes (entries) and directed labeled edges (relations) in SQLite, with full CRUD over HTTP. Backs the frontend graph UI.

The two sub-systems do **not** share storage. An entry is not a parsed agent scratchpad; an agent's `/mem write` does not create entries.

Agents can both **read** and **propose** into structured memory in real time during a turn:

- Reads (`/mem entries`, `/mem entry`, `/mem search`, `/mem expand`, `/mem density`) only ever surface the **curated graph** — entries and relations with `status="accepted"`.
- Writes from agents land in the **proposal queue** (`status="proposed"`) via `/mem propose entry` and `/mem propose link`. Proposed rows are invisible to every read path — both HTTP and the agent's own slash commands — until a human reviewer accepts them through the HTTP surface.

That split is the safety boundary: agents never directly mutate the curated graph their future selves will read.

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

Agents running inside `/v1/orchestrate` (or `/v1/conversations/:id/messages`) can read and propose mid-turn. Reads see only the curated graph; writes go to the proposal queue.

| Command | Effect |
|---------|--------|
| `/mem entries` | List the tenant's accepted entries (up to 100), newest first. |
| `/mem entries project,reference` | Same, filtered to one or more types. |
| `/mem entries tag=<name>` | Same, filtered to entries carrying a specific tag. |
| `/mem entry 42` | One accepted entry, with **neighbour titles** rendered inline on each incoming/outgoing edge. Saves a follow-up `/mem entry` per neighbour. |
| `/mem search <query>` | Relevance-ranked search across title + tags + content + source. Top 3 hits inline content excerpts; lower-ranked hits are listed as one-liners (cap 50). |
| `/mem expand 42 [depth=N]` | Subgraph around entry `42` to depth `N` (default 1, max 2; capped at 50 nodes). One round-trip for what would otherwise be N+1 chained `/mem entry` calls. |
| `/mem density 42` | Quick degree summary for an entry: in/out edge counts, distinct relation kinds, 2-hop reach. Use **before** redundant research to detect topics the graph already covers densely. |
| `/mem propose entry <type> <title>` | Propose a new typed node. Lands in `status="proposed"` (hidden from reads). |
| `/mem propose entry <type> <title> --artifact #<id>` | Same, but link an artifact to the proposed entry in one call. |
| `/mem propose link <src_id> <relation> <dst_id>` | Propose a directed edge between two entries (either or both may be proposed). |

Read output lands in a `[/mem entries]` / `[/mem entry]` / `[/mem search]` / `[/mem expand]` / `[/mem density]` tool-result block, framed by `[END MEMORY]`. Propose output lands in a `[/mem propose entry …]` or `[/mem propose link …]` block — typically `OK: proposed entry #88 [project] …` so the agent can reference the new id in the same turn.

### Recommended retrieval workflow

Before doing fresh research, the agent should probe the existing graph:

```
/mem search neanderthal gene flow              → ranked hits, top-3 inlined
/mem expand 42 depth=2                         → cluster around the most-relevant hit
/mem density 42                                → "is this area already richly linked?"
```

The smarter ranker means a search rarely needs to be followed by individual `/mem entry` reads; the inlined excerpts on the top hits usually answer the agent's question. `/mem expand` replaces a chain of `/mem entry` calls when an agent needs to follow relations several hops out. `/mem density` is the "should I research more or is this enough" probe — sparse neighbourhood suggests adding entries; dense suggests synthesis.

### Why proposals can't be read back

Agents see only the curated graph — even from their own slash commands. `/mem entry <id>` on a still-proposed id returns `ERR: entry <id> not found`. This is deliberate: it prevents one agent from priming itself (or another agent in the same fleet) on unreviewed content, and it prevents a tenant's pending review queue from leaking across orchestrator boundaries.

The reader and writer are both bound to the request's authenticated tenant — sub-agents invoked via `/agent` and parallel children spawned via `/parallel` inherit the same tenant scope. CLI/REPL contexts (`arbiter --send`, the interactive REPL) don't have a tenant; both `/mem entries…` and `/mem propose…` return ERR there. This surface is API-only.

## Proposal queue

When agents call `/mem propose entry` or `/mem propose link`, rows land in the database with `status="proposed"`:

- They are **invisible to every read path**: HTTP `GET /v1/memory/entries`, `GET /v1/memory/entries/:id`, `GET /v1/memory/relations`, `GET /v1/memory/graph`, and the agent's own `/mem entries|entry|search|expand|density` all filter to `status="accepted"`.
- They surface only through [`GET /v1/memory/proposals`](../memory/proposals.md) (the reviewer UI surface).

### Reviewing a proposal — accept

- Entry: [`PATCH /v1/memory/entries/:id`](../memory/entries/patch.md) with body `{"status": "accepted"}`.
- Relation: [`PATCH /v1/memory/relations/:id`](../memory/relations/patch.md) with body `{"status": "accepted"}`. Both endpoint entries must already be accepted; if either is still proposed, the PATCH returns `409` and the reviewer accepts the entry endpoints first.

### Reviewing a proposal — reject

- Entry: [`DELETE /v1/memory/entries/:id`](../memory/entries/delete.md). This also cascade-deletes any proposed (or accepted) relation pointing to or from this entry.
- Relation: [`DELETE /v1/memory/relations/:id`](../memory/relations/delete.md).

There is no separate `/reject` verb — `DELETE` is the rejection path. Once an entry or relation has been accepted, the same endpoints continue to work for normal CRUD; the proposal-queue mechanics simply stop applying.

### End-to-end flow (agent proposing two new related entries)

1. Agent emits `/mem propose entry project Investigate cache stampede` → response includes `OK: proposed entry #88 ...`.
2. Agent emits `/mem propose entry reference RFC-0042` → `OK: proposed entry #89 ...`.
3. Agent emits `/mem propose link 88 relates_to 89` → `OK: proposed relation #12 ...`.
4. Reviewer hits `GET /v1/memory/proposals` and sees both entries + the relation.
5. Reviewer accepts entries first: `PATCH /v1/memory/entries/88 {"status":"accepted"}`, same for `89`.
6. Reviewer accepts the edge: `PATCH /v1/memory/relations/12 {"status":"accepted"}`.
7. The next agent turn sees `#88`, `#89`, and the edge between them in `/mem entries` and `/mem entry 88`.

## See also

- [`POST /v1/memory/entries`](../memory/entries/create.md), [`GET /v1/memory/entries`](../memory/entries/list.md), [`GET /v1/memory/entries/:id`](../memory/entries/get.md), [`PATCH /v1/memory/entries/:id`](../memory/entries/patch.md), [`DELETE /v1/memory/entries/:id`](../memory/entries/delete.md)
- [`POST /v1/memory/relations`](../memory/relations/create.md), [`GET /v1/memory/relations`](../memory/relations/list.md), [`PATCH /v1/memory/relations/:id`](../memory/relations/patch.md), [`DELETE /v1/memory/relations/:id`](../memory/relations/delete.md)
- [`GET /v1/memory/graph`](../memory/graph.md)
- [`GET /v1/memory/proposals`](../memory/proposals.md)
- [Artifacts](artifacts.md) — for the memory↔artifact link
