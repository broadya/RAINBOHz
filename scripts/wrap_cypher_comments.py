#!/usr/bin/env python3
"""
100% LLM-generated code

wrap_cypher_comments.py

Refills the full-line `//` comments in Cypher DDL files so that each line
ends at or before a fixed column (64 by default, counted across the whole
line including indent and `//`). Words are never hyphenated or broken: a
word that crosses the limit is kept whole and the line runs over.

Only comment text changes. Rules, so that DDL meaning is never touched:

  * Only lines whose first non-space characters are `//` are rewritten.
    Every other line passes through byte for byte.
  * Trailing comments (code then `//` on the same line) are left alone.
  * Everything inside `/* ... */` passes through untouched.
  * `//` or `/*` inside a quoted string or backtick identifier is not a
    comment.

Consecutive full-line comments with the same indent form a paragraph, which
is refilled. A paragraph ends at:

  * an empty comment line (`//`), a non-comment line, or a change of indent;
  * a rule line -- one punctuation character repeated, e.g. `// ======`;
  * a preformatted line, kept verbatim: text containing `->`, `<-` or `|`,
    text starting with extra spaces, or a run of two or more spaces between
    words (tables, graph paths, aligned columns);
  * a line ending in `:`;
  * a deliberate break: a line with no closing punctuation, followed by a
    line not starting with a lowercase letter, where the previous line was
    already over the limit or the next word would have fitted on it (a
    heading, or one entry of a list of values).

A line of the form `NAME = ...` (e.g. `0 = directly in front`) always starts
a new paragraph.

A list item (`1.`, `1)`, `2a.`, `a)`, `-`, `*`) always starts a new
paragraph. It is wrapped with a hanging indent under its text, and only
lines carrying that hanging indent continue it; an unindented line after a
list item starts a fresh paragraph. That keeps a heading such as
`1) Be a direct physical representation` apart from the prose below it, and
makes a second run a no-op.

Usage:
    python wrap_cypher_comments.py schema.cypher [more.cypher ...]

    # Report what would change, write nothing, exit 1 if anything would:
    python wrap_cypher_comments.py --check schema.cypher

    # A different limit:
    python wrap_cypher_comments.py --width 72 schema.cypher
"""

from __future__ import annotations

import argparse
import difflib
import re
import sys
import textwrap

DEFAULT_WIDTH = 64

_RULE = re.compile(r"([^\w\s])\1{2,}")
_LIST_ITEM = re.compile(r"(\d+[a-z]?[.)]|[a-z][.)]|[-*])\s+\S")
_ALIGNED = re.compile(r"\S\s{2,}\S")
_DEFINITION = re.compile(r"[^\s=]+ = ")


def _scan_code(line: str, in_block: bool) -> tuple[int | None, bool]:
    """Scan one line of code for comments, honouring strings.

    Returns (column of a `//` comment outside strings or None, whether a
    `/* ... */` block is still open at the end of the line).
    """
    i = 0
    quote = None
    n = len(line)
    while i < n:
        c = line[i]
        if in_block:
            if line.startswith("*/", i):
                in_block = False
                i += 2
                continue
        elif quote:
            if c == "\\" and quote != "`":
                i += 2
                continue
            if c == quote:
                quote = None
        elif c in "'\"`":
            quote = c
        elif line.startswith("//", i):
            return i, False
        elif line.startswith("/*", i):
            in_block = True
            i += 2
            continue
        i += 1
    return None, in_block


class _Paragraph:
    def __init__(self, indent: str, first: str, hang: int) -> None:
        self.indent = indent
        self.parts = [first]
        self.hang = hang  # 0 for prose; text column of a list item otherwise

    def render(self, width: int) -> list[str]:
        prefix = self.indent + "// "
        lines = textwrap.wrap(
            " ".join(p.strip() for p in self.parts),
            width=max(width - len(prefix), 1),
            subsequent_indent=" " * self.hang,
            break_long_words=False,
            break_on_hyphens=False,
        )
        return [prefix + text for text in lines]


def wrap_text(source: str, width: int = DEFAULT_WIDTH) -> str:
    out: list[str] = []
    para: _Paragraph | None = None

    def flush() -> None:
        nonlocal para
        if para is not None:
            out.extend(para.render(width))
            para = None

    in_block = False
    lines = source.split("\n")
    for line in lines:
        stripped = line.lstrip()
        is_comment = not in_block and stripped.startswith("//")
        if not is_comment:
            flush()
            _, in_block = _scan_code(line, in_block)
            out.append(line)
            continue

        indent = line[: len(line) - len(stripped)]
        text = stripped[2:]
        body = text[1:] if text.startswith(" ") else text
        body = body.rstrip()

        if para is not None and para.indent != indent:
            flush()

        # Continuation of a list item: indented exactly to its text column.
        if para is not None and para.hang:
            lead = len(body) - len(body.lstrip(" "))
            if lead == para.hang and body.strip():
                rest = body.strip()
                if not (_RULE.fullmatch(rest) or _is_preformatted(rest)):
                    para.parts.append(rest)
                    if rest.endswith(":"):
                        flush()
                    continue

        if not body.strip():
            flush()
            out.append(indent + "//")
        elif _RULE.fullmatch(body.strip()) or _is_preformatted(body):
            flush()
            out.append(line.rstrip())
        elif m := _LIST_ITEM.match(body):
            flush()
            para = _Paragraph(indent, body, hang=m.end() - 1)
            if body.endswith(":"):
                flush()
        else:
            if (
                para is None
                or para.hang
                or _DEFINITION.match(body)
                or _deliberate_break(para, body, width)
            ):
                flush()
                para = _Paragraph(indent, body, hang=0)
            else:
                para.parts.append(body)
            if body.endswith(":"):
                flush()
    flush()
    return "\n".join(out)


def _deliberate_break(para: _Paragraph, body: str, width: int) -> bool:
    """True if the author ended the previous line on purpose.

    The previous line has no closing punctuation, this line does not start
    with a lowercase letter, and either the previous line was already over
    the limit or this line's first word would have fitted on it -- so the
    break cannot have come from wrapping. Lines this tool wraps never
    satisfy the last condition, which keeps a second run a no-op.
    """
    last = para.parts[-1].rstrip()
    if last.endswith((".", ",", ";", ":", "!", "?")) or body[0].islower():
        return False
    used = len(para.indent) + len("// ") + len(last)
    return used > width or used + 1 + len(body.split()[0]) <= width


def _is_preformatted(body: str) -> bool:
    return (
        body.startswith((" ", "\t"))
        or "->" in body
        or "<-" in body
        or "|" in body
        or _ALIGNED.search(body) is not None
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Refill full-line // comments in Cypher DDL files."
    )
    parser.add_argument("files", nargs="+", help="Cypher files to process")
    parser.add_argument(
        "--width",
        type=int,
        default=DEFAULT_WIDTH,
        help=f"line length limit (default {DEFAULT_WIDTH})",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="write nothing; print a diff and exit 1 if any file would change",
    )
    args = parser.parse_args(argv)

    changed = []
    for path in args.files:
        with open(path, encoding="utf-8", newline="") as f:
            before = f.read()
        after = wrap_text(before, args.width)
        if after == before:
            continue
        changed.append(path)
        if args.check:
            sys.stdout.writelines(
                difflib.unified_diff(
                    before.splitlines(keepends=True),
                    after.splitlines(keepends=True),
                    fromfile=path,
                    tofile=path + " (wrapped)",
                )
            )
        else:
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(after)
            print(f"wrapped: {path}")

    if args.check and changed:
        print(
            f"\n{len(changed)} file(s) would change: {', '.join(changed)}",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
