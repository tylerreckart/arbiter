# Structured memory

A typed graph of facts that arbiter agents read and write across sessions. Entries are nodes, relations are directed labeled edges. The graph survives turn boundaries, persists across restarts, and is searchable mid-turn through `/mem` slash commands.

This document explains *why* the memory layer is shaped the way it is, *what* the data model looks like, and *how* an agent retrieves what it needs out of it. For the surface-level CRUD reference, the endpoint pages under [`memory/entries/`](../memory/entries/list.md) and [`memory/relations/`](../memory/relations/list.md) carry the request/response details.

## Why a typed, temporal graph

Three constraints shaped the design:

1. **Agents need to recall things across conversations.** A research turn that fetches three sources, derives a conclusion, and writes a brief is mostly *waste* if the next session can't find any of it. Without persistent memory the agent re-fetches the same pages and re-derives the same conclusions every turn.

2. **The world changes.** A "user prefers dark mode" entry stops being true the day the user switches themes. A "Q3 rollout plan" entry stops being load-bearing the day Q3 ends. Hard-deleting these is wrong — you lose audit, you lose the ability to ask "what did the agent believe last quarter," and concurrent reads race the deletion. Editing them in place is also wrong — the entry's text might still describe a real-but-historical decision.

3. **Search-by-keyword isn't enough.** Agents query memory the way humans query notes: ambiguous phrasing, synonyms, partial recall ("that thing about Honeycomb pricing — I think it had a number"). A `LIKE %query%` scan misses everything that doesn't share substrings; pure-vector search loses precision on names and numbers. The retrieval layer needs both lexical and structural signals, and it needs to know which scope to look in first.

The structured-memory layer answers each:

- **Typed nodes + directed edges** give the graph enough shape to support retrieval that follows relations (`/mem expand`, `/mem density`) without becoming a free-form note pile.
- **Temporal validity windows** (`valid_from` / `valid_to`) let facts retire without erasing them. Invalidation is one-directional and reversible only by hard delete + recreate.
- **FTS5 + BM25 + NEAR proximity + metadata boosts + intent routing + RRF graduated search + optional query expansion + optional advisor reranker** give retrieval the layered behavior agents actually need: lexical first with proximity-aware scoring, then locality fused with tenant-wide candidates, then semantic reformulation and rerank when lexical scores are too close to call. Per-agent `MemoryConfig` toggles which advisor-driven layers fire — see [Memory enrichment](#memory-enrichment).

## What lives where

`/v1/memory` covers four sub-systems with non-overlapping responsibilities:

| Surface | Storage | Mutability | Lookup |
|---------|---------|------------|--------|
| **Memory entries** (this doc) | `memory_entries` table | Soft-delete via invalidate; hard-delete via DELETE; PATCH allowed on active rows | Ranked search, per-id, graph traversal |
| **Memory relations** (this doc) | `memory_relations` table | Hard-delete only | Per-source, per-target, per-pair queries |
| **File scratchpads** ([list](../memory/list-scratchpads.md), [get](../memory/get-scratchpad.md)) | `agent_scratchpad` table or filesystem fallback | Append-only via `/mem write`; full-overwrite via `/mem clear` | Whole-document read |
| **Artifacts** ([concepts](artifacts.md)) | `tenant_artifacts` table | Per-conversation; replaced on path collision; CASCADE on conversation delete | Per-id metadata + raw blob |

An entry is **not** a parsed scratchpad. An agent's `/mem write` does not create an entry. A scratchpad is for the agent's free-form working notes within a conversation; an entry is for facts the agent or operator wants to retain across conversations and surface in ranked retrieval.

## Graph structure

### Entries (nodes)

A memory entry is one row in `memory_entries`. The schema reference is in [data-model](data-model.md#memoryentry); the conceptual fields:

| Field | Purpose |
|-------|---------|
| `type` | Closed enum that partitions the graph into addressable categories. |
| `title` | Short human-readable name. The highest-weighted field in the FTS index. |
| `content` | The substance of the entry — what `/mem search` ranks against and what shows up in inline excerpts. **Required for agent writes.** A title-only entry is rejected because it can't be retrieved meaningfully. |
| `tags` | JSON array of strings. Free-form, agent- or operator-curated. Tag matches act as a ranking signal, not a filter — see [Retrieval](#retrieval). |
| `source` | Free-form provenance string ("planning", "ingest", a URL). |
| `artifact_id` | Optional FK to a [tenant artifact](artifacts.md). Lets a `/write --persist`'d file attach to the entry it describes; reads of the entry hydrate the artifact metadata inline. |
| `conversation_id` | Optional scope. NULL = unscoped (visible from every conversation). Positive = pinned to one conversation; conversation-local search ranks it above tenant-wide hits. |
| `valid_from`, `valid_to` | Temporal validity window — see [Temporal model](#temporal-model). |

The closed `type` enum is the primary axis along which agents navigate the graph:

| Type | What goes in it |
|------|-----------------|
| `user` | Durable facts about the human (role, preferences, constraints). |
| `feedback` | Corrections / "do this, not that" guidance from the user. |
| `project` | Active deliverables, decisions, in-flight work, briefs. |
| `reference` | External sources cited (papers, docs, vendor pages). |
| `learning` | Synthesised conclusions reached from multiple sources. |
| `context` | Situational state worth retaining (current focus, blockers). |

Picking the right type is what makes `/mem entries type=project` useful — filing every research output as `reference` defeats the partitioning. Agents see the type legend in their system prompt and pick deliberately.

### Relations (edges)

A relation is a directed labeled edge between two entries. Symmetric semantics like `contradicts` are still stored directed; consumers dedupe at render time. The closed enum:

| Relation | Meaning |
|----------|---------|
| `relates_to` | Generic association — "these two are about the same thing." |
| `refines` | One entry sharpens or specialises another. |
| `contradicts` | One claims something the other denies; consumer should weigh trust. |
| `supersedes` | One replaces the other; the superseded entry is typically also invalidated. |
| `supports` | One provides evidence for the other (citation pattern). |

The unique index on `(tenant_id, source_id, target_id, relation)` makes the same triple unwriteable twice. Two distinct relations between the same pair (`A refines B` and `A supports B`) are allowed; a duplicate `A refines B` returns `409` with the existing id so the caller can deduplicate.

### Tags

Free-form. Tags cost nothing to maintain and act as additional ranking signal — a search that mentions a tag value boosts rows carrying that tag. Tags are not a substitute for `type`; agents using a tag where a type would have been correct lose the partitioning benefit.

### Conversation scope

Every entry created during a conversation is automatically pinned to that conversation's `conversation_id`. Entries created outside a conversation context (HTTP admin imports, scripted seeds, the historical pre-migration set) carry `NULL`.

The fallback semantic is the load-bearing detail: a conversation-scoped read returns rows pinned to that conversation **plus** rows that are unscoped. Without the OR-NULL fallback, a fresh conversation would see no memory at all on its first turn — useless. With it, unscoped reference material is universally visible while conversation-specific working memory ranks above the global pool inside that conversation.

## Temporal model

Two columns govern an entry's "is this true right now" state:

- `valid_from` (always set at insert) — when the fact became true. Defaults to `created_at`.
- `valid_to` (NULL while active, epoch when invalidated) — when it stopped being true.

### Why soft-delete is the default

The agent-facing `/mem invalidate <id>` and the HTTP `POST /v1/memory/entries/:id/invalidate` set `valid_to` rather than DELETE the row. Hard delete (DELETE on the entry, or cascade from the tenant) is still available for the cases where it's the right thing — but the *common* lifecycle is "fact retired, not erased."

Three reasons this matters in practice:

- **Audit and replay.** "What did the agent believe last week?" is a real question — diagnosing a bad recommendation, tracing an unintended action, or simply showing the user what changed. Soft-delete preserves the row; the `as_of=<epoch>` query parameter on [`GET /v1/memory/entries`](../memory/entries/list.md) reconstructs the active set at any past timestamp.
- **Concurrency safety.** Hard deletes race with concurrent reads — an agent mid-turn might read the row right before another invocation deletes it, then write a relation pointing at a now-missing endpoint. Soft delete is observable but not destructive: the read still completes; the relation still has both endpoints in the DB; the next read filters them out.
- **Idempotent invalidation.** Calling invalidate on an already-invalidated row returns `false` (HTTP 404) without changing `valid_to`. That makes invalidate safe to retry — networks fail, agents emit duplicates, callers shouldn't have to track which ids they've already retired.

`update_entry` and `get_entry` both filter to `valid_to IS NULL` by default, so the agent path of least resistance respects the temporal window. To correct the content of a historical row, an operator hard-deletes and re-creates with the right `valid_from`.

### Reading history

`GET /v1/memory/entries?as_of=<epoch>` returns the active set at that timestamp:

```
WHERE valid_from <= as_of
  AND (valid_to IS NULL OR valid_to > as_of)
```

The window is half-open `[valid_from, valid_to)` — at the exact invalidation moment the row is *not* in the result. Inside the window it is. This avoids the boundary ambiguity of "did the fact stop being true at second N or N+1."

### Hard delete is still available

`DELETE /v1/memory/entries/:id` and the FK cascade from `DELETE /v1/admin/tenants/:id` still erase the row entirely, taking the FTS5 index entries and any cascading memory_relations with it. Use it for actually-wrong rows, GDPR / privacy purges, or test cleanup — not for retiring facts that were once true.

## Retrieval

The retrieval pipeline is layered. Each layer is independent and the cheaper layers fire universally; the LLM-cost layers are opt-in per request (HTTP) or per agent (slash surface).

| Layer | Cost | Where it lives |
|-------|------|----------------|
| **Lexical (BM25 + Porter stemmer + stopwords)** | free | FTS5 index, always on |
| **NEAR proximity** | free | FTS query construction, always on |
| **Metadata boost** | free | `list_entries` SQL, always on |
| **Intent routing** | free (regex) | HTTP `intent=auto` (default) / agent `MemoryConfig::intent_routing` |
| **RRF graduated merge** | free | `search_entries_graduated`, always on when conv. scoped |
| **Query expansion** | 1 advisor call | HTTP `expand=<model>` / agent `MemoryConfig::search_expand` |
| **Rerank (single-stage)** | 1 advisor call | HTTP `rerank=<model>` / agent `--rerank` flag |
| **Rerank (two-stage fine)** | 2 advisor calls | HTTP `rerank=…` + `rerank_fine=…` |
| **Validity filter** | free | `valid_to IS NULL` (default) or `as_of` window |

### Lexical (always on)

Okapi-BM25 over an FTS5 index on `(title, content, tags, source)` with per-field weights `(10, 4, 8, 2)`. The tokenizer is `porter unicode61 remove_diacritics 2` — case-folding, English stemming, accent-stripping. Stopwords are stripped at query time before quoting; common conversational filler ("what", "the", "is") doesn't dilute scoring across hundreds of thousands of irrelevant rows.

### NEAR proximity (always on)

For 2-to-6-token queries, the FTS expression is `"phrase" OR NEAR("t1" "t2" ..., 8) OR ("t1" OR "t2" OR ...)`. The NEAR clause matches rows where every token appears within 8 word positions of any other — bridges the gap between strict phrase (high precision, low recall) and bag-of-words (high recall, low precision) without an extra index.

### Metadata boost (always on)

When the caller passes `types=[…]` or a `tag`, matching rows have their BM25 score multiplied (1.3× for type, 1.2× for tag). Mismatched rows still appear, just lower in the order. This is what makes `/mem entries type=project` a hard filter (no `q`, type acts as `WHERE`) but `/mem search query type=project` a soft boost.

### Intent routing (heuristic, free)

A regex-based question classifier detects cue words in `q` and adds a soft type boost when the caller didn't supply one explicitly. The cue map:

| Cue words | Boosted types |
|-----------|---------------|
| `favorite`, `prefer`, `love`, `hate`, `enjoy`, `dislike`, `wants`, `liked` | `user`, `feedback` |
| `what is`, `where is`, `who is`, `how many`, `url`, `address`, `endpoint` | `reference` |
| `how to`, `how do`, `why is`, `explain`, `tutorial`, `guide` | `learning` |
| `when`, `before`, `after`, `since`, `first`, `recent`, `ago`, `did` | `project` |

Caller-supplied `type=…` always wins; intent only fires when `type` is absent. Disable per-request with HTTP `intent=off` or per-agent with `memory.intent_routing=false`.

### RRF graduated merge (free, fires when conversation-scoped)

When the request includes `graduated=true` (HTTP) or runs inside a conversation (agent), `search_entries_graduated` runs **two** passes and reciprocal-rank-fuses the rankings:

- **Pass 1** — conversation-scoped: `WHERE conversation_id = ? OR conversation_id IS NULL`, weighted **1.5×** in fusion.
- **Pass 2** — tenant-wide: drops the conversation filter, weighted **1.0×** in fusion.

Each entry's fused score is `Σ_p weight_p / (60 + rank_p)`. Conversation-local hits keep their locality bias on close calls (1.5× factor), but a *strong* tenant-wide hit can outrank a *weak* conversation-local one. This fixes the multi-session case where the answer lives in another conversation; the previous "skip pass 2 if pass 1 returned cap hits" sandbagged tenant-wide candidates whenever the local pass saturated.

### Query expansion (1 advisor call, opt-in)

When `expand=<model>` is set on the HTTP request — or the calling agent has `memory.search_expand=true` — the server calls the model once to generate **2 paraphrases** of `q`, runs all three variants (original + 2 paraphrases) through the full FTS pipeline, and reciprocal-rank-fuses the rankings (original at weight 1.0, paraphrases at 0.7). Each variant gets its own intent classification, so a paraphrase that shifts the question shape from "what is" to "where is" picks up an appropriate type boost.

This is the no-embedding alternative to dense retrieval. It catches paraphrased queries — "when did the user travel to Japan?" finds an entry that says "I just got back from Tokyo" — without any new index, table, or external dependency. ~150 ms + ~$0.0001 per request at Haiku speeds. Failures (advisor unavailable, unparseable output) are benign: search proceeds with the original query alone and a `note` is surfaced in the response's `expansion` block (or in the agent's reply).

### Rerank (1 or 2 advisor calls, opt-in)

When `rerank=<model>` is set — or the agent appends `--rerank` — the top-N candidates after fusion go through an LLM for a final reorder. The reranker prompt enriches each candidate with `(type · YYYY-MM-DD · superseded)` metadata so the model can pick the most-recent non-superseded entry on temporal/knowledge-update questions. Without that metadata the reranker would be reordering by title + excerpt only — it can't pick the most-recent preference, prefer a `type=preference` row over a `type=research` row that mentions the same topic, or skip an explicitly invalidated entry.

Adding `rerank_fine=<stronger-model>` engages two-stage rerank: pass 1 with the cheap `rerank` model coarse-orders the wide pool; the top 8 advance to pass 2 with the stronger model and 1500-byte excerpts. The remaining tail is preserved after pass 2 so R@K beyond 8 doesn't collapse below the single-stage baseline.

The reranker uses `make_advisor_invoker(caller_id)` so the model is the calling agent's `advisor.model` — one-shot, history-less, attributed to the caller via `cost_cb_`. Failures fall back to FTS order with a populated `note`.

### How `/mem search` works under the hood

When an agent emits `/mem search deployment notes`, the reader callback inside the request handler:

1. Reads the calling agent's `MemoryConfig` (search_expand, intent_routing).
2. Classifies question intent if enabled (free), boosting matching types.
3. If `search_expand` is on and the agent has an advisor model, generates 2 paraphrases and fans out the FTS query across all three variants.
4. Runs `search_entries_graduated(...)` per variant: conversation pass + tenant-wide pass, RRF-fused.
5. RRF-fuses across query variants if expansion fired.
6. If `--rerank` was on the command, reranks the top-N through the agent's advisor model.
7. Renders top-3 with content excerpts inline; remaining hits are one-line summaries with `(YYYY-MM-DD · superseded)` metadata in the line.
8. Marks conversation-pinned hits with `[conversation]` so the agent can tell local context from broader tenant memory at a glance.

When expansion fires, the rendered output prefixes `(also searched: '<paraphrase 1>' | '<paraphrase 2>')` so the agent can audit what recall surface ran.

### Why metadata is a signal, not a gate

`/mem entries type=project` is a hard filter — the caller is browsing a category, and rows of other types are explicitly excluded. But `/mem search query` is a different shape: the caller is trying to *find* something, and aggressive filtering loses recall. If they passed `types=[project]` because they expect the answer to be a project entry, but the answer is actually a `reference` or `learning` entry that mentions the same query terms, a hard filter would hide it.

The layer treats type and tag matches in *search* mode as score multipliers (~30% for type, ~20% for tag) rather than `WHERE` clauses. Project entries rank higher when the caller passed `types=[project]`, but the reference and learning matches still appear — the agent gets the answer it needed even when it over-specified the filter. Hard-filtering remains the default in *browse* mode (no `q`).

## Memory enrichment

Three of the retrieval and ingest layers use one LLM call apiece. They're opt-in per-request on the HTTP surface and per-agent on the slash surface, so an agent's call to `/mem search` or `/mem add entry` can pick up the right enrichment automatically without the agent having to pass any flag.

The `Constitution::MemoryConfig` block in an agent's JSON controls which advisor-driven layers fire on its `/mem` operations. All four toggles default conservatively — agents that haven't opted in keep the pre-config behavior:

| Toggle | Default | What it controls |
|--------|---------|------------------|
| `intent_routing` | `true` | Heuristic question classifier on `/mem search`. Free; defaults on because the worst case is "no boost applied" — monotonic vs. off. |
| `search_expand` | `false` | Query expansion on `/mem search`. Costs one advisor call per search. Closes the recall gap on paraphrased queries. |
| `auto_tag` | `false` | Auto-tagging on `/mem add entry`. Advisor extracts 2-4 topical tags from title+content; merged into caller-supplied tags. Tags get an 8× BM25 weight, so this is the single cheapest way to boost retrieval signal on agent ingest paths. |
| `auto_supersede` | `false` | Auto-supersession on `/mem add entry`. After the new entry is created, the advisor inspects the top-5 same-type FTS hits on the title for direct contradictions and stamps `valid_to=now()` on flagged ids. Use only when the agent's writes typically replace prior facts (research, engineering decisions). The prompt biases toward "leave alone" — false positives erase legitimate prior memory. |

Configure via the `memory` block in agent JSON:

```json
{
  "name": "research",
  "advisor": { "model": "claude-opus-4-7", "mode": "consult" },
  "memory": {
    "search_expand": true,
    "auto_tag": true,
    "auto_supersede": true
  },
  "capabilities": ["/fetch", "/mem", "/agent"]
}
```

`search_expand`, `auto_tag`, and `auto_supersede` need an `advisor.model` configured — they invoke the agent's advisor, attributed via `cost_cb_` like a direct `/advise` call. Without an advisor model the toggles silently no-op (the helpers return `(advisor unavailable)` notes; the search/write proceeds without enrichment). For agents that don't have an advisor, leave the `memory` block out — `intent_routing` is on by default and runs without LLM cost.

When an advisor-driven layer fires, the result surfaces in the operation's output:

- `/mem search` prefix: `(also searched: '<paraphrase 1>' | '<paraphrase 2>')`
- `/mem add entry` trailing lines: `auto-tagged: tag1, tag2, ...` and `superseded: #38, #22`

This makes per-agent enrichment auditable in the agent's own transcript without instrumenting the LLM call separately.

## Traversal

Beyond search, four read commands let agents navigate the graph structurally:

### `/mem entries`

Browse the current active set. Newest-first. Optional `type=foo,bar` and `tag=baz` filters apply as hard `WHERE` clauses (browse mode, not search mode). Use this for "show me my recent project entries" — not for finding a specific fact.

### `/mem entry <id>`

One entry, with its outgoing and incoming edges and the **neighbour titles inlined on each edge**. This is the "look up one node" operation; the inlined neighbour titles save a round-trip when the agent's next move would have been `/mem entry <neighbour_id>` anyway.

The `<id>` argument tolerates a leading `#`, matching the form `/mem entries` renders (`- #42  [project]  …`). `/mem entry 42` and `/mem entry #42` are equivalent. The same `#`-tolerance applies to `/mem expand`, `/mem density`, `/mem invalidate`, and both ids in `/mem add link <src> <rel> <dst>`. Agents can copy ids verbatim from any rendered output without manual stripping.

### `/mem expand <id> [depth=N]`

Breadth-first subgraph rooted at `<id>`, capped at depth 2 and 50 nodes. One round-trip for what would otherwise be N+1 sequential `/mem entry` calls. Renders as a tree with relation labels on each edge.

Use case: an agent knows the seed entry is relevant and wants to follow `refines` / `supports` / `contradicts` chains a couple of hops out without manually walking each neighbour.

### `/mem density <id>`

Degree summary: in-edge count, out-edge count, distinct relation kinds, 2-hop reach. No content rendered — this is the "is this area already richly linked?" probe.

The recommended workflow: **before** doing fresh research on a topic, `/mem density` the most relevant existing entry. A dense neighbourhood (many edges, many distinct relations, big 2-hop reach) suggests the area is already covered — synthesise from existing entries instead of fetching new sources. A sparse one suggests it's worth adding.

## Recommended workflows

### Retrieval before research

```
/mem search neanderthal gene flow              → ranked hits, top-3 inlined
/mem density 42                                → "is this area already richly linked?"
/mem expand 42 depth=2                         → cluster around the most-relevant hit if dense
```

The smarter ranker means a search rarely needs follow-up `/mem entry` reads; the inlined excerpts on the top hits usually answer the agent's question. Agents that go straight to fetching URLs without checking memory first are leaving recall on the table.

### Recording new findings

```
/mem add entry project Q3 observability rollout plan
   <body — required, the synthesis>
/endmem
/mem add entry reference Honeycomb pricing page (live fetch 2026-04)
   <facts, numbers, source URL>
/endmem
/mem add entry learning Honeycomb wins for trace-first small teams
   <conclusion synthesised from the above>
/endmem
/mem add link 88 supports 89
/mem add link 90 refines 88
```

A research-and-write turn typically produces one `project` (the deliverable), N `reference` entries (cited sources), one `learning` entry (the recommendation/synthesis), and a few relations linking them. Filing everything as `reference` defeats the type partitioning.

### Retiring stale facts

```
/mem invalidate 42
```

Soft-delete. The row stays in the DB and remains reachable via `as_of` reads (audit / replay) but disappears from default `/mem entries`, `/mem entry`, and `/mem search` results. Use when a recorded fact is no longer true: the user pivoted, a project shipped, a source contradicted. Distinct from a hard delete — there's no agent-facing surface for hard delete.

### Pairing artifacts with entries

```
/write --persist notes/honeycomb-pricing-cap.md
   <long-form notes from a fetch>
/endwrite
/mem add entry reference Honeycomb pricing page (live fetch 2026-04) --artifact #42
   <one-paragraph synthesis with the key facts>
/endmem
```

The full content lives in the artifact (no token cost on the next turn unless explicitly read); the entry's content is the searchable summary that `/mem search` ranks against. Future agents find the entry by query, see the artifact link, and `/read #<aid>` to fetch the long-form when they actually need it.

## Closed enums (reference)

Validated server-side and rejected with `400 {"error":"..."}` if violated. Adding values is a coordinated frontend+API change.

| Field        | Allowed values |
|--------------|----------------|
| `entry.type` | `user`, `feedback`, `project`, `reference`, `learning`, `context` |
| `relation`   | `relates_to`, `refines`, `contradicts`, `supersedes`, `supports` |

## Per-entry constraints (reference)

| Field         | Constraint |
|---------------|------------|
| `title`       | Non-empty, ≤ 200 chars |
| `content`     | ≤ 64 KiB |
| `source`      | ≤ 200 chars |
| `tags`        | JSON array of strings; each tag 1–64 chars; up to 32 tags. Always present in responses; pass `[]` (or omit) for none. |
| `artifact_id` | Optional FK to a [tenant artifact](artifacts.md). Validated against the tenant's catalogue. |
| `conversation_id` | Optional FK to a tenant conversation. Validated against the tenant's catalogue. |
| `valid_from`  | Set automatically on insert. Not editable. |
| `valid_to`    | Set by `POST .../invalidate`. Idempotent; cannot be cleared except by hard delete. |

## Per-relation constraints (reference)

- `source_id != target_id` — self-loops return `400 "self-loops not allowed"`.
- Both endpoints must belong to the calling tenant — otherwise `400 "entries belong to different tenants"`. (No 404-per-side, to avoid leaking whether the other tenant's id exists.)
- Relations are **directed and per-type**. The same pair can have multiple relations of different kinds; the same `(source, target, relation)` triple cannot exist twice — a duplicate returns `409 {"error":"...", "existing_id": N}`.
- Relations stay in the table when their endpoints are invalidated (soft-delete). They cascade-DELETE only when their endpoints are hard-deleted. Consumers that want "active relations only" should filter both endpoints' `valid_to IS NULL` at read time.

## Agent slash surface

Agents running inside `/v1/orchestrate` (or `/v1/conversations/:id/messages`) can read and write mid-turn. Reads surface every active entry and relation; writes land directly in the graph and are visible immediately.

| Command | Effect |
|---------|--------|
| `/mem entries` | List the tenant's active entries (newest first). |
| `/mem entries project,reference` | Same, filtered by type. Hard filter in browse mode. |
| `/mem entries tag=<name>` | Same, filtered by tag. Hard filter in browse mode. |
| `/mem entry 42` | One entry with neighbour titles inlined on each edge. |
| `/mem search <query>` | FTS5 + BM25 ranking with NEAR proximity, RRF graduated fusion (conv. + tenant-wide), and (when `memory.intent_routing`) heuristic type-boost. Top 3 hits get content excerpts; entries render with `(YYYY-MM-DD)` and `(superseded)` markers when present. With `memory.search_expand` on, also runs query reformulation and prefixes the output with `(also searched: …)`. |
| `/mem search <query> --rerank` | Same, then advisor-model rerank of the top-N. Reranker prompt enriches each candidate with `(type · YYYY-MM-DD · superseded)` so it can pick the most-recent non-superseded entry on temporal/knowledge-update questions. Costs one LLM call. |
| `/mem expand 42 [depth=N]` | BFS subgraph (depth max 2, ≤ 50 nodes). |
| `/mem density 42` | Degree summary: in/out edges, relation kinds, 2-hop reach. |
| `/mem add entry <type> <title> [--artifact #<id>]` … `/endmem` | Create a new entry. Body required. Pinned to the active conversation automatically. With `memory.auto_tag` on, surfaces `auto-tagged: tag1, tag2, …` in the OK response. With `memory.auto_supersede` on, surfaces `superseded: #N, #M` for entries the advisor flagged as factually replaced. |
| `/mem add link <src_id> <relation> <dst_id>` | Create a directed edge. |
| `/mem invalidate 42` | Soft-delete: set `valid_to = now()`. Row stays for `as_of` reads. |

Read output lands in a `[/mem entries]` / `[/mem entry]` / `[/mem search]` / `[/mem expand]` / `[/mem density]` tool-result block, framed by `[END MEMORY]`. Write and invalidate output lands in `[/mem add entry …]` / `[/mem add link …]` / `[/mem invalidate …]` blocks — typically `OK: …` so the agent can reference the new id (or confirm the invalidation) in the same turn.

## Pipeline memory at delegation boundaries

When the master delegates to a sub-agent via `/agent` (or `/parallel`), the runtime probes the structured-memory reader for entries scoped to the active conversation and prepends a `Pipeline memory` section to the sub-agent's `[DELEGATION CONTEXT]` block. The intent is so the sub-agent walks in already knowing what siblings have written this conversation — no guessing `/mem search` queries to discover what scout wrote two turns ago.

Constraints:

- **Conversation-scoped only.** Entries written in *this* conversation, not the tenant's full history. A sub-agent on a fresh conversation sees an empty pipeline memory rather than residue from prior runs.
- **Capped at 15 entries.** Newest first. Designed to be a compact pointer list, not a content dump — sub-agents `/mem entry #<id>` to read the full body of any entry that looks relevant.
- **Title + type + tags only.** Bodies are not included; the agent fetches them on demand.
- **No active conversation → empty.** Raw `/v1/orchestrate` calls (no `conversation_id`) and CLI / REPL contexts (no tenant store wired) skip the probe entirely. Pipeline memory is an HTTP-API affordance.
- **`#`-prefixed ids.** The rendered ids carry a `#` — exactly the form `/mem entry`, `/mem expand`, `/mem density`, `/mem invalidate`, `/mem add link` accept.

The injected section reads like:

```
[DELEGATION CONTEXT]
Original request: …
Delegated by: index
Pipeline depth: 1/2
Pipeline memory (entries written by prior agents this conversation —
use /mem entry #<id> to read full content before searching or
restating from training):
3 entries (newest first):
- #281  [project]  Q3 observability rollout — initial brief
- #280  [reference]  Honeycomb pricing page (live fetch 2026-04)  [pricing]
- #279  [learning]  Honeycomb wins for trace-first small teams
[END DELEGATION CONTEXT]

…sub-agent's actual instructions follow…
```

This structurally addresses one of the most common multi-agent failure modes: the writer agent receives the master's task, has no idea the researcher already filed sources to the graph, and falls back to "graph is cold, synthesise from training" mode. With pipeline memory, it sees the entries directly and can `/mem entry #<id>` for full content.

## Tenant scoping

The reader and writer are both bound to the request's authenticated tenant — sub-agents invoked via `/agent` and parallel children spawned via `/parallel` inherit the same tenant scope. CLI/REPL contexts (`arbiter --send`, the interactive REPL) don't have a tenant; both `/mem entries…` and `/mem add…` return ERR there. This surface is API-only.

## See also

- [`POST /v1/memory/entries`](../memory/entries/create.md), [`GET /v1/memory/entries`](../memory/entries/list.md), [`GET /v1/memory/entries/:id`](../memory/entries/get.md), [`PATCH /v1/memory/entries/:id`](../memory/entries/patch.md), [`DELETE /v1/memory/entries/:id`](../memory/entries/delete.md), [`POST /v1/memory/entries/:id/invalidate`](../memory/entries/invalidate.md)
- [`POST /v1/memory/relations`](../memory/relations/create.md), [`GET /v1/memory/relations`](../memory/relations/list.md), [`DELETE /v1/memory/relations/:id`](../memory/relations/delete.md)
- [`GET /v1/memory/graph`](../memory/graph.md)
- [Artifacts](artifacts.md) — for the memory↔artifact link
- [Data model](data-model.md#memoryentry) — exact field shapes
