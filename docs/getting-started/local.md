# Local Install

Install Arbiter locally, seed the starter agents, and open the terminal
workspace. You control the binary and configuration; model requests go to the
provider you configure.

## Prerequisites

- A provider key for whichever model family you want agents to use:
  - **OpenRouter** — `OPENROUTER_API_KEY` (or save to `~/.arbiter/openrouter_api_key`) for hosted models.
  - **Ollama** — set model ids to `ollama/<model>` for local models.
  - **Ollama** — set `OLLAMA_HOST` if not at `http://localhost:11434`.
- Only one is required. Multiple can coexist; different agents can target different providers.
- Optional: run `arbiter --setup-tools` to enable `/search` (Brave key), `/browse` (Playwright MCP), and other MCP servers. Without search configured, agents fall back to `/fetch` on URLs they already know.

## Install the binary

The installer supports **macOS Apple silicon** and **Linux x86_64**, selects the
newest published release containing a compatible binary, and verifies its
SHA-256 checksum:

```bash
curl -fsSL https://arbiter.run/install.sh | sh
```

Review the [installer source](https://github.com/tylerreckart/arbiter/blob/main/web/install.sh)
before running it if you prefer. Set `INSTALL_DIR` to install somewhere other
than `/usr/local/bin`; the script uses `sudo` only when the target directory is
not writable.

Pin a specific release:

```bash
curl -fsSL https://arbiter.run/install.sh | ARBITER_VERSION=v0.8.5 sh
```

### From source

Any platform with C++20:

```bash
git clone https://github.com/tylerreckart/arbiter.git
cd arbiter
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build
```

Build deps: OpenSSL, libcurl, SQLite3, a C++20 compiler. Distro-specific install commands are in [`CONTRIBUTING.md`](../../CONTRIBUTING.md#linux).

## First run

```bash
export OPENROUTER_API_KEY="sk-or-..."
arbiter --init                          # seed ~/.arbiter/ with starter agents
arbiter                                 # launch the terminal client
```

`--init` writes nine starter agents into `~/.arbiter/agents/*.json` (reviewer, research, writer, devops, planner, backend, frontend, marketer, social) and is safe to re-run — existing files are preserved unless you pass `--force`. See [`cli/init.md`](../cli/init.md) for the full layout.

`arbiter` opens the interactive TUI. Type a message and the master agent (`index`) decides whether to answer directly or delegate. Switch agents with `/use <name>`; send a one-off to a specific agent with `/send <name> <message>`. Full TUI reference: [`tui/`](../tui/index.md).

Try this first:

> Help me turn this project into a concrete plan. Ask what you need to know,
> then delegate research and review to separate agents.

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
├── openrouter_api_key     OpenRouter key (alternative to env var)
├── search_api_key         Brave Search key for /search (via --setup-tools)
├── tenants.db             tenant identities for --api
├── sessions/              per-cwd TUI session snapshots
├── memory/                per-agent persistent scratchpads
├── mcp_servers.json       optional MCP server registry (via --setup-tools)
└── a2a_agents.json        optional remote A2A agent registry
```

Full reference: [`cli/environment.md`](../cli/environment.md).

## Next steps

- **Author your own agent.** Drop a JSON into `~/.arbiter/agents/<id>.json`. Schema: [`api/agents/create.md`](../api/agents/create.md). Concepts: [Writ](../concepts/writ.md), [Advisor](../concepts/advisor.md).
- **Read the design philosophy.** [`philosophy.md`](../philosophy.md).
- **Wire up integrations.** `arbiter --setup-tools` for search / browse / MCP, or edit the files by hand — [MCP servers](../concepts/mcp.md), [A2A](../concepts/a2a.md).
