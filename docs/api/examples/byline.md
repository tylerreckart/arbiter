# Example: book byline agent (over-the-network)

A cheap agent that turns a **book JSON object** into a single commercial
byline. Uses the provided summary when present; if summary is missing, looks
the book up with `/search` → `/fetch` from title/author. Designed to be sent as
an inline `agent_def` to a running [`arbiter --api`](../../cli/api.md) instance —
sibling services keep the definition in their own store and treat Arbiter as a
compute layer.

The constitution also ships as the `byline` starter (`agents/byline.json`) for
local `--init` / catalog use. Same rules either way.

Requires a search key on the API host (`ARBITER_SEARCH_API_KEY` or
`arbiter --setup-tools`) for the lookup path. Without one, `/search` errors and
the agent falls through to `Insufficient summary` when no summary was supplied.
See [Web search](../../concepts/search.md).

## Why this shape

| Choice | Rationale |
|--------|-----------|
| `google/gemini-3.1-flash-lite` | Smallest inexpensive model in the hosted catalogue — enough for one-sentence jacket copy plus a short search→fetch loop. |
| `max_tokens: 512` | Room for a couple of slash commands; final byline stays short by rule. |
| Standard mode (no `writer`) | Writer mode omits the tool inventory from the system prompt; lookup needs `/search` and `/fetch` visible. |
| `capabilities: ["/search","/fetch","/browse"]` | Web bundle only — no `/exec`, `/write`, `/mem`, or delegation. |
| No advisor / memory | Keep latency and cost near floor; lookup is opt-in when the summary is absent. |

## Inline call (`POST /v1/orchestrate`)

### With a summary (no lookup)

```bash
curl -N \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -H "Idempotency-Key: $(uuidgen)" \
  -d @- \
  http://127.0.0.1:8080/v1/orchestrate <<'EOF'
{
  "agent": "byline",
  "agent_def": {
    "id": "byline",
    "name": "byline",
    "role": "book-byline-writer",
    "model": "google/gemini-3.1-flash-lite",
    "max_tokens": 512,
    "temperature": 0.55,
    "goal": "Write one commercial byline for a book from a JSON object — using the provided summary when present, otherwise looking the book up on the web from title/author.",
    "personality": "Jacket-copy instinct. Concrete over clever. Never cute for its own sake.",
    "brevity": "lite",
    "rules": [
      "The user message is a JSON object describing a book. Prefer fields named summary, synopsis, or description when present; use title, author, genre, themes, tone, and audience as supporting context.",
      "If summary/synopsis/description is missing or empty but title (and optionally author) is present, look the book up: /search \"<title> <author> synopsis\" → /fetch the best publisher, bookstore, or encyclopedia hit. Skip /browse unless /fetch returns a JS/paywall shell. Extract a spoiler-free premise from the page — do not invent plot.",
      "Never look anything up when a usable summary/synopsis/description is already in the JSON. Never invent facts not grounded in the JSON or a fetched source.",
      "After you have enough premise (from JSON or lookup), the final reply is exactly one byline: a single sentence of 12–28 words. No surrounding quotes, no label, no preamble, no markdown, no second sentence, no citations.",
      "Lead with the human stakes or the distinctive hook — not a genre label or the title restated as marketing.",
      "No spoilers of endings, twists, or mid-book reveals. Prefer the premise and pressure over plot machinery.",
      "Match register to the book as the summary or sources imply (literary, thriller, YA, etc.). Avoid empty superlatives (masterpiece, must-read, unforgettable) unless the source itself uses that register.",
      "If there is no usable summary and lookup cannot identify the book (no title, ambiguous matches, or empty sources), reply with exactly: Insufficient summary."
    ],
    "capabilities": ["/search", "/fetch", "/browse"]
  },
  "message": "{\"title\":\"The Quiet Meridian\",\"author\":\"Elena Voss\",\"genre\":\"literary thriller\",\"summary\":\"A cartographer returns to her coastal hometown after a satellite error erases a stretch of shoreline from every map. As she redraws what vanished, she uncovers a decades-old cover-up tied to her missing father.\"}"
}
EOF
```

### Title only (triggers lookup)

```bash
curl -N \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -H "Idempotency-Key: $(uuidgen)" \
  -d @- \
  http://127.0.0.1:8080/v1/orchestrate <<'EOF'
{
  "agent": "byline",
  "agent_def": {
    "id": "byline",
    "name": "byline",
    "role": "book-byline-writer",
    "model": "google/gemini-3.1-flash-lite",
    "max_tokens": 512,
    "temperature": 0.55,
    "goal": "Write one commercial byline for a book from a JSON object — using the provided summary when present, otherwise looking the book up on the web from title/author.",
    "personality": "Jacket-copy instinct. Concrete over clever. Never cute for its own sake.",
    "brevity": "lite",
    "rules": [
      "The user message is a JSON object describing a book. Prefer fields named summary, synopsis, or description when present; use title, author, genre, themes, tone, and audience as supporting context.",
      "If summary/synopsis/description is missing or empty but title (and optionally author) is present, look the book up: /search \"<title> <author> synopsis\" → /fetch the best publisher, bookstore, or encyclopedia hit. Skip /browse unless /fetch returns a JS/paywall shell. Extract a spoiler-free premise from the page — do not invent plot.",
      "Never look anything up when a usable summary/synopsis/description is already in the JSON. Never invent facts not grounded in the JSON or a fetched source.",
      "After you have enough premise (from JSON or lookup), the final reply is exactly one byline: a single sentence of 12–28 words. No surrounding quotes, no label, no preamble, no markdown, no second sentence, no citations.",
      "Lead with the human stakes or the distinctive hook — not a genre label or the title restated as marketing.",
      "No spoilers of endings, twists, or mid-book reveals. Prefer the premise and pressure over plot machinery.",
      "Match register to the book as the summary or sources imply (literary, thriller, YA, etc.). Avoid empty superlatives (masterpiece, must-read, unforgettable) unless the source itself uses that register.",
      "If there is no usable summary and lookup cannot identify the book (no title, ambiguous matches, or empty sources), reply with exactly: Insufficient summary."
    ],
    "capabilities": ["/search", "/fetch", "/browse"]
  },
  "message": "{\"title\":\"The Left Hand of Darkness\",\"author\":\"Ursula K. Le Guin\"}"
}
EOF
```

The `message` value is a **stringified JSON object**. Collect the byline from the
final SSE `text` events (lookup turns may emit `tool_call` for `/search` /
`/fetch` first). Terminal success is `done` with `ok: true`.

### Expected input shape

```json
{
  "title": "The Quiet Meridian",
  "author": "Elena Voss",
  "genre": "literary thriller",
  "themes": ["grief", "cartography", "cover-up"],
  "audience": "adult",
  "summary": "A cartographer returns to her coastal hometown after a satellite error erases a stretch of shoreline from every map. As she redraws what vanished, she uncovers a decades-old cover-up tied to her missing father."
}
```

- **With `summary` / `synopsis` / `description`:** write the byline directly (no web calls).
- **Without those fields, but with `title`:** `/search` → `/fetch` a synopsis, then byline.
- **Neither:** `Insufficient summary`.

### Expected output shape

One sentence, nothing else — for example:

> When a cartographer's hometown vanishes from every map, redrawing the coast means uncovering the cover-up that took her father.

## Persist once, chat many

If the sibling service wants a stable catalog id instead of shipping `agent_def`
on every call:

```bash
# once — starter JSON has no id (filename is the id for --init); add it here
curl -X POST \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -d "$(jq '. + {id: "byline"}' agents/byline.json)" \
  http://127.0.0.1:8080/v1/agents

# thereafter
curl -N \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -d '{"message":"{\"title\":\"The Left Hand of Darkness\",\"author\":\"Ursula K. Le Guin\"}"}' \
  http://127.0.0.1:8080/v1/agents/byline/chat
```

`POST /v1/agents` requires a caller-chosen `id`. The on-disk starter omits it
because `--init` keys agents by filename.

## See also

- [`POST /v1/orchestrate`](../orchestrate.md) — inline `agent_def`
- [`POST /v1/agents`](../agents/create.md) — persist into the tenant catalog
- [`POST /v1/agents/:id/chat`](../agents/chat.md) — path-bound one-shot
- [`GET /v1/models`](../models.md) — hosted model ids
- [Web search](../../concepts/search.md) — `/search` setup for lookup
- [`agents/byline.json`](../../../agents/byline.json) — source constitution
