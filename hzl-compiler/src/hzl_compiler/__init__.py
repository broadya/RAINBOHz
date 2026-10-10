# SPDX-License-Identifier: Apache-2.0

"""hzl-compiler: lowers HzL source to the canonical model and emits it as Cypher.

HzL (Hertz Language) is the practitioner-facing language of RAINBOHz. This
package reads a score and every file it references, lowers the higher-level
constructs (scales, harmonic series, products of state sets, envelopes) to the
lowest level, a table-shaped canonical model, and emits that as a Cypher script
that `rendering-engine` loads into `engine-db`. See docs/design.md.
"""

__version__ = "0.1.0"

# The HzL language version this compiler reads. Every source file states it
# in its header, so that a file written for a later language is refused rather
# than misread.
HZL_VERSION = "0.1.0"
