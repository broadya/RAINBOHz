#!/usr/bin/env python3
"""
scale_compiler.py

This script reads a scale YAML file and compiles it into a partial_state_set YAML file.
The scale file describes musical scales with a list of notes (each note having properties like frequency).
The output partial_state_set will preserve the order of notes and include the note identifier,
with metadata (name, namespace, layer, labels) transformed (by appending '_derived').
"""

import os
import sys
import argparse
import yaml

# Import common helper functions.
from compiler_common import load_yaml_file, get_yaml_file_path, validate_property, save_yaml_file

# Constant output directory for compiled state sets.
OUTPUT_DIR = "/Users/alanbroady/Development/RAINBOHz/hzl-language/tests/test_output"

def compile_scale(scale_data: dict) -> dict:
    """
    Compile a scale definition into a partial_state_set.
    
    Expected scale YAML structure:
    
    scale:
      language: "hzl_0.0.1_experimental"
      type: "scale"
      name: "C_major"
      namespace: "scales"
      layer: "default"
      labels:
        - "major"
        - "diatonic"
      notes:
        - note: "C4"
          frequency: 261.63
        - note: "D4"
          frequency: 293.66
        ... etc.
    
    This function:
      - Copies common metadata and appends "_derived" to name and namespace.
      - Iterates over the ordered "notes" list and for each note, creates a partial_state.
      - Validates numeric properties (using ALLOWED_RANGES from compiler_common.py).
      - If a property is not specified, it is omitted in the output.
    """
    scale_name = scale_data.get("name")
    scale_namespace = scale_data.get("namespace")
    scale_layer = scale_data.get("layer")
    scale_labels = scale_data.get("labels", [])
    
    notes = scale_data.get("notes", [])
    if not notes:
        raise ValueError("Scale file must contain a 'notes' list with at least one note.")
    
    partial_states = []
    i=0
    for note_entry in notes:
        # Each note_entry must have a 'note' key.
        if "note" not in note_entry:
            raise ValueError("Every note entry must contain a 'note' key for identification.")
        ps = {}  # This will hold the properties for this partial state.
        # Propagate the note identifier.
        ps["labels"] = [note_entry["note"]]
        
        # Copy over all other properties provided for the note.
        for key, value in note_entry.items():
            if key == "note":
                continue
            # If the property is numeric, perform validation.
            if isinstance(value, (int, float)):
                try:
                    validate_property(key, value)
                except Exception as e:
                    raise ValueError(f"Validation failed for note '{note_entry['note']}' property '{key}': {e}")
            ps[key] = value

        ps["partial_index"] = i
        i=i+1
        partial_states.append({"partial_state": ps})
    
    # Generate the output metadata by appending '_derived'
    output_name = scale_name + "_derived"
    output_namespace = scale_namespace + "_derived"
    
    compiled_state_set = {
        "partial_state_set": {
            "name": output_name,
            "namespace": output_namespace,
            "labels": scale_labels,
            "layer": scale_layer,
            "partial_states": partial_states
        }
    }
    return compiled_state_set

def main():
    parser = argparse.ArgumentParser(
        description="Compile a scale YAML file into a partial_state_set YAML file."
    )
    parser.add_argument("input_file", help="Path to the scale YAML file.")
    args = parser.parse_args()
    
    try:
        scale_data_full = load_yaml_file(args.input_file)
        # Expect the top-level key to be "scale".
        scale_data = scale_data_full.get("scale")
        if scale_data is None:
            raise ValueError("The input file must contain a top-level 'scale' key.")
        
        compiled = compile_scale(scale_data)
        # Write the output YAML file.
        save_yaml_file(compiled, compiled["partial_state_set"]["name"] + ".yaml");
    except Exception as e:
        print(f"Error during compilation: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
