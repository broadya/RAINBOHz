# SPDX-License-Identifier: Apache-2.0

# RAINBOHz build entry point.
#
# Conventional Makefile, run from the repository root. This is the
# software-engineering build: documentation, and compilation of the components
# (the C++ rendering engine and playback client, and the Python HzL compiler
# and its tests).
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

# The interpreter the root virtualenv is created from (3.12 or later). Every
# Python step runs in that venv, never with $(PYTHON) itself, so that one
# Python, with the packages requirements-dev.txt names, serves the whole build.
PYTHON      ?= python3
VENV        := .venv
VENV_BIN    := $(CURDIR)/$(VENV)/bin
VENV_PY     := $(VENV_BIN)/python
VENV_STAMP  := $(VENV)/.installed

SCHEMA_TOOL := scripts/ladybug_schema_to_mermaid.py

# Validates DDL by executing it against a throwaway LadybugDB instance, so
# that "valid" means what the real database accepts. Needs the `lbug` CLI
# (brew install ladybug); override with LBUG=/path/to/lbug.
VALIDATE_TOOL := scripts/validate_ladybug_schema.py
LBUG          ?= lbug

# Refills full-line // comments in DDL to 64 columns without touching the DDL.
WRAP_TOOL := scripts/wrap_cypher_comments.py

# Applies schema DDL to a database. Not a plain `lbug < file.cypher`: that
# silently skips statements and cannot report failure. See scripts/ladybug_ddl.py.
LOAD_TOOL := scripts/load_schema.py

# engine-db is the schema of the one physical database, and the only one the
# system runs on. canonical-db is a design artefact describing the
# composer-facing model; it is kept loadable but nothing loads it. See AGENTS.md.
ENGINE_SCHEMA    := engine-db/schema/engine-schema.cypher
CANONICAL_SCHEMA := canonical-db/schema/canonical-schema.cypher
DDL_SCHEMAS      := $(ENGINE_SCHEMA) $(CANONICAL_SCHEMA)

# Database locations. Override to build a database elsewhere, e.g.
# `make init-engine-db ENGINE_DB=/tmp/scratch.lbug`.
ENGINE_DB    ?= engine-db/db/engine.lbug
CANONICAL_DB ?= canonical-db/db/canonical.lbug

# The root README.md is generated: the author's introduction, then the
# agent-maintained repository layout and developer guide. Edit the parts, not
# README.md. The introduction is the author's own text and AI agents must not
# modify it (AGENTS.md, Documentation Ownership).
README_PARTS := \
	docs/readme/introduction.md \
	docs/readme/developer-guide.md

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

# What `make run-rendering-engine-server` serves on and from. The engine has no
# defaults for these -- a server's interface and readable files are always
# said outright -- so these are development conveniences: loopback only, the
# repository as the input root (so the examples can be loaded), and the same
# frames directory as a batch run.
#   make run-rendering-engine-server RE_PORT=9000 RE_ARGS=--verbose
RE_HOST       ?= 127.0.0.1
RE_PORT       ?= 8080
RE_INPUT_ROOT ?= .

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

# playback-client (C++). Same presets and tools as rendering-engine; its own
# build tree, so neither component's build disturbs the other's.
PC_DIR    := playback-client
PC_PRESET ?= $(RE_PRESET)
PC_BUILD  := $(PC_DIR)/build/$(PC_PRESET)

# What `make run-playback-client` plays: a range of the score loaded in the
# server at PC_SERVER, which `make run-rendering-engine-server` starts at the
# default address. The client does not load a score; do that first with
# PUT /v1/score (rendering-engine/docs/server.md §6).
#   make run-playback-client PC_START=0:0 PC_END=4:0 PC_LOOPS=3 PC_ARGS=--debug
PC_SERVER ?= http://$(RE_HOST):$(RE_PORT)/v1
PC_START  ?= 0:0
PC_END    ?= 4:0
PC_LOOPS  ?= 1
# Extra client options, e.g. PC_ARGS="--debug --keep-buffer".
PC_ARGS   ?=

PC_SOURCES := $(shell find $(PC_DIR)/src $(PC_DIR)/include $(PC_DIR)/test \
	-name '*.cpp' -o -name '*.hpp' 2>/dev/null)
PC_DOXYGEN_OUT := $(PC_DIR)/build/doxygen
PC_DOC_SOURCES := $(filter-out $(PC_DIR)/test/%,$(PC_SOURCES)) \
	$(PC_DIR)/README.md $(PC_DIR)/Doxyfile $(wildcard $(PC_DIR)/docs/*.md)

.PHONY: all docs readme clean-docs api-docs-rendering-engine api-docs-playback-client \
        validate-schemas validate-engine-schema validate-canonical-schema \
        init-engine-db init-canonical-db \
        reset-engine-db reset-canonical-db \
        rendering-engine configure-rendering-engine test-rendering-engine \
        run-rendering-engine run-rendering-engine-server \
        format-rendering-engine tidy-rendering-engine \
        clean-rendering-engine \
        playback-client configure-playback-client test-playback-client \
        run-playback-client format-playback-client tidy-playback-client \
        clean-playback-client \
        hzl-compiler hzl-compile test-hzl-compiler format-hzl-compiler lint-hzl-compiler \
        test-hzl-oracle run-hzl-score \
        clean-hzl-compiler venv clean-venv build test clean help

## all: documentation only (diagrams and API docs) -- compiling a component is an explicit request
all: docs

## help: list the documented targets
help:
	@grep -E '^## ' $(MAKEFILE_LIST) | sed 's/^## /  /'

# ---------------------------------------------------------------------------
# Documentation
# ---------------------------------------------------------------------------

## docs: regenerate README.md, schema Mermaid diagrams and Doxygen API documentation
docs: readme $(DIAGRAMS) api-docs-rendering-engine api-docs-playback-client

# README.md is committed, because GitHub renders it, so clean-docs leaves it.

## readme: join the README parts in docs/readme/ into README.md
readme: README.md

README.md: $(README_PARTS)
	{ echo '<!-- Generated by `make readme` from docs/readme/. Edit those files, not this one. -->'; \
	  cat $(README_PARTS); } > $@

engine-db/docs/engine-schema.mmd: engine-db/schema/engine-schema.cypher $(SCHEMA_TOOL) | $(VENV_STAMP)
	@mkdir -p $(@D)
	$(VENV_PY) $(SCHEMA_TOOL) $< > $@

canonical-db/docs/canonical-schema.mmd: canonical-db/schema/canonical-schema.cypher $(SCHEMA_TOOL) | $(VENV_STAMP)
	@mkdir -p $(@D)
	$(VENV_PY) $(SCHEMA_TOOL) $< > $@

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

## api-docs-playback-client: generate playback-client Doxygen HTML
api-docs-playback-client: $(PC_DOXYGEN_OUT)/html/index.html

$(PC_DOXYGEN_OUT)/html/index.html: $(PC_DOC_SOURCES)
	@command -v $(DOXYGEN) >/dev/null || { echo "doxygen not found; brew install doxygen"; exit 1; }
	@mkdir -p $(PC_DOXYGEN_OUT)
	cd $(PC_DIR) && PC_DOXYGEN_OUT=$(abspath $(PC_DOXYGEN_OUT)) $(DOXYGEN) Doxyfile
	@touch $@

## clean-docs: remove generated diagrams and API documentation
clean-docs:
	rm -f $(DIAGRAMS)
	rm -rf $(RE_DOXYGEN_OUT) $(PC_DOXYGEN_OUT)

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
validate-schemas: venv
	$(VENV_PY) $(VALIDATE_TOOL) --lbug $(LBUG) \
	    $(ENGINE_SCHEMA) $(CANONICAL_SCHEMA)

## validate-engine-schema: check engine-db DDL loads into a real LadybugDB
validate-engine-schema: venv
	$(VENV_PY) $(VALIDATE_TOOL) --lbug $(LBUG) $(ENGINE_SCHEMA)

## validate-canonical-schema: check canonical-db DDL loads into a real LadybugDB
validate-canonical-schema: venv
	$(VENV_PY) $(VALIDATE_TOOL) --lbug $(LBUG) $(CANONICAL_SCHEMA)

# The schemas are hand-edited, so rewriting them is only ever run by their
# author; neither target is part of `test`.

## format-schema-comments: refill DDL comments to 64 columns, in place
format-schema-comments: venv
	$(VENV_PY) $(WRAP_TOOL) $(DDL_SCHEMAS)

## check-schema-comments: show what format-schema-comments would change
check-schema-comments: venv
	$(VENV_PY) $(WRAP_TOOL) --check $(DDL_SCHEMAS)

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
init-engine-db: venv
	$(VENV_PY) $(LOAD_TOOL) --lbug $(LBUG) \
	    --schema-dir engine-db/schema --db $(ENGINE_DB)

## init-canonical-db: apply the canonical schema to $(CANONICAL_DB)
init-canonical-db: venv
	$(VENV_PY) $(LOAD_TOOL) --lbug $(LBUG) \
	    --schema-dir canonical-db/schema --db $(CANONICAL_DB)

## reset-engine-db: DESTROY $(ENGINE_DB) and recreate it from schema
reset-engine-db: venv
	$(VENV_PY) $(LOAD_TOOL) --lbug $(LBUG) --reset --yes \
	    --schema-dir engine-db/schema --db $(ENGINE_DB)

## reset-canonical-db: DESTROY $(CANONICAL_DB) and recreate it from schema
reset-canonical-db: venv
	$(VENV_PY) $(LOAD_TOOL) --lbug $(LBUG) --reset --yes \
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
configure-rendering-engine: venv
	cd $(RE_DIR) && cmake --preset $(RE_PRESET)

$(RE_BUILD)/CMakeCache.txt: | $(VENV_STAMP)
	cd $(RE_DIR) && cmake --preset $(RE_PRESET)

## rendering-engine: build the rendering engine
rendering-engine: $(RE_BUILD)/CMakeCache.txt | $(VENV_STAMP)
	cd $(RE_DIR) && cmake --build --preset $(RE_PRESET)

## test-rendering-engine: build and run the rendering engine tests
test-rendering-engine: rendering-engine
	cd $(RE_DIR) && ctest --preset $(RE_PRESET)

## run-rendering-engine: run the engine in batch mode against $(RE_STATE) and $(RE_REQUEST)
run-rendering-engine: rendering-engine
	$(RE_BUILD)/bin/rendering-engine --mode batch \
	    --schema $(RE_SCHEMA) --state $(RE_STATE) \
	    --request $(RE_REQUEST) --out-dir $(RE_OUT_DIR) $(RE_ARGS)

## run-rendering-engine-server: serve REST on $(RE_HOST):$(RE_PORT) until POST /v1/shutdown (rendering-engine/docs/server.md)
run-rendering-engine-server: rendering-engine
	$(RE_BUILD)/bin/rendering-engine --mode server --schema $(RE_SCHEMA) \
	    --host $(RE_HOST) --port $(RE_PORT) \
	    --input-root $(RE_INPUT_ROOT) --out-dir $(RE_OUT_DIR) $(RE_ARGS)

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
# playback-client (C++)
# ---------------------------------------------------------------------------
#
# A CLI that loops a range of the score loaded in rendering-engine's REST
# service through the default audio device (playback-client/docs/design.md).

## configure-playback-client: run CMake configure only
configure-playback-client: venv
	cd $(PC_DIR) && cmake --preset $(PC_PRESET)

$(PC_BUILD)/CMakeCache.txt: | $(VENV_STAMP)
	cd $(PC_DIR) && cmake --preset $(PC_PRESET)

## playback-client: build the playback client
playback-client: $(PC_BUILD)/CMakeCache.txt | $(VENV_STAMP)
	cd $(PC_DIR) && cmake --build --preset $(PC_PRESET)

# The end-to-end test serves with rendering-engine's binary for the same
# preset, so the engine is built first.

## test-playback-client: build the client and the engine, then run the client's tests
test-playback-client: playback-client rendering-engine
	cd $(PC_DIR) && ctest --preset $(PC_PRESET)

## run-playback-client: play $(PC_START) to $(PC_END), $(PC_LOOPS) loop(s), from the server at $(PC_SERVER)
run-playback-client: playback-client
	$(PC_BUILD)/bin/playback-client --server $(PC_SERVER) \
	    --start $(PC_START) --end $(PC_END) --loops $(PC_LOOPS) $(PC_ARGS)

## format-playback-client: apply .clang-format in place
format-playback-client:
	@test -n "$(CLANG_FORMAT)" || { echo "clang-format not found"; exit 1; }
	$(CLANG_FORMAT) -i $(PC_SOURCES)

## tidy-playback-client: run clang-tidy (requires a configured preset)
tidy-playback-client: $(PC_BUILD)/CMakeCache.txt
	@test -n "$(CLANG_TIDY)" || { echo "clang-tidy not found; brew install llvm"; exit 1; }
	$(CLANG_TIDY) -p $(PC_BUILD) $(CLANG_TIDY_ARGS) $(filter %.cpp,$(PC_SOURCES))

## clean-playback-client: remove the playback-client build tree
clean-playback-client:
	rm -rf $(PC_DIR)/build

# ---------------------------------------------------------------------------
# Python
# ---------------------------------------------------------------------------
#
# One virtualenv at the root, .venv, created from $(PYTHON) on first use and
# reinstalled when requirements-dev.txt or the compiler's pyproject.toml
# changes. Every target that runs Python depends on it. Gitignored.

## venv: create the root virtualenv every Python step uses, from requirements-dev.txt
venv: $(VENV_STAMP)

$(VENV_STAMP): requirements-dev.txt hzl-compiler/pyproject.toml
	$(PYTHON) -m venv $(VENV)
	$(VENV_PY) -m pip install --quiet --upgrade pip
	$(VENV_PY) -m pip install --quiet -r requirements-dev.txt
	@touch $@

## clean-venv: remove the root virtualenv
clean-venv:
	rm -rf $(VENV)

# ---------------------------------------------------------------------------
# hzl-compiler (Python)
# ---------------------------------------------------------------------------
#
# The compiler is installed editable into the root venv (requirements-dev.txt),
# so that editing the sources needs no reinstall. Its build/ is gitignored.

HZL_DIR   := hzl-compiler

# What `make hzl-compile` compiles, and where the Cypher goes. Override to
# compile your own score:
#   make hzl-compile HZL_SCORE=/path/to/scores/mine.yaml
HZL_SCORE ?= $(HZL_DIR)/examples/tempo-map-score/scores/tempo_map_score.yaml
HZL_OUT   ?= $(HZL_DIR)/build/$(notdir $(basename $(HZL_SCORE))).cypher
# Extra compiler options, e.g. HZL_ARGS="--sample-rate 44100 --dump-canonical out.json".
HZL_ARGS  ?=

## hzl-compiler: make the compiler usable (it lives in the root venv; same as make venv)
hzl-compiler: venv

## hzl-compile: compile $(HZL_SCORE) to $(HZL_OUT)
hzl-compile: hzl-compiler
	$(VENV_PY) -m hzl_compiler compile $(HZL_SCORE) -o $(HZL_OUT) $(HZL_ARGS)

## test-hzl-compiler: run the compiler's tests
test-hzl-compiler: hzl-compiler
	cd $(HZL_DIR) && $(VENV_PY) -m pytest

# ruff and mypy are configured in hzl-compiler/pyproject.toml and installed
# into the venv with the dev extras. format-* fixes what ruff can fix safely
# and then formats; lint-* changes nothing and fails on any finding, which
# makes make test fail too.

## format-hzl-compiler: apply ruff's safe fixes and formatting in place
format-hzl-compiler: hzl-compiler
	cd $(HZL_DIR) && $(VENV_BIN)/ruff check --fix --exit-zero . && $(VENV_BIN)/ruff format .

## lint-hzl-compiler: check the compiler with ruff (lint, formatting) and mypy (part of make test)
lint-hzl-compiler: hzl-compiler
	cd $(HZL_DIR) && $(VENV_BIN)/ruff check . && $(VENV_BIN)/ruff format --check . && $(VENV_BIN)/mypy

# The compiled twin must give the engine a database it transforms into the
# same physical values as the hand-written score, which is what the oracle's
# JSON mirror holds. The oracle runner takes --state and --score separately,
# so it checks the compiled route against the hand-written one unchanged.

## test-hzl-oracle: compile the tempo-map twin, render it, and check it against the hand-written score's oracle
test-hzl-oracle: hzl-compiler rendering-engine
	$(VENV_PY) -m hzl_compiler compile -q \
	    $(HZL_DIR)/examples/tempo-map-score/scores/tempo_map_score.yaml \
	    -o $(HZL_DIR)/build/oracle/tempo-map-score.cypher
	$(VENV_PY) $(RE_DIR)/test/oracle/run_oracle_check.py \
	    --engine $(RE_BUILD)/bin/rendering-engine --schema $(ENGINE_SCHEMA) \
	    --state $(HZL_DIR)/build/oracle/tempo-map-score.cypher \
	    --request $(RE_DIR)/examples/tempo-map-request.yaml \
	    --score engine-db/examples/tempo-map-score.json \
	    --out-dir $(HZL_DIR)/build/oracle/frames

## run-hzl-score: compile $(HZL_SCORE), then render it with $(RE_REQUEST) into $(RE_OUT_DIR)
run-hzl-score: hzl-compile rendering-engine
	$(RE_BUILD)/bin/rendering-engine --mode batch \
	    --schema $(RE_SCHEMA) --state $(HZL_OUT) \
	    --request $(RE_REQUEST) --out-dir $(RE_OUT_DIR) $(RE_ARGS)

## clean-hzl-compiler: remove the compiler's build output
clean-hzl-compiler:
	rm -rf $(HZL_DIR)/build

# ---------------------------------------------------------------------------
# Aggregates
# ---------------------------------------------------------------------------

## build: build every compiled component
build: rendering-engine playback-client hzl-compiler

# Schema validation first: it is seconds rather than minutes, needs no
# compiler, and a schema that does not load makes the rest moot.

## test: run every component's tests
test: validate-schemas lint-hzl-compiler test-hzl-compiler test-rendering-engine test-playback-client test-hzl-oracle

## clean: remove all generated artefacts
clean: clean-docs clean-rendering-engine clean-playback-client clean-hzl-compiler clean-venv
