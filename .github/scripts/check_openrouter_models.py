#!/usr/bin/env python3
"""Check shipped model ids against OpenRouter's public model list.

No API key. Fails when the default master model, the wizard's first
pick, a catalog id, or a starter-agent model is absent. The hyphenated
Haiku id is accepted while OpenRouter still serves the dotted alias.
"""

from __future__ import annotations

import json
import pathlib
import re
import sys
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[2]
MODELS_URL = "https://openrouter.ai/api/v1/models"
# OpenRouter accepts anthropic/claude-haiku-4-5 as an alias of the dotted id.
ALIAS_OF = {
    "anthropic/claude-haiku-4-5": "anthropic/claude-haiku-4.5",
}


def fetch_ids() -> set[str]:
    last = None
    for _ in range(3):
        try:
            with urllib.request.urlopen(MODELS_URL, timeout=30) as resp:
                payload = json.load(resp)
            return {item["id"] for item in payload.get("data", [])}
        except Exception as exc:  # noqa: BLE001 — retry then report
            last = exc
    sys.exit(f"could not fetch {MODELS_URL}: {last}")


def quoted_after(path: pathlib.Path, pattern: str) -> str:
    text = path.read_text(encoding="utf-8")
    match = re.search(pattern, text)
    if not match:
        sys.exit(f"could not find {pattern} in {path}")
    return match.group(1)


def catalog_ids() -> list[str]:
    text = (ROOT / "src/model_catalog.cpp").read_text(encoding="utf-8")
    return [
        item
        for item in re.findall(r'\{\s*"([^"]+)"\s*,\s*"openrouter"', text)
        if "/" in item
    ]


def agent_ids() -> list[tuple[str, str]]:
    found = []
    for path in sorted((ROOT / "agents").glob("*.json")):
        data = json.loads(path.read_text(encoding="utf-8"))
        if data.get("model"):
            found.append((str(path.relative_to(ROOT)), data["model"]))
        advisor = data.get("advisor")
        if isinstance(advisor, dict) and advisor.get("model"):
            found.append((str(path.relative_to(ROOT)) + " advisor", advisor["model"]))
    return found


def known(model_id: str, live: set[str]) -> bool:
    if model_id in live:
        return True
    alias = ALIAS_OF.get(model_id)
    return alias is not None and alias in live


def main() -> None:
    live = fetch_ids()
    if not live:
        sys.exit("OpenRouter model list was empty")

    default = quoted_after(
        ROOT / "src/orchestrator.cpp",
        r'keys\.count\("openrouter"\)\)\s*return\s+"([^"]+)"',
    )
    wizard = quoted_after(
        ROOT / "src/cli_helpers.cpp",
        r'kRecommendedIds\[\]\s*=\s*\{\s*"([^"]+)"',
    )

    missing = []
    for label, model_id in [("default master model", default), ("wizard pick", wizard)]:
        if not known(model_id, live):
            missing.append(f"{label}: {model_id}")
    for model_id in catalog_ids():
        if not known(model_id, live):
            missing.append(f"model_catalog.cpp: {model_id}")
    for label, model_id in agent_ids():
        if not known(model_id, live):
            missing.append(f"{label}: {model_id}")

    if missing:
        sys.exit("model ids missing from OpenRouter /api/v1/models:\n  " + "\n  ".join(missing))
    print(f"OpenRouter model ids ok ({len(live)} published); default {default}")


if __name__ == "__main__":
    main()
