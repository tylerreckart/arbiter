# Voice

Voice is another **surface** on the shared orchestration loop — not a second runtime, and not audio inside the agent core. Clients (the [Intercom](https://github.com/tylerreckart/intercom) bridge, a phone, an ESP32) speak PCM at the edge; Arbiter continues to reason in text + [SSE events](sse-events.md).

In-process STT/TTS and a companion PCM stream are tracked as [#207](https://github.com/tylerreckart/arbiter/issues/207). Until that lands, Intercom (or any similar bridge) is the supported path: device audio → STT → `POST /v1/conversations/:id/messages` → TTS of the assistant text.

## What Arbiter owns

Two knobs make the text middle speakable so the bridge does not have to stuff a reminder into every user turn (which pollutes conversation history and fights the constitution).

| Knob | Where | Effect |
|------|--------|--------|
| `mode: "spoken"` | Agent constitution / `agent_def` | Replaces the specialist or index **voice block** with a TTS register: spoken English, no markdown/LaTeX/lists, short answers. Writs still work; file work uses `/write` rather than TUI ` ```diff ` dumps. Identity still comes from `NAME` / `PERSONALITY` / `GOAL`. |
| `channel: "voice"` | Request body on orchestrate / conversation messages / agent chat | Per-turn overlay. Sets `channel` on the **ingress** agent so non-spoken constitutions grow a `SPOKEN OUTPUT` block. Echoed on `request_received`. **Suppresses intent reroute** so a spoken session is not handed a compressed specialist mid-conversation. |

`mode: "spoken"` is the right default for a dedicated voice agent (Intercom's Arthur). `channel: "voice"` is what a bridge should send on every turn — including when the addressed agent is `index` — so the stored user message stays the transcript alone.

`channel: "text"` (or omitting the field) is today's behaviour.

## What the bridge owns

STT, TTS, barge-in cancel (`POST /v1/requests/:id/cancel`), device auth, and PCM framing stay out of tree. Intercom already:

- Creates a conversation per device (`agent_id` + snapshotted `agent_def`)
- Streams each turn over SSE (`text` depth 0 + `done`)
- Maps Intercom `turn_id` to `Idempotency-Key`
- Cancels the Arbiter `request_id` when the device barges in

Recommended request body from the bridge:

```json
{
  "message": "<stt transcript>",
  "channel": "voice"
}
```

Do **not** append a parenthetical “this is a voice intercom…” suffix to `message`. The constitution overlay is the reminder; the transcript is what should persist and compact.

Pass `original_query` on follow-ups if you also want the advisor gate pinned to the first utterance. Voice channel already keeps **routing** sticky; `original_query` is the advisor's original-task pin.

## Events the bridge should consume

Unchanged catalog. For speech:

- `request_received` — take `request_id` immediately for cancel; `channel` is `"voice"` when requested.
- `text` at `depth == 0` — master deltas, already stripped of `/cmd` lines by the same `StreamFilter` the TUI uses. Sub-agent text is not what you speak.
- `tool_call` — optional progress cue while tools run (fillers).
- `done` — full `content` if you buffered rather than streaming TTS.

Master text is suppressed during delegation iterations (status lines only) until the synthesis turn. That is the correct spoken behaviour: do not read routing chatter aloud.

## Registers (do not confuse “voice”)

Constitution **VOICE:** is prose style, not audio:

| `mode` | Register | Typical surface |
|--------|----------|-----------------|
| `standard` (default) | Compressed field report | Specialists |
| `conversational` | Collaborative complete sentences | `index`, named conversational agents |
| `spoken` | TTS / intercom | Voice bridges |
| `writer` / `planner` | Task-specific | Starters |

A named agent with `mode: "conversational"` is **not** told it is `index`. Spoken mode never uses the TUI `CODE CHANGE FORMAT` block.

## See also

- [#207](https://github.com/tylerreckart/arbiter/issues/207) — first-class audio modality (PCM stream, tenant VoiceConfig)
- [SSE event catalog](sse-events.md)
- [Intent](intent.md) — voice channel skips classify+reroute
- [`POST /v1/orchestrate`](../api/orchestrate.md)
- [`POST /v1/conversations/:id/messages`](../api/conversations/messages-post.md)
