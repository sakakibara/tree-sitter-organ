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
# Requires: Node + pnpm (or npm) so we can install tree-sitter-cli.

.PHONY: build install clean test prepass spec-check test-spec

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
build: $(ORG_SO)

# Compile org.so directly (bypasses `tree-sitter build`'s C compilation
# so we can include the Plan B pre-pass sources alongside parser.c +
# scanner.c). `tree-sitter generate` still produces parser.c from
# grammar.js.
ORG_SO_SOURCES = src/parser.c src/scanner.c \
                 src/prepass.c src/prepass_scalar.c src/prepass_simd.c \
                 src/interval_tree.c
ORG_SO_HEADERS = src/prepass.h src/interval_tree.h
ORG_SO_CFLAGS  = -std=c99 -O2 -Wall -Wextra -Wpedantic -fPIC -I src

$(BUILD_DIR):
	@mkdir -p $@

$(ORG_SO): $(ORG_SO_SOURCES) $(ORG_SO_HEADERS) | $(BUILD_DIR)
	$(CC) $(ORG_SO_CFLAGS) -shared -o $@ $(ORG_SO_SOURCES)

# `src/parser.c` and `src/tree_sitter/*.h` are generated from grammar.js by
# `tree-sitter generate`. Both are committed to the repo so fresh clones can
# build without needing tree-sitter-cli (= no Node/pnpm dependency for end
# users). Maintainers re-run `tree-sitter generate` after editing grammar.js
# and commit the regenerated files.
src/parser.c: grammar.js node_modules/.bin/tree-sitter
	./node_modules/.bin/tree-sitter generate

# `pnpm install` is preferred (matches the lockfile); fall back to npm if pnpm
# isn't available so contributors without pnpm can still build.
node_modules/.bin/tree-sitter:
	@if command -v pnpm >/dev/null 2>&1; then \
		pnpm install --silent; \
	else \
		npm install --silent; \
	fi

PREPASS_SOURCES = src/prepass.c src/interval_tree.c \
                  src/prepass_scalar.c src/prepass_simd.c
PREPASS_HEADERS = src/prepass.h src/interval_tree.h
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

test: build
	./node_modules/.bin/tree-sitter test

# Verify spec/org.abnf and grammar.js list the same set of named rules.
# The script reads spec/.spec-check-ignores for shape-only ABNF rules
# that have no 1:1 grammar.js counterpart by design.
spec-check:
	@node scripts/check-abnf-sync.js grammar.js spec/org.abnf

# Per-rule positive/negative example tests under spec/examples/.
# Behavior matching: feeds each `+ input` line through the parser and
# asserts the named node appears; feeds each `- input` and asserts it
# does not.  Stronger than rule-name sync alone.
#
# tree-sitter-cli's auto-compile only knows about parser.c + scanner.c,
# so it links a broken org.so missing prepass_state_new.  Inject the
# proper Makefile-built org.so into the tree-sitter cache before
# running the runner.
TS_CACHE_DIR := $(HOME)/.cache/tree-sitter/lib
test-spec: build
	@mkdir -p $(TS_CACHE_DIR)
	@cp $(ORG_SO) $(TS_CACHE_DIR)/org.so
	@touch $(TS_CACHE_DIR)/org.so
	@node scripts/test-rule-examples.js

clean:
	rm -rf build/
	rm -f org.so prepass.so prepass_scalar.so prepass_simd.so   # legacy artefacts
	rm -rf node_modules
