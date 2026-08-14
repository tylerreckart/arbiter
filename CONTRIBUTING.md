# Contributing to arbiter

Thanks for taking a look. Arbiter is pre-1.0 and the surface — slash
commands, agent constitutions, SSE event shapes, the HTTP API — is still
moving. PRs are welcome; please read this first.

## Quick start

```bash
git clone https://github.com/tylerreckart/arbiter.git
cd arbiter-core
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The build needs OpenSSL, libcurl, SQLite3, and a C++20 compiler.
CI builds against macOS arm64, macOS x86_64, and Ubuntu 22.04.

### Linux

```bash
sudo apt-get install build-essential cmake pkg-config \
    libssl-dev libcurl4-openssl-dev libsqlite3-dev
```

### macOS

```bash
brew install cmake openssl@3
# point CMake at brew's openssl when configuring:
cmake -B build -DCMAKE_BUILD_TYPE=Debug \
    -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
```

Use the system libcurl (default). Preferring Homebrew curl embeds
`/opt/homebrew/opt/curl/...` into the binary and breaks machines that
don't have that formula; only pass `-DCURL_ROOT="$(brew --prefix curl)"`
if you intentionally want that for local development.

## Running

The dev binary lands at `build/arbiter`.

```bash
build/arbiter --init              # generate auth token + starter agents
build/arbiter                     # interactive REPL
build/arbiter --api --port 8080   # HTTP+SSE server
build/arbiter --send <agent> "msg"  # one-shot
```

You'll need an OpenRouter key for hosted models — `OPENROUTER_API_KEY` in the
environment or saved at `~/.arbiter/openrouter_api_key`. Local models can use
Ollama with `ollama/<model>` ids.

## Project layout

```
src/                    runtime, REPL, HTTP server
include/                public headers (mirror src/)
agents/                 example agent constitutions (json)
tests/                  unit tests + PTY-based line-editor tests
docs/api/               HTTP API reference (concept + endpoint pages)
third_party/doctest/    vendored test framework
.github/workflows/      CI and release pipelines
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
- Do not bump `CMakeLists.txt`'s `project(... VERSION ...)` in a
  feature PR.  Merges to `main` cut the next patch automatically
  (see Versioning below).

## Versioning and releases

Arbiter is pre-1.0 and treats a merge to `main` as a patch release
(`0.12.2` → `0.12.3`). `main` requires pull requests, so `version.yml`
opens a `chore/release-vX.Y.Z` PR (CMakeLists `INDEX_VERSION`,
`CHANGELOG.md` `[Unreleased]` cut, site/docs pins) rather than
pushing to `main`. Squash-merging that PR tags `vX.Y.Z` and
dispatches the binary release.

Leave the version number alone in ordinary PRs. Notes go under
`[Unreleased]`; the bump job cuts them on merge.

The Actions repo setting **Allow GitHub Actions to create and approve
pull requests** must be on, or the bot cannot open the release PR.
If required reviews block auto-merge, merge `chore/release-v*` by
hand (keep the `chore(release): vX.Y.Z` title).

Minor and major are never automatic.  To ship one:

- **Minor, major, or a specific X.Y.Z** — Actions → Version bump →
  Run workflow.  Pick `minor` / `major`, or type an explicit version.
- **Version in the PR itself** — change `project(arbiter VERSION …)`
  (and optionally cut the changelog heading).  On merge the workflow
  tags that version instead of auto-patching.  Use this for a
  prepare-release PR.
- **Skip a release** — put `[skip version]` in the squash title or
  merge commit message.  Tag later by hand or via workflow_dispatch.
- **Tag only** — `git tag vX.Y.Z && git push origin vX.Y.Z` still
  runs `release.yml` directly.  Keep CMakeLists in sync with the tag
  or the binary will advertise a stale `INDEX_VERSION`.

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
