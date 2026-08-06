# Example: book byline agent (over-the-network)

A minimal, tool-less agent that turns a **book summary JSON object** into a
single commercial byline. Designed to be sent as an inline `agent_def` to a
running [`arbiter --api`](../../cli/api.md) instance — sibling services keep the
definition in their own store and treat Arbiter as a cheap compute layer.

The constitution also ships as the `byline` starter (`agents/byline.json`) for
local `--init` / catalog use. Same rules either way.

## Why this shape

| Choice | Rationale |
|--------|-----------|
| `google/gemini-3.1-flash-lite` | Smallest inexpensive model in the hosted catalogue — enough for one-sentence jacket copy. |
| `max_tokens: 96` | Bylines are short; hard-cap stops rambling. |
| `mode: "writer"` | Prose-oriented base prompt without the tool-inventory tax. |
| `capabilities: ["byline"]` | Non-slash token → **no tool bundles**. (Empty `capabilities` would grant *all* tools.) |
| No advisor / memory | One-shot transform; keep latency and cost near floor. |

## Inline call (`POST /v1/orchestrate`)

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
    "mode": "writer",
    "model": "google/gemini-3.1-flash-lite",
    "max_tokens": 96,
    "temperature": 0.55,
    "goal": "From a JSON book-summary object, write one commercial byline — a single sentence that sells the book without spoiling it.",
    "personality": "Jacket-copy instinct. Concrete over clever. Never cute for its own sake.",
    "brevity": "lite",
    "rules": [
      "The user message is a JSON object describing a book. Prefer fields named summary, synopsis, or description; use title, author, genre, themes, tone, and audience only as supporting context when present.",
      "Invent nothing that is not grounded in the provided JSON. Do not look anything up. Do not use tools or slash commands.",
      "Reply with exactly one byline: a single sentence of 12–28 words. No surrounding quotes, no label, no preamble, no markdown, no second sentence.",
      "Lead with the human stakes or the distinctive hook from the summary — not a genre label or the title restated as marketing.",
      "No spoilers of endings, twists, or mid-book reveals. Prefer the premise and pressure over plot machinery.",
      "Match register to the book as the summary implies (literary, thriller, YA, etc.). Avoid empty superlatives (masterpiece, must-read, unforgettable) unless the summary itself uses that register.",
      "If the JSON has no usable summary/synopsis/description, reply with exactly: Insufficient summary."
    ],
    "capabilities": ["byline"]
  },
  "message": "{\"title\":\"The Quiet Meridian\",\"author\":\"Elena Voss\",\"genre\":\"literary thriller\",\"summary\":\"A cartographer returns to her coastal hometown after a satellite error erases a stretch of shoreline from every map. As she redraws what vanished, she uncovers a decades-old cover-up tied to her missing father.\"}"
}
EOF
```

The `message` value is a **stringified JSON object** (or you can embed the object
fields in a short prose wrapper — the agent only needs the summary text).
Collect the byline from SSE `text` events; ignore tool noise (there should be
none). Terminal success is `done` with `ok: true`.

### Expected input shape

Any subset is fine; `summary` / `synopsis` / `description` is the only required
signal:

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
  -d '{"message":"{\"summary\":\"…\"}"}' \
  http://127.0.0.1:8080/v1/agents/byline/chat
```

`POST /v1/agents` requires a caller-chosen `id`. The on-disk starter omits it
because `--init` keys agents by filename.

## See also

- [`POST /v1/orchestrate`](../orchestrate.md) — inline `agent_def`
- [`POST /v1/agents`](../agents/create.md) — persist into the tenant catalog
- [`POST /v1/agents/:id/chat`](../agents/chat.md) — path-bound one-shot
- [`GET /v1/models`](../models.md) — hosted model ids
- [`agents/byline.json`](../../../agents/byline.json) — source constitution
