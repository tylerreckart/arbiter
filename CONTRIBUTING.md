# Contributing to arbiter

Thanks for taking a look. Arbiter is pre-1.0 and the surface — slash
commands, agent constitutions, SSE event shapes, the HTTP API — is still
moving. PRs are welcome; please read this first.

## Quick start

```bash
git clone https://github.com/arbitercorp/arbiter-core.git
cd arbiter-core
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The build needs OpenSSL, libcurl, SQLite3, and a C++20 compiler.
libedit (or GNU readline) is optional but recommended for the
terminal client. CI builds against macOS arm64, macOS x86_64, and
Ubuntu 22.04.

### Linux

```bash
sudo apt-get install build-essential cmake pkg-config \
    libssl-dev libcurl4-openssl-dev libedit-dev
```

### macOS

```bash
brew install cmake openssl@3 libedit
# point CMake at brew's openssl when configuring:
cmake -B build -DCMAKE_BUILD_TYPE=Debug \
    -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
```

## Running

The dev binary lands at `build/arbiter`.

```bash
build/arbiter --init              # generate auth token + starter agents
build/arbiter                     # interactive REPL
build/arbiter --api --port 8080   # HTTP+SSE server
build/arbiter --send <agent> "msg"  # one-shot
```

You'll need at least one provider key — `ANTHROPIC_API_KEY` or
`OPENAI_API_KEY` in the environment, or saved at
`~/.arbiter/api_key` / `~/.arbiter/openai_api_key`.

## Project layout

```
src/                    runtime, REPL, HTTP server
include/                public headers (mirror src/)
agents/                 example agent constitutions (json)
tests/                  unit tests + PTY-based line-editor tests
docs/api/               HTTP API reference (concept + endpoint pages)
third_party/doctest/    vendored test framework
.github/workflows/      CI, release, homebrew bump
```

Source files use a long-form-comment convention: explain *why*, not
*what*. New code that adds non-obvious behavior, hidden constraints, or
workarounds for specific bugs should carry a comment that survives a
future reader cold-reading the file.

## Tests

```bash
ctest --test-dir build --output-on-failure       # all suites
ctest --test-dir build -R unit_artifacts -V      # just one suite, verbose
```

Suites:

- `unit_*` — focused unit tests (commands, api_client, stream_filter,
  artifacts, memory_entries, tenant_agents, mcp, constitution).
- `line_editor` — PTY integration tests that drive the real `arbiter`
  binary through a pseudo-terminal. Slower (~12s); kept narrow to real
  input-handling behavior, not visual chrome.

PRs that touch `src/commands.cpp` or `src/tenant_store.cpp` should run
the relevant unit suites locally before pushing — those exercise the
security-sensitive surface.

CI runs everything on the same matrix as releases. A red CI is a
non-starter for merging.

## Pull requests

- One concern per PR. Refactors and feature additions should be
  separate.
- Keep the working tree green: build clean and tests pass on every
  commit, not just the tip. Use `git rebase -i` to fix up before push.
- Commit messages: imperative subject under 72 chars; body explains
  motivation when the diff isn't self-evident.
- Public-API changes (HTTP, SSE event shape, slash-command vocabulary,
  CLI flags) need a `CHANGELOG.md` entry under `[Unreleased]` and a
  note in the PR body about whether the change is backward-compatible.
- New code without comments explaining *why* it exists is likely to
  get review notes.

## What's in scope

Bug fixes, performance improvements, new agent capabilities, new tool
commands (with safety review), HTTP API additions, doc fixes, test
additions. Anything that makes the runtime more reliable or easier to
deploy.

## What's probably not in scope

- Major architectural rewrites without prior discussion in an issue.
- New billing primitives in the runtime — billing belongs in the
  external billing service the runtime calls out to, not here.
- New unsandboxed surfaces. `/exec` is the only unsandboxed tool, and
  any analog has to start out gated behind an explicit operator opt-in.
- Cosmetic-only changes to the welcome card / chrome. Those are not
  tested for a reason.

When in doubt, open an issue first. Faster than rebasing a 2000-line
PR after a "we don't actually want this" review.

## Reporting security issues

Don't open public issues. See [`SECURITY.md`](SECURITY.md) for the
private disclosure path.

## License

By contributing, you agree that your contributions will be licensed
under the [Apache License 2.0](LICENSE), the same license as the
project.
