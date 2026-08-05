# arbiter-ios

Minimal SwiftUI chat client for a deployed Arbiter HTTP+SSE API
(default `https://api.arbiter.run`).

## Requirements

- Xcode 15+ (Swift 5.9 / iOS 17+)
- A tenant token from the server: `arbiter --add-tenant <name>` → `atr_…`
- API reachable over HTTPS

## Open & run

```bash
open arbiter-ios/ArbiterIOS.xcodeproj
```

1. Select the **ArbiterIOS** target.
2. Set your **Team** under Signing & Capabilities (or use a simulator with automatic signing).
3. Run on a simulator or device.
4. Tap **Settings** (gear), paste your `atr_…` token, confirm base URL, Save.
5. Send a message.

## What it does

| Action | API |
|--------|-----|
| First send | `POST /v1/conversations` then `POST /v1/conversations/:id/messages` |
| Follow-ups | `POST /v1/conversations/:id/messages` (SSE) |
| Stop | `POST /v1/requests/:id/cancel` |
| Health check | `GET /v1/health` |

The UI streams `text` events with `depth == 0` into the assistant bubble, shows
`tool_call` status in a banner, and stores the token in the Keychain.

## Layout

```
arbiter-ios/
├── ArbiterIOS.xcodeproj
└── ArbiterIOS/
    ├── ArbiterIOSApp.swift
    ├── Models/
    ├── Networking/     # ArbiterClient + SSEParser
    ├── Storage/        # Keychain + AppConfig
    ├── ViewModels/
    └── Views/
```

## Configuration

- **Base URL** — Settings, or defaults to `https://api.arbiter.run`
- **Agent id** — Settings, default `index`
- **Token** — Keychain service `run.arbiter.ios` (never commit tokens)

## Next steps (not in this scaffold)

- Conversation list / history reload (`GET /v1/conversations`, messages list)
- Markdown rendering for assistant replies
- Tool-call timeline UI
- App icon assets

## Docs

- [Secure remote API](../docs/getting-started/server.md)
- [Conversations](../docs/api/conversations/create.md)
- [SSE events](../docs/concepts/sse-events.md)
