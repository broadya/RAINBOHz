#!/usr/bin/env python3
"""
This module contains common helper functions used by the compiler.
"""

import os
import math
import yaml
import argparse
from typing import Any, Dict, Tuple

# Constant output directory for compiled state sets.
OUTPUT_DIR = "/Users/alanbroady/Development/RAINBOHz/hzl-language/tests/test_output"

# Define allowed ranges with an explicit type annotation.
ALLOWED_RANGES: Dict[str, Tuple[float, float]] = {
    "frequency": (0, 20000),
    "phase": (-2 * math.pi, 2 * math.pi),
    "amplitude": (0, 1),
    "azimuth": (-math.pi, math.pi),
    "elevation": (-math.pi / 2, math.pi / 2),
    "distance": (-1000, 1000)
}

def load_yaml_file(filepath: str) -> Any:
    """Load a YAML file and return its contents."""
    with open(filepath, 'r') as f:
        return yaml.safe_load(f)
    
def save_yaml_file(compiled_data: dict, filename) -> None:
    """Write the compiled partial_state_set YAML file to OUTPUT_DIR."""
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    output_filepath = os.path.join(OUTPUT_DIR, filename)
    with open(output_filepath, 'w') as f:
        yaml.dump(compiled_data, f)
    print(f"Compiled YAML written to {output_filepath}")

def get_yaml_file_path(base_dir: str, namespace: str, name: str) -> str:
    """
    Construct the path to a YAML file given a base directory,
    a namespace, and a name (with '.yaml' appended).
    """
    return os.path.join(base_dir, namespace, name + ".yaml")

def validate_property(name: str, value: float) -> bool:
    """Validate a property against its allowed range."""
    if name == "frequency":
        low, high = ALLOWED_RANGES["frequency"]
    elif name == "phase":
        low, high = ALLOWED_RANGES["phase"]
    elif name == "amplitude":
        low, high = ALLOWED_RANGES["amplitude"]
    elif name in ("azimuth"):
        low, high = ALLOWED_RANGES["azimuth"]
    elif name in ("elevation"):
        low, high = ALLOWED_RANGES["elevation"]
    elif name in ("distance"):
        low, high = ALLOWED_RANGES["distance"]
    else:
        return True  # No validation needed for unspecified properties.
    if not (low <= value <= high):
        raise ValueError(f"Property '{name}' with value {value} is out of allowed range [{low}, {high}].")
    return True

def validate_partial(partial: Dict[str, Any]) -> bool:
    """
    Validate the properties of a partial.
    Expects a dictionary with keys like 'frequency', 'phase', etc.
    """
    for prop in ("frequency", "phase", "amplitude", "azimuth", "elevation", "distance"):
        if prop in partial and partial[prop] is not None:
            validate_property(prop, partial[prop])
    return True

def init_compiler(
    tool_description: str
) -> Tuple[str, str, str, str, Dict[str, Any]]:
    """
    Common initialization for compiler scripts.
    Parses these command-line args:
      1. input_dir          (base directory for DSL YAML files)
      2. output_dir         (where generated files go)
      3. source_namespace   (slash-delimited namespace)
      4. source_name        (name of the source YAML file, no .yaml)

    Returns:
      input_dir, output_dir, source_name, source_namespace, source_doc
    """
    parser = argparse.ArgumentParser(description=tool_description)
    parser.add_argument("input_dir",     help="Base directory for input DSL YAML files")
    parser.add_argument("output_dir",    help="Directory to write output files")
    parser.add_argument("source_namespace",
                        help="Namespace of the source (slash-delimited)")
    parser.add_argument("source_name",   help="Name of the source YAML file (no .yaml)")
    args = parser.parse_args()

    # Locate the source YAML
    try:
        source_path = get_yaml_file_path(
            args.input_dir,
            args.source_namespace,
            args.source_name
        )
    except Exception as e:
        print(f"Error resolving source path: {e}", file=sys.stderr)
        sys.exit(1)

    if not os.path.isfile(source_path):
        print(f"Error: could not find source file at {source_path}", file=sys.stderr)
        sys.exit(1)

    # Load the YAML document
    try:
        source_doc = load_yaml_file(source_path)
    except Exception as e:
        print(f"Error loading YAML from {source_path}: {e}", file=sys.stderr)
        sys.exit(1)

    # Ensure output dir exists
    os.makedirs(args.output_dir, exist_ok=True)

    return (
        args.input_dir,
        args.output_dir,
        args.source_namespace,
        args.source_name,
        source_doc,
    )


