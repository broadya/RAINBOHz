#!/usr/bin/env python3
"""
This module contains the top-level compiler logic for Virtual Partial State Sets,
including the flattened Cartesian product transformation.
"""

import os
import sys
import argparse
import yaml
from typing import Any, Dict, List

# Import our common helpers.
from compiler_common import load_yaml_file, get_yaml_file_path, validate_partial

# Base directory for input YAML files (adjust as needed).
BASE_DIR = "/Users/alanbroady/Development/RAINBOHz/hzl-language/tests/test_data"
# Constant output directory for compiled state sets.
OUTPUT_DIR = "/Users/alanbroady/Development/RAINBOHz/hzl-language/tests/test_output"

def load_virtual_partial_state_set(filepath: str) -> Dict[str, Any]:
    """Load the virtual partial state set from a YAML file."""
    data = load_yaml_file(filepath)
    return data["virtual_partial_state_set"]

def load_transformation(transformation_ref: Dict[str, str]) -> Dict[str, Any]:
    """
    Given a transformation reference (with 'namespace' and 'name'),
    load and return the corresponding YAML data.
    """
    trans_path = get_yaml_file_path(BASE_DIR, transformation_ref["namespace"], transformation_ref["name"])
    data = load_yaml_file(trans_path)
    return data["partial_state_set_transformation"]

def load_source_partial_state_sets(source_refs: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    """
    Given a list of source partial state set references, load and return
    the corresponding YAML data as a list.
    Each item is expected to have a key 'partial_state_set' with its own dictionary.
    """
    source_sets: List[Dict[str, Any]] = []
    for item in source_refs:
        ref = item["partial_state_set"]
        path = get_yaml_file_path(BASE_DIR, ref["namespace"], ref["name"])
        data = load_yaml_file(path)
        source_sets.append(data["partial_state_set"])
    return source_sets

def transform_partial_pair(partialA: Dict[str, Any], partialB: Dict[str, Any]) -> Dict[str, Any]:
    """
    Combine two partials (from source sets A and B) into one output partial.
    Rules:
      - frequency: multiply if both present; otherwise use the present value.
      - amplitude: multiply if both present; otherwise use the present value.
      - phase, azimuth, elevation:
          * If both are present, output their mean (i.e. (A+B)/2).
          * If one is None, output the non-None value.
          * If both are missing, omit the property.
    """
    out_partial: Dict[str, Any] = {}

    # Frequency
    if "frequency" in partialA and "frequency" in partialB:
        out_partial["frequency"] = partialA["frequency"] * partialB["frequency"]
    elif "frequency" in partialA:
        out_partial["frequency"] = partialA["frequency"]
    elif "frequency" in partialB:
        out_partial["frequency"] = partialB["frequency"]

    # Amplitude
    if "amplitude" in partialA and "amplitude" in partialB:
        out_partial["amplitude"] = partialA["amplitude"] * partialB["amplitude"]
    elif "amplitude" in partialA:
        out_partial["amplitude"] = partialA["amplitude"]
    elif "amplitude" in partialB:
        out_partial["amplitude"] = partialB["amplitude"]

    # Phase
    phaseA = partialA.get("phase")
    phaseB = partialB.get("phase")
    if phaseA is None and phaseB is None:
        out_partial["phase"] = None
    elif phaseA is None:
        out_partial["phase"] = phaseB
    elif phaseB is None:
        out_partial["phase"] = phaseA
    else:
        out_partial["phase"] = (phaseA + phaseB) / 2

    # Spatial Horizontal
    azimuth_A = partialA.get("azimuth")
    azimuth_B = partialB.get("azimuth")
    if azimuth_A is None and azimuth_B is None:
        pass  # Omit property.
    elif azimuth_A is None:
        out_partial["azimuth"] = azimuth_B
    elif azimuth_B is None:
        out_partial["azimuth"] = azimuth_A
    else:
        out_partial["azimuth"] = (azimuth_A + azimuth_B) / 2

    # Spatial Vertical
    elevation_A = partialA.get("elevation")
    elevation_B = partialB.get("elevation")
    if elevation_A is None and elevation_B is None:
        pass
    elif elevation_A is None:
        out_partial["elevation"] = elevation_B
    elif elevation_B is None:
        out_partial["elevation"] = elevation_A
    else:
        out_partial["elevation"] = (elevation_A + elevation_B) / 2

    # Spatial Distance
    distance_A = partialA.get("distance")
    distance_B = partialB.get("distance")
    if distance_A is None and distance_B is None:
        pass
    elif distance_A is None:
        out_partial["distance"] = distance_B
    elif elevation_B is None:
        out_partial["distance"] = distance_A
    else:
        out_partial["distance"] = (distance_A + distance_B) / 2

    return out_partial

def transform_flattened_cartesian_product(transformation: Dict[str, Any],
                                            source_sets: List[Dict[str, Any]]) -> Dict[str, Any]:
    """
    Apply the "Flattened Cartesian Product" transformation.
    Assumes exactly two source partial state sets.
    For each partial in source set A and each partial in source set B,
    generate an output partial using transform_partial_pair().
    Also, the output labels are the union of the labels from both source sets
    and the transformation.
    """
    output_partials: List[Dict[str, Any]] = []
    setA = source_sets[0].get("partial_states", [])
    setB = source_sets[1].get("partial_states", [])

    for a_item in setA:
        partialA = a_item.get("partial_state", {})
        validate_partial(partialA)
        for b_item in setB:
            partialB = b_item.get("partial_state", {})
            validate_partial(partialB)
            combined = transform_partial_pair(partialA, partialB)
            output_partials.append({"partial_state": combined})

    # Combine labels from both source sets and transformation (as a set).
    labels_A = set(source_sets[0].get("labels", []))
    labels_B = set(source_sets[1].get("labels", []))
    trans_labels = set(transformation.get("labels", []))
    out_labels = list(labels_A | labels_B | trans_labels)

    # Construct the output Partial State Set.
    compiled_state_set: Dict[str, Any] = {
        "partial_state_set": {
            "name": "CompiledPartialStateSet",  # You can generate this dynamically.
            "namespace": "Compiled",
            "labels": out_labels,
            "layer": source_sets[0].get("layer", ""),  # Or use a custom rule.
            "partial_states": output_partials,
        }
    }
    return compiled_state_set

def compile_virtual_partial_state_set(vpss_filepath: str) -> Dict[str, Any]:
    """
    Compile a Virtual Partial State Set YAML file into a Partial State Set.
    Steps:
      1. Load the Virtual Partial State Set.
      2. Load the referenced transformation YAML.
      3. Load the referenced source Partial State Sets.
      4. Validate inputs.
      5. Apply the transformation (currently, only "Flattened Cartesian Product" is supported).
    """
    # Step 1: Load the Virtual Partial State Set.
    vpss = load_virtual_partial_state_set(vpss_filepath)

    # Step 2: Load the transformation.
    transformation_ref = vpss.get("transformation")
    if transformation_ref is None or not isinstance(transformation_ref, dict):
        raise ValueError("Missing or invalid 'transformation' key in virtual partial state set")
    # At this point, transformation_ref is guaranteed to be a dict.
    transformation: Dict[str, str] = transformation_ref  # Now type is known.
    transformation_data = load_transformation(transformation)

    # Step 3: Load the source partial state sets.
    source_refs = vpss.get("partial_state_sets", [])
    source_state_sets = load_source_partial_state_sets(source_refs)

    # Step 4: Validate inputs (already partly done in transform_partial_pair).

    # Step 5: Process the transformation.
    trans_list = transformation_data.get("transformations", [])
    if (trans_list and
        trans_list[0].get("transformation", {}).get("type") == "Flattened Cartesian Product"):
        compiled_state_set = transform_flattened_cartesian_product(transformation, source_state_sets)
    else:
        raise NotImplementedError("Transformation type not supported.")

    return compiled_state_set

def write_output(compiled_state_set: Dict[str, Any]) -> None:
    """Write the compiled Partial State Set to a file in OUTPUT_DIR."""
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    output_name = compiled_state_set["partial_state_set"]["name"] + ".yaml"
    output_filepath = os.path.join(OUTPUT_DIR, output_name)
    with open(output_filepath, 'w') as f:
        yaml.dump(compiled_state_set, f)
    print(f"Compiled Partial State Set written to {output_filepath}")

def main():
    parser = argparse.ArgumentParser(
        description="Compile a Virtual Partial State Set into a Partial State Set."
    )
    parser.add_argument("input_file", help="Path to the Virtual Partial State Set YAML file.")
    args = parser.parse_args()

    try:
        compiled_state_set = compile_virtual_partial_state_set(args.input_file)
        write_output(compiled_state_set)
    except Exception as e:
        print(f"Error during compilation: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
