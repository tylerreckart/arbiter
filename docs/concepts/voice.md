# Voice

Voice is another **surface** on the shared orchestration loop — not a second runtime, and not audio inside the agent core. Clients (the [Intercom](https://github.com/tylerreckart/intercom) bridge, a phone, an ESP32) speak PCM at the edge; Arbiter continues to reason in text + [SSE events](sse-events.md).

In-process STT/TTS and a companion PCM stream are tracked as [#207](https://github.com/tylerreckart/arbiter/issues/207). Until that lands, Intercom (or any similar bridge) is the supported path: device audio → STT → `POST /v1/conversations/:id/messages` → TTS of the assistant text.

## What Arbiter owns

Two knobs make the text middle speakable so the bridge does not have to stuff a reminder into every user turn (which pollutes conversation history and fights the constitution).

| Knob | Where | Effect |
|------|--------|--------|
| `mode: "spoken"` | Agent constitution / `agent_def` | Replaces the specialist or index **voice block** with a TTS conversation register: spoken English, contractions and natural cadence, no markdown/LaTeX/lists. Writs still work; file work uses `/write` rather than TUI ` ```diff ` dumps. Identity still comes from `NAME` / `PERSONALITY` / `GOAL`. |
| `channel: "voice"` | Request body on orchestrate / conversation messages / agent chat | Per-turn overlay. Sets `channel` on the **ingress** agent so non-spoken constitutions grow a `SPOKEN OUTPUT` block (last-wins on user-facing register: dispatch compression does not apply to speech). File delivery leaves the TUI diff path. Echoed on `request_received`. **Suppresses intent reroute** so a spoken session is not handed a compressed specialist mid-conversation. |

`mode: "spoken"` is the right default for a dedicated voice agent (Intercom's Arthur). `channel: "voice"` is what a bridge should send on every turn — including when the addressed agent is `index` — so the stored user message stays the transcript alone.

`channel: "text"` (or omitting the field) is today's behaviour.

## Spoken register (why it is not a briefing)

The first spoken block was a TTS-safe specialist: “short complete sentences”, a hard one-to-three / at-most-five cap, and the dispatch template `[answer]. [evidence]. [next step]`. That is what made replies feel robotic on Kokoro — not missing SSML.

Spoken mode is now **conversational speech for the ear**:

- Cadence over compression. A short question gets a short answer; a real explanation is allowed to run. Stop when the listener has it — don't pad, don't telegram.
- A brief spoken beat of orientation is fine (`Right — the build is green`). No canned openers or `does that help` closers. After a real answer, leave space; ask a question only when you need one.
- Punctuate for the ear (commas and periods where you'd breathe). Write dates and ordinals in words when they will be heard. No SSML, phonetic spellings, or stage directions — Intercom already sanitizes hyphens/ordinals in `to_speakable` and classifies delivery for Kokoro.
- `REASONING` and tool names stay private. Writs still occupy their own lines and are stripped before speech.

`channel: "voice"` on a specialist or on `index` does **not** replace identity. The overlay at the end of the prompt tells the model the dispatch / screen register does not apply to what is said out loud.

Dedicated voice agents should set `brevity: "lite"` and a slightly higher `temperature` (Arthur uses `0.55`) in `agent_def`. Those are agent knobs, not channel semantics. Give them the `/mem` capability (Arthur already does) so the spoken **memory habit** fires.

## Personal-assistant memory

`/mem` already exists for every agent that lists it. Research-shaped COMMAND RULES tell specialists to probe the graph before a literature review. Spoken agents were not pushed to use the same tools as a PA — preferences, people, open loops, “remember that.”

When `mode: "spoken"` or `channel: "voice"` **and** the mem bundle is on, the constitution grows a **MEMORY HABIT** block (plus a compact overlay bullet on non-spoken voice turns):

- **Recall first.** If the user refers to preferences, past decisions, open loops, people, or says remember / recall, emit `/mem search` (and `/mem expand` when a hit looks right) *before* answering from scratch. A lookup turn may be writs-only; speak after `[TOOL RESULTS]`. Skip the search when this conversation already holds the fact. Prefer memory + history over re-asking.
- **Write as you go.** After learning a durable fact, `/mem add entry` with the right type (`user`, `feedback`, `context`, `project`) in the **same turn** as the spoken reply. Body required. Don't file small talk; don't dump everything as `reference`. Prefer entries over `/mem write` scratchpad for facts that should surface next week.
- **Speech stays natural.** Writs on their own lines; `StreamFilter` strips them. Never name `/mem`. Use what you found (`You take the coffee black, so…`). A short “I'll keep that” is enough when they asked you to remember.

Types are the same closed enum as [structured memory](structured-memory.md). Voice just weights the personal ones (`user` / `feedback` / `context` / `project`) instead of the research triple (`project` / `reference` / `learning`).

Intercom already keeps a conversation per device, so conversation-scoped entries rank on the next PTT and unscoped rows remain visible. No bridge-contract change.

## What the bridge owns

STT, TTS, barge-in cancel (`POST /v1/requests/:id/cancel`), device auth, and PCM framing stay out of tree. Intercom already:

- Creates a conversation per device (`agent_id` + snapshotted `agent_def`)
- Streams each turn over SSE (`text` depth 0 + `done`)
- Maps Intercom `turn_id` to `Idempotency-Key`
- Cancels the Arbiter `request_id` when the device barges in
- Sends `channel: "voice"` on every turn (do not change this)
- Sanitizes TTS text at the edge (`to_speakable`) and plays filler PCM on `tool_call`

Recommended request body from the bridge:

```json
{
  "message": "<stt transcript>",
  "channel": "voice"
}
```

Do **not** append a parenthetical “this is a voice intercom…” suffix to `message`. The constitution overlay is the reminder; the transcript is what should persist and compact.

Pass `original_query` on follow-ups if you also want the advisor gate pinned to the first utterance. Voice channel already keeps **routing** sticky; `original_query` is the advisor's original-task pin.

### Intercom: what to change, what not to

The HTTP / SSE contract is unchanged (`channel: "voice"`, `mode: "spoken"`, depth-0 `text`, cancel, sticky intent). Do not move audio into Arbiter for this.

On the Intercom side, optional follow-ups that *help* the new register:

- Drop stacked “one to three sentences unless they asked for more” (and similar caps) from `config/arthur.agent.json` `rules`. The constitution now owns cadence; a second cap fights it.
- Keep `/mem` on Arthur's `capabilities` (already there). Do not add a “remember this” suffix to `message` — MEMORY HABIT is the reminder.
- Optional: `memory.auto_tag` / `search_expand` on Arthur if an advisor is configured — retrieval quality, not habit.
- Keep Arthur's identity (`sir`, British, leave space). Keep `to_speakable`, instant-ack fillers, and `early_flush_words` — those are edge TTS, not LLM shaping. Filler PCM on `tool_call` covers the silent `/mem search` lookup turn.
- Do not inject SSML into Arbiter `text` events, and do not expect Arbiter to emit it.

## Events the bridge should consume

Unchanged catalog. For speech:

- `request_received` — take `request_id` immediately for cancel; `channel` is `"voice"` when requested.
- `text` at `depth == 0` — master deltas, already stripped of `/cmd` lines by the same `StreamFilter` the TUI uses. Sub-agent text is not what you speak.
- `tool_call` — optional progress cue while tools run (fillers).
- `done` — full `content` if you buffered rather than streaming TTS.

Master `text` deltas stream live (including before a later `/agent` writ). Skip `→ delegating: …` status lines — those are routing chrome, not spoken answer. If you only want the final synthesis spoken, wait until after the last `tool_call` on `stream_id=0` rather than voicing every delta.

## Registers (do not confuse “voice”)

Constitution **VOICE:** is prose style, not audio:

| `mode` | Register | Typical surface |
|--------|----------|-----------------|
| `standard` (default) | Compressed field report | Specialists |
| `conversational` | Collaborative complete sentences | `index`, named conversational agents |
| `spoken` | Conversational speech for TTS / intercom | Voice bridges |
| `writer` / `planner` | Task-specific | Starters |

A named agent with `mode: "conversational"` is **not** told it is `index`. Spoken mode (and `channel: "voice"`) never uses the TUI `CODE CHANGE FORMAT` block.

## See also

- [#207](https://github.com/tylerreckart/arbiter/issues/207) — first-class audio modality (PCM stream, tenant VoiceConfig)
- [Structured memory](structured-memory.md) — entry types and `/mem` retrieval the spoken habit uses
- [SSE event catalog](sse-events.md)
- [Intent](intent.md) — voice channel skips classify+reroute
- [`POST /v1/orchestrate`](../api/orchestrate.md)
- [`POST /v1/conversations/:id/messages`](../api/conversations/messages-post.md)
