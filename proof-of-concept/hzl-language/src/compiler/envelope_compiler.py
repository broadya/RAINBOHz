#!/usr/bin/env python3
"""
envelope_compiler.py

This module demonstrates how to load an envelope YAML file and convert it
into instances of our data model defined in envelope_model.py.
It shows the use of dataclasses such as EnvelopeSpec, EnvelopeStage, and EnvelopeTransform.
"""

import os
import sys
import yaml
import re

from abc import ABC, abstractmethod
from typing import Dict, Any, Tuple, List

# Import our common helpers.
from compiler_common import load_yaml_file, get_yaml_file_path, validate_partial, save_yaml_file, init_compiler
from hzl_types import EnvelopeSpec, EnvelopeStage, EnvelopeTransform, PartialSet, Partial, PartialState, PhaseCoordinate

# ============================================================
# Parsing the Envelope YAML
# ============================================================

def parse_envelope_transform(data: Dict[str, Any]) -> EnvelopeTransform:
    et = data.get("envelope_transform")
    if et is None:
        raise ValueError("Missing 'envelope_transform' key in transform list entry.")

    filter_str = et["filter"]
    t_ref = et["transformation"]
    transformation_ref: Tuple[str, str] = (t_ref["namespace"], t_ref["name"])
    src = et.get("source_partial_state_set")
    if src is not None:
        if "namespace" not in src or "name" not in src:
            raise ValueError("Invalid 'source_partial_state_set'; must have namespace and name.")
        source_partial_state_set = (src["namespace"], src["name"])
    else:
        source_partial_state_set = None

    return EnvelopeTransform(
        curve=curve,
        filter=filter_str,
        transformation_ref=transformation_ref,
        source_partial_state_set=source_partial_state_set
    )


def parse_envelope_stage(data: Dict[str, Any]) -> EnvelopeStage:
    stage = data.get("envelope_stage")
    if stage is None:
        raise ValueError("Missing 'envelope_stage' key in stage list entry.")

    time = stage.get("time")
    curve = stage.get("curve")
    transforms_data = stage.get("envelope_transforms", [])

    transforms: List[EnvelopeTransform] = []
    for entry in transforms_data:
        transforms.append(parse_envelope_transform(entry))

    return EnvelopeStage(time=time, envelope_transforms=transforms)


def parse_envelope_spec(data: Dict[str, Any]) -> EnvelopeSpec:
    # Extract metadata
    hzl_version = data.get("hzl_version")
    hzl_type = data.get("hzl_type")
    name = data.get("name")
    namespace = data.get("namespace")
    layer = data.get("layer")
    labels = data.get("labels", [])

    # initial partial state reference
    init_ref = data.get("initial_partial_state_set")
    if init_ref is None or "namespace" not in init_ref or "name" not in init_ref:
        raise ValueError("Missing or invalid initial partial state set reference.")
    initial_partial_state_ref: Tuple[str, str] = (init_ref["namespace"], init_ref["name"])

    # parse stages
    stages_data = data.get("envelope_stages", [])
    envelope_stages: List[EnvelopeStage] = [
        parse_envelope_stage(entry) for entry in stages_data
    ]

    return EnvelopeSpec(
        hzl_version=hzl_version,
        hzl_type=hzl_type,
        name=name,
        namespace=namespace,
        layer=layer,
        labels=labels,
        initial_partial_state_ref=initial_partial_state_ref,
        envelope_stages=envelope_stages
    )

# ============================================================
# Compiling the Envelope - Initial State
# ============================================================

def build_partial_set(
    raw_pss: Dict[str, Any],
    envelope_labels: List[str]
) -> PartialSet:
    """
    Construct a PartialSet from raw partial_state_set dict.
    Merges the raw state-set labels with envelope-level labels.
    Initializes envelopes and adds the first phase coordinate.
    """
    # Metadata
    pss_name = raw_pss.get("name")
    pss_namespace = raw_pss.get("namespace")
    pss_layer = raw_pss.get("layer")
    pss_labels = raw_pss.get("labels", [])

    # Merge top-level labels
    merged_labels = list(dict.fromkeys(pss_labels + envelope_labels))

    partial_set = PartialSet(
        name=pss_name,
        namespace=pss_namespace,
        layer=pss_layer,
        labels=merged_labels
    )

    # Process each partial_state entry
    from hzl_types import PhaseCoordinate
    for entry in raw_pss.get("partial_states", []):
        raw_partial = entry.get("partial_state")
        if raw_partial is None:
            raise ValueError("Missing 'partial_state' in raw definition.")

        # Validate numeric properties
        validate_partial(raw_partial)

        # Mandatory identifier
        identifier = str(raw_partial.get("identifier"))
        if not isinstance(identifier, str) or '/' in identifier or '#' in identifier:
            raise ValueError(f"Invalid or missing partial 'identifier': {identifier}")

        # Build PartialState
        ps_labels = raw_partial.get("labels", [])
        ps = PartialState(
            labels=ps_labels.copy(),
            frequency=raw_partial.get("frequency"),
            amplitude=raw_partial.get("amplitude"),
            azimuth=raw_partial.get("azimuth"),
            elevation=raw_partial.get("elevation"),
            distance=raw_partial.get("distance"),
            phase=raw_partial.get("phase", 0.0),
            identifier=identifier,
            namespace=pss_namespace
        )

        # Construct the Partial, initialize envelopes and current_state
        partial = Partial(
            labels=ps_labels.copy(),
            current_state=ps,
            namespace=pss_namespace,
            identifier=identifier
        )

        # Initialize frequency envelope with initial level
        if ps.frequency is not None:
            partial.frequency_envelope.levels.append(ps.frequency)
        # Initialize amplitude envelope
        if ps.amplitude is not None:
            partial.amplitude_envelope.levels.append(ps.amplitude)
        # Initialize azimuth/envelope if present
        if ps.azimuth is not None:
            partial.azimuth_envelope.levels.append(ps.azimuth)
        if ps.elevation is not None:
            partial.elevation_envelope.levels.append(ps.elevation)
        if ps.distance is not None:
            partial.distance_envelope.levels.append(ps.distance)

        # Initialize phase coordinates: first and boundary at time 0.0
        # First coordinate: explicit phase or default 0.0
        first_value = ps.phase if ps.phase is not None else 0.0
        coord_start = PhaseCoordinate(time=0.0, value=first_value, natural=False)
        partial.phase_coordinates.append(coord_start)

        # Key using '#' as separator
        fqid = f"{pss_namespace}#{identifier}"
        partial_set.partials[fqid] = partial

    return partial_set

def load_partial_state_set(
    input_dir: str,
    input_ref: Tuple[str, str]
) -> Dict[str, Any]:
    """
    Locate and load the initial partial_state_set YAML file based on the
    reference in EnvelopeSpec. Returns the raw partial_state_set dictionary.
    """
    namespace, name = input_ref
    # Construct the file path to the partial_state_set YAML
    path = get_yaml_file_path(input_dir, namespace, name)

    # Load the YAML document
    doc = load_yaml_file(path)

    # Extract the 'partial_state_set' section
    pss = doc.get("partial_state_set")
    if pss is None:
        raise ValueError(f"File '{path}' does not contain a top-level 'partial_state_set' key.")
    return pss

# ============================================================
# Compiling the Envelope Stages
# ============================================================

def load_transformation_definition(
    input_dir: str,
    transform_ref: Tuple[str, str],
    stage_time: float,
    stage_curve: str,
    elapsed_time: float
) -> PartialStateSetTransformation:
    """
    Given a (namespace, name) reference to a partial_state_set_transformation,
    plus the current stage's duration, curve, and cumulative elapsed_time,
    load the YAML, read its 'transformation' block, and instantiate the
    appropriate Transformation subclass with all metadata.
    """
    namespace, name = transform_ref
    path = get_yaml_file_path(input_dir, namespace, name)
    doc = load_yaml_file(path)

    # Top-level key is 'partial_state_set_transformation'
    top = doc.get("partial_state_set_transformation")
    if not isinstance(top, dict):
        raise ValueError(f"Expected 'partial_state_set_transformation' mapping in {path}")

    # Read common metadata
    hzl_version = top["hzl_version"]
    hzl_type    = top["hzl_type"]
    labels      = top.get("labels", [])
    layer       = top["layer"]

    # The inner 'transformation' block:
    inner = top.get("transformation")
    if not isinstance(inner, dict):
        raise ValueError(f"Missing 'transformation' block in {path}")

    # Read the type discriminator
    ttype = inner.get("transformation_type")
    if not isinstance(ttype, str):
        raise ValueError(f"Missing or invalid 'transformation_type' in {path}")

    # Dispatch to the correct class
    base_kwargs = dict(
        name=name,
        namespace=namespace,
        layer=layer,
        labels=labels,
        time=stage_time,
        curve=stage_curve,
        elapsed_time=elapsed_time,
        partial_set_1=None  # will be set later by caller
    )

    if ttype == "Flattened Cartesian Product":
        transformation = FlattenedCartesianProductTransformation(**base_kwargs)
    elif ttype == "Update":
        transformation = UpdateTransformation(**base_kwargs)
    else:
        raise ValueError(f"Unknown transformation_type '{ttype}' in {path}")

    return transformation

def process_envelope_transform(
    input_dir: str,
    stage_time: float,
    stage_curve: str,
    transform: EnvelopeTransform,
    current_partial_set: PartialSet,
    elapsed_time: float
) -> None:
    """
    Apply a single EnvelopeTransform to current_partial_set in place.

    - transform.curve, transform.filter, transform.transformation_ref,
      transform.source_partial_state_set are all available here.
    - You should:
        1. Filter current_partial_set.partials by matching transform.filter against each Partial.labels.
        2. Optionally load the source PartialSet if transform.source_partial_state_set is not None.
        3. Invoke the appropriate PartialStateSetTransformation (e.g. Update or FCP).
        4. Update envelopes (levels, times, curves) and phase_coordinates on each affected Partial.
    """

    # Compile the filter regex once
    pattern = re.compile(transform.filter)

    # Create the next set, reusing metadata lists (no .copy())
    next_partial_set = PartialSet(
        name=current_partial_set.name,
        namespace=current_partial_set.namespace,
        layer=current_partial_set.layer,
        labels=current_partial_set.labels  # shared reference
    )

    # Greedily include any Partial whose labels match
    for fqid, partial in current_partial_set.partials.items():
        if any(pattern.search(lbl) for lbl in partial.labels):
            next_partial_set.partials[fqid] = partial

    transformation = load_transformation_definition(
        input_dir,
        transform.transformation_ref,
        stage_time,
        stage_curve,
        elapsed_time
    )

    # Now set partial_set_1 and (if provided) partial_set_2:
    transformation.partial_set_1 = next_partial_set
    if transform.source_partial_state_set is not None:
        raw_pss = load_partial_state_set(input_dir, transform.source_partial_state_set)
        transformation.partial_set_2 = build_partial_set(raw_pss, spec.labels)

    # Finally invoke the transform:
    transformation.transform()

def process_envelope_stage(
    input_dir: str,
    stage: EnvelopeStage,
    current_partial_set: PartialSet,
    elapsed_time: float
) -> None:
    """
    Apply all transforms in one EnvelopeStage to current_partial_set.

    - stage.time is the duration for this stage.
    - stage.envelope_transforms is the ordered list of EnvelopeTransform.
    This function should loop through each transform and call
    process_envelope_transform.
    """
    for transform in stage.envelope_transforms:
        process_envelope_transform(input_dir, stage.time, stage.curve, transform, current_partial_set, elapsed_time)
    # After all transforms, you may want to increment some internal stage counter
    # or perform any per-stage housekeeping here.


def compile_envelope_stages(
    input_dir: str,
    spec: EnvelopeSpec,
    initial_partial_set: PartialSet
) -> PartialSet:
    """
    Top‑level compiler function:
    - Takes the parsed EnvelopeSpec and the initial PartialSet.
    - Loops over spec.envelope_stages, calling process_envelope_stage.
    - Returns the final PartialSet, which corresponds to the completed audio fragment.
    """
    elapsed_time = 0.0
    current_partial_set = initial_partial_set
    for stage in spec.envelope_stages:
        elapsed_time += stage.time
        process_envelope_stage(input_dir, stage, current_partial_set, elapsed_time)
    return current_partial_set


def main():
    # [Initialization as before]
    arg_input_dir, arg_output_dir, arg_name, arg_namespace, doc = init_compiler(
        "Compile an Envelope into an audio fragment"
    )

    envelope_data = doc["envelope"]
    spec = parse_envelope_spec(envelope_data)
    raw_pss = load_partial_state_set(arg_input_dir, spec.initial_partial_state_ref)
    initial_pss = build_partial_set(raw_pss, spec.labels)

    # Now compile
    final_partial_set = compile_envelope_stages(arg_input_dir, spec, initial_pss)

    # TODO: serialize final_partial_set to audio_fragment YAML
    print(f"Compiled audio fragment with {len(final_partial_set.partials)} partials.")


if __name__ == "__main__":
    main()

"""
Pseudo Code

Some intialisation, must establish the filename of the Envelope YAML files and set the paths to read / write other YAML files.

Parse the Envelope and construct corresponding objects of types EnvelopeSpec, EnvelopeState, EnvelopeTransform

Identify the initial state partial state set.

Load the initial partial state set into the data model

    Load from the YAML file into memory.

    Constuct an empty PartialSet callsed current_partial_set.

    Add the labels into the labels of the PartialSet

    Initialise partials with a List of Partial to be populated.

    For each partial do the following -

        Read the state of the partial, constructing a PartialState object and Partial object.

        Construct the Partial object as follows -

            Add the PartialState as the current state.
            Add the labels from the intiial state
            If present add frequency value, amplitude value etc. over all values.
            If present construct a PhaseCoordinate with time 0.0 and value from state.
            Add this PhaseCoordinate if it was present
            Otherwise, add a PhaseCoordinate with time 0 and phase 0.
            Namespace and identifer are the same as a PartialState. Redundant but makes processing easier.

        Add this Partial object to the partials in curent_partial_set, with a key that is the fully qualifier identifier - namespace/identifier from PartialState.

For each EnvelopeStage, perform transformations as needed -

    For each EnvelopeTransform in the EnvelopeStage -

        Create a empty set of Partial called next_partial_set.
        Loop through the partials list in current_partial_set

            If a partial's labels match the regexp in the EnvelopeTransform, add that partial (as a reference) in the next_partial_set.

        This results in a List of Partials in next_partial_set, where the current_partial_set corresponds to all envelope stages so far.

        Now perform a transformation on next_partial_set. This is just like the transformations in virtual_partial_state_set_compiler... but we need to refactor that code, so not reusing yet.

        This works as follows -

        Read the transformation YAML into the appropriate subclass of PartialStateSetTransformation, let's call it transformation.
        partial_set_1 --> next_partial_set
        In the EnvelopeTransform there is a reference to another YAML file the source PartialStateSet - read the YAML file and populate a PartialStateSet that we call source_partial_set
        partial_set_2 --> source_partial_set

        Perform transform from the PartialStateSetTransformation subclass called transformation.
        This MUST also update the envelopes, which is OK because they are already in the data structure - but it's annoying because when we do transforms outside of envelope processing, then we don't want this. Therefore, some flag is needed in the tranform function.
        Note here, update is the most obvious one and it's where we start. It performs matching by using the identifier.

        This resuts in next_partial_set being already updated... and seeing as it's all references we're done. No need to copy anything back.
        It is allowed for next_partial_set to have added some partials (which may or may not be in the same namespace - depends on the types of transformation), so...

        Merge using this pattern (because where the key is the same, object will be the same)
        for k, v in second.items():
            if k not in first:
                first[k] = v
        
Once all envelope stages are performed, current_partial_set describes the audio fragment.

Serialise current_partial_set to an audio_fragment YAML file.
         
"""

"""
TODOs in other areas...

- Change from index to identifier and make it a UUID- 

"""