# tree-sitter-organ build
#
# Build outputs are platform-specific to avoid one host's binary clobbering
# another's on a shared checkout (e.g. macOS host + Linux container with
# the same path mounted). Layout:
#
#   build/<os>-<arch>/org.so
#   build/<os>-<arch>/prepass.so
#   build/<os>-<arch>/prepass_scalar.so
#   build/<os>-<arch>/prepass_simd.so
#
# `make` (default target `build`) builds for the current platform. The plugin
# resolves `parser_path` against the same `<os>-<arch>` triple at runtime so
# the right binary is loaded.
#
# The default build needs only a C compiler: parser.c and the tree_sitter
# headers are committed.  `make generate` (maintainers, after a grammar.js
# edit) is the only target that needs Node + pnpm/npm for tree-sitter-cli.

.PHONY: build generate check-generated install clean test check-c prepass spec-check test-spec test-crlf test-bare-cr test-no-error _freshness

# ---------------------------------------------------------------------------
# Platform detection. uname -s lower-cased + uname -m gives darwin-arm64,
# linux-x86_64, linux-aarch64, etc. — same scheme nvim-treesitter uses.
UNAME_S := $(shell uname -s | tr '[:upper:]' '[:lower:]')
UNAME_M := $(shell uname -m)
PLATFORM := $(UNAME_S)-$(UNAME_M)
BUILD_DIR := build/$(PLATFORM)

ORG_SO          := $(BUILD_DIR)/org.so
PREPASS_SO      := $(BUILD_DIR)/prepass.so
PREPASS_SCALAR  := $(BUILD_DIR)/prepass_scalar.so
PREPASS_SIMD    := $(BUILD_DIR)/prepass_simd.so

# Default target.
build: _freshness $(ORG_SO)

# Warn (never fail) when grammar.js has uncommitted edits that the
# committed parser.c hasn't caught up to -- the maintainer forgot to
# `make generate`.  Gated on an actual uncommitted grammar.js change so
# a fresh clone or tarball (where checkout mtimes are arbitrary) stays
# silent; CI's check-generated is the hard guard.
_freshness:
	@if git rev-parse --git-dir >/dev/null 2>&1 \
	   && ! git diff --quiet -- grammar.js 2>/dev/null \
	   && [ src/parser.c -ot grammar.js ]; then \
		printf 'WARNING: grammar.js edited since src/parser.c was generated; run `make generate`.\n' >&2; \
	fi

# Compile org.so directly (bypasses `tree-sitter build`'s C compilation
# so we can include the pre-pass sources alongside parser.c + scanner.c).
# `tree-sitter generate` still produces parser.c from grammar.js.
ORG_SO_SOURCES = src/parser.c src/scanner.c \
                 src/prepass.c src/prepass_scalar.c src/prepass_simd.c
ORG_SO_HEADERS = src/prepass.h
ORG_SO_CFLAGS  = -std=c99 -O2 -Wall -Wextra -Wpedantic -fPIC -I src \
                 -DORGAN_PREPASS_USE_SIMD=1 $(CFLAGS)

$(BUILD_DIR):
	@mkdir -p $@

$(ORG_SO): $(ORG_SO_SOURCES) $(ORG_SO_HEADERS) | $(BUILD_DIR)
	$(CC) $(ORG_SO_CFLAGS) -shared -o $@ $(ORG_SO_SOURCES)

# src/parser.c, src/grammar.json, src/node-types.json and src/tree_sitter/*.h
# are generated from grammar.js by `tree-sitter generate`, and committed so
# the default build needs no tree-sitter-cli (no Node/pnpm for end users).
# This target is maintainer-only: run it after editing grammar.js and commit
# the regenerated files.  It is deliberately NOT a prerequisite of the build
# -- otherwise a missing node_modules/ (every fresh clone) would drag the
# whole npm toolchain into a build that only needs a C compiler.
generate: node_modules/.bin/tree-sitter
	./node_modules/.bin/tree-sitter generate

# CI guard: regenerating from grammar.js must reproduce the committed
# sources byte-for-byte.  Fails the build if they have drifted, catching a
# grammar.js edit that was committed without its regenerated parser.c.
check-generated: generate
	git diff --exit-code -- src/parser.c src/grammar.json src/node-types.json src/tree_sitter

# `pnpm install` is preferred (matches the lockfile); fall back to npm if pnpm
# isn't available so contributors without pnpm can still build.
node_modules/.bin/tree-sitter:
	@if command -v pnpm >/dev/null 2>&1; then \
		pnpm install --silent; \
	else \
		npm install --silent; \
	fi

PREPASS_SOURCES = src/prepass.c src/prepass_index.c src/interval_tree.c \
                  src/prepass_scalar.c src/prepass_simd.c
PREPASS_HEADERS = src/prepass.h src/prepass_index.h src/interval_tree.h
PREPASS_CFLAGS  = -std=c99 -O2 -Wall -Wextra -Wpedantic -fPIC

ifeq ($(ORGAN_PREPASS_USE_SIMD),0)
PREPASS_CFLAGS += -DORGAN_PREPASS_USE_SIMD=0
else
PREPASS_CFLAGS += -DORGAN_PREPASS_USE_SIMD=1
endif

$(PREPASS_SO): $(PREPASS_SOURCES) $(PREPASS_HEADERS) | $(BUILD_DIR)
	$(CC) $(PREPASS_CFLAGS) -shared -o $@ $(PREPASS_SOURCES)

$(PREPASS_SCALAR): $(PREPASS_SOURCES) $(PREPASS_HEADERS) | $(BUILD_DIR)
	$(CC) -std=c99 -O2 -Wall -Wextra -Wpedantic -fPIC -DORGAN_PREPASS_USE_SIMD=0 -shared -o $@ $(PREPASS_SOURCES)

$(PREPASS_SIMD): $(PREPASS_SOURCES) $(PREPASS_HEADERS) | $(BUILD_DIR)
	$(CC) -std=c99 -O2 -Wall -Wextra -Wpedantic -fPIC -DORGAN_PREPASS_USE_SIMD=1 -shared -o $@ $(PREPASS_SOURCES)

prepass: $(PREPASS_SO)

install: build
	@echo "tree-sitter-organ built at: $(CURDIR)/$(ORG_SO)"

# tree-sitter-cli auto-compiles only parser.c + scanner.c, producing a
# library that fails at dlopen (missing prepass symbols).  Point the CLI
# at a repo-local lib dir seeded with the Makefile-built org.so instead.
# The CLI looks the library up under its own platform extension, which is
# .dylib on macOS and .so elsewhere; seeding the other name silently gets
# ignored and the CLI compiles its own broken copy.
TS_LIBDIR := $(BUILD_DIR)/ts-lib
ifeq ($(UNAME_S),darwin)
TS_LIB := $(TS_LIBDIR)/org.dylib
else
TS_LIB := $(TS_LIBDIR)/org.so
endif

test: build node_modules/.bin/tree-sitter
	@mkdir -p $(TS_LIBDIR)
	@cp $(ORG_SO) $(TS_LIB)
	@touch $(TS_LIB)
	TREE_SITTER_LIBDIR=$(TS_LIBDIR) node scripts/run-corpus-tests.js
	TREE_SITTER_LIBDIR=$(TS_LIBDIR) node scripts/test-crlf.js
	TREE_SITTER_LIBDIR=$(TS_LIBDIR) node scripts/test-bare-cr.js
	TREE_SITTER_LIBDIR=$(TS_LIBDIR) node scripts/large-input-stress-test.js
	TREE_SITTER_LIBDIR=$(TS_LIBDIR) node scripts/adversarial-no-error.js --gate

test-no-error: build node_modules/.bin/tree-sitter
	@mkdir -p $(TS_LIBDIR)
	@cp $(ORG_SO) $(TS_LIB)
	@touch $(TS_LIB)
	TREE_SITTER_LIBDIR=$(TS_LIBDIR) node scripts/adversarial-no-error.js --gate

test-crlf: build node_modules/.bin/tree-sitter
	@mkdir -p $(TS_LIBDIR)
	@cp $(ORG_SO) $(TS_LIB)
	@touch $(TS_LIB)
	TREE_SITTER_LIBDIR=$(TS_LIBDIR) node scripts/test-crlf.js

test-bare-cr: build node_modules/.bin/tree-sitter
	@mkdir -p $(TS_LIBDIR)
	@cp $(ORG_SO) $(TS_LIB)
	@touch $(TS_LIB)
	TREE_SITTER_LIBDIR=$(TS_LIBDIR) node scripts/test-bare-cr.js

# C-level scanner tests under ASan/UBSan.  The harness includes
# scanner.c directly, so only the prepass sources are linked.
CHECK_C_BIN := $(BUILD_DIR)/scanner_tests
CHECK_C_SRCS = tests/scanner_tests.c src/prepass.c src/prepass_index.c \
               src/prepass_scalar.c src/prepass_simd.c src/interval_tree.c

check-c: | $(BUILD_DIR)
	$(CC) -std=c99 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
		-Wall -Wextra -I src -o $(CHECK_C_BIN) $(CHECK_C_SRCS)
	./$(CHECK_C_BIN)

# Verify spec/org.abnf and grammar.js list the same set of named rules.
# The script reads spec/.spec-check-ignores for shape-only ABNF rules
# that have no 1:1 grammar.js counterpart by design.
spec-check:
	@node scripts/check-abnf-sync.js grammar.js spec/org.abnf

# Per-rule positive/negative example tests under spec/examples/.
# Behavior matching: feeds each `+ input` line through the parser and
# asserts the named node appears; feeds each `- input` and asserts it
# does not.  Stronger than rule-name sync alone.
test-spec: build node_modules/.bin/tree-sitter
	@mkdir -p $(TS_LIBDIR)
	@cp $(ORG_SO) $(TS_LIB)
	@touch $(TS_LIB)
	TREE_SITTER_LIBDIR=$(TS_LIBDIR) node scripts/test-rule-examples.js

clean:
	rm -rf build/
	rm -f org.so prepass.so prepass_scalar.so prepass_simd.so   # legacy artefacts
	rm -rf node_modules
