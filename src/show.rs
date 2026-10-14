//! Show commands. Layout matches `rocm-smi.c`.

use crate::core::*;
use crate::print_logf;
use std::io::Write;

pub fn show_version() {
    let mut v: amdsmi_version_t = z();
    println!("ROCM-SMI version: {SMI_VERSION}");
    if ok(unsafe { amdsmi_get_lib_version(&mut v) }) {
        println!("ROCM-SMI-LIB version: {}.{}.{}", v.major, v.minor, v.release);
    }
}

pub fn show_concise(devs: &[i32]) {
    if ctx().json {
        println!("NOT_SUPPORTED: Cannot print JSON/CSV output for concise output");
        std::process::exit(1);
    }
    let headers = [
        "Device", "Node", "IDs", "", "Temp", "Power", "Partitions", "SCLK", "MCLK", "Fan", "Perf", "PwrCap",
        "VRAM%", "GPU%",
    ];
    let mut subs = vec![String::new(); 14];
    let mut cells: Vec<Vec<String>> = Vec::new();
    let mut temp_label = "(Edge)".to_string();
    let mut power_label = "(Avg)".to_string();
    let nd = devs.len();
    if nd > 0 {
        let d0 = devs[0] as usize;
        if temp_c(d0, amdsmi_temperature_type_t::AMDSMI_TEMPERATURE_TYPE_EDGE).is_none() {
            if temp_c(d0, amdsmi_temperature_type_t::AMDSMI_TEMPERATURE_TYPE_JUNCTION).is_some() {
                temp_label = "(Junction)".into();
            } else if temp_c(d0, amdsmi_temperature_type_t::AMDSMI_TEMPERATURE_TYPE_VRAM).is_some() {
                temp_label = "(Memory)".into();
            }
        }
        if let Some(p) = power_watts(d0) {
            power_label = p.label.into();
        }
    }
    subs[2] = "(DID,".into();
    subs[3] = "GUID)".into();
    subs[4] = temp_label.clone();
    subs[5] = power_label;
    subs[6] = "(Mem, Compute, ID)".into();
    for c in 0..14 {
        let hs = headers[c].len();
        let ss = subs[c].len();
        if hs > ss {
            subs[c].push_str(&" ".repeat(hs - ss));
        }
    }
    let mut widths = [0i32; 14];
    for c in 0..14 {
        widths[c] = headers[c].len() as i32 + 2;
    }
    for i in 0..nd {
        let d = devs[i] as usize;
        let mut row = vec![String::new(); 14];
        let did = device_id_str(d);
        let guid = guid_str(d);
        let mp = mem_part(d);
        let cp = comp_part(d);
        let pid = part_id_str(d);
        let mut perf = "N/A".to_string();
        let mut sclk = "N/A".to_string();
        let mut mclk = "N/A".to_string();
        let ts = if temp_label.contains("Junction") {
            amdsmi_temperature_type_t::AMDSMI_TEMPERATURE_TYPE_JUNCTION
        } else if temp_label.contains("Memory") {
            amdsmi_temperature_type_t::AMDSMI_TEMPERATURE_TYPE_VRAM
        } else {
            amdsmi_temperature_type_t::AMDSMI_TEMPERATURE_TYPE_EDGE
        };
        let tc = temp_c(d, ts);
        if let Some(c) = current_clk_mhz(d, amdsmi_clk_type_t::AMDSMI_CLK_TYPE_SYS) {
            sclk = format!("{:.0}Mhz", c.mhz);
        }
        if let Some(c) = current_clk_mhz(d, amdsmi_clk_type_t::AMDSMI_CLK_TYPE_MEM) {
            mclk = format!("{:.0}Mhz", c.mhz);
        }
        row[9] = match fan_level_pct(d) {
            Ok(f) => format!("{}%", f.pct),
            Err(_) => "N/A".into(),
        };
        let mut pl = 0;
        if ok(unsafe { amdsmi_get_gpu_perf_level(dev_handle(d), &mut pl) }) {
            if let Some(n) = perf_name(pl) {
                perf = n.into();
            }
        }
        let vp = vram_pct(d);
        row[0] = sn(128, &format!("{d}"));
        row[1] = sn(128, &format!("{}", node_id(d)));
        row[2] = sn(128, &format!("{did}, "));
        row[3] = sn(128, &guid);
        row[4] = sn(128, &match tc {
            Some(t) => format!("{t:.1}\u{00B0}C"),
            None => "N/A".into(),
        });
        row[5] = sn(128, &match power_watts(d) {
            Some(p) if p.w != 0.0 => format!("{:.1}W", p.w),
            _ => "N/A".into(),
        });
        row[6] = sn(128, &format!("{mp}, {cp}, {pid}"));
        row[7] = sn(128, &sclk);
        row[8] = sn(128, &mclk);
        row[10] = sn(128, &perf);
        row[11] = sn(128, &match power_cap_w(d) {
            Some(w) => format!("{w:.1}W"),
            None => "N/A".into(),
        });
        row[12] = sn(128, &match vp {
            Some(v) => format!("{v}%"),
            None => "N/A".into(),
        });
        row[13] = sn(128, &match gpu_busy(d) {
            Some(b) => format!("{b}%"),
            None => "N/A".into(),
        });
        for c in 0..14 {
            let l = row[c].len() as i32 + 2;
            if l > widths[c] {
                widths[c] = l;
            }
        }
        cells.push(row);
    }
    let mut hdrpad = headers.iter().map(|h| sn(64, h)).collect::<Vec<_>>();
    for c in 0..14 {
        if subs[c].len() > hdrpad[c].len() {
            let extra = subs[c].len() - hdrpad[c].len();
            hdrpad[c].push_str(&" ".repeat(extra));
        }
        if hdrpad[c].len() as i32 + 2 > widths[c] {
            widths[c] = hdrpad[c].len() as i32 + 2;
        }
    }
    let mut line = String::new();
    for c in 0..14 {
        append_padded(&mut line, widths[c], &hdrpad[c]);
    }
    let wsum = line.len() as i32;
    print_spacer(Some(HEADER), "=", wsum);
    print_spacer(Some(" Concise Info "), "=", wsum);
    println!("{line}");
    line.clear();
    for c in 0..14 {
        append_padded(&mut line, widths[c], &subs[c]);
    }
    println!("\u{1b}[3m{line}\u{1b}[0m");
    print_spacer(None, "=", wsum);
    for row in &cells {
        line.clear();
        for c in 0..14 {
            append_padded(&mut line, widths[c], &row[c]);
        }
        println!("{line}");
    }
    print_spacer(None, "=", wsum);
    print_spacer(Some(FOOTER), "=", wsum);
}

pub fn show_hw(devs: &[i32]) {
    if ctx().json {
        println!("NOT_SUPPORTED: Cannot print JSON/CSV output for concise hardware output");
        std::process::exit(1);
    }
    let headers = [
        "GPU", "NODE", "DID", "GUID", "GFX VER", "GFX RAS", "SDMA RAS", "UMC RAS", "VBIOS", "BUS", "PARTITION ID",
    ];
    let mut widths = [0i32; 11];
    for c in 0..11 {
        widths[c] = headers[c].len() as i32 + 2;
    }
    let mut cells = Vec::new();
    for &id in devs {
        let d = id as usize;
        let mut a: amdsmi_asic_info_t = z();
        let mut vb: amdsmi_vbios_info_t = z();
        unsafe {
            amdsmi_get_gpu_asic_info(dev_handle(d), &mut a);
            amdsmi_get_gpu_vbios_info(dev_handle(d), &mut vb);
        }
        let mut row = vec![String::new(); 11];
        row[0] = sn(256, &format!("{d}"));
        row[1] = sn(256, &format!("{}", node_id(d)));
        row[2] = sn(256, &device_id_str(d));
        row[3] = sn(256, &guid_str(d));
        let gfx = if a.target_graphics_version != u64::MAX && a.target_graphics_version != 0 {
            format!("gfx{:x}", a.target_graphics_version)
        } else {
            "N/A".into()
        };
        row[4] = sn(256, &gfx);
        for (col, block) in [
            (5, amdsmi_gpu_block_t::AMDSMI_GPU_BLOCK_GFX),
            (6, amdsmi_gpu_block_t::AMDSMI_GPU_BLOCK_SDMA),
            (7, amdsmi_gpu_block_t::AMDSMI_GPU_BLOCK_UMC),
        ] {
            let mut st = 0;
            row[col] = if ok(unsafe { amdsmi_get_gpu_ecc_status(dev_handle(d), block, &mut st) }) {
                sn(256, ras_state_name(st))
            } else {
                "N/A".into()
            };
        }
        row[8] = sn(256, &vbios_ver(&vb));
        row[9] = sn(256, &bdf_str(d));
        row[10] = sn(256, &part_id_str(d));
        for c in 0..11 {
            let l = row[c].len() as i32 + 2;
            if l > widths[c] {
                widths[c] = l;
            }
        }
        cells.push(row);
    }
    let mut line = String::new();
    for c in 0..11 {
        append_padded(&mut line, widths[c], headers[c]);
    }
    let wsum = line.len() as i32;
    print_spacer(Some(" Concise Hardware Info "), "=", wsum);
    println!("{line}");
    for row in &cells {
        line.clear();
        for c in 0..11 {
            append_padded(&mut line, widths[c], &row[c]);
        }
        println!("{line}");
    }
    print_spacer(None, "=", wsum);
}

pub fn show_id(devs: &[i32]) {
    print_spacer(Some(" ID "), "=", 0);
    for &id in devs {
        let d = id as usize;
        let mut a: amdsmi_asic_info_t = z();
        unsafe { amdsmi_get_gpu_asic_info(dev_handle(d), &mut a); }
        let did = device_id_str(d);
        let rev = if a.rev_id != 0xFFFF_FFFF { format!("0x{:02x}", a.rev_id) } else { "N/A".into() };
        let sub = if a.subsystem_id != 0 { format!("0x{:04x}", a.subsystem_id) } else { "N/A".into() };
        let guid = guid_str(d);
        let name = cstr_arr(&a.market_name);
        print_logf!(id, Some("Device Name"), "\t\t{}", if name.is_empty() { "N/A" } else { &name });
        print_logf!(id, Some("Device ID"), "\t\t{did}");
        print_logf!(id, Some("Device Rev"), "\t\t{rev}");
        print_logf!(id, Some("Subsystem ID"), "\t{sub}");
        print_logf!(id, Some("GUID"), "\t\t{guid}");
    }
    print_spacer(None, "=", 0);
}

pub fn show_vbios(devs: &[i32]) {
    print_spacer(Some(" VBIOS "), "=", 0);
    for &id in devs {
        let mut v: amdsmi_vbios_info_t = z();
        if ok(unsafe { amdsmi_get_gpu_vbios_info(dev_handle(id as usize), &mut v) }) {
            print_log(id, Some("VBIOS version"), Some(&vbios_ver(&v)));
        } else {
            print_log(id, Some("VBIOS version"), Some("N/A"));
        }
    }
    print_spacer(None, "=", 0);
}

pub fn show_driver(devs: &[i32]) {
    print_spacer(Some(" Version of System Component "), "=", 0);
    let mut info: amdsmi_driver_info_t = z();
    let ver = if !devs.is_empty()
        && ok(unsafe { amdsmi_get_gpu_driver_info(dev_handle(devs[0] as usize), &mut info) })
    {
        let s = cstr_arr(&info.driver_version);
        if s.is_empty() {
            read_file("/sys/module/amdgpu/version", 128).unwrap_or_else(|| "N/A".into())
        } else {
            s
        }
    } else {
        read_file("/sys/module/amdgpu/version", 128).unwrap_or_else(|| "N/A".into())
    };
    if ctx().json {
        json_add(-1, "Driver version", &ver);
    } else {
        println!("Driver version: {ver}");
    }
    print_spacer(None, "=", 0);
}

pub fn show_temp(devs: &[i32]) {
    let s = [
        ("edge", amdsmi_temperature_type_t::AMDSMI_TEMPERATURE_TYPE_EDGE),
        ("junction", amdsmi_temperature_type_t::AMDSMI_TEMPERATURE_TYPE_JUNCTION),
        ("memory", amdsmi_temperature_type_t::AMDSMI_TEMPERATURE_TYPE_VRAM),
        ("hbm0", amdsmi_temperature_type_t::AMDSMI_TEMPERATURE_TYPE_HBM_0),
        ("hbm1", amdsmi_temperature_type_t::AMDSMI_TEMPERATURE_TYPE_HBM_1),
        ("hbm2", amdsmi_temperature_type_t::AMDSMI_TEMPERATURE_TYPE_HBM_2),
        ("hbm3", amdsmi_temperature_type_t::AMDSMI_TEMPERATURE_TYPE_HBM_3),
    ];
    print_spacer(Some(" Temperature "), "=", 0);
    for &id in devs {
        for (n, t) in s {
            if let Some(v) = temp_c(id as usize, t) {
                let m = format!("Temperature (Sensor {n}) (C)");
                print_logf!(id, Some(&m), "{v:.1}");
            }
        }
    }
    print_spacer(None, "=", 0);
}

pub fn show_fan(devs: &[i32]) {
    print_spacer(Some(" Current Fan Metric "), "=", 0);
    for &id in devs {
        match fan_level_pct(id as usize) {
            Err(st) if st == amdsmi_status_t::AMDSMI_STATUS_NOT_SUPPORTED => {
                print_log(id, Some("Not supported"), None);
            }
            Err(_) => print_logf!(id, None, "Unable to detect fan speed for GPU {id}"),
            Ok(f) if f.level == 0 || f.pct == 0 => {
                print_logf!(id, None, "Unable to detect fan speed for GPU {id}");
            }
            Ok(f) => {
                if ctx().json {
                    print_logf!(id, Some("Fan speed (level)"), "{}", f.level);
                    print_logf!(id, Some("Fan speed (%)"), "{}", f.pct);
                } else {
                    print_logf!(id, Some("Fan Level"), "{} ({}%)", f.level, f.pct);
                }
                if f.rpm >= 0 {
                    print_logf!(id, Some("Fan RPM"), "{}", f.rpm);
                }
            }
        }
    }
    print_spacer(None, "=", 0);
}

pub fn show_power(devs: &[i32]) {
    print_spacer(Some(" Power Consumption "), "=", 0);
    for &id in devs {
        let Some(p) = power_watts(id as usize) else { continue };
        let metric = if p.ptype == "CURRENT SOCKET" {
            "Current Socket Graphics Package Power (W)"
        } else {
            "Average Graphics Package Power (W)"
        };
        let num = fmt_py_num(p.w);
        print_log(id, Some(metric), Some(&num));
    }
    print_spacer(None, "=", 0);
}

pub fn show_use(devs: &[i32]) {
    print_spacer(Some(" % time GPU is busy "), "=", 0);
    for &id in devs {
        match gpu_busy(id as usize) {
            Some(p) => print_logf!(id, Some("GPU use (%)"), "{p}"),
            None => print_log(id, Some("GPU use Unsupported"), None),
        }
        let mut u: amdsmi_utilization_counter_t = z();
        u.type_ = amdsmi_utilization_counter_type_t::AMDSMI_COARSE_GRAIN_GFX_ACTIVITY;
        let mut ts = 0u64;
        if ok(unsafe { amdsmi_get_utilization_count(dev_handle(id as usize), &mut u, 1, &mut ts) })
            && util_value_ok(u.value)
        {
            print_logf!(id, Some("GFX Activity"), "{}", u.value);
        } else {
            print_log(id, Some("GFX Activity"), Some("N/A"));
        }
    }
    print_spacer(None, "=", 0);
}

pub fn show_memuse(devs: &[i32]) {
    print_spacer(Some(" Current Memory Use "), "=", 0);
    for &id in devs {
        match mem_busy_pct(id as usize) {
            Some(b) => print_logf!(id, Some("GPU memory use (%)"), "{b}"),
            None => print_log(id, Some("GPU memory use Unsupported"), None),
        }
        let mut u: amdsmi_utilization_counter_t = z();
        u.type_ = amdsmi_utilization_counter_type_t::AMDSMI_COARSE_GRAIN_MEM_ACTIVITY;
        let mut ts = 0u64;
        if ok(unsafe { amdsmi_get_utilization_count(dev_handle(id as usize), &mut u, 1, &mut ts) })
            && util_value_ok(u.value)
        {
            print_logf!(id, Some("Memory Activity"), "{}", u.value);
        } else {
            print_log(id, Some("Memory Activity"), Some("N/A"));
        }
    }
    print_spacer(None, "=", 0);
}

pub fn show_voltage(devs: &[i32]) {
    print_spacer(Some(" Current voltage "), "=", 0);
    for &id in devs {
        let mut mv = 0i64;
        if ok(unsafe {
            amdsmi_get_gpu_volt_metric(
                dev_handle(id as usize),
                amdsmi_voltage_type_t::AMDSMI_VOLT_TYPE_VDDGFX,
                amdsmi_voltage_metric_t::AMDSMI_VOLT_CURRENT,
                &mut mv,
            )
        }) {
            print_logf!(id, Some("Voltage (mV)"), "{mv}");
        }
    }
    print_spacer(None, "=", 0);
}

pub fn show_clocks_current(devs: &[i32], only: Option<&str>) {
    if only.is_none() {
        print_spacer(Some(" Current clock frequencies "), "=", 0);
    }
    for &id in devs {
        for c in clks() {
            if let Some(only) = only {
                if !str_ieq(only, c.name) && !(str_ieq(only, "sclk") && c.ty == amdsmi_clk_type_t::AMDSMI_CLK_TYPE_SYS) {
                    continue;
                }
            }
            if c.ty == amdsmi_clk_type_t::AMDSMI_CLK_TYPE_PCIE {
                let mut bw: amdsmi_pcie_bandwidth_t = z();
                if !ok(unsafe { amdsmi_get_gpu_pci_bandwidth(dev_handle(id as usize), &mut bw) }) {
                    continue;
                }
                if bw.transfer_rate.current >= bw.transfer_rate.num_supported {
                    continue;
                }
                let cur = bw.transfer_rate.current as usize;
                print_logf!(
                    id,
                    Some("pcie clock level"),
                    "{} ({:.1}GT/s x{})",
                    bw.transfer_rate.current,
                    bw.transfer_rate.frequency[cur] as f64 / 1e9,
                    bw.lanes[cur]
                );
                continue;
            }
            let Some(cur) = current_clk_mhz(id as usize, c.ty) else { continue };
            let lv = fmt_level(cur.level, cur.deep);
            if only.is_some() {
                let metric = format!("{} clock level", c.name);
                print_logf!(id, Some(&metric), "{lv} ({:.0}Mhz)", cur.mhz);
            } else if ctx().json {
                let a = format!("{} clock speed:", c.name);
                let b = format!("{} clock level:", c.name);
                print_logf!(id, Some(&a), "({:.0}Mhz)", cur.mhz);
                print_log(id, Some(&b), Some(&lv));
            } else {
                let a = format!("{} clock level: {lv}", c.name);
                print_logf!(id, Some(&a), "({:.0}Mhz)", cur.mhz);
            }
        }
    }
    if only.is_none() {
        print_spacer(None, "=", 0);
    }
}

pub fn show_clocks_supported(devs: &[i32]) {
    print_spacer(Some(" Supported clock frequencies "), "=", 0);
    for (i, &id) in devs.iter().enumerate() {
        for c in clks() {
            if c.ty == amdsmi_clk_type_t::AMDSMI_CLK_TYPE_PCIE {
                let mut bw: amdsmi_pcie_bandwidth_t = z();
                if !ok(unsafe { amdsmi_get_gpu_pci_bandwidth(dev_handle(id as usize), &mut bw) }) {
                    continue;
                }
                print_logf!(id, None, "Supported PCIe frequencies on GPU{id}");
                for j in 0..bw.transfer_rate.num_supported as usize {
                    let idx = format!("{j}");
                    let star = if j == bw.transfer_rate.current as usize { " *" } else { "" };
                    let fr = format!(
                        "{:.1}GT/s x{}{star}",
                        bw.transfer_rate.frequency[j] as f64 / 1e9,
                        bw.lanes[j]
                    );
                    print_log(id, Some(&idx), Some(&fr));
                }
                print_log(id, Some(""), None);
                continue;
            }
            let mut f: amdsmi_frequencies_t = z();
            if !ok(unsafe { amdsmi_get_clk_freq(dev_handle(id as usize), c.ty, &mut f) }) || f.num_supported == 0 {
                continue;
            }
            print_logf!(id, None, "Supported {} frequencies on GPU{id}", c.name);
            let deep = f.has_deep_sleep;
            for j in 0..f.num_supported as usize {
                let idx = fmt_level(j as i32, deep);
                let star = if j == f.current as usize { " *" } else { "" };
                let fr = format!("{:.0}Mhz{star}", f.frequency[j] as f64 / 1e6);
                print_log(id, Some(&idx), Some(&fr));
            }
            print_log(id, Some(""), None);
        }
        if i + 1 < devs.len() {
            print_spacer(None, "-", 0);
        }
    }
    print_spacer(None, "=", 0);
}

pub fn show_perf(devs: &[i32]) {
    print_spacer(Some(" Show Performance Level "), "=", 0);
    for &id in devs {
        let mut p = 0;
        if ok(unsafe { amdsmi_get_gpu_perf_level(dev_handle(id as usize), &mut p) }) {
            if let Some(n) = perf_name(p) {
                print_log(id, Some("Performance Level"), Some(n));
                continue;
            }
        }
        print_log(id, Some("Performance Level Unsupported"), None);
    }
    print_spacer(None, "=", 0);
}

pub fn show_overdrive(devs: &[i32], which: &str) {
    print_spacer(Some(" OverDrive Level "), "=", 0);
    for &id in devs {
        let mut od = 0u32;
        let st = unsafe {
            if which == "mclk" {
                amdsmi_get_gpu_mem_overdrive_level(dev_handle(id as usize), &mut od)
            } else {
                amdsmi_get_gpu_overdrive_level(dev_handle(id as usize), &mut od)
            }
        };
        if !ok(st) {
            continue;
        }
        let metric = if which == "mclk" {
            "GPU Memory OverDrive value (%)"
        } else {
            "GPU OverDrive value (%)"
        };
        print_logf!(id, Some(metric), "{od}");
    }
    print_spacer(None, "=", 0);
}

pub fn show_maxpower(devs: &[i32]) {
    print_spacer(Some(" Power Cap "), "=", 0);
    for &id in devs {
        match power_cap_w(id as usize) {
            Some(w) => print_logf!(id, Some("Max Graphics Package Power (W)"), "{w:.1}"),
            None => print_log(id, Some("Max Graphics Package Power Unsupported"), None),
        }
    }
    print_spacer(None, "=", 0);
}

pub fn show_bus(devs: &[i32]) {
    print_spacer(Some(" PCI Bus ID "), "=", 0);
    for &id in devs {
        let b = bdf_str(id as usize);
        print_log(id, Some("PCI Bus"), Some(&b));
    }
    print_spacer(None, "=", 0);
}

pub fn show_meminfo(devs: &[i32], types: &[String]) {
    let all = [
        ("VRAM", amdsmi_memory_type_t::AMDSMI_MEM_TYPE_VRAM),
        ("VIS_VRAM", amdsmi_memory_type_t::AMDSMI_MEM_TYPE_VIS_VRAM),
        ("GTT", amdsmi_memory_type_t::AMDSMI_MEM_TYPE_GTT),
    ];
    let nt = types.len();
    print_spacer(Some(" Memory Usage (Bytes) "), "=", 0);
    for &id in devs {
        let limit = if nt == 0 { 3 } else { nt };
        for t in 0..limit {
            let name_owned = if nt == 0 { all[t].0.to_string() } else { types[t].clone() };
            let mut name = name_owned.as_str();
            let mut mt = amdsmi_memory_type_t::AMDSMI_MEM_TYPE_VRAM;
            let mut found = false;
            for (n, ty) in all {
                if str_ieq(name, n) || str_ieq(name, "all") {
                    mt = ty;
                    found = true;
                    name = n;
                    break;
                }
            }
            let all_one = nt == 1 && str_ieq(&types[0], "all");
            if !found && !all_one {
                print_logf!(id, Some("Invalid memory type"), "{}", types[t]);
                continue;
            }
            if all_one {
                for (n, ty) in all {
                    let (u, tot) = mem_bytes(id as usize, ty).unwrap_or((0, 0));
                    let up = format!("{n} Total Memory (B)");
                    print_logf!(id, Some(&up), "{tot}");
                    let up = format!("{n} Total Used Memory (B)");
                    print_logf!(id, Some(&up), "{u}");
                }
                break;
            }
            let (u, tot) = mem_bytes(id as usize, mt).unwrap_or((0, 0));
            let up = format!("{name} Total Memory (B)");
            print_logf!(id, Some(&up), "{tot}");
            let up = format!("{name} Total Used Memory (B)");
            print_logf!(id, Some(&up), "{u}");
        }
    }
    print_spacer(None, "=", 0);
}

pub fn show_memvendor(devs: &[i32]) {
    print_spacer(Some(" Memory Vendor "), "=", 0);
    for &id in devs {
        let mut brand = [0 as std::os::raw::c_char; 256];
        if ok(unsafe { amdsmi_get_gpu_vram_vendor(dev_handle(id as usize), brand.as_mut_ptr(), brand.len() as u32) }) {
            let s = cstr_arr(&brand);
            if !s.is_empty() {
                print_log(id, Some("GPU memory vendor"), Some(&s));
            }
        }
    }
    print_spacer(None, "=", 0);
}

pub fn show_product(devs: &[i32]) {
    print_spacer(Some(" Product Info "), "=", 0);
    for &id in devs {
        let d = id as usize;
        let mut a: amdsmi_asic_info_t = z();
        let mut v: amdsmi_vbios_info_t = z();
        unsafe {
            amdsmi_get_gpu_asic_info(dev_handle(d), &mut a);
            amdsmi_get_gpu_vbios_info(dev_handle(d), &mut v);
        }
        let did = device_id_str(d);
        let rev = if a.rev_id != 0xFFFF_FFFF { format!("0x{:02x}", a.rev_id) } else { "N/A".into() };
        let sub = if a.subsystem_id != 0 { format!("0x{:04x}", a.subsystem_id) } else { "N/A".into() };
        let guid = guid_str(d);
        let gfx = if a.target_graphics_version != 0 && a.target_graphics_version != u64::MAX {
            format!("gfx{:x}", a.target_graphics_version)
        } else {
            "N/A".into()
        };
        let part = {
            let p = cstr_arr(&v.part_number);
            if p.is_empty() { cstr_arr(&v.version) } else { p }
        };
        let mut sku = "N/A".to_string();
        if let Some(p1) = part.find('-') {
            if let Some(rel) = part[p1 + 1..].find('-') {
                let p2 = p1 + 1 + rel;
                let mid = &part[p1 + 1..p2];
                if mid.len() > 1 && mid.len() < 64 {
                    sku = mid.to_string();
                }
            }
        }
        let market = cstr_arr(&a.market_name);
        let vendor = cstr_arr(&a.vendor_name);
        print_logf!(id, Some("Card Series"), "\t\t{}", if market.is_empty() { "N/A" } else { &market });
        print_logf!(id, Some("Card Model"), "\t\t{did}");
        print_logf!(id, Some("Card Vendor"), "\t\t{}", if vendor.is_empty() { "N/A" } else { &vendor });
        print_logf!(id, Some("Card SKU"), "\t\t{sku}");
        print_logf!(id, Some("Subsystem ID"), "\t{sub}");
        print_logf!(id, Some("Device Rev"), "\t\t{rev}");
        print_logf!(id, Some("Node ID"), "\t\t{}", node_id(d));
        print_logf!(id, Some("GUID"), "\t\t{guid}");
        print_logf!(id, Some("GFX Version"), "\t\t{gfx}");
    }
    print_spacer(None, "=", 0);
}

pub fn show_serial(devs: &[i32]) {
    print_spacer(Some(" Serial Number "), "=", 0);
    for &id in devs {
        let mut b: amdsmi_board_info_t = z();
        unsafe { amdsmi_get_gpu_board_info(dev_handle(id as usize), &mut b); }
        let sn = cstr_arr(&b.product_serial);
        let sn = if !sn.is_empty() && sn != "N/A" { sn } else { "N/A".into() };
        print_log(id, Some("Serial Number"), Some(&sn));
    }
    print_spacer(None, "=", 0);
}

pub fn show_uid(devs: &[i32]) {
    print_spacer(Some(" Unique ID "), "=", 0);
    for &id in devs {
        let buf = unique_id_str(id as usize);
        print_log(id, Some("Unique ID"), Some(&buf));
    }
    print_spacer(None, "=", 0);
}

pub fn show_fw(devs: &[i32], blocks: &[String]) {
    print_spacer(Some(" Firmware Information "), "=", 0);
    for &id in devs {
        let mut fw: amdsmi_fw_info_t = z();
        if !ok(unsafe { amdsmi_get_fw_info(dev_handle(id as usize), &mut fw) }) {
            continue;
        }
        let n = (fw.num_fw_info as usize).min(amdsmi_fw_block_t::AMDSMI_FW_ID__MAX as usize);
        for k in 0..n {
            let Some(nm) = fw_name(fw.fw_info_list[k].fw_id) else { continue };
            if fw.fw_info_list[k].fw_version == 0 {
                continue;
            }
            let mut want = blocks.is_empty();
            for b in blocks {
                if str_ieq(b, "all") || str_ieq(b, nm) {
                    want = true;
                }
            }
            if !want {
                continue;
            }
            let ver = fmt_fw(nm, fw.fw_info_list[k].fw_version);
            let metric = format!("{nm} firmware version");
            if nm.len() <= 2 {
                print_logf!(id, Some(&metric), "\t\t{ver}");
            } else {
                print_logf!(id, Some(&metric), "\t{ver}");
            }
        }
    }
    print_spacer(None, "=", 0);
}

fn scale_hz(v: u64) -> u64 {
    if v > 1_000_000 { v / 1_000_000 } else { v }
}

pub fn show_range(devs: &[i32], which: &str) {
    let title = format!(" Show Valid {which} Range ");
    print_spacer(Some(&title), "=", 0);
    for &id in devs {
        let mut od: amdsmi_od_volt_freq_data_t = z();
        if !ok(unsafe { amdsmi_get_gpu_od_volt_info(dev_handle(id as usize), &mut od) }) {
            print_logf!(id, None, "Unable to display {which} range");
            continue;
        }
        if which == "sclk" || which == "mclk" {
            let r = if which == "sclk" { &od.curr_sclk_range } else { &od.curr_mclk_range };
            let lo = scale_hz(r.lower_bound);
            let hi = scale_hz(r.upper_bound);
            if r.lower_bound == u64::MAX || r.upper_bound == u64::MAX {
                print_logf!(id, None, "Unable to display {which} range");
            } else {
                print_logf!(id, None, "Valid {which} range: {lo}Mhz - {hi}Mhz");
            }
        } else {
            let mut nr = od.num_regions;
            if nr == 0 {
                print_log(id, Some("Voltage curve regions unsupported."), None);
                continue;
            }
            let mut regs = vec![z(); nr as usize];
            if ok(unsafe {
                amdsmi_get_gpu_od_volt_curve_regions(dev_handle(id as usize), &mut nr, regs.as_mut_ptr())
            }) {
                for j in 0..nr as usize {
                    print_logf!(
                        id,
                        None,
                        "Region {j}: Valid voltage range: {}mV - {}mV",
                        regs[j].volt_range.lower_bound,
                        regs[j].volt_range.upper_bound
                    );
                }
            }
        }
    }
    print_spacer(None, "=", 0);
}

pub fn show_vc(devs: &[i32]) {
    print_spacer(Some(" Voltage Curve Points "), "=", 0);
    for &id in devs {
        let mut od: amdsmi_od_volt_freq_data_t = z();
        if !ok(unsafe { amdsmi_get_gpu_od_volt_info(dev_handle(id as usize), &mut od) }) {
            continue;
        }
        for p in 0..AMDSMI_NUM_VOLTAGE_CURVE_POINTS as usize {
            let mut hz = od.curve.vc_points[p].frequency;
            let mv = od.curve.vc_points[p].voltage;
            if hz > 1_000_000 {
                hz /= 1_000_000;
            }
            print_logf!(id, None, "Voltage point {p}: {hz}Mhz {mv}mV");
        }
    }
    print_spacer(None, "=", 0);
}

pub fn show_clkvolt(devs: &[i32]) {
    if ctx().json {
        return;
    }
    print_spacer(Some(" GPU Memory clock frequencies and voltages "), "=", 0);
    for &id in devs {
        let mut od: amdsmi_od_volt_freq_data_t = z();
        if !ok(unsafe { amdsmi_get_gpu_od_volt_info(dev_handle(id as usize), &mut od) }) {
            continue;
        }
        let s0 = scale_hz(od.curr_sclk_range.lower_bound);
        let s1 = scale_hz(od.curr_sclk_range.upper_bound);
        let m0 = scale_hz(od.curr_mclk_range.lower_bound);
        let m1 = scale_hz(od.curr_mclk_range.upper_bound);
        print_log(id, Some("OD_SCLK:"), None);
        print_logf!(id, None, "0: {s0}Mhz");
        print_logf!(id, None, "1: {s1}Mhz");
        print_log(id, Some("OD_MCLK:"), None);
        print_logf!(id, None, "0: {m0}Mhz");
        print_logf!(id, None, "1: {m1}Mhz");
        if od.sclk_freq_limits.upper_bound != 0 || od.mclk_freq_limits.upper_bound != 0 {
            print_log(id, Some("OD_RANGE:"), None);
        }
        if od.sclk_freq_limits.upper_bound != 0 {
            let a = scale_hz(od.sclk_freq_limits.lower_bound);
            let b = scale_hz(od.sclk_freq_limits.upper_bound);
            print_logf!(id, None, "SCLK:     {a}Mhz        {b}Mhz");
        }
        if od.mclk_freq_limits.upper_bound != 0 {
            let a = scale_hz(od.mclk_freq_limits.lower_bound);
            let b = scale_hz(od.mclk_freq_limits.upper_bound);
            print_logf!(id, None, "MCLK:     {a}Mhz        {b}Mhz");
        }
    }
    print_spacer(None, "=", 0);
}

pub fn show_profile(devs: &[i32]) {
    if ctx().json {
        return;
    }
    print_spacer(Some(" Show Power Profiles "), "=", 0);
    for &id in devs {
        let mut st: amdsmi_power_profile_status_t = z();
        if !ok(unsafe { amdsmi_get_gpu_power_profile_presets(dev_handle(id as usize), 0, &mut st) }) {
            continue;
        }
        let mut shown = 0;
        for bit in 0..7 {
            let mask = 1u64 << bit;
            if st.available_profiles & mask != 0 {
                shown += 1;
                let star = if st.current as u64 == mask { "*" } else { "" };
                print_logf!(
                    id,
                    None,
                    "{shown}. Available power profile (#{} of 7): {}{star}",
                    bit + 1,
                    profile_name(mask)
                );
            }
        }
    }
    print_spacer(None, "=", 0);
}

pub fn show_bw(devs: &[i32]) {
    print_spacer(Some(" Measured PCIe Bandwidth "), "=", 0);
    for &id in devs {
        let mut p: amdsmi_pcie_info_t = z();
        if ok(unsafe { amdsmi_get_pcie_info(dev_handle(id as usize), &mut p) }) && p.pcie_metric.pcie_bandwidth != 0 {
            print_logf!(
                id,
                Some("Current PCIe bandwidth (MB/s)"),
                "{:.3}",
                p.pcie_metric.pcie_bandwidth as f64 / 8.0
            );
        } else {
            let mut sent = 0u64;
            let mut recv = 0u64;
            let mut maxsz = 0u64;
            if ok(unsafe { amdsmi_get_gpu_pci_throughput(dev_handle(id as usize), &mut sent, &mut recv, &mut maxsz) }) {
                let bw = ((sent + recv) as f64 * maxsz as f64) / 1024.0 / 1024.0;
                print_logf!(
                    id,
                    Some("Estimated maximum PCIe bandwidth over the last second (MB/s)"),
                    "{bw:.3}"
                );
            }
        }
    }
    print_spacer(None, "=", 0);
}

pub fn show_replay(devs: &[i32]) {
    print_spacer(Some(" PCIe Replay Counter "), "=", 0);
    for &id in devs {
        let mut c = 0u64;
        if ok(unsafe { amdsmi_get_gpu_pci_replay_counter(dev_handle(id as usize), &mut c) }) {
            print_logf!(id, Some("PCIe Replay Count"), "{c}");
        }
    }
    print_spacer(None, "=", 0);
}

pub fn show_energy(devs: &[i32]) {
    print_spacer(Some(" Consumed Energy "), "=", 0);
    for &id in devs {
        let mut e = 0u64;
        let mut ts = 0u64;
        let mut res = 0f32;
        if ok(unsafe { amdsmi_get_energy_count(dev_handle(id as usize), &mut e, &mut res, &mut ts) }) {
            print_logf!(id, Some("Energy counter"), "{e}");
            print_logf!(id, Some("Accumulated Energy (uJ)"), "{:.2}", e as f64 * res as f64);
        }
    }
    print_spacer(None, "=", 0);
}

pub fn show_pages(devs: &[i32], which: &str) {
    print_spacer(Some(" Pages Info "), "=", 0);
    for &id in devs {
        let mut n = 0u32;
        if !ok(unsafe { amdsmi_get_gpu_memory_reserved_pages(dev_handle(id as usize), &mut n, std::ptr::null_mut()) }) {
            continue;
        }
        if n == 0 {
            continue;
        }
        let mut recs = vec![z::<amdsmi_retired_page_record_t>(); n as usize];
        if !ok(unsafe { amdsmi_get_gpu_memory_reserved_pages(dev_handle(id as usize), &mut n, recs.as_mut_ptr()) }) {
            continue;
        }
        println!("\nGPU[{id}]: \t{which} PAGES INFO");
        println!("    Page address   Page size    Status");
        for r in recs.iter().take(n as usize) {
            let st = if r.status == amdsmi_memory_page_status_t::AMDSMI_MEM_PAGE_STATUS_PENDING {
                "pending"
            } else if r.status == amdsmi_memory_page_status_t::AMDSMI_MEM_PAGE_STATUS_UNRESERVABLE {
                "unreservable"
            } else {
                "reserved"
            };
            if which != "all" && which != st {
                continue;
            }
            println!("    0x{:x}   0x{:x}    {st}", r.page_address, r.page_size);
        }
    }
    print_spacer(None, "=", 0);
}

pub fn show_ras(devs: &[i32], blocks: &[String]) {
    print_spacer(Some(" RAS Info "), "=", 0);
    for &id in devs {
        for blk in ras_blocks() {
            let mut want = blocks.is_empty();
            for b in blocks {
                if str_ieq(b, "all") || str_ieq(b, blk.name) {
                    want = true;
                }
            }
            if !want {
                continue;
            }
            let mut st = 0;
            if !ok(unsafe { amdsmi_get_gpu_ecc_status(dev_handle(id as usize), blk.bit, &mut st) }) {
                continue;
            }
            print_logf!(id, None, "Block {} ECC: {}", blk.name, ras_state_name(st));
            let mut ec: amdsmi_error_count_t = z();
            if ok(unsafe { amdsmi_get_gpu_ecc_count(dev_handle(id as usize), blk.bit, &mut ec) }) {
                print_logf!(id, None, "Block {} correctable: {}", blk.name, ec.correctable_count);
                print_logf!(id, None, "Block {} uncorrectable: {}", blk.name, ec.uncorrectable_count);
            }
        }
    }
    print_spacer(None, "=", 0);
}

pub fn show_xgmi(devs: &[i32]) {
    print_spacer(Some(" XGMI Error Status "), "=", 0);
    for &id in devs {
        let mut st = 0;
        if !ok(unsafe { amdsmi_gpu_xgmi_error_status(dev_handle(id as usize), &mut st) }) {
            continue;
        }
        let s = if st == amdsmi_xgmi_status_t::AMDSMI_XGMI_STATUS_NO_ERRORS {
            "No errors"
        } else if st == amdsmi_xgmi_status_t::AMDSMI_XGMI_STATUS_ERROR {
            "Error"
        } else {
            "Multiple errors"
        };
        print_log(id, Some("XGMI Error status"), Some(s));
    }
    print_spacer(None, "=", 0);
}

pub fn show_partitions(devs: &[i32], which: i32) {
    print_spacer(Some(if which != 0 { " Memory Partition " } else { " Compute Partition " }), "=", 0);
    for &id in devs {
        if which != 0 {
            let buf = mem_part(id as usize);
            print_log(id, Some("Memory Partition"), Some(&buf));
        } else {
            let buf = comp_part(id as usize);
            print_log(id, Some("Compute Partition"), Some(&buf));
        }
    }
    print_spacer(None, "=", 0);
}

fn topo_header(devs: &[i32]) {
    print!("      ");
    for _ in devs {
        print!("{:<12} ", " ");
    }
    print!("\r      ");
    for &d in devs {
        print!("GPU{d:<9} ");
    }
    println!();
}

fn topo_cell_row<F>(devs: &[i32], mut cell_of: F)
where
    F: FnMut(i32, i32) -> String,
{
    for &a in devs {
        if !ctx().json {
            print!("GPU{a:<3} ");
        }
        for &b in devs {
            let cell = cell_of(a, b);
            if !ctx().json {
                print!("{cell:<12} ");
            }
        }
        if !ctx().json {
            println!();
        }
    }
}

pub fn show_topo_weight(devs: &[i32]) {
    print_spacer(Some(" Weight between two GPUs "), "=", 0);
    if !ctx().json {
        topo_header(devs);
    }
    topo_cell_row(devs, |a, b| {
        let cell = if a == b {
            "0".into()
        } else {
            let mut w = 0u64;
            if ok(unsafe { amdsmi_topo_get_link_weight(dev_handle(a as usize), dev_handle(b as usize), &mut w) }) {
                format!("{w}")
            } else {
                "N/A".into()
            }
        };
        if ctx().json {
            let key = format!("(Topology) Weight between DRM devices {a} and {b}");
            json_add(-1, &key, &cell);
        }
        cell
    });
}

pub fn show_topo_hops(devs: &[i32]) {
    print_spacer(Some(" Hops between two GPUs "), "=", 0);
    if !ctx().json {
        topo_header(devs);
    }
    topo_cell_row(devs, |a, b| {
        let cell = if a == b {
            "0".into()
        } else {
            let mut hops = 0u64;
            let mut ty = amdsmi_link_type_t::AMDSMI_LINK_TYPE_UNKNOWN;
            if ok(unsafe {
                amdsmi_topo_get_link_type(dev_handle(a as usize), dev_handle(b as usize), &mut hops, &mut ty)
            }) {
                format!("{hops}")
            } else {
                "N/A".into()
            }
        };
        if ctx().json {
            let key = format!("(Topology) Hops between DRM devices {a} and {b}");
            json_add(-1, &key, &cell);
        }
        cell
    });
}

pub fn show_topo_type(devs: &[i32]) {
    print_spacer(Some(" Link Type between two GPUs "), "=", 0);
    if !ctx().json {
        topo_header(devs);
    }
    topo_cell_row(devs, |a, b| {
        let cell = if a == b {
            "0".to_string()
        } else {
            let mut hops = 0u64;
            let mut ty = amdsmi_link_type_t::AMDSMI_LINK_TYPE_UNKNOWN;
            if ok(unsafe {
                amdsmi_topo_get_link_type(dev_handle(a as usize), dev_handle(b as usize), &mut hops, &mut ty)
            }) {
                link_type_name(ty).to_string()
            } else {
                "XXXX".to_string()
            }
        };
        if ctx().json {
            let key = format!("(Topology) Link type between DRM devices {a} and {b}");
            json_add(-1, &key, &cell);
        }
        cell
    });
}

pub fn show_topo_access(devs: &[i32]) {
    print_spacer(Some(" Link accessibility between GPUs "), "=", 0);
    if !ctx().json {
        topo_header(devs);
    }
    topo_cell_row(devs, |a, b| {
        let cell = if a == b {
            "true".to_string()
        } else {
            let mut acc = false;
            if !ok(unsafe { amdsmi_is_P2P_accessible(dev_handle(a as usize), dev_handle(b as usize), &mut acc) }) {
                "N/A".into()
            } else if acc {
                "true".into()
            } else {
                "false".into()
            }
        };
        if ctx().json {
            let key = format!("(Topology) Access between DRM devices {a} and {b}");
            json_add(-1, &key, &cell);
        }
        cell
    });
}

pub fn show_numa(devs: &[i32]) {
    print_spacer(Some(" Numa Nodes "), "=", 0);
    for &id in devs {
        let mut n = 0u32;
        if ok(unsafe { amdsmi_topo_get_numa_node_number(dev_handle(id as usize), &mut n) }) {
            print_logf!(id, Some("(Topology) Numa Node"), "{n}");
        }
        let mut aff = 0i32;
        if ok(unsafe { amdsmi_get_gpu_topo_numa_affinity(dev_handle(id as usize), &mut aff) }) {
            print_logf!(id, Some("(Topology) Numa Affinity"), "{aff}");
        }
    }
}

pub fn show_nodes_bw(devs: &[i32]) {
    print_spacer(Some(" Bandwidth table (GB/s) "), "=", 0);
    if !ctx().json {
        topo_header(devs);
    }
    topo_cell_row(devs, |a, b| {
        let cell = if a != b {
            let mut mn = 0u64;
            let mut mx = 0u64;
            if ok(unsafe {
                amdsmi_get_minmax_bandwidth_between_processors(
                    dev_handle(a as usize),
                    dev_handle(b as usize),
                    &mut mn,
                    &mut mx,
                )
            }) {
                format!("{mx}")
            } else {
                "N/A".into()
            }
        } else {
            "0".into()
        };
        if ctx().json {
            let key = format!("(Topology) Bandwidth between {a} and {b}");
            json_add(-1, &key, &cell);
        }
        cell
    });
}

pub fn show_pids(verbose: &str) {
    print_spacer(Some(" KFD Processes "), "=", 0);
    if ndev() == 0 {
        print_log(-1, Some("No KFD PIDs currently running"), None);
        print_spacer(None, "=", 0);
        return;
    }
    let mut maxp = 0u32;
    if !ok(unsafe { amdsmi_get_gpu_process_list(dev_handle(0), &mut maxp, std::ptr::null_mut()) }) || maxp == 0 {
        print_log(-1, Some("No KFD PIDs currently running"), None);
        print_spacer(None, "=", 0);
        return;
    }
    let mut list = vec![z::<amdsmi_proc_info_t>(); maxp as usize];
    if !ok(unsafe { amdsmi_get_gpu_process_list(dev_handle(0), &mut maxp, list.as_mut_ptr()) }) {
        print_log(-1, Some("No KFD PIDs currently running"), None);
        print_spacer(None, "=", 0);
        return;
    }
    print_log(-1, Some("KFD process information:"), None);
    if verbose == "details" {
        println!(
            "{:<8} {:<16} {:<8} {:<12} {:<12} {}",
            "PID", "PROCESS NAME", "GPU", "VRAM USED", "SDMA USED", "CU OCCUPANCY"
        );
    } else {
        println!(
            "{:<8} {:<16} {:<8} {:<12} {:<12} {}",
            "PID", "PROCESS NAME", "GPU(s)", "VRAM USED", "SDMA USED", "CU OCCUPANCY"
        );
    }
    let n = ndev() as i32;
    for i in 0..maxp as usize {
        let name = proc_name(list[i].pid);
        let cu = if list[i].cu_occupancy == 0xFFFF_FFFF { 0 } else { list[i].cu_occupancy };
        println!(
            "{:<8} {:<16} {:<8} {:<12} {:<12} {}",
            list[i].pid, name, n, list[i].memory_usage.vram_mem, list[i].sdma_usage, cu
        );
    }
    print_spacer(None, "=", 0);
}

pub fn show_pidgpus(pids: &[String]) {
    print_spacer(Some(" GPUs Indexed by PID "), "=", 0);
    if ndev() == 0 {
        print_log(-1, Some("No KFD PIDs currently running"), None);
        print_spacer(None, "=", 0);
        return;
    }
    let mut hs = [std::ptr::null_mut(); MAX_DEV];
    let n = ndev();
    for i in 0..n {
        hs[i] = dev_handle(i);
    }
    let mut maxp = 0u32;
    if !ok(unsafe { amdsmi_get_gpu_process_list_by_pid(hs.as_mut_ptr(), n as u32, std::ptr::null_mut(), &mut maxp) })
        || maxp == 0
    {
        print_log(-1, Some("No KFD PIDs currently running"), None);
        print_spacer(None, "=", 0);
        return;
    }
    let mut procs = vec![z::<amdsmi_proc_info_by_pid_t>(); maxp as usize];
    if !ok(unsafe { amdsmi_get_gpu_process_list_by_pid(hs.as_mut_ptr(), n as u32, procs.as_mut_ptr(), &mut maxp) }) {
        print_log(-1, Some("Unable to get list of KFD PIDs. A kernel update may be needed"), None);
        print_spacer(None, "=", 0);
        return;
    }
    for p in procs.iter().take(maxp as usize) {
        let mut want = pids.is_empty();
        for a in pids {
            if c_atoi(a) as u32 == p.pid {
                want = true;
            }
        }
        if !want {
            continue;
        }
        let mut line = sn(256, &format!("PID {} is using {} DRM device(s):", p.pid, p.num_gpus));
        for i in 0..p.num_gpus as usize {
            if line.len() >= 256 - 8 {
                break;
            }
            let extra = format!(" {}", p.gpus[i].gpu_index);
            let room = 255 - line.len();
            let eb = extra.as_bytes();
            line.push_str(&String::from_utf8_lossy(&eb[..eb.len().min(room)]));
        }
        print_log(-1, Some(&line), None);
    }
    print_spacer(None, "=", 0);
}

pub fn show_metrics(devs: &[i32]) {
    print_spacer(Some(" GPU Metrics "), "=", 0);
    for &id in devs {
        let mut m = Box::new(z::<amdsmi_gpu_metrics_t>());
        if !ok(unsafe { amdsmi_get_gpu_metrics_info(dev_handle(id as usize), m.as_mut()) }) {
            continue;
        }
        print_logf!(
            id,
            Some("Metric Version and Size (Bytes)"),
            "{}.{} {}",
            m.common_header.format_revision,
            m.common_header.content_revision,
            m.common_header.structure_size
        );
        let pm = |name: &str, unit: &str, s: &str| {
            let metric = format!("{name} ({unit})");
            print_log(id, Some(&metric), Some(s));
        };
        pm("temperature_edge", "C", &na_u16(m.temperature_edge));
        pm("temperature_hotspot", "C", &na_u16(m.temperature_hotspot));
        pm("temperature_mem", "C", &na_u16(m.temperature_mem));
        pm("temperature_vrgfx", "C", &na_u16(m.temperature_vrgfx));
        pm("temperature_vrsoc", "C", &na_u16(m.temperature_vrsoc));
        pm("temperature_vrmem", "C", &na_u16(m.temperature_vrmem));
        pm("average_gfx_activity", "%", &na_u16(m.average_gfx_activity));
        pm("average_umc_activity", "%", &na_u16(m.average_umc_activity));
        pm("average_mm_activity", "%", &na_u16(m.average_mm_activity));
        pm("average_socket_power", "W", &na_u16(m.average_socket_power));
        pm("energy_accumulator", "15.259uJ (2^-16)", &na_u64(m.energy_accumulator));
        pm("system_clock_counter", "ns", &na_u64(m.system_clock_counter));
        pm("average_gfxclk_frequency", "MHz", &na_u16(m.average_gfxclk_frequency));
        pm("average_socclk_frequency", "MHz", &na_u16(m.average_socclk_frequency));
        pm("average_uclk_frequency", "MHz", &na_u16(m.average_uclk_frequency));
        pm("average_vclk0_frequency", "MHz", &na_u16(m.average_vclk0_frequency));
        pm("average_dclk0_frequency", "MHz", &na_u16(m.average_dclk0_frequency));
        pm("average_vclk1_frequency", "MHz", &na_u16(m.average_vclk1_frequency));
        pm("average_dclk1_frequency", "MHz", &na_u16(m.average_dclk1_frequency));
        pm("current_gfxclk", "MHz", &na_u16(m.current_gfxclk));
        pm("current_socclk", "MHz", &na_u16(m.current_socclk));
        pm("current_uclk", "MHz", &na_u16(m.current_uclk));
        pm("current_vclk0", "MHz", &na_u16(m.current_vclk0));
        pm("current_dclk0", "MHz", &na_u16(m.current_dclk0));
        pm("current_vclk1", "MHz", &na_u16(m.current_vclk1));
        pm("current_dclk1", "MHz", &na_u16(m.current_dclk1));
        pm("throttle_status", "N/A", &na_u32(m.throttle_status));
        pm("current_fan_speed", "rpm", &na_u16(m.current_fan_speed));
        pm("pcie_link_width", "Lanes", &na_u16(m.pcie_link_width));
        pm("pcie_link_speed", "0.1 GT/s", &na_u16(m.pcie_link_speed));
        pm("voltage_soc", "mV", &na_u16(m.voltage_soc));
        pm("voltage_gfx", "mV", &na_u16(m.voltage_gfx));
        pm("voltage_mem", "mV", &na_u16(m.voltage_mem));
        pm("current_socket_power", "W", &na_u16(m.current_socket_power));
        let list = u16_list(&m.vcn_activity[..AMDSMI_MAX_NUM_VCN as usize]);
        print_log(id, Some("vcn_activity (%)"), Some(&list));
        pm("xgmi_link_width", "GB/s", &na_u16(m.xgmi_link_width));
        pm("xgmi_link_speed", "Gbps", &na_u16(m.xgmi_link_speed));
        pm("pcie_bandwidth_acc", "GB/s", &na_u64(m.pcie_bandwidth_acc));
        pm("pcie_bandwidth_inst", "GB/s", &na_u64(m.pcie_bandwidth_inst));
        pm("pcie_replay_count_acc", "Count", &na_u64(m.pcie_replay_count_acc));
        let list = u64_list(&m.xgmi_read_data_acc[..AMDSMI_MAX_NUM_XGMI_LINKS as usize]);
        print_log(id, Some("xgmi_read_data_acc (kB)"), Some(&list));
        let list = u64_list(&m.xgmi_write_data_acc[..AMDSMI_MAX_NUM_XGMI_LINKS as usize]);
        print_log(id, Some("xgmi_write_data_acc (kB)"), Some(&list));
        let list = u16_list(&m.current_gfxclks[..AMDSMI_MAX_NUM_GFX_CLKS as usize]);
        print_log(id, Some("current_gfxclks (MHz)"), Some(&list));
        let list = u16_list(&m.current_socclks[..AMDSMI_MAX_NUM_CLKS as usize]);
        print_log(id, Some("current_socclks (MHz)"), Some(&list));
        let list = u16_list(&m.jpeg_activity[..8]);
        print_log(id, Some("jpeg_activity (%)"), Some(&list));
        pm("vram_max_bandwidth", "GB/s", &na_u64(m.vram_max_bandwidth));
        pm("num_partition", "N/A", &na_u16(m.num_partition));
    }
    print_spacer(None, "=", 0);
}

pub fn show_events(devs: &[i32], types: &[String]) {
    let names = [
        "VMFAULT",
        "THERMAL_THROTTLE",
        "GPU_PRE_RESET",
        "GPU_POST_RESET",
        "MIGRATE_START",
        "MIGRATE_END",
        "PAGE_FAULT_START",
        "PAGE_FAULT_END",
        "QUEUE_EVICTION",
        "QUEUE_RESTORE",
        "UNMAP_FROM_GPU",
        "PROCESS_START",
        "PROCESS_END",
    ];
    print_spacer(Some(" Show Events "), "=", 0);
    print_log(-1, Some("press 'q' and then Enter to quit"), None);
    let mut mask = 0u64;
    if types.is_empty() {
        mask = (1u64 << 13) - 1;
    } else {
        for t in types {
            let mut found = false;
            for (i, n) in names.iter().enumerate() {
                if str_ieq(t, n) {
                    mask |= 1u64 << i;
                    found = true;
                }
            }
            if !found {
                eprintln!("Ignoring unrecognized event type {t}");
            }
        }
        if mask == 0 {
            mask = (1u64 << 13) - 1;
        }
    }
    println!("DEVICE\tTIME\tTYPE\tDESCRIPTION");
    for &id in devs {
        unsafe {
            amdsmi_init_gpu_event_notification(dev_handle(id as usize));
            amdsmi_set_gpu_event_notification_mask(dev_handle(id as usize), mask);
        }
    }
    loop {
        let mut rfds: libc::fd_set = z();
        unsafe {
            libc::FD_ZERO(&mut rfds);
            libc::FD_SET(libc::STDIN_FILENO, &mut rfds);
        }
        let mut tv = libc::timeval { tv_sec: 0, tv_usec: 0 };
        let sel = unsafe {
            libc::select(libc::STDIN_FILENO + 1, &mut rfds, std::ptr::null_mut(), std::ptr::null_mut(), &mut tv)
        };
        if sel > 0 {
            let mut line = [0u8; 16];
            let n = unsafe { libc::read(libc::STDIN_FILENO, line.as_mut_ptr() as *mut _, 15) };
            if n > 0 && (line[0] == b'q' || line[0] == 3) {
                break;
            }
        }
        let mut data = [z::<amdsmi_evt_notification_data_t>(); 8];
        let mut n = 8u32;
        if ok(unsafe { amdsmi_get_gpu_event_notification(1000, &mut n, data.as_mut_ptr()) }) && n > 0 {
            for e in 0..n as usize {
                let mut di = 0usize;
                let mut found = false;
                for i in 0..ndev() {
                    if dev_handle(i) == data[e].processor_handle {
                        di = i;
                        found = true;
                        break;
                    }
                }
                let nm = if data[e].event >= 1 && data[e].event <= 13 {
                    names[(data[e].event - 1) as usize]
                } else {
                    "UNKNOWN"
                };
                let msg = cstr_arr(&data[e].message);
                let shown = if found { di } else { 0 };
                print!("\rGPU[{shown}]:\t{nm}\t{msg}\n");
            }
        }
    }
    for &id in devs {
        unsafe { amdsmi_stop_gpu_event_notification(dev_handle(id as usize)); }
    }
    print_spacer(None, "=", 0);
}

pub fn show_tempgraph(devs: &[i32]) {
    print_spacer(Some(" Temperature Graph "), "=", 0);
    print_log(-1, Some("press Ctrl-C to quit (showing 30 samples)"), None);
    for _cycle in 0..30 {
        for &id in devs {
            let mut good = temp_c(id as usize, amdsmi_temperature_type_t::AMDSMI_TEMPERATURE_TYPE_EDGE);
            if good.is_none() {
                good = temp_c(id as usize, amdsmi_temperature_type_t::AMDSMI_TEMPERATURE_TYPE_JUNCTION);
            }
            let mut pct = good.map(|t| t as i32).unwrap_or(0);
            if pct < 0 {
                pct = 0;
            }
            if pct > 100 {
                pct = 100;
            }
            print!("GPU[{id}] Temp {pct:3}\u{00B0}C |");
            for k in 0..60 {
                print!("{}", if k < (pct * 60) / 100 { '#' } else { ' ' });
            }
            println!("|");
        }
        let _ = std::io::stdout().flush();
        std::thread::sleep(std::time::Duration::from_millis(500));
    }
    print_spacer(None, "=", 0);
}

