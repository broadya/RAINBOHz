#!/usr/bin/env python3
"""
This module contains common helper functions used by the compiler.
"""

import os
import math
import yaml
from typing import Any, Dict, Tuple

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
