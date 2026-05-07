# Local install

Run arbiter on your own machine. You bring the provider keys; you control the binary. Free, open-source, fully featured. Ten minutes from `curl` to first agent reply.

If you'd rather call arbiter without operating it, see the [hosted preview](hosted.md) instead.

## Prerequisites

- A provider key for whichever model family you want agents to use:
  - **Anthropic** — `ANTHROPIC_API_KEY` (or save to `~/.arbiter/api_key`).
  - **OpenAI** — `OPENAI_API_KEY` (or save to `~/.arbiter/openai_api_key`).
  - **Ollama** — set `OLLAMA_HOST` if not at `http://localhost:11434`.
- Only one is required. Multiple can coexist; different agents can target different providers.
- Optional: a Brave Search key (`ARBITER_SEARCH_API_KEY`) if you want `/search` to resolve. Without it, agents fall back to `/fetch` on URLs they already know.

## Install the binary

**macOS arm64**

```bash
curl -L https://github.com/tylerreckart/arbiter/releases/latest/download/arbiter-macos-arm64.tar.gz \
  | tar xz -C /usr/local/bin
```

**Linux x86_64**

```bash
curl -L https://github.com/tylerreckart/arbiter/releases/latest/download/arbiter-linux-x86_64.tar.gz \
  | tar xz -C /usr/local/bin
```

**From source** (any platform with C++20)

```bash
git clone https://github.com/tylerreckart/arbiter.git
cd arbiter
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build
```

Build deps: OpenSSL, libcurl, SQLite3, a C++20 compiler. `libedit` or GNU readline is optional but recommended for line editing in the terminal client. Distro-specific install commands are in [`CONTRIBUTING.md`](../../CONTRIBUTING.md#linux).

## First run

```bash
export ANTHROPIC_API_KEY="sk-ant-..."   # or OPENAI_API_KEY
arbiter --init                          # seed ~/.arbiter/ with starter agents
arbiter                                 # launch the terminal client
```

`--init` writes nine starter agents into `~/.arbiter/agents/*.json` (reviewer, research, writer, devops, planner, backend, frontend, marketer, social) and is safe to re-run — existing files are preserved unless you pass `--force`. See [`cli/init.md`](../cli/init.md) for the full layout.

`arbiter` opens the interactive TUI. Type a message and the master agent (`index`) decides whether to answer directly or delegate. Switch agents with `/use <name>`; send a one-off to a specific agent with `/send <name> <message>`. Full TUI reference: [`tui/`](../tui/index.md).

## Try one-shot mode

For scripts and CI hooks:

```bash
arbiter --send reviewer "review: if (arr.length = 0) return;"
```

The agent runs the full orchestration loop internally — tool calls, sub-agent delegation, advisor gating — and prints the final reply on stdout. See [`cli/send.md`](../cli/send.md).

## Run as an HTTP server

If you want other clients (web UIs, automations, mobile) to drive arbiter:

```bash
arbiter --add-tenant me                       # prints the bearer token — save it
arbiter --api --port 8080                     # bind 127.0.0.1 by default
```

Authenticate with `Authorization: Bearer <token>`. The endpoint catalogue is at [`api/`](../api/index.md). Production deployments belong behind a reverse proxy for TLS and rate limiting — see [`concepts/operations.md`](../concepts/operations.md).

## Where things live

```
~/.arbiter/
├── agents/*.json          agent constitutions (edit these)
├── api_key                Anthropic key (alternative to env var)
├── openai_api_key         OpenAI key
├── tenants.db             tenant identities for --api
├── sessions/              per-cwd TUI session snapshots
├── memory/                per-agent persistent scratchpads
├── mcp_servers.json       optional MCP server registry
└── a2a_agents.json        optional remote A2A agent registry
```

Full reference: [`cli/environment.md`](../cli/environment.md).

## Next steps

- **Author your own agent.** Drop a JSON into `~/.arbiter/agents/<id>.json`. Schema: [`api/agents/create.md`](../api/agents/create.md). Concepts: [Writ](../concepts/writ.md), [Advisor](../concepts/advisor.md).
- **Read the design philosophy.** [`philosophy.md`](../philosophy.md).
- **Wire up integrations.** [MCP servers](../concepts/mcp.md) for external tools, [A2A](../concepts/a2a.md) for remote agents.
