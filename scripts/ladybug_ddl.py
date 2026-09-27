#!/usr/bin/env python3
"""
100% LLM-generated code

ladybug_ddl.py

Shared machinery for submitting LadybugDB DDL to a real database: statement
splitting, execution, and error detection. Imported by both the schema
validator and the database loader so that the subtle parts live in one place.

Why statements are submitted one at a time
------------------------------------------
Three behaviours of the `lbug` CLI (established by experiment against 0.19.1)
make it unsafe as a batch loader:

1.  Failures are reported on **stdout**, not stderr.
2.  It exits **0** even when statements fail.
3.  Fed a whole schema file on stdin, it **silently skips** some statements --
    they neither execute nor report an error. The same statement succeeds when
    submitted on its own.

(3) is the dangerous one: it means a schema can half-load with no diagnostic
whatsoever. So every statement is submitted in its own `lbug` process against a
persistent database, and failure is detected by scanning output for error
markers rather than by checking the exit code.
"""

from __future__ import annotations

import re
import shutil
import subprocess
from dataclasses import dataclass

# `lbug` emits a cursor-position-report escape whenever it reads a prompt.
ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")

# Errors are printed as plain text on stdout, e.g.
#   Error: Binder exception: Table SoundSource does not exist.
ERROR_RE = re.compile(r"^\s*(Error|Exception|Parser exception)\b.*", re.MULTILINE)

# Re-applying a schema to a populated database. Benign when loading
# idempotently; a genuine duplicate declaration when validating a single file.
ALREADY_EXISTS_RE = re.compile(r"already exists in catalog", re.IGNORECASE)

# Enough of a statement to identify it in a report.
DDL_HEAD_RE = re.compile(
    r"\b(CREATE\s+(?:NODE|REL)\s+TABLE(?:\s+GROUP)?)\s+(?:IF\s+NOT\s+EXISTS\s+)?"
    r"[`\"]?(\w+)",
    re.IGNORECASE,
)


@dataclass
class Statement:
    text: str        # comment-stripped statement, no trailing semicolon
    start_line: int  # 1-based, first line of executable text
    end_line: int    # 1-based, line carrying the terminating semicolon

    @property
    def label(self) -> str:
        """A short identifier for the statement, for reporting."""
        match = DDL_HEAD_RE.search(self.text)
        if match:
            kind = " ".join(match.group(1).split()).upper()
            return f"{kind} {match.group(2)}"
        collapsed = " ".join(self.text.split())
        return collapsed[:60] + ("..." if len(collapsed) > 60 else "")

    @property
    def lines(self) -> str:
        if self.start_line == self.end_line:
            return f"line {self.start_line}"
        return f"lines {self.start_line}-{self.end_line}"


@dataclass
class Result:
    ok: bool
    diagnostic: str = ""
    already_exists: bool = False


def split_statements(source: str) -> list[Statement]:
    """Split Cypher DDL into statements, tracking source line numbers.

    Comments are stripped, and semicolons inside string literals or quoted
    identifiers are not treated as terminators. A missing semicolon therefore
    merges two statements into one -- which is exactly what LadybugDB itself
    sees, so the resulting error is attributed to the correct line range.
    """
    statements: list[Statement] = []
    buffer: list[str] = []
    line = 1
    start_line: int | None = None

    index = 0
    length = len(source)
    while index < length:
        char = source[index]
        pair = source[index : index + 2]

        if pair == "//":
            while index < length and source[index] != "\n":
                index += 1
            continue

        if pair == "/*":
            index += 2
            while index < length and source[index : index + 2] != "*/":
                if source[index] == "\n":
                    line += 1
                index += 1
            index += 2
            continue

        if char in "'\"`":
            quote = char
            buffer.append(char)
            index += 1
            while index < length:
                if source[index] == "\\" and quote != "`":
                    buffer.append(source[index : index + 2])
                    index += 2
                    continue
                buffer.append(source[index])
                if source[index] == "\n":
                    line += 1
                if source[index] == quote:
                    index += 1
                    break
                index += 1
            continue

        if char == ";":
            text = "".join(buffer).strip()
            if text:
                statements.append(
                    Statement(text, start_line if start_line else line, line)
                )
            buffer = []
            start_line = None
            index += 1
            continue

        if not char.isspace() and start_line is None:
            start_line = line

        buffer.append(char)
        if char == "\n":
            line += 1
        index += 1

    trailing = "".join(buffer).strip()
    if trailing:
        statements.append(Statement(trailing, start_line if start_line else line, line))

    return statements


def clean(output: str) -> str:
    """Strip terminal escape sequences from lbug output."""
    return ANSI_RE.sub("", output)


def find_lbug(candidate: str) -> str | None:
    """Resolve the lbug CLI, or None if it is not installed."""
    return shutil.which(candidate)


def run_statement(lbug: str, db_path: str, statement: str) -> Result:
    """Execute one statement against db_path in its own lbug process."""
    try:
        completed = subprocess.run(
            [lbug, db_path, "--no_stats", "--no_progress_bar"],
            input=statement + ";\n",
            capture_output=True,
            text=True,
            timeout=120,
        )
    except subprocess.TimeoutExpired:
        return Result(False, "timed out after 120s")

    combined = clean(completed.stdout) + clean(completed.stderr)
    errors = [m.group(0).strip() for m in ERROR_RE.finditer(combined)]
    if errors:
        joined = "\n".join(errors)
        return Result(False, joined, already_exists=bool(ALREADY_EXISTS_RE.search(joined)))

    # The exit code is not a reliable failure signal, but a crash still matters.
    if completed.returncode != 0:
        return Result(False, f"lbug exited {completed.returncode}")

    return Result(True)


def show_tables(lbug: str, db_path: str) -> list[tuple[str, str]]:
    """Return sorted (name, type) for every table present in the database."""
    completed = subprocess.run(
        [lbug, db_path, "--no_stats", "--no_progress_bar", "--mode", "csv"],
        input="CALL SHOW_TABLES() RETURN name, type;\n",
        capture_output=True,
        text=True,
        timeout=120,
    )
    tables: list[tuple[str, str]] = []
    for row in clean(completed.stdout).splitlines():
        parts = [cell.strip().strip('"') for cell in row.split(",")]
        if len(parts) == 2 and parts[1] in ("NODE", "REL"):
            tables.append((parts[0], parts[1]))
    return sorted(tables)
