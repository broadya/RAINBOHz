#!/usr/bin/env python3
"""
harmonic_series_compiler.py

This script reads a harmonic series YAML file and compiles it into a partial state set YAML file.
It uses a subset of SymPy to evaluate algebraic expressions (written in infix notation) for each harmonic.
"""

import os
import sys
import argparse
import yaml
import math
import sympy
from sympy import sympify, symbols, pi
from typing import Dict, Any, List, Optional

# Import common helper functions from compiler_common.py
from compiler_common import load_yaml_file, get_yaml_file_path, validate_property, save_yaml_file

# Define the symbol for harmonic_index and evaluation context for expressions.
h_index = symbols('harmonic_index')
# Allow both "pi" and the Unicode π as synonyms.
EVAL_CONTEXT = {"harmonic_index": h_index, "pi": pi, "π": pi}

def evaluate_expression(expr_str: str, harmonic_value: int) -> float:
    """
    Evaluate an algebraic expression given as a string, substituting harmonic_index.
    Returns the computed float value.
    """
    try:
        expr = sympify(expr_str, locals=EVAL_CONTEXT)
        # Substitute the current harmonic index into the expression.
        result = expr.subs({h_index: harmonic_value})
        # Ensure the result is a float (FP64 precision is default in Python's float)
        return float(result)
    except Exception as e:
        raise ValueError(f"Failed to evaluate expression '{expr_str}' with harmonic_index={harmonic_value}: {e}")

def generate_property_values(data: Dict[str, Any], constant_key: str, expr_key: str, harmonic_count: int, base_value: float = 1.0) -> list:
    """
    For a given property, generate a list of values for each harmonic index.
    
    - If the constant (e.g. 'frequency') is provided, return a list of that constant repeated.
    - If an expression (e.g. 'frequency_expression') is provided, evaluate it for each harmonic index (starting at 1).
      For frequency, the evaluated value is multiplied by base_value.
    - If neither is provided, return None.
    - If both are provided, raise an error.
    """
    # 'data' is expected to be a dictionary for the harmonic series parameters.
    # This function is generic; the caller must provide the keys to check.
    # Note: constant_key and expr_key are mutually exclusive.
    values = None  # default: property not defined
    return_values = []
    
    # We'll assume the caller already checked that not both keys are provided.
    if constant_key in data:
        constant_val = data[constant_key]
        # Build a list of constant values.
        return_values = [constant_val for _ in range(harmonic_count)]
    elif expr_key in data:
        expr_str = data[expr_key]
        # Evaluate the expression for harmonic_index from 1 to harmonic_count.
        return_values = [evaluate_expression(expr_str, i) for i in range(1, harmonic_count + 1)]
        # For frequency, multiply by the base_frequency.
        if constant_key == "frequency" and "base_frequency" in data:
            base_frequency = data["base_frequency"]
            return_values = [base_frequency * v for v in return_values]
    return return_values if return_values else None

def compile_harmonic_series(harmonic_series: dict) -> dict:
    """
    Compile a harmonic series definition into a partial_state_set.
    
    The harmonic_series YAML file is expected to contain:
      - Common properties: name, namespace, layer, labels.
      - base_frequency (optional, defaults to 1).
      - number_of_harmonics.
      - For each property (frequency, amplitude, phase, azimuth, elevation, distance),
        either a constant (e.g. 'frequency') or an expression (e.g. 'frequency_expression').
      - Note: If both constant and expression are provided for the same property, error.
      
    Returns a dictionary representing a partial_state_set YAML structure.
    """
    # Extract common properties
    hs_name = harmonic_series.get("name")
    hs_namespace = harmonic_series.get("namespace")
    hs_layer = harmonic_series.get("layer")
    hs_labels = harmonic_series.get("labels", [])
    
    # Base frequency defaults to 1 if not provided.
    base_frequency = harmonic_series.get("base_frequency", 1)
    harmonic_series["base_frequency"] = base_frequency  # Ensure it's set.
    
    harmonic_count = harmonic_series.get("number_of_harmonics")
    if not harmonic_count or not isinstance(harmonic_count, int):
        raise ValueError("number_of_harmonics must be provided as an integer.")
    
    # For each property, ensure that both a constant and an expression are not provided.
    properties = ["frequency", "amplitude", "phase", "azimuth", "elevation", "distance"]
    computed_values = {}
    for prop in properties:
        constant_key = prop
        expr_key = f"{prop}_expression"
        if constant_key in harmonic_series and expr_key in harmonic_series:
            raise ValueError(f"Both {constant_key} and {expr_key} are provided; only one is allowed.")
        # Generate a list of values if provided.
        vals = generate_property_values(harmonic_series, constant_key, expr_key, harmonic_count, base_value=base_frequency)
        if vals is not None:
            computed_values[prop] = vals

    # Build the list of partial_state entries.
    partial_states = []
    for i in range(harmonic_count):
        state = {}
        for prop, values in computed_values.items():
            # Each value is computed for the harmonic index; assign it for the i-th partial.
            state[prop] = values[i]
            # Optionally, you could add further validation here using validate_property().
            # For example:
            validate_property(prop, state[prop])
        # Also add an internal index if needed.
        state["partial_index"] = i+1
        state["labels"] = [f"harmonic {i}"]
        partial_states.append({"partial_state": state})
    
    # Create the output partial_state_set metadata:
    output_name = hs_name + "_derived"
    output_namespace = hs_namespace + "_derived"
    
    compiled_state_set = {
        "partial_state_set": {
            "name": output_name,
            "namespace": output_namespace,
            "labels": hs_labels,  # Labels are copied directly.
            "layer": hs_layer,
            "partial_states": partial_states
        }
    }
    
    return compiled_state_set

def main():
    parser = argparse.ArgumentParser(
        description="Compile a harmonic series YAML file into a partial_state_set YAML file."
    )
    parser.add_argument("input_file", help="Path to the harmonic series YAML file.")
    args = parser.parse_args()
    
    try:
        # Load the harmonic series YAML file.
        harmonic_series_data = load_yaml_file(args.input_file)
        # Expect top-level key "harmonic_series"
        hs = harmonic_series_data.get("harmonic_series")
        if hs is None:
            raise ValueError("The input file must contain a top-level 'harmonic_series' key.")
        
        # Compile the harmonic series into a partial_state_set.
        compiled = compile_harmonic_series(hs)
        # Write the output YAML file.
        save_yaml_file(compiled, compiled["partial_state_set"]["name"] + ".yaml");
    except Exception as e:
        print(f"Error during compilation: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
