//! Shared state, logging, discovery, and libamd_smi queries.
//! Behavior follows `rocm-smi.c`.

#![allow(non_upper_case_globals, non_camel_case_types, non_snake_case, dead_code, unused)]

use std::ffi::{CStr, CString};
use std::io::{BufRead, Write};
use std::os::raw::{c_char, c_int};

pub mod ffi {
    #![allow(non_upper_case_globals, non_camel_case_types, non_snake_case, dead_code, unused)]
    include!(concat!(env!("OUT_DIR"), "/amdsmi.rs"));
}

pub use ffi::*;

pub const SMI_VERSION: &str = "4.0.0+c";
pub const APP_WIDTH: i32 = 90;
pub const MAX_DEV: usize = 32;
pub const CLOCK_JSON_VERSION: i32 = 1;
const DRM_IOCTL_AMDGPU_INFO_WAKE: libc::c_ulong = 0x40206445;

pub const HEADER: &str = " ROCm System Management Interface ";
pub const FOOTER: &str = " End of ROCm SMI Log ";

pub type Status = amdsmi_status_t::Type;

pub struct Dev {
    pub h: amdsmi_processor_handle,
    pub card: u32,
    pub render: u32,
    pub have_enum: bool,
}

struct Kv {
    key: String,
    val: String,
}

struct JObj {
    name: String,
    items: Vec<Kv>,
}

pub struct Ctx {
    pub json: bool,
    pub csv: bool,
    pub loglevel: i32,
    pub retcode: i32,
    pub autorespond: Option<String>,
    pub devs: Vec<Dev>,
    jobs: Vec<JObj>,
}

static mut CTX: Ctx = Ctx {
    json: false,
    csv: false,
    loglevel: 30,
    retcode: 0,
    autorespond: None,
    devs: Vec::new(),
    jobs: Vec::new(),
};

pub fn ctx() -> &'static mut Ctx {
    unsafe { &mut *std::ptr::addr_of_mut!(CTX) }
}

pub fn ok(st: Status) -> bool {
    st == amdsmi_status_t::AMDSMI_STATUS_SUCCESS
}

pub fn die_oom() -> ! {
    eprintln!("ERROR: out of memory");
    std::process::exit(1);
}

pub fn z<T>() -> T {
    unsafe { std::mem::zeroed() }
}

fn is_c_space(b: u8) -> bool {
    unsafe { libc::isspace(b as c_int) != 0 }
}

pub fn trim_owned(s: &str) -> String {
    let b = s.as_bytes();
    let mut a = 0;
    let mut e = b.len();
    while a < e && is_c_space(b[a]) {
        a += 1;
    }
    while e > a && is_c_space(b[e - 1]) {
        e -= 1;
    }
    String::from_utf8_lossy(&b[a..e]).into_owned()
}

/// Copy at most `cap - 1` bytes, matching snprintf into a `cap`-byte buffer.
pub fn sn(cap: usize, s: &str) -> String {
    if cap == 0 {
        return String::new();
    }
    let b = s.as_bytes();
    let n = b.len().min(cap - 1);
    String::from_utf8_lossy(&b[..n]).into_owned()
}

pub fn cstr_arr(a: &[c_char]) -> String {
    let bytes: Vec<u8> = a.iter().take_while(|&&c| c != 0).map(|&c| c as u8).collect();
    String::from_utf8_lossy(&bytes).into_owned()
}

pub fn str_eq(a: &str, b: &str) -> bool {
    a == b
}

pub fn str_ieq(a: &str, b: &str) -> bool {
    if a.len() != b.len() {
        return a.bytes().zip(b.bytes()).all(|(x, y)| x.to_ascii_lowercase() == y.to_ascii_lowercase())
            && a.len() == b.len();
    }
    a.bytes().zip(b.bytes()).all(|(x, y)| x.to_ascii_lowercase() == y.to_ascii_lowercase())
}

pub fn c_atoi(s: &str) -> i32 {
    let b = s.as_bytes();
    let mut i = 0;
    while i < b.len() && is_c_space(b[i]) {
        i += 1;
    }
    let mut sign = 1i32;
    if i < b.len() && (b[i] == b'+' || b[i] == b'-') {
        if b[i] == b'-' {
            sign = -1;
        }
        i += 1;
    }
    let mut n: i32 = 0;
    while i < b.len() && b[i].is_ascii_digit() {
        n = n.wrapping_mul(10).wrapping_add((b[i] - b'0') as i32);
        i += 1;
    }
    n.wrapping_mul(sign)
}

pub fn c_strtoull(s: &str, mut base: u32) -> u64 {
    let b = s.as_bytes();
    let mut i = 0;
    while i < b.len() && is_c_space(b[i]) {
        i += 1;
    }
    if i < b.len() && (b[i] == b'+' || b[i] == b'-') {
        i += 1;
    }
    if base == 0 {
        if i + 1 < b.len() && b[i] == b'0' && (b[i + 1] == b'x' || b[i + 1] == b'X') {
            base = 16;
            i += 2;
        } else if i < b.len() && b[i] == b'0' {
            base = 8;
            i += 1;
        } else {
            base = 10;
        }
    } else if base == 16 && i + 1 < b.len() && b[i] == b'0' && (b[i + 1] == b'x' || b[i + 1] == b'X') {
        i += 2;
    }
    let mut n: u64 = 0;
    while i < b.len() {
        let d = (b[i] as char).to_digit(base);
        match d {
            Some(d) => {
                n = n.wrapping_mul(base as u64).wrapping_add(d as u64);
                i += 1;
            }
            None => break,
        }
    }
    n
}

/// strtoull(base 16) that requires the whole trimmed string to be consumed and non-zero.
pub fn parse_hex_full(s: &str) -> Option<u64> {
    let t = trim_owned(s);
    if t.is_empty() {
        return None;
    }
    let b = t.as_bytes();
    let mut i = 0;
    if b.len() >= 2 && b[0] == b'0' && (b[1] == b'x' || b[1] == b'X') {
        i = 2;
    }
    if i >= b.len() {
        return None;
    }
    let mut n: u64 = 0;
    while i < b.len() {
        let d = (b[i] as char).to_digit(16)?;
        n = n.wrapping_mul(16).wrapping_add(d as u64);
        i += 1;
    }
    if n == 0 {
        None
    } else {
        Some(n)
    }
}

pub fn c_atof(s: &str) -> f64 {
    let b = s.as_bytes();
    let mut i = 0;
    while i < b.len() && is_c_space(b[i]) {
        i += 1;
    }
    let start = i;
    if i < b.len() && (b[i] == b'+' || b[i] == b'-') {
        i += 1;
    }
    let mut saw = false;
    while i < b.len() && b[i].is_ascii_digit() {
        saw = true;
        i += 1;
    }
    if i < b.len() && b[i] == b'.' {
        i += 1;
        while i < b.len() && b[i].is_ascii_digit() {
            saw = true;
            i += 1;
        }
    }
    if i < b.len() && (b[i] == b'e' || b[i] == b'E') {
        let mut j = i + 1;
        if j < b.len() && (b[j] == b'+' || b[j] == b'-') {
            j += 1;
        }
        let e0 = j;
        while j < b.len() && b[j].is_ascii_digit() {
            j += 1;
        }
        if j > e0 {
            i = j;
        }
    }
    if !saw {
        return 0.0;
    }
    std::str::from_utf8(&b[start..i])
        .ok()
        .and_then(|t| t.parse().ok())
        .unwrap_or(0.0)
}

pub fn read_file(path: &str, n: usize) -> Option<String> {
    let mut f = std::fs::File::open(path).ok()?;
    let mut buf = vec![0u8; n.saturating_sub(1)];
    use std::io::Read;
    let r = std::io::Read::read(&mut f, &mut buf).unwrap_or(0);
    buf.truncate(r);
    let s = String::from_utf8_lossy(&buf).into_owned();
    Some(trim_owned(&s))
}

pub fn read_u64_file(path: &str) -> Option<u64> {
    let buf = read_file(path, 128)?;
    if buf.is_empty() {
        return None;
    }
    let b = buf.as_bytes();
    let mut i = 0;
    let (neg, rest_start) = if b.first() == Some(&b'-') {
        (true, 1)
    } else if b.first() == Some(&b'+') {
        (false, 1)
    } else {
        (false, 0)
    };
    i = rest_start;
    let mut base = 10u32;
    if i + 1 < b.len() && b[i] == b'0' && (b[i + 1] == b'x' || b[i + 1] == b'X') {
        base = 16;
        i += 2;
    } else if i < b.len() && b[i] == b'0' && b.len() > i + 1 {
        base = 8;
        i += 1;
    }
    if i >= b.len() && base != 8 {
        return None;
    }
    if base == 8 && i > rest_start && i == b.len() {
        return Some(0);
    }
    if i >= b.len() {
        return None;
    }
    let mut n: u64 = 0;
    let start_digits = i;
    while i < b.len() {
        match (b[i] as char).to_digit(base) {
            Some(d) => {
                n = n.wrapping_mul(base as u64).wrapping_add(d as u64);
                i += 1;
            }
            None => break,
        }
    }
    if i == start_digits {
        return None;
    }
    if neg {
        n = n.wrapping_neg();
    }
    Some(n)
}

pub fn path_exists(path: &str) -> bool {
    let c = match CString::new(path) {
        Ok(c) => c,
        Err(_) => return false,
    };
    unsafe { libc::access(c.as_ptr(), libc::F_OK) == 0 }
}

fn jobj_get(name: &str, create: bool) -> Option<usize> {
    let c = ctx();
    if let Some(i) = c.jobs.iter().position(|j| j.name == name) {
        return Some(i);
    }
    if !create {
        return None;
    }
    if c.jobs.len() >= MAX_DEV + 1 {
        return None;
    }
    c.jobs.push(JObj {
        name: sn(32, name),
        items: Vec::new(),
    });
    Some(c.jobs.len() - 1)
}

pub fn json_add_name(name: &str, key: &str, val: &str) {
    let Some(idx) = jobj_get(name, true) else { return };
    let c = ctx();
    let key = sn(192, key);
    let val = sn(512, val);
    if let Some(k) = c.jobs[idx].items.iter_mut().find(|k| k.key == key) {
        k.val = val;
        return;
    }
    c.jobs[idx].items.push(Kv { key, val });
}

pub fn json_add(dev: i32, key: &str, val: &str) {
    let name = if dev < 0 {
        "system".to_string()
    } else {
        format!("card{dev}")
    };
    json_add_name(&name, key, val);
}

fn json_escape(s: &str) -> String {
    let mut out = String::new();
    for &b in s.as_bytes() {
        if b == b'"' || b == b'\\' {
            if out.len() + 3 >= 640 && out.len() > 200 {
                /* caller uses different sizes; bound loosely then trim */
            }
            out.push('\\');
        }
        if b < 0x20 {
            continue;
        }
        out.push(b as char);
    }
    out
}

fn json_escape_n(s: &str, n: usize) -> String {
    let mut out = String::new();
    for &b in s.as_bytes() {
        if out.len() + 2 >= n {
            break;
        }
        if b == b'"' || b == b'\\' {
            if out.len() + 3 >= n {
                break;
            }
            out.push('\\');
        }
        if b < 0x20 {
            continue;
        }
        out.push(b as char);
    }
    out
}

pub fn json_dump() {
    let jobs: Vec<(String, Vec<(String, String)>)> = ctx()
        .jobs
        .iter()
        .filter(|j| !j.items.is_empty())
        .map(|j| {
            (
                j.name.clone(),
                j.items.iter().map(|k| (k.key.clone(), k.val.clone())).collect(),
            )
        })
        .collect();
    print!("{{");
    let mut first_obj = true;
    for (name, items) in &jobs {
        if !first_obj {
            print!(",");
        }
        first_obj = false;
        let ek = json_escape_n(name, 256);
        print!("\"{ek}\": {{");
        for (k, (key, val)) in items.iter().enumerate() {
            let ek = json_escape_n(key, 256);
            let ev = json_escape_n(val, 640);
            print!("{}\"{ek}\": \"{ev}\"", if k == 0 { "" } else { ", " });
        }
        print!("}}");
    }
    println!("}}");
}

pub fn json_empty() -> bool {
    ctx().jobs.iter().all(|j| j.items.is_empty())
}

pub fn csv_dump(devs: &[i32]) {
    let nd = devs.len();
    let sys = ctx()
        .jobs
        .iter()
        .any(|j| j.name == "system" && !j.items.is_empty());
    if sys && nd == 0 {
        println!("name, value");
        if let Some(system) = ctx().jobs.iter().find(|j| j.name == "system") {
            for k in &system.items {
                println!("\"{}\", \"{}\"", k.key, k.val);
            }
        }
        return;
    }
    let mut headers: Vec<String> = Vec::new();
    for &d in devs {
        let name = format!("card{d}");
        let Some(o) = ctx().jobs.iter().find(|j| j.name == name) else { continue };
        for k in &o.items {
            if !headers.iter().any(|h| h == &k.key) && headers.len() < 512 {
                headers.push(k.key.clone());
            }
        }
    }
    print!("device");
    for h in &headers {
        print!(",{h}");
    }
    println!();
    for &d in devs {
        let name = format!("card{d}");
        print!("card{d}");
        let obj = ctx().jobs.iter().find(|j| j.name == name);
        for h in &headers {
            let v = obj
                .and_then(|o| o.items.iter().find(|k| k.key == *h))
                .map(|k| k.val.as_str())
                .unwrap_or("N/A");
            print!(",");
            for ch in v.chars() {
                if ch != ',' {
                    print!("{ch}");
                }
            }
        }
        println!();
    }
}

pub fn print_spacer(display: Option<&str>, fill: &str, fit: i32) {
    if ctx().json {
        return;
    }
    let fill_b = fill.as_bytes().first().copied().unwrap_or(b'=');
    let mut width = if fit > 0 { fit } else { APP_WIDTH };
    if width % 2 != 0 {
        width += 1;
    }
    let display = display.unwrap_or("");
    if !display.is_empty() {
        let mut shown = sn(256, display);
        let mut dlen = shown.len() as i32;
        if dlen % 2 != 0 && (dlen as usize) + 1 < 256 {
            shown.push(fill_b as char);
            dlen += 1;
        }
        let mut pad = (width - dlen) / 2;
        if pad < 0 {
            pad = 0;
        }
        for _ in 0..pad {
            print!("{}", fill_b as char);
        }
        print!("{shown}");
        let rest = width - dlen - pad;
        for _ in 0..rest {
            print!("{}", fill_b as char);
        }
        println!();
    } else {
        for _ in 0..width {
            print!("{}", fill_b as char);
        }
        println!();
    }
}

pub fn print_log(dev: i32, metric: Option<&str>, value: Option<&str>) {
    let has_val = value.map(|v| !v.is_empty()).unwrap_or(false);
    if ctx().json {
        if let (Some(m), Some(v)) = (metric, value) {
            if has_val {
                json_add(dev, m, v);
            }
        }
        return;
    }
    if dev < 0 {
        if let Some(m) = metric {
            if has_val {
                println!("{m}: {}", value.unwrap_or(""));
            } else {
                println!("{m}");
            }
        } else {
            println!("{}", value.unwrap_or(""));
        }
        return;
    }
    match metric {
        None => println!("GPU[{dev}]\t\t: {}", value.unwrap_or("")),
        Some(m) if has_val => println!("GPU[{dev}]\t\t: {m}: {}", value.unwrap_or("")),
        Some(m) => println!("GPU[{dev}]\t\t: {m}"),
    }
    let _ = std::io::stdout().flush();
}

pub fn print_logf_str(dev: i32, metric: Option<&str>, formatted: &str) {
    let buf = sn(768, formatted);
    print_log(dev, metric, Some(&buf));
}

pub fn log_err(msg: &str) {
    if ctx().loglevel > 40 {
        return;
    }
    eprintln!("ERROR: {msg}");
}

pub fn log_warn(msg: &str) {
    if ctx().loglevel > 30 {
        return;
    }
    eprintln!("WARNING: {msg}");
}

#[macro_export]
macro_rules! print_logf {
    ($dev:expr, $metric:expr, $($t:tt)*) => {{
        let s = format!($($t)*);
        $crate::core::print_logf_str($dev, $metric, &s);
    }};
}

#[macro_export]
macro_rules! log_errf {
    ($($t:tt)*) => {{
        let s = format!($($t)*);
        $crate::core::log_err(&s);
    }};
}

#[macro_export]
macro_rules! log_warnf {
    ($($t:tt)*) => {{
        let s = format!($($t)*);
        $crate::core::log_warn(&s);
    }};
}

pub fn driver_live() -> bool {
    if !path_exists("/sys/module/amdgpu") {
        return false;
    }
    if !path_exists("/sys/module/amdgpu/initstate") {
        return true;
    }
    match read_file("/sys/module/amdgpu/initstate", 64) {
        Some(buf) => buf.contains("live"),
        None => false,
    }
}

fn wake_card(card: u32, render: u32) {
    let path = if render != 0 {
        format!("/dev/dri/renderD{render}")
    } else {
        format!("/dev/dri/card{card}")
    };
    let Ok(cpath) = CString::new(path) else { return };
    let fd = unsafe { libc::open(cpath.as_ptr(), libc::O_RDWR | libc::O_CLOEXEC) };
    if fd < 0 {
        return;
    }
    let mut req = [0u8; 32];
    unsafe {
        libc::ioctl(fd, DRM_IOCTL_AMDGPU_INFO_WAKE, req.as_mut_ptr());
        libc::close(fd);
    }
}

pub fn discover() -> i32 {
    unsafe {
        let mut st = amdsmi_init(amdsmi_init_flags_t::AMDSMI_INIT_AMD_GPUS as u64);
        if !ok(st) {
            st = amdsmi_init(amdsmi_init_flags_t::AMDSMI_INIT_AMD_APUS as u64);
        }
        if !ok(st) {
            return -1;
        }
        let mut sc: u32 = 0;
        if !ok(amdsmi_get_socket_handles(&mut sc, std::ptr::null_mut())) || sc == 0 {
            let mut pc: u32 = MAX_DEV as u32;
            let mut hs = [std::ptr::null_mut(); MAX_DEV];
            if ok(amdsmi_get_processor_handles_by_type(
                std::ptr::null_mut(),
                amdsmi_processor_type_t::AMDSMI_PROCESSOR_TYPE_AMD_GPU,
                hs.as_mut_ptr(),
                &mut pc,
            )) {
                for i in 0..pc as usize {
                    if ctx().devs.len() >= MAX_DEV {
                        break;
                    }
                    ctx().devs.push(Dev {
                        h: hs[i],
                        card: 0,
                        render: 0,
                        have_enum: false,
                    });
                }
            }
        } else {
            let mut socks = vec![std::ptr::null_mut(); sc as usize];
            if !ok(amdsmi_get_socket_handles(&mut sc, socks.as_mut_ptr())) {
                return if ctx().devs.is_empty() { -1 } else { 0 };
            }
            socks.truncate(sc as usize);
            for sock in socks {
                let mut pc: u32 = 0;
                if !ok(amdsmi_get_processor_handles(sock, &mut pc, std::ptr::null_mut())) || pc == 0 {
                    continue;
                }
                let mut hs = vec![std::ptr::null_mut(); pc as usize];
                if !ok(amdsmi_get_processor_handles(sock, &mut pc, hs.as_mut_ptr())) {
                    continue;
                }
                hs.truncate(pc as usize);
                for h in hs {
                    if ctx().devs.len() >= MAX_DEV {
                        break;
                    }
                    let mut t = amdsmi_processor_type_t::AMDSMI_PROCESSOR_TYPE_UNKNOWN;
                    amdsmi_get_processor_type(h, &mut t);
                    if t != amdsmi_processor_type_t::AMDSMI_PROCESSOR_TYPE_AMD_GPU
                        && t != amdsmi_processor_type_t::AMDSMI_PROCESSOR_TYPE_AMD_APU
                        && t != amdsmi_processor_type_t::AMDSMI_PROCESSOR_TYPE_UNKNOWN
                    {
                        continue;
                    }
                    ctx().devs.push(Dev {
                        h,
                        card: 0,
                        render: 0,
                        have_enum: false,
                    });
                }
            }
        }
        let n = ctx().devs.len();
        for i in 0..n {
            let h = ctx().devs[i].h;
            let mut en: amdsmi_enumeration_info_t = z();
            if ok(amdsmi_get_gpu_enumeration_info(h, &mut en)) {
                ctx().devs[i].card = en.drm_card;
                ctx().devs[i].render = en.drm_render;
                ctx().devs[i].have_enum = true;
                wake_card(en.drm_card, en.drm_render);
            }
        }
        if ctx().devs.is_empty() {
            -1
        } else {
            0
        }
    }
}

pub fn ndev() -> usize {
    ctx().devs.len()
}

pub fn dev_handle(id: usize) -> amdsmi_processor_handle {
    ctx().devs[id].h
}

pub fn dev_card(id: usize) -> u32 {
    ctx().devs[id].card
}

pub struct ClkMap {
    pub name: &'static str,
    pub ty: amdsmi_clk_type_t::Type,
}

pub fn clks() -> &'static [ClkMap] {
    use amdsmi_clk_type_t::*;
    &[
        ClkMap { name: "dcef", ty: AMDSMI_CLK_TYPE_DCEF },
        ClkMap { name: "dclk0", ty: AMDSMI_CLK_TYPE_DCLK0 },
        ClkMap { name: "dclk1", ty: AMDSMI_CLK_TYPE_DCLK1 },
        ClkMap { name: "fclk", ty: AMDSMI_CLK_TYPE_DF },
        ClkMap { name: "mclk", ty: AMDSMI_CLK_TYPE_MEM },
        ClkMap { name: "pcie", ty: AMDSMI_CLK_TYPE_PCIE },
        ClkMap { name: "sclk", ty: AMDSMI_CLK_TYPE_SYS },
        ClkMap { name: "socclk", ty: AMDSMI_CLK_TYPE_SOC },
        ClkMap { name: "vclk0", ty: AMDSMI_CLK_TYPE_VCLK0 },
        ClkMap { name: "vclk1", ty: AMDSMI_CLK_TYPE_VCLK1 },
    ]
}

pub fn perf_name(p: amdsmi_dev_perf_level_t::Type) -> Option<&'static str> {
    use amdsmi_dev_perf_level_t::*;
    Some(match p {
        AMDSMI_DEV_PERF_LEVEL_AUTO => "auto",
        AMDSMI_DEV_PERF_LEVEL_LOW => "low",
        AMDSMI_DEV_PERF_LEVEL_HIGH => "high",
        AMDSMI_DEV_PERF_LEVEL_MANUAL => "manual",
        AMDSMI_DEV_PERF_LEVEL_STABLE_STD => "stable_std",
        AMDSMI_DEV_PERF_LEVEL_STABLE_PEAK => "stable_peak",
        AMDSMI_DEV_PERF_LEVEL_STABLE_MIN_MCLK => "stable_min_mclk",
        AMDSMI_DEV_PERF_LEVEL_STABLE_MIN_SCLK => "stable_min_sclk",
        AMDSMI_DEV_PERF_LEVEL_DETERMINISM => "determinism",
        _ => return None,
    })
}

pub fn perf_from_name(s: &str) -> Option<amdsmi_dev_perf_level_t::Type> {
    use amdsmi_dev_perf_level_t::*;
    let t = [
        ("auto", AMDSMI_DEV_PERF_LEVEL_AUTO),
        ("low", AMDSMI_DEV_PERF_LEVEL_LOW),
        ("high", AMDSMI_DEV_PERF_LEVEL_HIGH),
        ("manual", AMDSMI_DEV_PERF_LEVEL_MANUAL),
        ("stable_std", AMDSMI_DEV_PERF_LEVEL_STABLE_STD),
        ("stable_peak", AMDSMI_DEV_PERF_LEVEL_STABLE_PEAK),
        ("stable_min_mclk", AMDSMI_DEV_PERF_LEVEL_STABLE_MIN_MCLK),
        ("stable_min_sclk", AMDSMI_DEV_PERF_LEVEL_STABLE_MIN_SCLK),
        ("determinism", AMDSMI_DEV_PERF_LEVEL_DETERMINISM),
    ];
    t.iter().find(|(n, _)| str_ieq(s, n)).map(|(_, v)| *v)
}

pub fn clk_lookup(name: &str) -> Option<amdsmi_clk_type_t::Type> {
    for c in clks() {
        if str_ieq(name, c.name)
            || (str_ieq(name, "soc") && c.ty == amdsmi_clk_type_t::AMDSMI_CLK_TYPE_SOC)
            || (str_ieq(name, "df") && c.ty == amdsmi_clk_type_t::AMDSMI_CLK_TYPE_DF)
        {
            return Some(c.ty);
        }
    }
    None
}

pub fn temp_c(dev: usize, t: amdsmi_temperature_type_t::Type) -> Option<f64> {
    let mut v: i64 = 0;
    let st = unsafe {
        amdsmi_get_temp_metric(dev_handle(dev), t, amdsmi_temperature_metric_t::AMDSMI_TEMP_CURRENT, &mut v)
    };
    if !ok(st) {
        return None;
    }
    if v > 1000 || v < -1000 {
        Some(v as f64 / 1000.0)
    } else {
        Some(v as f64)
    }
}

pub struct CurClk {
    pub mhz: f64,
    pub level: i32,
    pub deep: bool,
}

pub fn current_clk_mhz(dev: usize, t: amdsmi_clk_type_t::Type) -> Option<CurClk> {
    let mut f: amdsmi_frequencies_t = z();
    if !ok(unsafe { amdsmi_get_clk_freq(dev_handle(dev), t, &mut f) }) {
        return None;
    }
    if f.num_supported == 0 || f.current >= f.num_supported {
        return None;
    }
    Some(CurClk {
        mhz: f.frequency[f.current as usize] as f64 / 1e6,
        level: f.current as i32,
        deep: f.has_deep_sleep,
    })
}

pub fn fmt_level(level: i32, deep: bool) -> String {
    if deep && level == 0 {
        "S".into()
    } else if deep {
        format!("{}", level - 1)
    } else {
        format!("{level}")
    }
}

pub fn fmt_py_num(v: f64) -> String {
    let tmp = format!("{v:.10}");
    let Some(dot) = tmp.find('.') else { return tmp };
    let b = tmp.as_bytes();
    let mut e = b.len();
    while e > dot + 1 && b[e - 1] == b'0' {
        e -= 1;
    }
    if e == dot + 1 {
        let mut s = tmp[..dot].to_string();
        s.push_str(".0");
        s
    } else {
        tmp[..e].to_string()
    }
}

pub fn util_value_ok(v: u64) -> bool {
    v != u64::MAX && v != 0xFFFF_FFFF
}

pub fn mem_busy_pct(dev: usize) -> Option<u32> {
    let path = format!("/sys/class/drm/card{}/device/mem_busy_percent", dev_card(dev));
    read_u64_file(&path).map(|v| v as u32)
}

pub struct PowerW {
    pub w: f64,
    pub ptype: &'static str,
    pub label: &'static str,
}

pub fn power_watts(dev: usize) -> Option<PowerW> {
    let mut p: amdsmi_power_info_t = z();
    if !ok(unsafe { amdsmi_get_power_info(dev_handle(dev), &mut p) }) {
        return None;
    }
    if p.average_socket_power != 0xFFFF_FFFF {
        return Some(PowerW { w: p.average_socket_power as f64, ptype: "AVERAGE", label: "(Avg)" });
    }
    if p.current_socket_power != 0xFFFF_FFFF {
        return Some(PowerW {
            w: p.current_socket_power as f64,
            ptype: "CURRENT SOCKET",
            label: "(Socket)",
        });
    }
    if p.socket_power != u64::MAX && p.socket_power != 0 {
        let w = if p.socket_power > 100_000 {
            p.socket_power as f64 / 1e6
        } else {
            p.socket_power as f64
        };
        return Some(PowerW { w, ptype: "AVERAGE", label: "(Avg)" });
    }
    None
}

pub struct Fan {
    pub level: i64,
    pub pct: i32,
    pub rpm: i64,
    pub st: Status,
}

pub fn fan_level_pct(dev: usize) -> Result<Fan, Status> {
    let mut lv: i64 = 0;
    let st = unsafe { amdsmi_get_gpu_fan_speed(dev_handle(dev), 0, &mut lv) };
    if !ok(st) {
        return Err(st);
    }
    let mut mx: u64 = 0;
    if !ok(unsafe { amdsmi_get_gpu_fan_speed_max(dev_handle(dev), 0, &mut mx) }) || mx == 0 {
        mx = 255;
    }
    let pct = ((lv * 100 + (mx as i64) / 2) / mx as i64) as i32;
    let mut rp: i64 = 0;
    let rpm = if ok(unsafe { amdsmi_get_gpu_fan_rpms(dev_handle(dev), 0, &mut rp) }) {
        rp
    } else {
        -1
    };
    Ok(Fan { level: lv, pct, rpm, st: amdsmi_status_t::AMDSMI_STATUS_SUCCESS })
}

pub fn gpu_busy(dev: usize) -> Option<u32> {
    let mut pct = 0u32;
    if ok(unsafe { amdsmi_get_gpu_busy_percent(dev_handle(dev), &mut pct) }) {
        Some(pct)
    } else {
        None
    }
}

pub fn mem_bytes(dev: usize, t: amdsmi_memory_type_t::Type) -> Option<(u64, u64)> {
    let mut used = 0u64;
    let mut total = 0u64;
    let uok = ok(unsafe { amdsmi_get_gpu_memory_usage(dev_handle(dev), t, &mut used) });
    let tok = ok(unsafe { amdsmi_get_gpu_memory_total(dev_handle(dev), t, &mut total) });
    if uok && tok {
        Some((used, total))
    } else {
        None
    }
}

pub fn vram_pct(dev: usize) -> Option<i32> {
    let (u, t) = mem_bytes(dev, amdsmi_memory_type_t::AMDSMI_MEM_TYPE_VRAM)?;
    if t == 0 {
        return None;
    }
    Some(((100.0 * u as f64) / t as f64) as i32)
}

pub fn power_cap_w(dev: usize) -> Option<f64> {
    let mut c: amdsmi_power_cap_info_t = z();
    if !ok(unsafe { amdsmi_get_power_cap_info(dev_handle(dev), 0, &mut c) }) {
        return None;
    }
    if c.power_cap > 100_000 {
        Some(c.power_cap as f64 / 1e6)
    } else {
        Some(c.power_cap as f64)
    }
}

pub fn device_id_str(dev: usize) -> String {
    let mut a: amdsmi_asic_info_t = z();
    if !ok(unsafe { amdsmi_get_gpu_asic_info(dev_handle(dev), &mut a) })
        || a.device_id == 0
        || a.device_id == 0xFFFF_FFFF_FFFF_FFFF
    {
        return "N/A".into();
    }
    format!("0x{:x}", a.device_id)
}

pub fn guid_str(dev: usize) -> String {
    let mut k: amdsmi_kfd_info_t = z();
    if ok(unsafe { amdsmi_get_gpu_kfd_info(dev_handle(dev), &mut k) }) && k.kfd_id != u64::MAX {
        format!("{}", k.kfd_id)
    } else {
        "N/A".into()
    }
}

pub fn node_id(dev: usize) -> u32 {
    let mut k: amdsmi_kfd_info_t = z();
    if ok(unsafe { amdsmi_get_gpu_kfd_info(dev_handle(dev), &mut k) }) && k.node_id != 0xFFFF_FFFF {
        k.node_id
    } else {
        dev as u32
    }
}

pub fn unique_id_str(dev: usize) -> String {
    let path = format!("/sys/class/drm/card{}/device/unique_id", dev_card(dev));
    if let Some(raw) = read_file(&path, 128) {
        if !raw.is_empty() {
            if let Some(u) = parse_hex_full(&raw) {
                return format!("0x{u:x}");
            }
        }
    }
    let path = format!("/sys/class/kfd/kfd/topology/nodes/{}/properties", node_id(dev));
    if let Ok(f) = std::fs::File::open(path) {
        for line in std::io::BufReader::new(f).lines() {
            let line = line.unwrap_or_default();
            if let Some(rest) = line.strip_prefix("unique_id ") {
                let tok = rest.split_whitespace().next().unwrap_or("");
                if let Ok(u) = tok.parse::<u64>() {
                    if u != 0 {
                        return format!("0x{u:x}");
                    }
                }
            }
        }
    }
    "N/A".into()
}

pub fn vbios_ver(v: &amdsmi_vbios_info_t) -> String {
    let part = cstr_arr(&v.part_number);
    if !part.is_empty() {
        return part;
    }
    let ver = cstr_arr(&v.version);
    if !ver.is_empty() {
        return ver;
    }
    "N/A".into()
}

pub fn part_name(t: amdsmi_accelerator_partition_type_t::Type) -> &'static str {
    use amdsmi_accelerator_partition_type_t::*;
    match t {
        AMDSMI_ACCELERATOR_PARTITION_SPX => "SPX",
        AMDSMI_ACCELERATOR_PARTITION_DPX => "DPX",
        AMDSMI_ACCELERATOR_PARTITION_TPX => "TPX",
        AMDSMI_ACCELERATOR_PARTITION_QPX => "QPX",
        AMDSMI_ACCELERATOR_PARTITION_CPX => "CPX",
        _ => "N/A",
    }
}

pub fn mem_part(dev: usize) -> String {
    let mut buf = [0 as c_char; 32];
    let st = unsafe { amdsmi_get_gpu_memory_partition(dev_handle(dev), buf.as_mut_ptr(), buf.len() as u32) };
    let s = cstr_arr(&buf);
    if !ok(st) || s.is_empty() {
        "N/A".into()
    } else {
        sn(32, &s)
    }
}

pub fn comp_part(dev: usize) -> String {
    let mut p: amdsmi_accelerator_partition_profile_t = z();
    let mut id = [0u32; 8];
    if ok(unsafe { amdsmi_get_gpu_accelerator_partition_profile(dev_handle(dev), &mut p, id.as_mut_ptr()) }) {
        part_name(p.profile_type).into()
    } else {
        "N/A".into()
    }
}

pub fn part_id_str(dev: usize) -> String {
    let mut k: amdsmi_kfd_info_t = z();
    if ok(unsafe { amdsmi_get_gpu_kfd_info(dev_handle(dev), &mut k) }) && k.current_partition_id != 0xFFFF_FFFF {
        format!("{}", k.current_partition_id)
    } else {
        "0".into()
    }
}

pub fn bdf_str(dev: usize) -> String {
    let mut b: amdsmi_bdf_t = z();
    if !ok(unsafe { amdsmi_get_gpu_device_bdf(dev_handle(dev), &mut b) }) {
        return "N/A".into();
    }
    let domain = unsafe { b.bdf.domain_number() };
    let bus = unsafe { b.bdf.bus_number() };
    let device = unsafe { b.bdf.device_number() };
    let func = unsafe { b.bdf.function_number() };
    format!("{domain:04X}:{bus:02X}:{device:02X}.{func:X}")
}

pub fn ras_state_name(s: amdsmi_ras_err_state_t::Type) -> &'static str {
    use amdsmi_ras_err_state_t::*;
    match s {
        AMDSMI_RAS_ERR_STATE_NONE => "NONE",
        AMDSMI_RAS_ERR_STATE_DISABLED => "DISABLED",
        AMDSMI_RAS_ERR_STATE_PARITY => "PARITY",
        AMDSMI_RAS_ERR_STATE_SING_C => "SING_C",
        AMDSMI_RAS_ERR_STATE_MULT_UC => "MULT_UC",
        AMDSMI_RAS_ERR_STATE_POISON => "POISON",
        AMDSMI_RAS_ERR_STATE_ENABLED => "ENABLED",
        _ => "N/A",
    }
}

pub struct RasBlk {
    pub name: &'static str,
    pub bit: amdsmi_gpu_block_t::Type,
}

pub fn ras_blocks() -> &'static [RasBlk] {
    use amdsmi_gpu_block_t::*;
    &[
        RasBlk { name: "UMC", bit: AMDSMI_GPU_BLOCK_UMC },
        RasBlk { name: "SDMA", bit: AMDSMI_GPU_BLOCK_SDMA },
        RasBlk { name: "GFX", bit: AMDSMI_GPU_BLOCK_GFX },
        RasBlk { name: "MMHUB", bit: AMDSMI_GPU_BLOCK_MMHUB },
        RasBlk { name: "ATHUB", bit: AMDSMI_GPU_BLOCK_ATHUB },
        RasBlk { name: "PCIE_BIF", bit: AMDSMI_GPU_BLOCK_PCIE_BIF },
        RasBlk { name: "HDP", bit: AMDSMI_GPU_BLOCK_HDP },
        RasBlk { name: "XGMI_WAFL", bit: AMDSMI_GPU_BLOCK_XGMI_WAFL },
        RasBlk { name: "DF", bit: AMDSMI_GPU_BLOCK_DF },
        RasBlk { name: "SMN", bit: AMDSMI_GPU_BLOCK_SMN },
        RasBlk { name: "SEM", bit: AMDSMI_GPU_BLOCK_SEM },
        RasBlk { name: "MP0", bit: AMDSMI_GPU_BLOCK_MP0 },
        RasBlk { name: "MP1", bit: AMDSMI_GPU_BLOCK_MP1 },
        RasBlk { name: "FUSE", bit: AMDSMI_GPU_BLOCK_FUSE },
        RasBlk { name: "MCA", bit: AMDSMI_GPU_BLOCK_MCA },
        RasBlk { name: "VCN", bit: AMDSMI_GPU_BLOCK_VCN },
        RasBlk { name: "JPEG", bit: AMDSMI_GPU_BLOCK_JPEG },
    ]
}

pub fn fw_name(id: amdsmi_fw_block_t::Type) -> Option<&'static str> {
    use amdsmi_fw_block_t::*;
    Some(match id {
        AMDSMI_FW_ID_SMU => "SMC",
        AMDSMI_FW_ID_CP_CE => "CE",
        AMDSMI_FW_ID_CP_PFP => "PFP",
        AMDSMI_FW_ID_CP_ME => "ME",
        AMDSMI_FW_ID_CP_MEC_JT1 => "MEC JT1",
        AMDSMI_FW_ID_CP_MEC_JT2 => "MEC JT2",
        AMDSMI_FW_ID_CP_MEC1 => "MEC",
        AMDSMI_FW_ID_CP_MEC2 => "MEC2",
        AMDSMI_FW_ID_RLC => "RLC",
        AMDSMI_FW_ID_SDMA0 => "SDMA0",
        AMDSMI_FW_ID_SDMA1 => "SDMA1",
        AMDSMI_FW_ID_SDMA2 => "SDMA2",
        AMDSMI_FW_ID_SDMA3 => "SDMA3",
        AMDSMI_FW_ID_SDMA4 => "SDMA4",
        AMDSMI_FW_ID_SDMA5 => "SDMA5",
        AMDSMI_FW_ID_SDMA6 => "SDMA6",
        AMDSMI_FW_ID_SDMA7 => "SDMA7",
        AMDSMI_FW_ID_VCN => "VCN",
        AMDSMI_FW_ID_UVD => "UVD",
        AMDSMI_FW_ID_VCE => "VCE",
        AMDSMI_FW_ID_MC => "MC",
        AMDSMI_FW_ID_ASD => "ASD",
        AMDSMI_FW_ID_TA_RAS => "TA RAS",
        AMDSMI_FW_ID_TA_XGMI => "TA XGMI",
        AMDSMI_FW_ID_CP_MES => "MES",
        AMDSMI_FW_ID_MES_KIQ => "MES KIQ",
        AMDSMI_FW_ID_PSP_SOSDRV => "SOS",
        _ => return None,
    })
}

pub fn fmt_fw(name: &str, ver: u64) -> String {
    let hexv = (ver & 0xFFFF_FFFF) as u32;
    if matches!(name, "VCN" | "VCE" | "UVD" | "SOS" | "ASD" | "MES" | "MES KIQ") {
        format!("0x{hexv:08x}")
    } else if matches!(name, "TA XGMI" | "TA RAS" | "SMC") {
        format!(
            "{:02}.{:02}.{:02}.{:02}",
            (hexv >> 24) & 0xFF,
            (hexv >> 16) & 0xFF,
            (hexv >> 8) & 0xFF,
            hexv & 0xFF
        )
    } else {
        format!("{ver}")
    }
}

pub fn link_type_name(t: amdsmi_link_type_t::Type) -> &'static str {
    use amdsmi_link_type_t::*;
    match t {
        AMDSMI_LINK_TYPE_PCIE => "PCIE",
        AMDSMI_LINK_TYPE_XGMI => "XGMI",
        AMDSMI_LINK_TYPE_INTERNAL => "INTERNAL",
        AMDSMI_LINK_TYPE_NUMA => "NUMA",
        _ => "XXXX",
    }
}

pub fn profile_name(mask: u64) -> &'static str {
    use amdsmi_power_profile_preset_masks_t::*;
    match mask {
        x if x == AMDSMI_PWR_PROF_PRST_CUSTOM_MASK as u64 => "CUSTOM",
        x if x == AMDSMI_PWR_PROF_PRST_VIDEO_MASK as u64 => "VIDEO",
        x if x == AMDSMI_PWR_PROF_PRST_POWER_SAVING_MASK as u64 => "POWER SAVING",
        x if x == AMDSMI_PWR_PROF_PRST_COMPUTE_MASK as u64 => "COMPUTE",
        x if x == AMDSMI_PWR_PROF_PRST_VR_MASK as u64 => "VR",
        x if x == AMDSMI_PWR_PROF_PRST_3D_FULL_SCR_MASK as u64 => "3D FULL SCREEN",
        x if x == AMDSMI_PWR_PROF_PRST_BOOTUP_DEFAULT as u64 => "BOOTUP DEFAULT",
        _ => "UNKNOWN",
    }
}

pub fn confirm_danger() -> bool {
    print!(
        "\n\
          ******WARNING******\n\
          \n\
          Operating your AMD GPU outside of official AMD specifications or outside of\n\
          factory settings, including but not limited to the conducting of overclocking,\n\
          over-volting or under-volting (including use of this interface software,\n\
          even if such software has been directly or indirectly provided by AMD or otherwise\n\
          affiliated in any way with AMD), may cause damage to your AMD GPU, system components\n\
          and/or result in system failure, as well as cause other problems.\n\
          DAMAGES CAUSED BY USE OF YOUR AMD GPU OUTSIDE OF OFFICIAL AMD SPECIFICATIONS OR\n\
          OUTSIDE OF FACTORY SETTINGS ARE NOT COVERED UNDER ANY AMD PRODUCT WARRANTY AND\n\
          MAY NOT BE COVERED BY YOUR BOARD OR SYSTEM MANUFACTURER'S WARRANTY.\n\
          Use this utility with caution.\n\
          \n\
          Do you accept these terms? [y/N] "
    );
    let _ = std::io::stdout().flush();
    if let Some(resp) = ctx().autorespond.clone() {
        println!("{resp}");
        return resp.as_bytes().first().is_some_and(|c| *c == b'y' || *c == b'Y');
    }
    let mut buf = [0u8; 64];
    let n = unsafe { libc::read(libc::STDIN_FILENO, buf.as_mut_ptr() as *mut _, 63) };
    if n <= 0 {
        return false;
    }
    buf[0] == b'y' || buf[0] == b'Y'
}

pub fn status_line(dev: i32, st: Status, what: &str) {
    if ok(st) {
        print_log(dev, Some(what), None);
        return;
    }
    let mut s: *const c_char = std::ptr::null();
    unsafe { amdsmi_status_code_to_string(st, &mut s) };
    let msg = if s.is_null() {
        "error".to_string()
    } else {
        unsafe { CStr::from_ptr(s) }.to_string_lossy().into_owned()
    };
    if st == amdsmi_status_t::AMDSMI_STATUS_NOT_SUPPORTED {
        print_log(dev, Some("Not supported on the given system"), None);
    } else if st == amdsmi_status_t::AMDSMI_STATUS_NO_PERM {
        print_log(dev, Some("Permission denied"), None);
    } else {
        print_logf!(dev, Some("Failed"), "{what} ({msg})");
    }
    ctx().retcode = 1;
}

pub fn append_padded(line: &mut String, width: i32, s: &str) {
    let sb = s.as_bytes();
    let mut sl = sb.len();
    let room = 319usize;
    if sl > room {
        sl = room;
    }
    let mut pad = 0usize;
    if width > sl as i32 {
        pad = width as usize - sl;
        if pad > room - sl {
            pad = room - sl;
        }
    }
    let mut col = Vec::with_capacity(sl + pad + 1);
    col.extend_from_slice(&sb[..sl]);
    col.extend(std::iter::repeat(b' ').take(pad));
    let used = line.len();
    let line_n = 2048usize;
    if line_n > used + 1 {
        let take = col.len().min(line_n - used - 1);
        line.push_str(&String::from_utf8_lossy(&col[..take]));
    }
}

pub fn na_u16(v: u16) -> String {
    if v == 0xFFFF {
        "N/A".into()
    } else {
        format!("{v}")
    }
}

pub fn na_u32(v: u32) -> String {
    if v == 0xFFFF_FFFF {
        "N/A".into()
    } else {
        format!("{v}")
    }
}

pub fn na_u64(v: u64) -> String {
    if v == u64::MAX {
        "N/A".into()
    } else {
        format!("{v}")
    }
}

fn push_bounded(buf: &mut String, s: &str, cap: usize) {
    let room = cap.saturating_sub(buf.len());
    let b = s.as_bytes();
    let n = b.len().min(room);
    buf.push_str(&String::from_utf8_lossy(&b[..n]));
}

pub fn u16_list(v: &[u16]) -> String {
    let mut buf = String::from("[");
    for (i, x) in v.iter().enumerate() {
        let t = if *x == 0xFFFF { "N/A".to_string() } else { format!("{x}") };
        if i > 0 {
            push_bounded(&mut buf, ", ", 511);
        }
        push_bounded(&mut buf, &t, 511);
    }
    push_bounded(&mut buf, "]", 511);
    buf
}

pub fn u64_list(v: &[u64]) -> String {
    let mut buf = String::from("[");
    for (i, x) in v.iter().enumerate() {
        let t = if *x == u64::MAX { "N/A".to_string() } else { format!("{x}") };
        if i > 0 {
            push_bounded(&mut buf, ", ", 511);
        }
        push_bounded(&mut buf, &t, 511);
    }
    push_bounded(&mut buf, "]", 511);
    buf
}

pub fn proc_name(pid: u32) -> String {
    let path = format!("/proc/{pid}/comm");
    read_file(&path, 64).filter(|s| !s.is_empty()).unwrap_or_else(|| "UNKNOWN".into())
}

pub fn errno_str() -> String {
    let e = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
    unsafe {
        let p = libc::strerror(e);
        if p.is_null() {
            "Unknown error".into()
        } else {
            CStr::from_ptr(p).to_string_lossy().into_owned()
        }
    }
}

pub fn shut_down() {
    unsafe { amdsmi_shut_down(); }
}
