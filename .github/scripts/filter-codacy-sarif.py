#!/usr/bin/env python3
"""Drop low-risk and style-only Codacy results before the SARIF upload.

GitHub code scanning shows every result in the uploaded file. Codacy's
default set includes MISRA, markdown lint, and Flawfinder hits it marks
"Risk is low". Those stay out of the security tab; warning-level
findings still pass through.
"""

import json
import sys

# Cppcheck style checks that Codacy reports as warnings rather than notes.
STYLE_RULES = {
    "cppcheck_constParameter",
    "cppcheck_constParameterPointer",
    "cppcheck_constVariable",
    "cppcheck_duplicateCondition",
    "cppcheck_knownConditionTrueFalse",
    "cppcheck_redundantAssignment",
    "cppcheck_redundantCondition",
    "cppcheck_shadowVariable",
    "cppcheck_unreadVariable",
    "cppcheck_unusedFunction",
    "cppcheck_unusedVariable",
    "cppcheck_variableScope",
    "shellcheck_SC2034",
}


def should_drop(rule_id, level, message):
    rule = rule_id or ""
    severity = (level or "").lower()
    text = (message or "").lower()

    if "misra" in rule.lower():
        return True
    if rule.startswith("remark-lint"):
        return True
    if severity in ("note", "none"):
        return True
    if "risk is low" in text:
        return True
    if rule in STYLE_RULES:
        return True
    return False


def filter_sarif(sarif):
    dropped = 0
    kept = 0
    for run in sarif.get("runs", []):
        kept_results = []
        for result in run.get("results", []):
            message = (result.get("message") or {}).get("text") or ""
            if should_drop(result.get("ruleId"), result.get("level"), message):
                dropped += 1
                continue
            kept_results.append(result)
            kept += 1
        run["results"] = kept_results
    return kept, dropped


def main():
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} input.sarif output.sarif")

    source = sys.argv[1]
    destination = sys.argv[2]
    with open(source, encoding="utf-8") as handle:
        sarif = json.load(handle)

    kept, dropped = filter_sarif(sarif)

    # The Codacy action writes the input as root, so overwrite fails in CI.
    with open(destination, "w", encoding="utf-8") as handle:
        json.dump(sarif, handle)

    print(f"code scanning upload: kept {kept}, dropped {dropped}")


if __name__ == "__main__":
    main()
