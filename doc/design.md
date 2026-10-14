# Design Specification

> **Note**: This document describes the current design. For the history of changes, see `ChangeLog.md`.

## Overview

`rocm-smi` shipped with every ROCm release until 10.1.0 removed it. This program is that missing tool.

It matches the ROCm System Management Interface CLI 4.x: the same flags, the same log layout, and the same concise table. The Rust sources under `src/` are the default build. `rocm-smi.c` is the C implementation of the same CLI. Telemetry and controls go through `libamd_smi`. A few values that the library does not expose are read from sysfs.

The reported tool version is `4.0.0+c` (`SMI_VERSION`). It is a user-space replacement for the Python `rocm-smi` front end, not a reimplementation of the kernel driver.

Out of scope:

- A Python runtime, `rocm_smi.py`, or the historical `librocm_smi64` API.
- CPU, NIC, or non-AMD GPU management.
- Persistent daemon behavior. Event and temperature-graph modes poll until the user quits.

## Directory layout

```
rocm-smi/
├── Makefile          # `build` is Rust, `cbuild` is C; both write ./rocm-smi
├── README.md         # why this tool exists, and how to build and run it
├── Cargo.toml        # Rust package
├── build.rs          # bindgen against amd_smi/amdsmi.h
├── src/              # Rust CLI, queries, and controls
├── rocm-smi.c        # C implementation of the same CLI
└── doc/
    ├── design.md     # this document
    └── ChangeLog.md  # change history
```

The `rocm-smi` binary is a build product and is not part of the source tree.

## Build

`make install` uses `curl` to download the Rust standalone toolchain into `.rust/` and fetches crates into `.rust/cargo-home/`. `make build` depends on that install, then compiles offline with `.rust/bin/cargo` and links against AMD SMI. `make cbuild` compiles `rocm-smi.c`. The C flags below apply to `cbuild`. `ROCM` is also passed to the Rust build so bindgen and the linker use that prefix. Bindgen needs system `libclang`.

| Variable | Default | Role |
|----------|---------|------|
| `CC` | `gcc` | C compiler |
| `CFLAGS` | `-O2 -std=c11 -Wall -Wextra -D_GNU_SOURCE` | Language and warnings |
| `ROCM` | `/opt/rocm` | ROCm install prefix |
| `CPPFLAGS` | `-I$(ROCM)/include` | `amd_smi/amdsmi.h` |
| `LDFLAGS` | `-L$(ROCM)/lib -Wl,-rpath,$(ROCM)/lib` | Library search and runtime path |
| `LIBS` | `-lamd_smi` | AMD SMI |

| Target | Effect |
|--------|--------|
| `install` | Install Rust `1.99.0` and the crate cache under `.rust/` |
| `all` / `build` | Install if needed, then build the Rust sources offline and write `./rocm-smi` |
| `cbuild` | Compile `rocm-smi.c` and write `./rocm-smi` |
| `run` | `build`, then run `./rocm-smi` with no arguments (concise table) |
| `clean` | Remove `./rocm-smi` and the Cargo `target/` directory. `.rust/` stays |

`.rust/` is an install product and is not part of the source tree.

Example:

```
make
./rocm-smi
./rocm-smi -t -u
```

## Runtime flow

`main` does the following, in order:

1. Parse arguments. `-h` / `--help` prints usage and exits 0. Unknown flags exit 2.
2. If the command mutates device state and the process is not root, re-exec through `sudo` with the same argv.
3. Require a live `amdgpu` module (`driver_live`). If it is absent, print an error and exit 0.
4. Initialize AMD SMI and discover GPUs (`discover`). With no devices, print `ERROR: No DRM devices available. Exiting` and exit 1.
5. `-V` / `--version` prints the tool version and `amdsmi_get_lib_version`, then shuts down.
6. Select devices. `-d` / `--device` limits the set; an unknown id warns and exits 0. With no `-d`, every discovered device is selected.
7. `-a` / `--showallinfo` turns on the bundled show flags listed under [Show commands](#show-commands). Profile and clock-frequency tables stay off when `--json` or `--csv` is set.
8. Dispatch show, set, and reset handlers. With no action flags, print the concise table.
9. If `--json` or `--csv` collected rows, dump them (skipped for the concise table, which refuses those formats).
10. Call `amdsmi_shut_down` and return `g_retcode`.

At log level `warning` or quieter (the default, numeric level 30 or higher), a failed library call still prints a status line but the process exit code is forced to 0. Stricter levels (`error`, `critical`) keep a non-zero exit when a call fails.

## Device discovery

`driver_live` checks `/sys/module/amdgpu`. If `initstate` exists, it must contain `live`. If `initstate` is missing, the module directory alone counts as live.

`discover`:

1. Calls `amdsmi_init(AMDSMI_INIT_AMD_GPUS)`. On failure, retries `AMDSMI_INIT_AMD_APUS`.
2. Walks socket handles and keeps processors of type GPU, APU, or unknown. If the socket list is empty, asks `amdsmi_get_processor_handles_by_type` with a NULL socket for `AMDSMI_PROCESSOR_TYPE_AMD_GPU`.
3. Stores up to `MAX_DEV` (32) handles.
4. Reads DRM card and render-node numbers from `amdsmi_get_gpu_enumeration_info`.
5. Wakes runtime power management with `DRM_IOCTL_AMDGPU_INFO` (`DRM_IOCTL_AMDGPU_INFO_WAKE`, `0x40206445`) on `/dev/dri/renderDN` or, if there is no render node, `/dev/dri/cardN`. A failed open is ignored.

Device index `N` in the CLI is the discovery order, printed as `GPU[N]` and `cardN`.

## Queries and units

Library sentinels (`0xFFFF`, `0xFFFFFFFF`, `UINT64_MAX`) print as `N/A`.

| Value | Source | Notes |
|-------|--------|-------|
| Temperature | `amdsmi_get_temp_metric` | Millidegrees (`abs(v) > 1000`) are scaled to °C. Concise mode prefers edge, then junction, then VRAM, and labels the column to match. |
| Clocks | `amdsmi_get_clk_freq` | Hz to MHz. Names: `dcef`, `dclk0`, `dclk1`, `fclk` (DF), `mclk`, `pcie`, `sclk`, `socclk`, `vclk0`, `vclk1`. Aliases `soc` and `df` are accepted. Deep-sleep level 0 prints as `S`. |
| Power | `amdsmi_get_power_info` | Prefers average socket watts, then current socket, then `socket_power`. Values above `100000` are treated as microwatts. |
| Power cap | `amdsmi_get_power_cap_info` | Same microwatt rule. |
| Fan | `amdsmi_get_gpu_fan_speed` | Percent is `level * 100 / max`, default max 255. RPM from `amdsmi_get_gpu_fan_rpms`. |
| GPU busy | `amdsmi_get_gpu_busy_percent` | |
| Memory busy | sysfs `mem_busy_percent` | See [Sysfs fallbacks](#sysfs-fallbacks). |
| VRAM percent | used / total VRAM | |
| Device ID | `amdsmi_get_gpu_asic_info` | Printed as `0x...`. |
| GUID, node, partition id | `amdsmi_get_gpu_kfd_info` | GUID is the decimal KFD id. Node falls back to the device index. Partition id falls back to `0`. |
| Unique ID | sysfs | `0x` hex. See [Sysfs fallbacks](#sysfs-fallbacks). |
| VBIOS | `amdsmi_get_gpu_vbios_info` | Part number, else version string. |
| BDF | `amdsmi_get_gpu_device_bdf` | `DDDD:BB:DD.F`. |
| Memory partition | `amdsmi_get_gpu_memory_partition` | |
| Compute partition | accelerator partition profile | `SPX`, `DPX`, `TPX`, `QPX`, `CPX`. |
| Performance level | `amdsmi_get_gpu_perf_level` | `auto`, `low`, `high`, `manual`, `stable_std`, `stable_peak`, `stable_min_mclk`, `stable_min_sclk`, `determinism`. |
| Firmware | `amdsmi_get_fw_info` | VCN/VCE/UVD/SOS/ASD/MES print `0x` hex. SMC and TA blocks print `AA.BB.CC.DD`. |
| Metrics | `amdsmi_get_gpu_metrics_info` | One log line per field, with the unit in the metric name. `0xFFFF` / max uint fields print `N/A`. |
| KFD processes | `amdsmi_get_gpu_process_list` | Process name from `/proc/<pid>/comm`. |

Numeric text that must match Python `str(float)` uses `fmt_py_num` (`22` becomes `22.0`, trailing zeros after the decimal are stripped except one).

## Output

### Log lines

Text mode follows `rocm_smi.py` `printLog` / `printLogSpacer`.

- Spacers are `=` rules of width `APP_WIDTH` (90) or the measured table width. An odd display string is padded with one extra fill character so the rule stays even.
- Per-device lines are `GPU[N]\t\t: <metric>: <value>`.
- System lines omit the `GPU[N]` prefix.
- Section titles are centered inside the spacer (` ROCm System Management Interface `, ` End of ROCm SMI Log `, and a title per command).

`--loglevel` accepts `debug` (10), `info` (20), `warning` (30, default), `error` (40), and `critical` (50), case-insensitive. `log_warn` prints only at level 30 or below. `log_err` prints only at level 40 or below.

### Concise table

Printed when no show/set/reset action is requested: a bare `./rocm-smi`, or `--alldevices` / `--json` / `--csv` with no other action and no `-d`.

Columns: Device, Node, IDs (DID, GUID), Temp, Power, Partitions (Mem, Compute, ID), SCLK, MCLK, Fan, Perf, PwrCap, VRAM%, GPU%. The sub-header row is italic (`\033[3m`). `--json` and `--csv` are rejected for this table (`NOT_SUPPORTED`, exit 1). `--showhw` uses the same rejection for its hardware table.

Both tables left-align columns with `append_padded`. Each column is copied into a 320-byte buffer; the text plus spaces never exceed 319 characters, then the column is appended to the row. Concise cells are 128 bytes, enough for a partition field of three 31-character names (`"%s, %s, %s"`). Hardware cells are `AMDSMI_MAX_STRING_LENGTH` (256), so a VBIOS part number or version is stored in full.

### JSON and CSV

`--json` stores metric/value pairs instead of printing log lines. Objects are named `card<N>` or `system`. `json_dump` emits one JSON object of string values. Quotes and backslashes are escaped; bytes below `0x20` are dropped.

`--csv` implies `--json` and prints a header row of metric names with one row per selected device. Commas inside values are stripped. A system-only result with no devices prints `name, value`.

An empty JSON store warns `No JSON data to report`.

## Show commands

`-a` / `--showallinfo` enables: id, VBIOS, driver version, fan, power, temperature, GPU use, memory use, voltage, clocks, max power, sclk overdrive, memory overdrive, performance level, product, serial, unique id, bus, pages, memory vendor, sclk range, mclk range, voltage range, voltage curve, energy, PCIe replay, compute partition, memory partition, metrics, firmware (all blocks), KFD PIDs, and PID-to-GPU. In text mode it also enables the power profile and the supported clock and clock/voltage tables.

| Flag | Handler | What it prints |
|------|---------|----------------|
| *(none)* | `show_concise` | Concise table |
| `--showhw` | `show_hw` | GPU, node, DID, GUID, GFX version, GFX/SDMA/UMC RAS, VBIOS, bus, partition id |
| `-i` / `--showid` | `show_id` | Device id, revision, subsystem |
| `-v` / `--showvbios` | `show_vbios` | VBIOS part or version |
| `--showdriverversion` | `show_driver` | Kernel driver version |
| `-t` / `--showtemp` | `show_temp` | Edge, junction, memory, HBM temperatures |
| `-f` / `--showfan` | `show_fan` | Level, percent, RPM |
| `-P` / `--showpower` | `show_power` | Socket power and type label |
| `-u` / `--showuse` | `show_use` | GPU busy percent |
| `--showmemuse` | `show_memuse` | VRAM, VIS, GTT used and total |
| `--showvoltage` | `show_voltage` | Current voltage |
| `-c` / `--showclocks` | `show_clocks_current` | Every known clock |
| `-g` / `--showgpuclocks` | `show_clocks_current` | `sclk` only |
| `-s` / `--showclkfrq` | `show_clocks_supported` | Supported frequencies |
| `-S` / `--showclkvolt` | `show_clkvolt` | Supported clocks and voltages |
| `-p` / `--showperflevel` | `show_perf` | DPM performance level |
| `-o` / `--showoverdrive` | `show_overdrive` | sclk OverDrive |
| `-m` / `--showmemoverdrive` | `show_overdrive` | mclk OverDrive |
| `-M` / `--showmaxpower` | `show_maxpower` | Power cap |
| `-l` / `--showprofile` | `show_profile` | Power profile preset |
| `-b` / `--showbw` | `show_bw` | Estimated PCIe bandwidth |
| `--showreplaycount` | `show_replay` | PCIe replay count |
| `--showmeminfo TYPE...` | `show_meminfo` | vram, vis_vram, gtt |
| `--showmemvendor` | `show_memvendor` | Memory vendor |
| `--showproductname` | `show_product` | Marketing and vendor names |
| `--showserial` | `show_serial` | Serial number |
| `--showuniqueid` | `show_uid` | 64-bit unique id |
| `--showbus` | `show_bus` | PCI BDF |
| `--showfwinfo [BLOCK...]` | `show_fw` | Firmware versions; no args means all known blocks |
| `--showsclkrange` / `--showmclkrange` / `--showvoltagerange` | `show_range` | Min/max |
| `--showvc` | `show_vc` | Voltage curve points |
| `--showpagesinfo` | `show_pages` | Retired, pending, and unreservable |
| `--showretiredpages` / `--showpendingpages` / `--showunreservablepages` | `show_pages` | One page class |
| `--showrasinfo [BLOCK...]` | `show_ras` | RAS state and error counts. Blocks: UMC, SDMA, GFX, MMHUB, ATHUB, PCIE_BIF, HDP, XGMI_WAFL, DF, SMN, SEM, MP0, MP1, FUSE, MCA, VCN, JPEG |
| `--showxgmierr` | `show_xgmi` | XGMI error status and count |
| `--showtopo` | weight, hops, type, NUMA | Combined topology |
| `--showtopoweight` / `--showtopohops` / `--showtopotype` / `--showtopoaccess` / `--showtoponuma` | one matrix each | Link weight, hops, type (`PCIE`, `XGMI`, `INTERNAL`, `NUMA`, else `XXXX`), access, NUMA node |
| `--shownodesbw` | `show_nodes_bw` | Bandwidth between NUMA nodes |
| `--showcomputepartition` / `--showmemorypartition` | `show_partitions` | Current partition |
| `--showmetrics` | `show_metrics` | GPU metrics table fields |
| `--showenergycounter` | `show_energy` | Energy accumulator |
| `--showpids [VERBOSE]` | `show_pids` | KFD PIDs. `details` changes the GPU column label; default verbosity is `summary` |
| `--showpidgpus [PID...]` | `show_pidgpus` | DRM devices used by each PID; no PID means all |
| `-e` / `--showevents [EVENT...]` | `show_events` | Blocks until `q` and Enter. Empty type list selects all 13 events: VMFAULT, THERMAL_THROTTLE, GPU_PRE_RESET, GPU_POST_RESET, MIGRATE_START, MIGRATE_END, PAGE_FAULT_START, PAGE_FAULT_END, QUEUE_EVICTION, QUEUE_RESTORE, UNMAP_FROM_GPU, PROCESS_START, PROCESS_END. Unknown names are ignored. Poll timeout is 1000 ms |
| `--showtempgraph` | `show_tempgraph` | 30 samples, 0.5 s apart, 60-column bar of edge (else junction) temperature |

## Set, reset, and files

Mutating flags require root. If `geteuid()` is not 0, the process replaces itself with `sudo` plus the original argv. A failed `execvp` exits 1.

Overclock, power-cap, clock-range, voltage-curve, and memory-partition changes call `confirm_danger` unless `--autorespond` is set. The prompt expects `y` or `Y`. Anything else aborts. `--autorespond` is applied to every such prompt; the help text marks it as not recommended.

| Flag | Behavior |
|------|----------|
| `--setperflevel LEVEL` | `amdsmi_set_gpu_perf_level`. Names match the show list. |
| `--setfan LEVEL` | Absolute speed, or a trailing `%` scaled by the fan max. |
| `--resetfans` | `amdsmi_reset_gpu_fan` (driver control). |
| `--setclock TYPE LEVEL` / `--setsclk` / `--setmclk` / `--setpcie` | Bitmask of levels 0–63 via `amdsmi_set_clk_freq`. |
| `--setsrange MIN MAX` / `--setmrange MIN MAX` | `amdsmi_set_gpu_od_clk_info` min and max. |
| `--setextremum min\|max sclk\|mclk MHZ` | `amdsmi_set_gpu_clk_limit`. |
| `--setslevel L MHZ MV` / `--setmlevel L MHZ MV` | Applied as a range whose min and max are both `MHZ`. The level index and millivolts are not written separately. |
| `--setvc POINT MHZ MV` | `amdsmi_set_gpu_od_volt_info`. |
| `--setoverdrive %` | `amdsmi_set_gpu_overdrive_level`. |
| `--setpoweroverdrive WATTS` / `--resetpoweroverdrive` | `amdsmi_set_power_cap`. Reset uses the default cap when the library reports one. |
| `--setprofile PROFILE` | Preset name or a number 1–7 (`1 << (n-1)`). Names: CUSTOM, VIDEO, POWER SAVING, COMPUTE, VR, 3D FULL SCREEN, BOOTUP DEFAULT. |
| `--resetprofile` | Sets BOOTUP DEFAULT. |
| `--setperfdeterminism SCLK` | `amdsmi_set_gpu_perf_determinism_mode`. |
| `--resetperfdeterminism` | Sets performance level `auto`. |
| `-r` / `--resetclocks` | OverDrive 0 and performance level `auto`. |
| `--setmemorypartition {NPS1,NPS2,NPS4,NPS8}` | `amdsmi_set_gpu_memory_partition` after confirmation. |
| `--setcomputepartition {CPX,SPX,DPX,TPX,QPX}` | Looks up the profile index, then `amdsmi_set_gpu_accelerator_partition_profile`. |
| `--resetxgmierr` | `amdsmi_reset_gpu_xgmi_error`. |
| `--gpureset` | Exactly one `-d` device. `amdsmi_reset_gpu`. Returns before other actions. |
| `--save FILE` | Refuses to overwrite. Writes `vJson` 1, per-clock current level, fan, sclk overdrive, mclk overdrive `"0"`, profile `"1"`, and perf level. |
| `--load FILE` | Minimal JSON scan for `"cardN"` objects. Rejects `vJson` other than 1. Restores fan (if not 0), sclk overdrive (if not 0), and perf level (if not `Unsupported`). |

`AMDSMI_STATUS_NOT_SUPPORTED` prints `Not supported on the given system`. `AMDSMI_STATUS_NO_PERM` prints `Permission denied`. Other failures print `Failed: ...` and set `g_retcode`.

## Sysfs fallbacks

Used only where `libamd_smi` does not return the value the CLI prints.

| Path | Use |
|------|-----|
| `/sys/module/amdgpu`, `.../initstate` | Driver-live check |
| `/sys/class/drm/cardN/device/mem_busy_percent` | Memory busy percent |
| `/sys/class/drm/cardN/device/unique_id` | Unique id, parsed as hex |
| `/sys/class/kfd/kfd/topology/nodes/<node>/properties` | `unique_id` line if the DRM file is missing |
| `/proc/<pid>/comm` | KFD process name |

## Not supported on this build

These flags are parsed and print an explicit not-supported section. They do not call a library setter:

- `--setmemoverdrive`
- `--resetcomputepartition`
- `--resetmemorypartition`
- `--rasenable`, `--rasdisable`, `--rasinject`

Concise output and `--showhw` refuse `--json` and `--csv`.

## Limits

- At most 32 devices.
- Concise table cells are truncated at 127 bytes. Hardware table cells are truncated at 255 bytes.
- One padded table column is at most 319 characters.
- JSON values are truncated to 511 bytes; keys to 191.
- `--showevents` and `--showtempgraph` are foreground loops, not background monitors.
- `--load` does not restore per-clock masks, memory overdrive, or the power profile string stored by `--save`.
