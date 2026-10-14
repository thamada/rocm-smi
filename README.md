# rocm-smi

`rocm-smi` had always shipped with ROCm, release after release. It is a shame that 10.1.0 dropped it. This program is that missing tool.

It matches the ROCm System Management Interface CLI 4.x: the same flags, the same log layout, and the same concise table. Telemetry and controls go through `libamd_smi`. The default build is Rust (`src/`). `rocm-smi.c` is the C implementation of the same CLI. Both write `./rocm-smi`.

Requires a live `amdgpu` driver and ROCm under `/opt/rocm` (override with `make ROCM=/path/to/rocm`).

## Build

Rust toolchain and crates stay inside this directory. `make install` downloads Rust 1.99.0 into `.rust/` and fetches crates into `.rust/cargo-home/`. `make build` uses that `cargo` only, offline. It does not use `~/.cargo` or a distro `cargo`.

```
make install   # .rust/ toolchain and crate cache
make build     # Rust binary ./rocm-smi (runs install first if needed)
make cbuild    # C binary ./rocm-smi
```

`make` and `make all` are the same as `make build`. `make run` builds the Rust binary and prints the concise table. `make clean` removes `./rocm-smi` and `target/`. It leaves `.rust/` in place.

| Target | Result |
|--------|--------|
| `install` | Rust 1.99.0 (`rustc`, `cargo`, `rust-std`) under `.rust/`, crates under `.rust/cargo-home/` |
| `build` | `./rocm-smi` from the Rust sources |
| `cbuild` | `./rocm-smi` from `rocm-smi.c` |
| `run` | `build`, then `./rocm-smi` |
| `clean` | Remove `./rocm-smi` and `target/` |

The Rust build needs `curl` to download the toolchain, and system `libclang` (`libclang-dev`) for bindgen. The C build needs `gcc`. Both link `libamd_smi` from `ROCM`.

```
make ROCM=/opt/rocm
make cbuild ROCM=/opt/rocm
```

## Usage

No arguments prints one row per GPU: temperature, power, clocks, fan, performance level, VRAM, and GPU use.

```
./rocm-smi
```

Limit a query to one device, or print a few metrics:

```
./rocm-smi -d 0 -t
./rocm-smi -t -u -P
./rocm-smi -a
```

| Flag | Meaning |
|------|---------|
| `-h`, `--help` | Full flag list |
| `-V`, `--version` | Tool and library version |
| `-d N` | Limit the command to device `N` |
| `-a` | Temperature, fan, clocks, power, identity, and related fields |
| `-t` / `-f` / `-P` / `-u` | Temperature, fan, power, GPU use |
| `-c` / `-g` | All current clocks, or sclk only |
| `--showmetrics` | GPU metrics block |
| `--json` / `--csv` | Machine-readable output (not for the concise table) |

Commands that change clocks, fan, power, or partitions need root. If you are not root, the tool re-executes itself with `sudo`.

```
./rocm-smi --setfan 50%
./rocm-smi --resetfans
```

Design details are in [doc/design.md](doc/design.md).
