# rocm-smi

ROCm 10.1.0 removed the `rocm-smi` tool, so this program was written as a replacement.

C command-line tool compatible with the ROCm System Management Interface (CLI 4.x). It prints the same concise table and log layout, and reads GPU telemetry through `libamd_smi`.

Requires a live `amdgpu` driver and ROCm under `/opt/rocm` (override with `make ROCM=/path/to/rocm`).

## Build

```
make
```

`make run` builds the binary and prints the concise table. `make clean` removes `./rocm-smi`.

## Usage

No arguments prints one row per GPU: temperature, power, clocks, fan, performance level, VRAM, and GPU use.

```
./rocm-smi
```

Show one device, or a few metrics:

```
./rocm-smi -d 0
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
