#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""CTest driver: check that the server's OpenAPI document is valid, and that its examples fit it.

    openapi_check.py --spec <openapi.yaml>

Validates the document against the OpenAPI 3.1 specification with
openapi-spec-validator, then validates every example in it (`example`, and
each value under `examples`) against the schema beside it, so that the
examples a reader of the document sees cannot drift from the schemas.

Also importable: run_server_check.py uses response_schema_validator() to
check the server's live responses against the document.

Exit status 0 when the document and every example are valid, 1 otherwise,
and 77 (CTest's SKIP_RETURN_CODE here) when PyYAML, jsonschema or
openapi-spec-validator is not available, so that the C++ test suite does not
depend on them.
"""

from __future__ import annotations

import argparse
import sys
from typing import Any, Iterator

SKIP = 77

# The URI the document is registered under, so that its `#/components/...`
# references resolve when a piece of it is used as a JSON Schema.
SPEC_URI = "urn:rainbohz:rendering-engine:openapi"


def load_spec(path: str) -> dict:
    """Read the document.

    Args:
        path: The YAML file.

    Returns:
        The document as a dict.
    """
    import yaml

    with open(path, encoding="utf-8") as handle:
        return yaml.safe_load(handle)


def schema_validator(spec: dict, schema: dict) -> Any:
    """A JSON Schema 2020-12 validator for a schema taken from the document.

    OpenAPI 3.1 schemas are JSON Schema 2020-12. A schema's references are
    relative to the whole document, so the document is registered under
    SPEC_URI and the schema's own references are made absolute against it.

    Args:
        spec: The whole document.
        schema: A schema object from it.

    Returns:
        A jsonschema validator.
    """
    import jsonschema
    from referencing import Registry, Resource
    from referencing.jsonschema import DRAFT202012

    def absolute(node: Any) -> Any:
        if isinstance(node, dict):
            return {
                key: (SPEC_URI + value if key == "$ref" and value.startswith("#") else absolute(value))
                for key, value in node.items()
            }
        if isinstance(node, list):
            return [absolute(value) for value in node]
        return node

    registry = Registry().with_resource(SPEC_URI, Resource(contents=spec, specification=DRAFT202012))
    return jsonschema.Draft202012Validator(absolute(schema), registry=registry)


def resolve(spec: dict, node: Any) -> Any:
    """Follow a `$ref` to a local component, if `node` is one."""
    while isinstance(node, dict) and "$ref" in node:
        ref = node["$ref"]
        if not ref.startswith("#/"):
            raise ValueError(f"only local references are supported: {ref}")
        node = spec
        for part in ref[2:].split("/"):
            node = node[part]
    return node


def response_schema_validator(spec: dict, path: str, method: str, status: int) -> Any:
    """A validator for the body of one documented response.

    Args:
        spec: The whole document.
        path: The path template as the document writes it, e.g. `/tempo-maps/{id}/points`.
        method: The method, lower case.
        status: The HTTP status.

    Returns:
        A jsonschema validator for the response's body.

    Raises:
        KeyError: If the document does not describe that response.
    """
    operation = spec["paths"][path][method]
    response = resolve(spec, operation["responses"][str(status)])
    media = response["content"]
    content = media.get("application/json") or media["application/problem+json"]
    return schema_validator(spec, content["schema"])


def examples(spec: dict, node: Any, where: str = "") -> Iterator[tuple[str, dict, Any]]:
    """Every example in the document, with the schema it illustrates.

    Args:
        spec: The whole document.
        node: Where to search from.
        where: The JSON pointer of `node`, for messages.

    Yields:
        (location, schema, example value) for each media type or parameter
        object that has a schema and an `example` or `examples`.
    """
    if isinstance(node, dict):
        if "schema" in node:
            if "example" in node:
                yield where + "/example", node["schema"], node["example"]
            for name, example in node.get("examples", {}).items():
                yield f"{where}/examples/{name}", node["schema"], resolve(spec, example)["value"]
        for key, value in node.items():
            if key not in ("example", "examples"):
                yield from examples(spec, value, f"{where}/{key}")
    elif isinstance(node, list):
        for index, value in enumerate(node):
            yield from examples(spec, value, f"{where}/{index}")


def main() -> int:
    """Validate the document, then every example in it."""
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--spec", required=True)
    args = parser.parse_args()

    try:
        import jsonschema  # noqa: F401
        import yaml  # noqa: F401
        from openapi_spec_validator import validate
        from openapi_spec_validator.validation.exceptions import OpenAPIValidationError
    except ImportError as error:
        print(f"openapi_check: {error.name} is not available; skipping the OpenAPI check")
        return SKIP

    spec = load_spec(args.spec)
    if spec.get("openapi") != "3.1.1":
        print(f"openapi_check: FAILED: the document is OpenAPI {spec.get('openapi')}, not 3.1.1", file=sys.stderr)
        return 1
    try:
        validate(spec)
    except OpenAPIValidationError as error:
        print(f"openapi_check: FAILED: the document is not valid OpenAPI: {error}", file=sys.stderr)
        return 1

    failures = 0
    checked = 0
    for where, schema, value in examples(spec, spec["paths"], "#/paths"):
        checked += 1
        for error in schema_validator(spec, schema).iter_errors(value):
            print(f"openapi_check: FAILED: {where}: {error.message}", file=sys.stderr)
            failures += 1
    for where, schema, value in examples(spec, spec.get("components", {}), "#/components"):
        checked += 1
        for error in schema_validator(spec, schema).iter_errors(value):
            print(f"openapi_check: FAILED: {where}: {error.message}", file=sys.stderr)
            failures += 1

    if failures:
        print(f"openapi_check: {failures} problem(s)", file=sys.stderr)
        return 1
    print(f"openapi_check: the document is valid OpenAPI {spec['openapi']}, and its {checked} example(s) fit their schemas")
    return 0


if __name__ == "__main__":
    sys.exit(main())
