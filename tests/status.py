#!/usr/bin/env python3
"""Audit FEATURES.md without mistaking declared labels for verified parity.

This tool deliberately does not produce a single completion percentage.
Inventory references may name a test case as `path/to/test:case-id`; those
references are checked against the test source, while suite-only references are
reported as less precise evidence.
"""
from __future__ import annotations

import collections
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
FEATURES = ROOT / "FEATURES.md"
EXPECTED = {"CLI options": 131, "Actions": 132, "Events": 13}
ID_PREFIX = {"CLI options": "OPT", "Actions": "ACT", "Events": "EVT"}
VALID_STATUSES = {"verified", "implemented", "implemented but unverified", "unstarted", "failing", "missing", "blocked", "n/a"}
TABLE_ROW = re.compile(r"^\|\s*([A-Z]{3}-\d{3})\s*\|(.+?)\|\s*(verified|implemented but unverified|implemented|unstarted|failing|missing|blocked|n/a)\s*\|\s*$")


def parse_inventory(text: str):
    section = None
    rows = collections.defaultdict(list)
    errors = []
    for line_no, line in enumerate(text.splitlines(), 1):
        if line.startswith("## "):
            title = line[3:].strip()
            section = title if title in EXPECTED else None
            continue
        if section is None or not line.startswith("|"):
            continue
        m = TABLE_ROW.match(line)
        if not m:
            if re.match(r"^\|\s*(OPT|ACT|EVT)-\d{3}\s*\|", line):
                errors.append(f"line {line_no}: malformed inventory row")
            continue
        feature_id, body, status = m.groups()
        # Keep the row's option/action name and verification column for audit.
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        rows[section].append({
            "id": feature_id,
            "body": body,
            "status": status,
            "line": line_no,
            "cells": cells,
        })
        if status not in VALID_STATUSES:
            errors.append(f"line {line_no}: invalid status {status}")
    return rows, errors


def main() -> int:
    text = FEATURES.read_text(encoding="utf-8")
    rows, errors = parse_inventory(text)
    print("cfzf compatibility status audit")
    print("Reference: fzf v0.74.3 @ 15f64c492a08f0840b81540c7d1de35737448086")
    print("")

    incomplete = False
    declared_verified = 0
    total_rows = 0
    missing_ids_total = 0
    named_case_refs = 0
    for section, expected in EXPECTED.items():
        entries = rows[section]
        ids = [entry["id"] for entry in entries]
        counts = collections.Counter(entry["status"] for entry in entries)
        prefix = ID_PREFIX[section]
        expected_ids = {f"{prefix}-{n:03d}" for n in range(1, expected + 1)}
        actual_ids = set(ids)
        missing_ids = sorted(expected_ids - actual_ids)
        unexpected_ids = sorted(actual_ids - expected_ids)
        duplicate_ids = sorted(k for k, n in collections.Counter(ids).items() if n > 1)
        total_rows += len(entries)
        declared_verified += counts["verified"]
        missing_ids_total += len(missing_ids)
        complete = not missing_ids and not unexpected_ids and not duplicate_ids
        if not complete:
            incomplete = True
        print(f"{section}: {len(entries)}/{expected} inventory rows; " +
              ", ".join(f"{s}={counts[s]}" for s in
                        ("verified", "implemented", "implemented but unverified", "unstarted", "failing", "missing", "blocked", "n/a")))
        if missing_ids:
            print("  MISSING IDS: " + ", ".join(missing_ids))
        if unexpected_ids:
            print("  UNEXPECTED IDS: " + ", ".join(unexpected_ids))
        if duplicate_ids:
            print("  DUPLICATE IDS: " + ", ".join(duplicate_ids))

        # A status is not considered evidenced merely because a test is referenced.
        # Named case references improve traceability but do not establish outcomes.
        for entry in entries:
            cells = entry["cells"]
            if len(cells) < 5:
                errors.append(f"line {entry['line']}: expected 5 table columns")
                continue
            verification = cells[3]
            if entry["status"] == "verified" and verification in ("-", ""):
                errors.append(f"line {entry['line']}: verified row has no test reference")
            if verification not in ("-", ""):
                for reference in verification.split(","):
                    reference = reference.strip()
                    if not reference:
                        continue
                    test_file, separator, case_id = reference.partition(":")
                    test_path = ROOT / test_file
                    if not test_path.is_file():
                        errors.append(
                            f"line {entry['line']}: referenced test does not exist: {test_file}"
                        )
                        continue
                    if separator:
                        named_case_refs += 1
                        test_source = test_path.read_text(encoding="utf-8")
                        if not case_id or case_id not in test_source:
                            errors.append(
                                f"line {entry['line']}: referenced test case does not exist: {reference}"
                            )

    print("")
    print(f"Inventory rows present: {total_rows}")
    print(f"Declared verified rows (not independently audited): {declared_verified}")
    print(f"Missing canonical inventory IDs: {missing_ids_total}")
    print(f"Named test-case references: {named_case_refs}")
    print("Feature completion percentage: NOT CALCULABLE")
    print("Reason: named test-case references establish traceability, not observed")
    print("test outcomes. A passing suite does not prove every feature marked")
    print("verified unless the referenced case actually runs and passes.")
    if errors:
        print("")
        print("AUDIT ERRORS:")
        for error in errors:
            print(f"  {error}")
        return 1
    if incomplete:
        print("")
        print("AUDIT WARNING: inventory is incomplete; do not use declared counts as a")
        print("canonical feature denominator. Resolve the missing/duplicate IDs.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
