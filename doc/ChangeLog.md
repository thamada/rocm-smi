# Change History

> **Note**:
>   This document is the change history. Timestamps come from the `date` command, in `YYYY-MM-DD HH:MM:SS` form.
>   Newest entries are listed first.

## 2026-10-14 17:24:55

**`rocm-smi.c`** — Concise and hardware tables build without `-Wformat-truncation`, and long partition and VBIOS strings fit in the cells.

#### Background

- `make` warned in `show_concise` and `show_hw`.
- The partition cell was 96 bytes. `"%s, %s, %s"` with three 31-character names needs up to 98 bytes.
- The VBIOS cell was 80 bytes. `part_number` and `version` are `AMDSMI_MAX_STRING_LENGTH` (256).
- Column padding used `snprintf` with `%-*s` and an `int` width. The compiler treated that width as unbounded.

#### `rocm-smi.c`

- Concise cells are 128 bytes. Hardware cells are `AMDSMI_MAX_STRING_LENGTH`. `snprintf` sizes come from `sizeof` the destination cell.
- `append_padded` left-aligns a column in a 320-byte buffer and appends it to the row. Padding stops at the remaining room in that buffer. The concise header, sub-header, and body, and the hardware header and body, use it.

#### `doc/design.md`

- Output describes the 128-byte concise cells, the 256-byte hardware cells, and the 319-character column cap. Limits lists the same truncation bounds.

**`doc/ChangeLog.md`**: this entry.

## 2026-10-14 14:46:13

**`rocm-smi.c` and `Makefile`** — C implementation of the ROCm SMI CLI 4.x front end, with `doc/design.md` written to match.

#### Background

- The repository held an empty `rocm-smi.c`, a placeholder `Makefile`, and stub `doc/` files.
- The goal is a single C binary that accepts the same flags and prints the same log layout and concise table as the ROCm `rocm-smi` tool, using `libamd_smi` instead of the Python front end.

#### `Makefile`

- Build `rocm-smi` from `rocm-smi.c` with `gcc`, C11, `-Wall -Wextra`, and `-D_GNU_SOURCE`.
- Headers and `libamd_smi` come from `/opt/rocm` (`ROCM`, `CPPFLAGS`, `LDFLAGS` with `-rpath`, `LIBS=-lamd_smi`).
- Targets: `all`, `build`, `run` (build then `./rocm-smi`), `clean`.

#### `rocm-smi.c`

- Version string `4.0.0+c`. Help text lists the CLI 4.x flag set (show, set, reset, topology, pages, RAS, events, JSON/CSV, log level, autorespond).
- Device discovery: live `amdgpu` check, `amdsmi_init` (GPU, then APU), socket or NULL-socket enumeration, up to 32 devices, DRM card/render ids, and a wake ioctl so runtime PM reports telemetry.
- Queries for temperature, clocks, power, fan, utilization, memory, partitions, firmware, RAS, topology, metrics, KFD processes, and pages. Unique id and memory-busy percent are read from sysfs when the library does not supply them.
- Text log spacers and `GPU[N]` lines match `rocm_smi.py`. No-argument invocation prints the concise table (Device, Node, DID/GUID, temp, power, partitions, SCLK, MCLK, fan, perf, power cap, VRAM%, GPU%).
- `--json` stores string fields per `cardN` / `system`. `--csv` prints one row per device. Concise and `--showhw` tables reject JSON/CSV.
- Mutating commands re-exec via `sudo` when not root. Overdrive, power cap, clock range, voltage curve, and memory partition ask for confirmation (`--autorespond` can answer).
- `--save` / `--load` use clock JSON version 1 (fan, sclk overdrive, performance level). Load does not apply clock masks or the stored profile.
- Explicitly unsupported, with a log line and no setter call: memory overdrive, reset-to-boot compute/memory partition, and RAS enable/disable/inject.
- `--setslevel` / `--setmlevel` write the given MHz as both ends of the clock range. The level index and millivolts are not sent separately.
- Default log level is `warning`. At that level and quieter, the process exit code is 0 even when a library call fails.

#### `doc/design.md`

- Describes the build, discovery, units, output formats, show/set/reset flags, sysfs fallbacks, unsupported flags, and limits.

**`doc/ChangeLog.md`**: this entry.
