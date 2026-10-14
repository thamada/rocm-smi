//! Set, reset, save, and load. Behavior matches `rocm-smi.c`.

use crate::core::*;
use crate::print_logf;
use std::io::{Read, Write};

pub fn set_perf(devs: &[i32], level: &str) {
    print_spacer(Some(" Set Performance Level "), "=", 0);
    let Some(p) = perf_from_name(level) else {
        print_logf!(-1, Some("Invalid performance level"), "{level}");
        print_spacer(None, "=", 0);
        return;
    };
    for &id in devs {
        let st = unsafe { amdsmi_set_gpu_perf_level(dev_handle(id as usize), p) };
        if ok(st) {
            print_logf!(id, None, "Successfully set performance level to {level}");
        } else {
            status_line(id, st, "set performance level");
        }
    }
    print_spacer(None, "=", 0);
}

pub fn set_fan(devs: &[i32], level: &str) {
    print_spacer(Some(" Set Fan Speed "), "=", 0);
    for &id in devs {
        let mut mx = 255u64;
        if !ok(unsafe { amdsmi_get_gpu_fan_speed_max(dev_handle(id as usize), 0, &mut mx) }) || mx == 0 {
            mx = 255;
        }
        let speed = if level.as_bytes().last() == Some(&b'%') {
            let tmp = &level[..level.len() - 1];
            (c_atof(tmp) * mx as f64 / 100.0) as u64
        } else {
            c_strtoull(level, 0)
        };
        status_line(id, unsafe { amdsmi_set_gpu_fan_speed(dev_handle(id as usize), 0, speed) }, "Fan speed set");
        let mut got = 0i64;
        if ok(unsafe { amdsmi_get_gpu_fan_speed(dev_handle(id as usize), 0, &mut got) }) {
            print_logf!(id, None, "Successfully set fan speed to {level}");
        }
    }
    print_spacer(None, "=", 0);
}

pub fn reset_fans(devs: &[i32]) {
    print_spacer(Some(" Reset Fans "), "=", 0);
    for &id in devs {
        let st = unsafe { amdsmi_reset_gpu_fan(dev_handle(id as usize), 0) };
        if ok(st) {
            print_log(id, Some("Successfully reset fan speed to driver control"), None);
        } else {
            status_line(id, st, "reset fan");
        }
    }
    print_spacer(None, "=", 0);
}

pub fn set_clocks(devs: &[i32], ty: &str, levels: &[i32]) {
    print_spacer(Some(" Set Clock Frequency "), "=", 0);
    let Some(ct) = clk_lookup(ty) else {
        print_logf!(-1, Some("Unsupported clock type"), "{ty}");
        print_spacer(None, "=", 0);
        return;
    };
    let mut mask = 0u64;
    for &lv in levels {
        if (0..64).contains(&lv) {
            mask |= 1u64 << lv;
        }
    }
    for &id in devs {
        let st = unsafe { amdsmi_set_clk_freq(dev_handle(id as usize), ct, mask) };
        if ok(st) {
            print_logf!(id, None, "Successfully set {ty} frequency mask");
        } else {
            status_line(id, st, "set clock");
        }
    }
    print_spacer(None, "=", 0);
}

pub fn set_power_od(devs: &[i32], watts: Option<&str>, reset: bool) {
    print_spacer(Some(if reset { " Reset Power OverDrive " } else { " Set Power OverDrive " }), "=", 0);
    if !reset && !confirm_danger() {
        print_log(-1, Some("Confirmation not given. Exiting without setting value"), None);
        print_spacer(None, "=", 0);
        std::process::exit(1);
    }
    for &id in devs {
        let mut c: amdsmi_power_cap_info_t = z();
        unsafe { amdsmi_get_power_cap_info(dev_handle(id as usize), 0, &mut c); }
        let cap = if reset {
            if c.default_power_cap != 0 { c.default_power_cap } else { c.power_cap }
        } else {
            let w = c_atof(watts.unwrap_or("0"));
            if c.power_cap > 100_000 { (w * 1_000_000.0) as u64 } else { w as u64 }
        };
        if ok(unsafe { amdsmi_set_power_cap(dev_handle(id as usize), 0, cap) }) {
            let shown = if reset { "default" } else { watts.unwrap_or("") };
            print_logf!(id, None, "Successfully set power cap to {shown}");
        } else {
            status_line(id, amdsmi_status_t::AMDSMI_STATUS_NOT_SUPPORTED, "set power cap");
        }
    }
    print_spacer(None, "=", 0);
}

pub fn set_overdrive(devs: &[i32], pct: &str) {
    let od = c_atoi(pct) as u32;
    print_spacer(Some(" Set OverDrive "), "=", 0);
    if !confirm_danger() {
        print_log(-1, Some("Confirmation not given. Exiting without setting value"), None);
        print_spacer(None, "=", 0);
        std::process::exit(1);
    }
    for &id in devs {
        let st = unsafe { amdsmi_set_gpu_overdrive_level(dev_handle(id as usize), od) };
        if ok(st) {
            print_logf!(id, None, "Successfully set OverDrive to {od}%");
        } else {
            status_line(id, st, "set overdrive");
        }
    }
    print_spacer(None, "=", 0);
}

pub fn set_extremum(devs: &[i32], which: &str, clk: &str, mhz: &str) {
    print_spacer(Some(" Set Valid sclk Extremum "), "=", 0);
    if !confirm_danger() {
        print_log(-1, Some("Confirmation not given. Exiting without setting value"), None);
        std::process::exit(1);
    }
    let Some(ct) = clk_lookup(clk) else {
        print_logf!(-1, Some("Unsupported clock"), "{clk}");
        return;
    };
    let lim = if str_ieq(which, "min") {
        amdsmi_clk_limit_type_t::AMDSMI_CLK_LIMIT_MIN
    } else {
        amdsmi_clk_limit_type_t::AMDSMI_CLK_LIMIT_MAX
    };
    for &id in devs {
        let st = unsafe { amdsmi_set_gpu_clk_limit(dev_handle(id as usize), ct, lim, c_atoi(mhz) as u64) };
        if ok(st) {
            print_logf!(id, None, "Successfully set {which} {clk} to {mhz}(MHz)");
        } else {
            status_line(id, st, "set extremum");
        }
    }
    print_spacer(None, "=", 0);
}

pub fn set_range(devs: &[i32], clk: &str, mn: &str, mx: &str) {
    let ct = if str_ieq(clk, "mclk") {
        amdsmi_clk_type_t::AMDSMI_CLK_TYPE_MEM
    } else {
        amdsmi_clk_type_t::AMDSMI_CLK_TYPE_SYS
    };
    print_spacer(Some(" Set Clock Range "), "=", 0);
    if !confirm_danger() {
        std::process::exit(1);
    }
    for &id in devs {
        let s1 = unsafe {
            amdsmi_set_gpu_od_clk_info(
                dev_handle(id as usize),
                amdsmi_freq_ind_t::AMDSMI_FREQ_IND_MIN,
                c_atoi(mn) as u64,
                ct,
            )
        };
        let s2 = unsafe {
            amdsmi_set_gpu_od_clk_info(
                dev_handle(id as usize),
                amdsmi_freq_ind_t::AMDSMI_FREQ_IND_MAX,
                c_atoi(mx) as u64,
                ct,
            )
        };
        if ok(s1) && ok(s2) {
            print_logf!(id, None, "Successfully set {clk} range to {mn}-{mx} MHz");
        } else {
            status_line(id, if ok(s1) { s2 } else { s1 }, "set range");
        }
    }
    print_spacer(None, "=", 0);
}

pub fn set_vc(devs: &[i32], pt: &str, mhz: &str, mv: &str) {
    print_spacer(Some(" Set Voltage Curve "), "=", 0);
    if !confirm_danger() {
        std::process::exit(1);
    }
    for &id in devs {
        let st = unsafe {
            amdsmi_set_gpu_od_volt_info(dev_handle(id as usize), c_atoi(pt) as u32, c_atoi(mhz) as u64, c_atoi(mv) as u64)
        };
        if ok(st) {
            print_logf!(id, None, "Successfully set voltage curve point {pt}");
        } else {
            status_line(id, st, "set voltage curve");
        }
    }
    print_spacer(None, "=", 0);
}

pub fn set_slevel(devs: &[i32], clk: &str, mhz: &str) {
    set_range(devs, clk, mhz, mhz);
}

pub fn reset_clocks(devs: &[i32]) {
    print_spacer(Some(" Reset Clocks "), "=", 0);
    for &id in devs {
        unsafe { amdsmi_set_gpu_overdrive_level(dev_handle(id as usize), 0); }
        print_log(id, Some("OverDrive set to 0"), None);
        if ok(unsafe { amdsmi_set_gpu_perf_level(dev_handle(id as usize), amdsmi_dev_perf_level_t::AMDSMI_DEV_PERF_LEVEL_AUTO) })
        {
            print_log(id, Some("Performance level set to auto"), None);
        }
    }
    print_spacer(None, "=", 0);
}

pub fn set_profile(devs: &[i32], spec: &str) {
    print_spacer(Some(" Set Power Profile "), "=", 0);
    let mut mask = 0u64;
    if str_ieq(spec, "CUSTOM") {
        mask = amdsmi_power_profile_preset_masks_t::AMDSMI_PWR_PROF_PRST_CUSTOM_MASK as u64;
    } else if str_ieq(spec, "VIDEO") {
        mask = amdsmi_power_profile_preset_masks_t::AMDSMI_PWR_PROF_PRST_VIDEO_MASK as u64;
    } else if str_ieq(spec, "POWER SAVING") || str_ieq(spec, "POWER_SAVING") {
        mask = amdsmi_power_profile_preset_masks_t::AMDSMI_PWR_PROF_PRST_POWER_SAVING_MASK as u64;
    } else if str_ieq(spec, "COMPUTE") {
        mask = amdsmi_power_profile_preset_masks_t::AMDSMI_PWR_PROF_PRST_COMPUTE_MASK as u64;
    } else if str_ieq(spec, "VR") {
        mask = amdsmi_power_profile_preset_masks_t::AMDSMI_PWR_PROF_PRST_VR_MASK as u64;
    } else if str_ieq(spec, "3D FULL SCREEN") || str_ieq(spec, "3D_FULL_SCREEN") {
        mask = amdsmi_power_profile_preset_masks_t::AMDSMI_PWR_PROF_PRST_3D_FULL_SCR_MASK as u64;
    } else if str_ieq(spec, "BOOTUP DEFAULT") || str_ieq(spec, "BOOTUP_DEFAULT") {
        mask = amdsmi_power_profile_preset_masks_t::AMDSMI_PWR_PROF_PRST_BOOTUP_DEFAULT as u64;
    } else {
        let n = c_atoi(spec);
        if (1..=7).contains(&n) {
            mask = 1u64 << (n - 1);
        }
    }
    for &id in devs {
        if mask == 0 {
            print_log(id, Some("Unrecognized profile"), None);
            continue;
        }
        let st = unsafe {
            amdsmi_set_gpu_power_profile(dev_handle(id as usize), 0, mask as amdsmi_power_profile_preset_masks_t::Type)
        };
        if ok(st) {
            print_logf!(id, None, "Successfully set power profile to {spec}");
        } else {
            status_line(id, st, "set profile");
        }
    }
    print_spacer(None, "=", 0);
}

pub fn reset_profile(devs: &[i32]) {
    set_profile(devs, "BOOTUP DEFAULT");
}

pub fn set_perfdet(devs: &[i32], mhz: u64) {
    print_spacer(Some(" Set Performance Determinism "), "=", 0);
    for &id in devs {
        let st = unsafe { amdsmi_set_gpu_perf_determinism_mode(dev_handle(id as usize), mhz) };
        if ok(st) {
            print_logf!(id, None, "Successfully set performance determinism to {mhz} MHz");
        } else {
            status_line(id, st, "set perf determinism");
        }
    }
    print_spacer(None, "=", 0);
}

pub fn reset_perfdet(devs: &[i32]) {
    set_perf(devs, "auto");
}

fn mempart_from_name(s: &str) -> Option<amdsmi_memory_partition_type_t::Type> {
    use amdsmi_memory_partition_type_t::*;
    if str_ieq(s, "NPS1") {
        Some(AMDSMI_MEMORY_PARTITION_NPS1)
    } else if str_ieq(s, "NPS2") {
        Some(AMDSMI_MEMORY_PARTITION_NPS2)
    } else if str_ieq(s, "NPS4") {
        Some(AMDSMI_MEMORY_PARTITION_NPS4)
    } else if str_ieq(s, "NPS8") {
        Some(AMDSMI_MEMORY_PARTITION_NPS8)
    } else {
        None
    }
}

fn comppart_from_name(s: &str) -> Option<amdsmi_accelerator_partition_type_t::Type> {
    use amdsmi_accelerator_partition_type_t::*;
    if str_ieq(s, "SPX") {
        Some(AMDSMI_ACCELERATOR_PARTITION_SPX)
    } else if str_ieq(s, "DPX") {
        Some(AMDSMI_ACCELERATOR_PARTITION_DPX)
    } else if str_ieq(s, "TPX") {
        Some(AMDSMI_ACCELERATOR_PARTITION_TPX)
    } else if str_ieq(s, "QPX") {
        Some(AMDSMI_ACCELERATOR_PARTITION_QPX)
    } else if str_ieq(s, "CPX") {
        Some(AMDSMI_ACCELERATOR_PARTITION_CPX)
    } else {
        None
    }
}

pub fn set_mempart(devs: &[i32], name: &str) {
    print_spacer(Some(" Set Memory Partition "), "=", 0);
    let Some(m) = mempart_from_name(name) else {
        print_logf!(-1, Some("Invalid memory partition"), "{name}");
        print_spacer(None, "=", 0);
        return;
    };
    if !confirm_danger() {
        std::process::exit(1);
    }
    for &id in devs {
        let st = unsafe { amdsmi_set_gpu_memory_partition(dev_handle(id as usize), m) };
        if ok(st) {
            print_logf!(id, None, "Successfully set memory partition to {name}");
        } else {
            status_line(id, st, "set memory partition");
        }
    }
    print_spacer(None, "=", 0);
}

pub fn set_comppart(devs: &[i32], name: &str) {
    print_spacer(Some(" Set Compute Partition "), "=", 0);
    let Some(want) = comppart_from_name(name) else {
        print_logf!(-1, Some("Invalid compute partition"), "{name}");
        print_spacer(None, "=", 0);
        return;
    };
    for &id in devs {
        let mut cfg = Box::new(z::<amdsmi_accelerator_partition_profile_config_t>());
        let st = unsafe { amdsmi_get_gpu_accelerator_partition_profile_config(dev_handle(id as usize), cfg.as_mut()) };
        let mut chosen = u32::MAX;
        if ok(st) {
            let n = (cfg.num_profiles as usize).min(AMDSMI_MAX_ACCELERATOR_PROFILE as usize);
            for p in 0..n {
                if cfg.profiles[p].profile_type == want {
                    chosen = cfg.profiles[p].profile_index;
                }
            }
        }
        if chosen == u32::MAX {
            print_log(id, Some("Not supported on the given system"), None);
            continue;
        }
        let st = unsafe { amdsmi_set_gpu_accelerator_partition_profile(dev_handle(id as usize), chosen) };
        if ok(st) {
            print_logf!(id, None, "Successfully set compute partition to {name}");
        } else {
            status_line(id, st, "set compute partition");
        }
    }
    print_spacer(None, "=", 0);
}

pub fn do_reset_gpu(dev: i32) {
    print_spacer(Some(" GPU Reset "), "=", 0);
    if ok(unsafe { amdsmi_reset_gpu(dev_handle(dev as usize)) }) {
        print_log(dev, Some("Successfully reset GPU"), None);
    } else {
        print_log(dev, Some("GPU reset failed"), None);
    }
    print_spacer(None, "=", 0);
}

pub fn reset_xgmi(devs: &[i32]) {
    print_spacer(Some(" Reset XGMI Error "), "=", 0);
    for &id in devs {
        if ok(unsafe { amdsmi_reset_gpu_xgmi_error(dev_handle(id as usize)) }) {
            print_log(id, Some("Successfully reset XGMI error count"), None);
        } else {
            print_log(id, Some("Not supported on the given system"), None);
        }
    }
    print_spacer(None, "=", 0);
}

pub fn save_settings(devs: &[i32], path: &str) {
    print_spacer(Some(" Save Settings "), "=", 0);
    if path_exists(path) {
        print_logf!(-1, None, "{path} already exists. Settings not saved");
        print_spacer(None, "=", 0);
        std::process::exit(1);
    }
    let mut out = String::from("{");
    for (i, &id) in devs.iter().enumerate() {
        let d = id as usize;
        let mut pl = 0;
        let mut perf = "Unsupported".to_string();
        out.push_str(&format!("{}\"card{id}\": {{", if i == 0 { "" } else { ", " }));
        out.push_str(&format!("\"vJson\": {CLOCK_JSON_VERSION}, \"clocks\": {{"));
        for (k, c) in clks().iter().enumerate() {
            let mut fr: amdsmi_frequencies_t = z();
            let cur = if ok(unsafe { amdsmi_get_clk_freq(dev_handle(d), c.ty, &mut fr) }) { fr.current } else { 0 };
            out.push_str(&format!("{}\"{}\": \"{cur}\"", if k == 0 { "" } else { ", " }, c.name));
        }
        out.push('}');
        let lv = fan_level_pct(d).map(|f| f.level).unwrap_or(0);
        let mut od = 0u32;
        unsafe { amdsmi_get_gpu_overdrive_level(dev_handle(d), &mut od); }
        if ok(unsafe { amdsmi_get_gpu_perf_level(dev_handle(d), &mut pl) }) {
            if let Some(n) = perf_name(pl) {
                perf = n.into();
            }
        }
        out.push_str(&format!(", \"fan\": {lv}, \"overdrivesclk\": \"{od}\", \"overdrivemclk\": \"0\""));
        out.push_str(&format!(", \"profile\": \"1\", \"perflevel\": \"{perf}\"}}"));
    }
    out.push_str("}\n");
    match std::fs::File::create(path) {
        Ok(mut f) => {
            if f.write_all(out.as_bytes()).is_err() {
                print_logf!(-1, None, "Cannot write {path}");
                print_spacer(None, "=", 0);
                return;
            }
        }
        Err(_) => {
            print_logf!(-1, None, "Cannot write {path}");
            print_spacer(None, "=", 0);
            return;
        }
    }
    print_logf!(-1, Some("Current settings successfully saved to"), "{path}");
    print_spacer(None, "=", 0);
}

fn json_find<'a>(s: &'a str, key: &str) -> Option<String> {
    let pat = format!("\"{key}\"");
    let p = s.find(&pat)?;
    let rest = &s[p + pat.len()..];
    let colon = rest.find(':')?;
    let p = rest[colon + 1..].trim_start_matches([' ', '\t']);
    if let Some(body) = p.strip_prefix('"') {
        let end = body.find('"')?;
        return Some(body[..end].to_string());
    }
    let b = p.as_bytes();
    let mut e = 0;
    while e < b.len() && b[e] != b',' && b[e] != b'}' && unsafe { libc::isspace(b[e] as libc::c_int) } == 0 {
        e += 1;
    }
    Some(String::from_utf8_lossy(&b[..e]).into_owned())
}

pub fn load_settings(path: &str) {
    print_spacer(Some(" Load Settings "), "=", 0);
    let mut f = match std::fs::File::open(path) {
        Ok(f) => f,
        Err(_) => {
            print_logf!(-1, None, "No settings file found at {path}");
            print_spacer(None, "=", 0);
            std::process::exit(1);
        }
    };
    let mut buf = String::new();
    if f.read_to_string(&mut buf).is_err() {
        return;
    }
    let mut search = buf.as_str();
    while let Some(rel) = search.find("\"card") {
        let p = &search[rel..];
        let dev = c_atoi(&p[5..]);
        let Some(end_rel) = p.find('}') else {
            search = &p[5..];
            continue;
        };
        if dev < 0 || dev as usize >= ndev() || end_rel >= 4096 {
            if end_rel >= 4096 {
                search = &p[end_rel..];
            } else {
                search = &p[5..];
            }
            continue;
        }
        let slice = &p[..end_rel];
        if let Some(ver) = json_find(slice, "vJson") {
            if !ver.is_empty() && c_atoi(&ver) != CLOCK_JSON_VERSION {
                print_logf!(
                    -1,
                    None,
                    "Unable to load legacy clock file - file v{ver} != current v{CLOCK_JSON_VERSION}"
                );
                break;
            }
        }
        if let Some(fan) = json_find(slice, "fan") {
            if !fan.is_empty() && fan != "0" {
                set_fan(&[dev], &fan);
            }
        }
        if let Some(od) = json_find(slice, "overdrivesclk") {
            if !od.is_empty() && od != "0" {
                set_overdrive(&[dev], &od);
            }
        }
        if let Some(perf) = json_find(slice, "perflevel") {
            if !perf.is_empty() && perf != "Unsupported" {
                set_perf(&[dev], &perf);
            }
        }
        print_logf!(dev, None, "Successfully loaded values from {path}");
        search = &p[end_rel..];
    }
    print_spacer(None, "=", 0);
}
