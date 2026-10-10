#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""
100% LLM-generated code

ladybug_schema_to_mermaid.py

Converts one or more LadybugDB DDL schema files into a Mermaid class diagram.

REL tables are rendered as association classes sitting between the NODE table
classes they connect, following RDBMS junction-table conventions.  Connections
are bidirectional (plain solid lines); cardinality markers follow the
multiplicity declared on the REL table.

Usage:
    python ladybug_schema_to_mermaid.py schema.cypher [more.cypher ...]

    # Wrap in a markdown code fence for direct embedding:
    python ladybug_schema_to_mermaid.py schema.cypher --markdown
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass, field
from typing import Optional


# ---------------------------------------------------------------------------
# Data model
# ---------------------------------------------------------------------------

@dataclass
class TypeField:
    name: str
    type_ref: "TypeRef"


@dataclass
class TypeRef:
    kind: str
    name: Optional[str] = None
    children: list[TypeField] = field(default_factory=list)
    element_type: Optional["TypeRef"] = None
    key_type: Optional["TypeRef"] = None
    value_type: Optional["TypeRef"] = None
    array_size: Optional[str] = None

    def display(self) -> str:
        if self.kind == "SCALAR":
            return self.name or "UNKNOWN"
        if self.kind == "LIST":
            base = self.element_type.display() if self.element_type else "UNKNOWN"
            return f"{base}[]"
        if self.kind == "ARRAY":
            base = self.element_type.display() if self.element_type else "UNKNOWN"
            size = self.array_size or ""
            return f"{base}[{size}]"
        return self.kind

    def is_complex(self) -> bool:
        return self.kind in {"LIST", "ARRAY", "STRUCT", "UNION", "MAP"}


@dataclass
class Property:
    name: str
    prop_type: TypeRef
    default: Optional[str] = None
    is_pk: bool = False


@dataclass
class NodeTable:
    name: str
    properties: list[Property] = field(default_factory=list)


@dataclass
class FromToPair:
    from_table: str
    to_table: str


@dataclass
class RelTable:
    name: str
    pairs: list[FromToPair] = field(default_factory=list)
    multiplicity: str = "MANY_MANY"
    properties: list[Property] = field(default_factory=list)


# ---------------------------------------------------------------------------
# Parsing helpers
# ---------------------------------------------------------------------------

_MULTIPLICITY_KEYWORDS = frozenset({"ONE_ONE", "ONE_MANY", "MANY_ONE", "MANY_MANY"})

# (from_end_label, to_end_label) for each multiplicity
_CARDINALITY: dict[str, tuple[str, str]] = {
    "MANY_MANY": ("*", "*"),
    "MANY_ONE":  ("*", "1"),
    "ONE_MANY":  ("1", "*"),
    "ONE_ONE":   ("1", "1"),
}


def _strip_comments(text: str) -> str:
    text = re.sub(r"//[^\n]*", "", text)
    return re.sub(r"--[^\n]*", "", text)


def _extract_body(text: str, after_open_paren: int) -> tuple[str, int]:
    """
    Walk forward from *after_open_paren* (the character immediately following
    an opening '(') and return (body, end_pos) where body is the text up to
    the matching closing ')' and end_pos is the index just past that ')'.
    Handles arbitrary nesting depth.
    """
    depth = 1
    i = after_open_paren
    while i < len(text) and depth > 0:
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
        i += 1
    return text[after_open_paren : i - 1], i


def _split_top_level_commas(text: str) -> list[str]:
    """
    Split *text* on commas that are not enclosed in parentheses, brackets, or
    angle brackets.
    Preserves complex types such as UNION(a INT64, b STRING) as a single item.
    """
    parts: list[str] = []
    depth = 0
    buf: list[str] = []
    for ch in text:
        if ch in "([<":
            depth += 1
            buf.append(ch)
        elif ch in ")]>":
            depth = max(0, depth - 1)
            buf.append(ch)
        elif ch == "," and depth == 0:
            item = "".join(buf).strip()
            if item:
                parts.append(item)
            buf = []
        else:
            buf.append(ch)
    item = "".join(buf).strip()
    if item:
        parts.append(item)
    return parts


def _find_top_level_keyword(text: str, keyword: str) -> Optional[int]:
    depth = 0
    upper = text.upper()
    target = keyword.upper()
    i = 0
    while i < len(text):
        ch = text[i]
        if ch in "([<":
            depth += 1
        elif ch in ")]>":
            depth = max(0, depth - 1)
        elif depth == 0 and upper.startswith(target, i):
            before_ok = i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")
            end = i + len(target)
            after_ok = end == len(text) or not (text[end].isalnum() or text[end] == "_")
            if before_ok and after_ok:
                return i
        i += 1
    return None


def _strip_outer_pair(text: str, open_ch: str, close_ch: str) -> Optional[str]:
    text = text.strip()
    if not (text.startswith(open_ch) and text.endswith(close_ch)):
        return None
    depth = 0
    for i, ch in enumerate(text):
        if ch == open_ch:
            depth += 1
        elif ch == close_ch:
            depth -= 1
            if depth == 0 and i != len(text) - 1:
                return None
    return text[1:-1].strip()


def _split_field_decl(item: str) -> Optional[tuple[str, str]]:
    item = item.strip()
    m = re.match(r"(\w+)\b", item)
    if not m:
        return None
    return m.group(1), item[m.end():].strip()


def _parse_named_type_fields(body: str) -> list[TypeField]:
    fields: list[TypeField] = []
    for item in _split_top_level_commas(body):
        parsed = _split_field_decl(item)
        if parsed:
            name, type_expr = parsed
            fields.append(TypeField(name, parse_type_expr(type_expr)))
    return fields


def _parse_type_args(text: str, keyword: str, open_ch: str, close_ch: str) -> Optional[str]:
    m = re.match(rf"{keyword}\s*", text, re.IGNORECASE)
    if not m:
        return None
    suffix = text[m.end():]
    inner = _strip_outer_pair(suffix, open_ch, close_ch)
    if inner is None and keyword.upper() == "LIST" and suffix.strip().endswith(">>"):
        # Some Ladybug schemas spell LIST<STRUCT field TYPE, ... >> with an
        # extra '>' after the STRUCT body. Interpret that as the list's element
        # being the shorthand STRUCT declaration.
        inner = _strip_outer_pair(suffix.strip()[:-1], open_ch, close_ch)
    return inner


def _extract_array_suffix(text: str) -> tuple[str, Optional[str]]:
    text = text.strip()
    if not text.endswith("]"):
        return text, None
    depth = 0
    for i in range(len(text) - 1, -1, -1):
        ch = text[i]
        if ch == "]":
            depth += 1
        elif ch == "[":
            depth -= 1
            if depth == 0:
                return text[:i].strip(), text[i + 1:-1].strip()
    return text, None


def parse_type_expr(type_expr: str) -> TypeRef:
    """Parse Ladybug type syntax into a recursive TypeRef tree."""
    text = re.sub(r"\s+", " ", type_expr.strip())
    if not text:
        return TypeRef("SCALAR", "UNKNOWN")

    base, array_size = _extract_array_suffix(text)
    if array_size is not None:
        if array_size == "":
            return TypeRef("LIST", element_type=parse_type_expr(base))
        return TypeRef("ARRAY", element_type=parse_type_expr(base), array_size=array_size)

    list_inner = _parse_type_args(text, "LIST", "<", ">")
    if list_inner is not None:
        return TypeRef("LIST", element_type=parse_type_expr(list_inner))

    struct_inner = _parse_type_args(text, "STRUCT", "(", ")")
    if struct_inner is not None:
        return TypeRef("STRUCT", children=_parse_named_type_fields(struct_inner))

    # Accept Ladybug-style LIST<STRUCT field TYPE, field TYPE> without STRUCT(...).
    m = re.match(r"STRUCT\s+(.+)$", text, re.IGNORECASE)
    if m:
        return TypeRef("STRUCT", children=_parse_named_type_fields(m.group(1)))

    union_inner = _parse_type_args(text, "UNION", "(", ")")
    if union_inner is not None:
        return TypeRef("UNION", children=_parse_named_type_fields(union_inner))

    map_inner = _parse_type_args(text, "MAP", "(", ")")
    if map_inner is not None:
        args = _split_top_level_commas(map_inner)
        if len(args) == 2:
            return TypeRef("MAP", key_type=parse_type_expr(args[0]), value_type=parse_type_expr(args[1]))

    return TypeRef("SCALAR", text.upper())


def _parse_property_item(item: str) -> Optional[Property]:
    parsed = _split_field_decl(item)
    if not parsed:
        return None
    name, rest = parsed
    default = None
    default_pos = _find_top_level_keyword(rest, "DEFAULT")
    if default_pos is not None:
        type_expr = rest[:default_pos].strip()
        default = rest[default_pos + len("DEFAULT"):].strip() or None
    else:
        type_expr = rest.strip()
    if not type_expr:
        return None
    return Property(name=name, prop_type=parse_type_expr(type_expr), default=default)


# ---------------------------------------------------------------------------
# DDL parsers
# ---------------------------------------------------------------------------

def _parse_properties(body: str) -> list[Property]:
    props: list[Property] = []
    pk_names: set[str] = set()

    for item in _split_top_level_commas(body):
        # PRIMARY KEY (colname) — always a separate, trailing item
        pk_m = re.match(r"PRIMARY\s+KEY\s*\(\s*(\w+)\s*\)", item, re.IGNORECASE)
        if pk_m:
            pk_names.add(pk_m.group(1))
            continue

        prop = _parse_property_item(item)
        if prop:
            props.append(prop)

    for p in props:
        p.is_pk = p.name in pk_names

    return props


def _parse_rel_body(body: str) -> tuple[list[FromToPair], str, list[Property]]:
    pairs: list[FromToPair] = []
    multiplicity = "MANY_MANY"
    props: list[Property] = []

    for item in _split_top_level_commas(body):
        words = item.strip().split()
        if len(words) > 1 and words[-1].upper() in _MULTIPLICITY_KEYWORDS:
            multiplicity = words[-1].upper()
            item = item[: item.upper().rfind(words[-1].upper())].rstrip()
            if not item:
                continue

        # FROM TableA TO TableB
        ft_m = re.match(r"FROM\s+(\w+)\s+TO\s+(\w+)", item, re.IGNORECASE)
        if ft_m:
            pairs.append(FromToPair(ft_m.group(1), ft_m.group(2)))
            continue

        # Multiplicity keyword (may appear anywhere in the list)
        if item.strip().upper() in _MULTIPLICITY_KEYWORDS:
            multiplicity = item.strip().upper()
            continue

        # Property on the REL table itself
        prop = _parse_property_item(item)
        if prop:
            props.append(prop)

    return pairs, multiplicity, props


def parse_schema(text: str) -> tuple[list[NodeTable], list[RelTable]]:
    text = _strip_comments(text)
    nodes: list[NodeTable] = []
    rels: list[RelTable] = []

    # Match CREATE [NODE|REL] TABLE Name ( — body extracted by depth tracking
    # to handle arbitrary nesting inside complex types.
    header_re = re.compile(
        r"CREATE\s+(NODE|REL)\s+TABLE\s+(\w+)\s*\(",
        re.IGNORECASE,
    )

    for m in header_re.finditer(text):
        table_kind = m.group(1).upper()
        table_name = m.group(2)
        body, _ = _extract_body(text, m.end())

        if table_kind == "NODE":
            nodes.append(NodeTable(
                name=table_name,
                properties=_parse_properties(body),
            ))
        else:
            pairs, mult, props = _parse_rel_body(body)
            rels.append(RelTable(
                name=table_name,
                pairs=pairs,
                multiplicity=mult,
                properties=props,
            ))

    return nodes, rels


# ---------------------------------------------------------------------------
# Mermaid rendering
# ---------------------------------------------------------------------------

def _safe_mermaid_id(*parts: str) -> str:
    raw = "_".join(p for p in parts if p)
    safe = re.sub(r"\W+", "_", raw).strip("_")
    if not safe:
        return "AnonymousType"
    if safe[0].isdigit():
        safe = f"_{safe}"
    return safe


def _field_line(prop: Property) -> str:
    pk_marker = "_pk" if prop.is_pk else ""
    return f"        +{prop.prop_type.display()} {prop.name}{pk_marker}"


def _type_field_line(field: TypeField) -> str:
    return f"        +{field.type_ref.display()} {field.name}"


def _render_complex_type(
    owner_id: str,
    prop_name: str,
    type_ref: TypeRef,
    lines: list[str],
    emitted: set[str],
) -> Optional[str]:
    """Render nested helper classes for complex property types.

    Returns the class id of the rendered helper class, or None for scalar-only
    types that do not require their own class.
    """
    if type_ref.kind == "SCALAR":
        return None

    class_id = _safe_mermaid_id(owner_id, prop_name, type_ref.kind.title())
    if class_id in emitted:
        return class_id
    emitted.add(class_id)

    lines.append(f"    class {class_id} {{")
    lines.append(f"        <<{type_ref.kind}>>")

    if type_ref.kind in {"STRUCT", "UNION"}:
        for child in type_ref.children:
            lines.append(_type_field_line(child))
        lines.append("    }")
        for child in type_ref.children:
            nested_id = _render_complex_type(class_id, child.name, child.type_ref, lines, emitted)
            if nested_id:
                lines.append(f"    {class_id} *-- {nested_id} : {child.name}")
        return class_id

    if type_ref.kind in {"LIST", "ARRAY"}:
        element_type = type_ref.element_type or TypeRef("SCALAR", "UNKNOWN")
        label = "element"
        if type_ref.kind == "ARRAY" and type_ref.array_size:
            label = f"element[{type_ref.array_size}]"
        lines.append(f"        +{element_type.display()} {label}")
        lines.append("    }")
        nested_id = _render_complex_type(class_id, "element", element_type, lines, emitted)
        if nested_id:
            lines.append(f"    {class_id} *-- {nested_id} : element")
        return class_id

    if type_ref.kind == "MAP":
        key_type = type_ref.key_type or TypeRef("SCALAR", "UNKNOWN")
        value_type = type_ref.value_type or TypeRef("SCALAR", "UNKNOWN")
        lines.append(f"        +{key_type.display()} key")
        lines.append(f"        +{value_type.display()} value")
        lines.append("    }")
        key_id = _render_complex_type(class_id, "key", key_type, lines, emitted)
        if key_id:
            lines.append(f"    {class_id} *-- {key_id} : key")
        value_id = _render_complex_type(class_id, "value", value_type, lines, emitted)
        if value_id:
            lines.append(f"    {class_id} *-- {value_id} : value")
        return class_id

    lines.append("    }")
    return class_id


def _render_property_type_details(owner_id: str, props: list[Property], lines: list[str]) -> None:
    emitted: set[str] = set()
    for prop in props:
        detail_id = _render_complex_type(owner_id, prop.name, prop.prop_type, lines, emitted)
        if detail_id:
            lines.append(f"    {owner_id} *-- {detail_id} : {prop.name}")

def render_mermaid(nodes: list[NodeTable], rels: list[RelTable]) -> str:
    lines: list[str] = ["classDiagram"]

    # NODE TABLE → class box
    for n in nodes:
        lines.append(f"    class {n.name} {{")
        for p in n.properties:
            lines.append(_field_line(p))
        lines.append("    }")
        _render_property_type_details(n.name, n.properties, lines)

    lines.append("")

    # REL TABLE → junction class box + bidirectional associations to node classes
    emitted: set[str] = set()

    for rt in rels:
        from_card, to_card = _CARDINALITY.get(rt.multiplicity, ("*", "*"))
        multi_pair = len(rt.pairs) > 1

        for pair in rt.pairs:
            # When a REL table has multiple FROM/TO pairs, each pair gets its
            # own class box. Mermaid class IDs must be unique, but their
            # displayed labels can stay as the REL table name because the
            # connected NODE classes identify the specific FROM/TO pair.
            if multi_pair:
                mermaid_id = f"{rt.name}__{pair.from_table}__{pair.to_table}"
            else:
                mermaid_id = rt.name

            if mermaid_id not in emitted:
                emitted.add(mermaid_id)
                class_decl = f"    class {mermaid_id}"
                if multi_pair:
                    class_decl += f'["{rt.name}"]'
                lines.append(f"{class_decl} {{")
                lines.append("        <<REL>>")
                for p in rt.properties:
                    lines.append(_field_line(p))
                lines.append("    }")
                _render_property_type_details(mermaid_id, rt.properties, lines)

            # Plain solid lines (bidirectional traversal in LadybugDB).
            # Cardinality sits on the node-table end of each line.
            lines.append(f'    {pair.from_table} "{from_card}" -- {mermaid_id}')
            lines.append(f'    {mermaid_id} -- "{to_card}" {pair.to_table}')

    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    args = sys.argv[1:]
    markdown = "--markdown" in args
    paths = [a for a in args if not a.startswith("--")]

    if not paths:
        sys.exit(
            "usage: ladybug_schema_to_mermaid.py <schema.cypher> [...] [--markdown]"
        )

    all_nodes: list[NodeTable] = []
    all_rels: list[RelTable] = []

    for path in paths:
        with open(path) as f:
            nodes, rels = parse_schema(f.read())
        all_nodes.extend(nodes)
        all_rels.extend(rels)

    diagram = render_mermaid(all_nodes, all_rels)

    if markdown:
        print("```mermaid")
        print(diagram)
        print("```")
    else:
        print(diagram)


if __name__ == "__main__":
    main()
