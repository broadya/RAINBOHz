# RAINBOHz build entry point.
#
# Conventional Makefile, run from the repository root. This is the
# software-engineering build: documentation, and compilation of the components.
# It is distinct from the DSL/content compilation pipeline described in
# AGENTS.md, which is Ninja-based and not yet implemented.
#
# The C++ components are built by CMake; the targets here are thin wrappers
# over `cmake --preset`, so that one command works from the repository root
# regardless of which component you are touching. Everything CMake needs is
# declared in each component's CMakePresets.json, and using CMake directly is
# equally valid.
#
# Note: proof-of-concept/ is archived reference material and is deliberately
# not part of any target here.

PYTHON      ?= python3
SCHEMA_TOOL := tools/openCypher-tools/ladybug_schema_to_mermaid.py

# Validates DDL by executing it against a throwaway LadybugDB instance, so
# that "valid" means what the real database accepts. Needs the `lbug` CLI
# (brew install ladybug); override with LBUG=/path/to/lbug.
VALIDATE_TOOL := tools/openCypher-tools/validate_ladybug_schema.py
LBUG          ?= lbug

# Applies schema DDL to a database. Not a plain `lbug < file.cypher`: that
# silently skips statements and cannot report failure. See scripts/ladybug_ddl.py.
LOAD_TOOL := scripts/load_schema.py

# engine-db is the schema of the one physical database, and the only one the
# system runs on. canonical-db is a design artefact describing the
# composer-facing model; it is kept loadable but nothing loads it. See AGENTS.md.
ENGINE_SCHEMA    := engine-db/schema/engine-schema.cypher
CANONICAL_SCHEMA := canonical-db/schema/canonical-schema.cypher

# Database locations. Override to build a database elsewhere, e.g.
# `make init-engine-db ENGINE_DB=/tmp/scratch.lbug`.
ENGINE_DB    ?= engine-db/db/engine.lbug
CANONICAL_DB ?= canonical-db/db/canonical.lbug

# Generated Mermaid diagrams (pure .mmd; wrap in Markdown at point of use).
DIAGRAMS := \
	engine-db/docs/engine-schema.mmd \
	canonical-db/docs/canonical-schema.mmd

# ---------------------------------------------------------------------------
# Toolchain discovery
# ---------------------------------------------------------------------------

UNAME_S := $(shell uname -s)

# Default CMake preset for the host. Override to build a different
# configuration, e.g. `make rendering-engine RE_PRESET=macos-release`.
ifeq ($(UNAME_S),Darwin)
RE_PRESET ?= macos-debug
else
RE_PRESET ?= linux-debug
endif

RE_DIR   := rendering-engine
RE_BUILD := $(RE_DIR)/build/$(RE_PRESET)

# What `make run-rendering-engine` loads: one schema file, one state file and
# one render request. The engine defaults to the in-repo engine-db schema when
# --schema is not given, so RE_SCHEMA is only needed to point it somewhere
# else; --state and --request have no default and are always passed. Rendered
# frames go under the build tree rather than the repository root. Override any
# of them to run against your own files:
#   make run-rendering-engine RE_STATE=/path/to/score.cypher RE_REQUEST=/path/to/request.yaml
RE_STATE   ?= engine-db/examples/minimum-score.cypher
RE_SCHEMA  ?= engine-db/schema/engine-schema.cypher
RE_REQUEST ?= $(RE_DIR)/examples/minimum-request.yaml
RE_OUT_DIR ?= $(RE_BUILD)/frames
# Extra engine options, e.g. RE_ARGS="--validate-db --verbose".
RE_ARGS    ?=

# Apple's clang ships no clang-tidy, and its clang-format lags. Prefer a
# Homebrew LLVM if one is installed (it is keg-only, so not on PATH).
BREW_LLVM_BIN := /opt/homebrew/opt/llvm/bin
CLANG_TIDY    ?= $(firstword $(wildcard $(BREW_LLVM_BIN)/clang-tidy) $(shell command -v clang-tidy))
CLANG_FORMAT  ?= $(firstword $(wildcard $(BREW_LLVM_BIN)/clang-format) $(shell command -v clang-format))

# compile_commands.json is written by Apple clang, which resolves the SDK
# implicitly. Homebrew's clang-tidy does not, and without the sysroot it cannot
# find libc++ -- every translation unit then fails at `#include <memory>` and
# buries the real findings under cascade errors.
ifeq ($(UNAME_S),Darwin)
CLANG_TIDY_ARGS ?= --extra-arg=-isysroot --extra-arg=$(shell xcrun --show-sdk-path)
endif

RE_SOURCES := $(shell find $(RE_DIR)/src $(RE_DIR)/include $(RE_DIR)/test \
	-name '*.cpp' -o -name '*.hpp' 2>/dev/null)

# Doxygen API documentation. Generated under the build tree, which is
# gitignored, but outside any preset directory: it does not depend on the
# build configuration. Tests are not documented. See rendering-engine/Doxyfile.
DOXYGEN         ?= doxygen
RE_DOXYGEN_OUT  := $(RE_DIR)/build/doxygen
RE_DOC_SOURCES  := $(filter-out $(RE_DIR)/test/%,$(RE_SOURCES)) \
	$(RE_DIR)/README.md $(RE_DIR)/Doxyfile

.PHONY: all docs clean-docs api-docs-rendering-engine \
        validate-schemas validate-engine-schema validate-canonical-schema \
        init-engine-db init-canonical-db \
        reset-engine-db reset-canonical-db \
        rendering-engine configure-rendering-engine test-rendering-engine \
        run-rendering-engine format-rendering-engine tidy-rendering-engine \
        clean-rendering-engine build test clean help

## all: documentation only (diagrams and API docs) -- compiling a component is an explicit request
all: docs

## help: list the documented targets
help:
	@grep -E '^## ' $(MAKEFILE_LIST) | sed 's/^## /  /'

# ---------------------------------------------------------------------------
# Documentation
# ---------------------------------------------------------------------------

## docs: regenerate schema Mermaid diagrams and Doxygen API documentation
docs: $(DIAGRAMS) api-docs-rendering-engine

engine-db/docs/engine-schema.mmd: engine-db/schema/engine-schema.cypher $(SCHEMA_TOOL)
	@mkdir -p $(@D)
	$(PYTHON) $(SCHEMA_TOOL) $< > $@

canonical-db/docs/canonical-schema.mmd: canonical-db/schema/canonical-schema.cypher $(SCHEMA_TOOL)
	@mkdir -p $(@D)
	$(PYTHON) $(SCHEMA_TOOL) $< > $@

# Doxygen reports every undocumented function or parameter as a warning, on
# stderr. The warnings are the convention's enforcement (AGENTS.md,
# Documentation Comments); they do not yet fail the build.

## api-docs-rendering-engine: generate rendering-engine Doxygen HTML
api-docs-rendering-engine: $(RE_DOXYGEN_OUT)/html/index.html

$(RE_DOXYGEN_OUT)/html/index.html: $(RE_DOC_SOURCES)
	@command -v $(DOXYGEN) >/dev/null || { echo "doxygen not found; brew install doxygen"; exit 1; }
	@mkdir -p $(RE_DOXYGEN_OUT)
	cd $(RE_DIR) && RE_DOXYGEN_OUT=$(abspath $(RE_DOXYGEN_OUT)) $(DOXYGEN) Doxyfile
	@touch $@

## clean-docs: remove generated diagrams and API documentation
clean-docs:
	rm -f $(DIAGRAMS)
	rm -rf $(RE_DOXYGEN_OUT)

# ---------------------------------------------------------------------------
# Schema validation
# ---------------------------------------------------------------------------
#
# Each .cypher file is replayed statement by statement into a temporary
# LadybugDB database, which is discarded afterwards. No project database is
# read or written, so these targets are safe to run at any time.
#
# Statements are submitted one per `lbug` invocation deliberately: the shell
# exits 0 on failure, reports errors on stdout, and when fed a whole file on
# stdin will silently skip statements. See the tool's docstring.

# One invocation covering both files, rather than a dependency on the two
# per-schema targets: make would stop at the first failing prerequisite, and
# the point of this target is to report the state of every schema at once.

## validate-schemas: check every schema loads into a real LadybugDB
validate-schemas:
	$(PYTHON) $(VALIDATE_TOOL) --lbug $(LBUG) \
	    $(ENGINE_SCHEMA) $(CANONICAL_SCHEMA)

## validate-engine-schema: check engine-db DDL loads into a real LadybugDB
validate-engine-schema:
	$(PYTHON) $(VALIDATE_TOOL) --lbug $(LBUG) $(ENGINE_SCHEMA)

## validate-canonical-schema: check canonical-db DDL loads into a real LadybugDB
validate-canonical-schema:
	$(PYTHON) $(VALIDATE_TOOL) --lbug $(LBUG) $(CANONICAL_SCHEMA)

# ---------------------------------------------------------------------------
# Databases
# ---------------------------------------------------------------------------
#
# init applies the schema to a real database, and is safe to repeat:
# declarations that already exist are reported as present rather than failing.
# It applies only *new* declarations, so it does not migrate existing tables --
# per AGENTS.md, schema migration during early development is drop-and-recreate,
# which is what reset does.
#
# The reset targets pass --yes: an explicit `make reset-...` is the
# confirmation. The reset_db.sh scripts prompt when run without -y.

## init-engine-db: apply the engine schema to $(ENGINE_DB)
init-engine-db:
	$(PYTHON) $(LOAD_TOOL) --lbug $(LBUG) \
	    --schema-dir engine-db/schema --db $(ENGINE_DB)

## init-canonical-db: apply the canonical schema to $(CANONICAL_DB)
init-canonical-db:
	$(PYTHON) $(LOAD_TOOL) --lbug $(LBUG) \
	    --schema-dir canonical-db/schema --db $(CANONICAL_DB)

## reset-engine-db: DESTROY $(ENGINE_DB) and recreate it from schema
reset-engine-db:
	$(PYTHON) $(LOAD_TOOL) --lbug $(LBUG) --reset --yes \
	    --schema-dir engine-db/schema --db $(ENGINE_DB)

## reset-canonical-db: DESTROY $(CANONICAL_DB) and recreate it from schema
reset-canonical-db:
	$(PYTHON) $(LOAD_TOOL) --lbug $(LBUG) --reset --yes \
	    --schema-dir canonical-db/schema --db $(CANONICAL_DB)

# ---------------------------------------------------------------------------
# rendering-engine (C++)
# ---------------------------------------------------------------------------
#
# The first build compiles Ladybug from source and takes a long time. Later
# builds are incremental, and ccache makes reconfiguring cheap.

# `cmake --preset` resolves presets relative to the working directory, so every
# recipe runs from the component directory rather than the repository root.

## configure-rendering-engine: run CMake configure only (fetches Ladybug on first use)
configure-rendering-engine:
	cd $(RE_DIR) && cmake --preset $(RE_PRESET)

$(RE_BUILD)/CMakeCache.txt:
	cd $(RE_DIR) && cmake --preset $(RE_PRESET)

## rendering-engine: build the rendering engine
rendering-engine: $(RE_BUILD)/CMakeCache.txt
	cd $(RE_DIR) && cmake --build --preset $(RE_PRESET)

## test-rendering-engine: build and run the rendering engine tests
test-rendering-engine: rendering-engine
	cd $(RE_DIR) && ctest --preset $(RE_PRESET)

## run-rendering-engine: run the engine in test mode against $(RE_STATE) and $(RE_REQUEST)
run-rendering-engine: rendering-engine
	$(RE_BUILD)/bin/rendering-engine --mode test \
	    --schema $(RE_SCHEMA) --state $(RE_STATE) \
	    --request $(RE_REQUEST) --out-dir $(RE_OUT_DIR) $(RE_ARGS)

## format-rendering-engine: apply .clang-format in place
format-rendering-engine:
	@test -n "$(CLANG_FORMAT)" || { echo "clang-format not found"; exit 1; }
	$(CLANG_FORMAT) -i $(RE_SOURCES)

## tidy-rendering-engine: run clang-tidy (requires a configured preset)
tidy-rendering-engine: $(RE_BUILD)/CMakeCache.txt
	@test -n "$(CLANG_TIDY)" || { echo "clang-tidy not found; brew install llvm"; exit 1; }
	$(CLANG_TIDY) -p $(RE_BUILD) $(CLANG_TIDY_ARGS) $(filter %.cpp,$(RE_SOURCES))

## clean-rendering-engine: remove the rendering-engine build tree
clean-rendering-engine:
	rm -rf $(RE_DIR)/build

# ---------------------------------------------------------------------------
# Aggregates
# ---------------------------------------------------------------------------

## build: build every compiled component
build: rendering-engine

# Schema validation first: it is seconds rather than minutes, needs no
# compiler, and a schema that does not load makes the rest moot.

## test: run every component's tests
test: validate-schemas test-rendering-engine

## clean: remove all generated artefacts
clean: clean-docs clean-rendering-engine
