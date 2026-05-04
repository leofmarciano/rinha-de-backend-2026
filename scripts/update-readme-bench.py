#!/usr/bin/env python3
"""Embed the latest k6 results.json into the README between auto-update markers."""
from __future__ import annotations

import datetime as dt
import json
import os
import pathlib
import subprocess
import sys

RESULTS = pathlib.Path("test/results.json")
README = pathlib.Path("README.md")
START = "<!-- BENCH-RESULTS:START -->"
END = "<!-- BENCH-RESULTS:END -->"


def short_sha() -> str:
    sha = os.environ.get("GITHUB_SHA")
    if sha:
        return sha[:7]
    return subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], text=True).strip()


def workflow_url() -> str | None:
    server = os.environ.get("GITHUB_SERVER_URL")
    repo = os.environ.get("GITHUB_REPOSITORY")
    run_id = os.environ.get("GITHUB_RUN_ID")
    if server and repo and run_id:
        return f"{server}/{repo}/actions/runs/{run_id}"
    return None


def render(data: dict) -> str:
    scoring = data["scoring"]
    breakdown = scoring["breakdown"]
    timestamp = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")

    header = f"**Última execução:** commit `{short_sha()}` em `{timestamp}`."
    run_link = workflow_url()
    if run_link:
        header += f" [Workflow run]({run_link})."

    table = (
        "| Métrica | Valor |\n"
        "|---------|-------|\n"
        f"| p99 latência | `{data['p99']}` |\n"
        f"| Score final | `{scoring['final_score']}` |\n"
        f"| Score p99 | `{scoring['p99_score']['value']}` |\n"
        f"| Score detecção | `{scoring['detection_score']['value']}` |\n"
        f"| ε (erro ponderado) | `{scoring['error_rate_epsilon']}` |\n"
        f"| TP / TN | `{breakdown['true_positive_detections']}` / "
        f"`{breakdown['true_negative_detections']}` |\n"
        f"| FP / FN | `{breakdown['false_positive_detections']}` / "
        f"`{breakdown['false_negative_detections']}` |\n"
        f"| HTTP errors | `{breakdown['http_errors']}` |\n"
        f"| Failure rate | `{scoring['failure_rate']}` |\n"
    )

    raw = json.dumps(data, indent=2, ensure_ascii=False)
    details = (
        "<details><summary>Resultado bruto (<code>test/results.json</code>)</summary>\n\n"
        "```json\n"
        f"{raw}\n"
        "```\n\n"
        "</details>"
    )

    return f"{header}\n\n{table}\n{details}"


def main() -> int:
    if not RESULTS.exists():
        print(f"missing {RESULTS}", file=sys.stderr)
        return 1
    data = json.loads(RESULTS.read_text())

    text = README.read_text()
    if START not in text or END not in text:
        print(f"README missing {START} / {END} markers", file=sys.stderr)
        return 1

    head = text.split(START, 1)[0]
    tail = text.split(END, 1)[1]
    block = render(data)
    new_text = f"{head}{START}\n{block}\n{END}{tail}"

    if new_text == text:
        print("README already up to date")
        return 0

    README.write_text(new_text)
    print(f"updated {README}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
