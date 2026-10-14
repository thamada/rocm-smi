CC       ?= gcc
CFLAGS   ?= -O2 -std=c11 -Wall -Wextra -D_GNU_SOURCE
ROCM     ?= /opt/rocm
CPPFLAGS ?= -I$(ROCM)/include
LDFLAGS  ?= -L$(ROCM)/lib -Wl,-rpath,$(ROCM)/lib
LIBS     ?= -lamd_smi

# Local Rust toolchain and crate cache. Nothing is taken from ~/.cargo or the distro cargo.
RUST_VERSION ?= 1.99.0
RUST_PREFIX  := $(CURDIR)/.rust
RUST_HOST    := $(shell uname -m)-unknown-linux-gnu
CARGO_HOME   := $(RUST_PREFIX)/cargo-home
CARGO        := $(RUST_PREFIX)/bin/cargo
export CARGO_HOME
export PATH := $(RUST_PREFIX)/bin:$(PATH)

.PHONY: all build rsbuild run clean install

all: build

# Download rustc/cargo into .rust/ and fetch crates into .rust/cargo-home/.
install: $(RUST_PREFIX)/.installed

$(RUST_PREFIX)/.installed: Cargo.toml Cargo.lock
	set -eu; \
	mkdir -p "$(RUST_PREFIX)" "$(CARGO_HOME)"; \
	if [ ! -x "$(CARGO)" ]; then \
		curl --proto '=https' --tlsv1.2 -fL \
			-o "$(RUST_PREFIX)/rust.tar.gz" \
			"https://static.rust-lang.org/dist/rust-$(RUST_VERSION)-$(RUST_HOST).tar.gz"; \
		tar -C "$(RUST_PREFIX)" -xzf "$(RUST_PREFIX)/rust.tar.gz"; \
		"$(RUST_PREFIX)/rust-$(RUST_VERSION)-$(RUST_HOST)/install.sh" \
			--prefix="$(RUST_PREFIX)" \
			--disable-ldconfig \
			--components=rustc,cargo,rust-std-$(RUST_HOST); \
		rm -rf "$(RUST_PREFIX)/rust-$(RUST_VERSION)-$(RUST_HOST)" "$(RUST_PREFIX)/rust.tar.gz"; \
	fi; \
	"$(CARGO)" fetch --locked --manifest-path "$(CURDIR)/Cargo.toml"; \
	touch "$@"

# C implementation. Output is ./rocm-smi, same path as the Rust build.
build: rocm-smi.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o rocm-smi rocm-smi.c $(LDFLAGS) $(LIBS)

# Rust implementation. Uses only the toolchain from `make install`.
rsbuild: $(RUST_PREFIX)/.installed
	ROCM=$(ROCM) $(CARGO) build --offline --locked --release --manifest-path $(CURDIR)/Cargo.toml
	cp -f target/release/rocm-smi rocm-smi

run: build
	./rocm-smi

clean:
	rm -f rocm-smi
	if [ -x "$(CARGO)" ]; then $(CARGO) clean --manifest-path $(CURDIR)/Cargo.toml; else rm -rf target; fi
