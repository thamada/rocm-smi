//! rocm-smi — ROCm System Management Interface (Rust)
//!
//! Command-line compatible with `rocm-smi.c` (CLI 4.x): the same flags,
//! the same log layout, and the same concise table.

mod core;
mod setcmd;
mod show;

use core::*;
use setcmd::*;
use show::*;
use std::ffi::CString;

struct Args {
    help: bool,
    version: bool,
    alldevices: bool,
    showhw: bool,
    showall: bool,
    showid: bool,
    showvbios: bool,
    showdriver: bool,
    showtempgraph: bool,
    showmclkrange: bool,
    showmemvendor: bool,
    showsclkrange: bool,
    showproduct: bool,
    showserial: bool,
    showuid: bool,
    showvrange: bool,
    showbus: bool,
    showpages: bool,
    showpending: bool,
    showretired: bool,
    showunres: bool,
    showfan: bool,
    showpower: bool,
    showtemp: bool,
    showuse: bool,
    showmemuse: bool,
    showvoltage: bool,
    showbw: bool,
    showclocks: bool,
    showgpuclocks: bool,
    showprofile: bool,
    showmaxpower: bool,
    showmod: bool,
    showod: bool,
    showperf: bool,
    showclkvolt: bool,
    showclkfrq: bool,
    showreplay: bool,
    showvc: bool,
    showxgmi: bool,
    showtopo: bool,
    showaccess: bool,
    showweight: bool,
    showhops: bool,
    showtype: bool,
    shownuma: bool,
    showenergy: bool,
    shownodes: bool,
    showcpart: bool,
    showmpart: bool,
    showmetrics: bool,
    resetclocks: bool,
    resetfans: bool,
    resetprofile: bool,
    resetpower: bool,
    resetxgmi: bool,
    resetperfdet: bool,
    resetcpart: bool,
    resetmpart: bool,
    gpureset: bool,
    devs: Vec<i32>,
    dev_given: bool,
    events: Vec<String>,
    have_events: bool,
    fw: Vec<String>,
    have_fw: bool,
    meminfo: Vec<String>,
    ras: Vec<String>,
    have_ras: bool,
    pidgpus: Vec<String>,
    have_pidgpus: bool,
    have_pids: bool,
    pids_verbose: String,
    setclock_type: Option<String>,
    setclock_level: Option<String>,
    setsclk: Vec<i32>,
    setmclk: Vec<i32>,
    setpcie: Vec<i32>,
    setslevel: Option<[String; 3]>,
    setmlevel: Option<[String; 3]>,
    setvc: Option<[String; 3]>,
    setsrange: Option<[String; 2]>,
    setmrange: Option<[String; 2]>,
    setextremum: Option<[String; 3]>,
    setfan: Option<String>,
    setperf: Option<String>,
    setod: Option<String>,
    setmod: Option<String>,
    setpower: Option<String>,
    setprofile: Option<String>,
    setcpart: Option<String>,
    setmpart: Option<String>,
    setperfdet: bool,
    perfdet_mhz: u64,
    ras_en: Option<[String; 2]>,
    ras_dis: Option<[String; 2]>,
    ras_inj: Option<String>,
    load_path: Option<String>,
    save_path: Option<String>,
}

fn usage() {
    let text = include_str!("usage_text.in").replace("{VER}", SMI_VERSION);
    print!("{text}");
}

fn is_opt(s: &str) -> bool {
    s.starts_with('-')
}

fn take_ints(argv: &[String], i: &mut usize, maxn: usize) -> Vec<i32> {
    let mut out = Vec::new();
    while *i + 1 < argv.len() && !is_opt(&argv[*i + 1]) && out.len() < maxn {
        *i += 1;
        out.push(c_atoi(&argv[*i]));
    }
    out
}

fn take_strs(argv: &[String], i: &mut usize, maxn: usize) -> Vec<String> {
    let mut out = Vec::new();
    while *i + 1 < argv.len() && !is_opt(&argv[*i + 1]) && out.len() < maxn {
        *i += 1;
        out.push(argv[*i].clone());
    }
    out
}

fn need_n(argv: &[String], i: &mut usize, n: usize) -> Vec<String> {
    let mut dst = Vec::with_capacity(n);
    for _ in 0..n {
        if *i + 1 >= argv.len() || is_opt(&argv[*i + 1]) {
            eprintln!("usage: rocm-smi: expected {n} argument(s)");
            std::process::exit(2);
        }
        *i += 1;
        dst.push(argv[*i].clone());
    }
    dst
}

fn need_one(argv: &[String], i: &mut usize) -> String {
    if *i + 1 >= argv.len() {
        std::process::exit(2);
    }
    *i += 1;
    argv[*i].clone()
}

fn parse_args(argv: &[String]) -> Args {
    let mut a = Args {
        help: false,
        version: false,
        alldevices: false,
        showhw: false,
        showall: false,
        showid: false,
        showvbios: false,
        showdriver: false,
        showtempgraph: false,
        showmclkrange: false,
        showmemvendor: false,
        showsclkrange: false,
        showproduct: false,
        showserial: false,
        showuid: false,
        showvrange: false,
        showbus: false,
        showpages: false,
        showpending: false,
        showretired: false,
        showunres: false,
        showfan: false,
        showpower: false,
        showtemp: false,
        showuse: false,
        showmemuse: false,
        showvoltage: false,
        showbw: false,
        showclocks: false,
        showgpuclocks: false,
        showprofile: false,
        showmaxpower: false,
        showmod: false,
        showod: false,
        showperf: false,
        showclkvolt: false,
        showclkfrq: false,
        showreplay: false,
        showvc: false,
        showxgmi: false,
        showtopo: false,
        showaccess: false,
        showweight: false,
        showhops: false,
        showtype: false,
        shownuma: false,
        showenergy: false,
        shownodes: false,
        showcpart: false,
        showmpart: false,
        showmetrics: false,
        resetclocks: false,
        resetfans: false,
        resetprofile: false,
        resetpower: false,
        resetxgmi: false,
        resetperfdet: false,
        resetcpart: false,
        resetmpart: false,
        gpureset: false,
        devs: Vec::new(),
        dev_given: false,
        events: Vec::new(),
        have_events: false,
        fw: Vec::new(),
        have_fw: false,
        meminfo: Vec::new(),
        ras: Vec::new(),
        have_ras: false,
        pidgpus: Vec::new(),
        have_pidgpus: false,
        have_pids: false,
        pids_verbose: "summary".into(),
        setclock_type: None,
        setclock_level: None,
        setsclk: Vec::new(),
        setmclk: Vec::new(),
        setpcie: Vec::new(),
        setslevel: None,
        setmlevel: None,
        setvc: None,
        setsrange: None,
        setmrange: None,
        setextremum: None,
        setfan: None,
        setperf: None,
        setod: None,
        setmod: None,
        setpower: None,
        setprofile: None,
        setcpart: None,
        setmpart: None,
        setperfdet: false,
        perfdet_mhz: 0,
        ras_en: None,
        ras_dis: None,
        ras_inj: None,
        load_path: None,
        save_path: None,
    };
    let mut i = 1usize;
    while i < argv.len() {
        let s = argv[i].as_str();
        if s == "-h" || s == "--help" {
            a.help = true;
        } else if s == "-V" || s == "--version" {
            a.version = true;
        } else if s == "--alldevices" {
            a.alldevices = true;
        } else if s == "--showhw" {
            a.showhw = true;
        } else if s == "-a" || s == "--showallinfo" {
            a.showall = true;
        } else if s == "-i" || s == "--showid" {
            a.showid = true;
        } else if s == "-v" || s == "--showvbios" {
            a.showvbios = true;
        } else if s == "--showdriverversion" {
            a.showdriver = true;
        } else if s == "--showtempgraph" {
            a.showtempgraph = true;
        } else if s == "--showmclkrange" {
            a.showmclkrange = true;
        } else if s == "--showmemvendor" {
            a.showmemvendor = true;
        } else if s == "--showsclkrange" {
            a.showsclkrange = true;
        } else if s == "--showproductname" {
            a.showproduct = true;
        } else if s == "--showserial" {
            a.showserial = true;
        } else if s == "--showuniqueid" {
            a.showuid = true;
        } else if s == "--showvoltagerange" {
            a.showvrange = true;
        } else if s == "--showbus" {
            a.showbus = true;
        } else if s == "--showpagesinfo" {
            a.showpages = true;
        } else if s == "--showpendingpages" {
            a.showpending = true;
        } else if s == "--showretiredpages" {
            a.showretired = true;
        } else if s == "--showunreservablepages" {
            a.showunres = true;
        } else if s == "-f" || s == "--showfan" {
            a.showfan = true;
        } else if s == "-P" || s == "--showpower" {
            a.showpower = true;
        } else if s == "-t" || s == "--showtemp" {
            a.showtemp = true;
        } else if s == "-u" || s == "--showuse" {
            a.showuse = true;
        } else if s == "--showmemuse" {
            a.showmemuse = true;
        } else if s == "--showvoltage" {
            a.showvoltage = true;
        } else if s == "-b" || s == "--showbw" {
            a.showbw = true;
        } else if s == "-c" || s == "--showclocks" {
            a.showclocks = true;
        } else if s == "-g" || s == "--showgpuclocks" {
            a.showgpuclocks = true;
        } else if s == "-l" || s == "--showprofile" {
            a.showprofile = true;
        } else if s == "-M" || s == "--showmaxpower" {
            a.showmaxpower = true;
        } else if s == "-m" || s == "--showmemoverdrive" {
            a.showmod = true;
        } else if s == "-o" || s == "--showoverdrive" {
            a.showod = true;
        } else if s == "-p" || s == "--showperflevel" {
            a.showperf = true;
        } else if s == "-S" || s == "--showclkvolt" {
            a.showclkvolt = true;
        } else if s == "-s" || s == "--showclkfrq" {
            a.showclkfrq = true;
        } else if s == "--showreplaycount" {
            a.showreplay = true;
        } else if s == "--showvc" {
            a.showvc = true;
        } else if s == "--showxgmierr" {
            a.showxgmi = true;
        } else if s == "--showtopo" {
            a.showtopo = true;
        } else if s == "--showtopoaccess" {
            a.showaccess = true;
        } else if s == "--showtopoweight" {
            a.showweight = true;
        } else if s == "--showtopohops" {
            a.showhops = true;
        } else if s == "--showtopotype" {
            a.showtype = true;
        } else if s == "--showtoponuma" {
            a.shownuma = true;
        } else if s == "--showenergycounter" {
            a.showenergy = true;
        } else if s == "--shownodesbw" {
            a.shownodes = true;
        } else if s == "--showcomputepartition" {
            a.showcpart = true;
        } else if s == "--showmemorypartition" {
            a.showmpart = true;
        } else if s == "--showmetrics" {
            a.showmetrics = true;
        } else if s == "-r" || s == "--resetclocks" {
            a.resetclocks = true;
        } else if s == "--resetfans" {
            a.resetfans = true;
        } else if s == "--resetprofile" {
            a.resetprofile = true;
        } else if s == "--resetpoweroverdrive" {
            a.resetpower = true;
        } else if s == "--resetxgmierr" {
            a.resetxgmi = true;
        } else if s == "--resetperfdeterminism" {
            a.resetperfdet = true;
        } else if s == "--resetcomputepartition" {
            a.resetcpart = true;
        } else if s == "--resetmemorypartition" {
            a.resetmpart = true;
        } else if s == "--gpureset" {
            a.gpureset = true;
        } else if s == "--json" {
            ctx().json = true;
        } else if s == "--csv" {
            ctx().csv = true;
            ctx().json = true;
        } else if s == "-d" || s == "--device" {
            a.devs = take_ints(argv, &mut i, MAX_DEV);
            a.dev_given = true;
            if a.devs.is_empty() {
                eprintln!("rocm-smi: --device requires a device id");
                std::process::exit(2);
            }
        } else if s == "-e" || s == "--showevents" {
            a.have_events = true;
            a.events = take_strs(argv, &mut i, 32);
        } else if s == "--showfwinfo" {
            a.have_fw = true;
            a.fw = take_strs(argv, &mut i, 32);
        } else if s == "--showmeminfo" {
            a.meminfo = take_strs(argv, &mut i, 8);
            if a.meminfo.is_empty() {
                eprintln!("rocm-smi: --showmeminfo requires TYPE");
                std::process::exit(2);
            }
        } else if s == "--showpids" {
            a.have_pids = true;
            if i + 1 < argv.len() && !is_opt(&argv[i + 1]) {
                i += 1;
                a.pids_verbose = argv[i].clone();
            }
        } else if s == "--showpidgpus" {
            a.have_pidgpus = true;
            a.pidgpus = take_strs(argv, &mut i, 32);
        } else if s == "--showrasinfo" {
            a.have_ras = true;
            a.ras = take_strs(argv, &mut i, 32);
        } else if s == "--setclock" {
            let p = need_n(argv, &mut i, 2);
            a.setclock_type = Some(p[0].clone());
            a.setclock_level = Some(p[1].clone());
        } else if s == "--setsclk" {
            a.setsclk = take_ints(argv, &mut i, 64);
        } else if s == "--setmclk" {
            a.setmclk = take_ints(argv, &mut i, 64);
        } else if s == "--setpcie" {
            a.setpcie = take_ints(argv, &mut i, 64);
        } else if s == "--setslevel" {
            let p = need_n(argv, &mut i, 3);
            a.setslevel = Some([p[0].clone(), p[1].clone(), p[2].clone()]);
        } else if s == "--setmlevel" {
            let p = need_n(argv, &mut i, 3);
            a.setmlevel = Some([p[0].clone(), p[1].clone(), p[2].clone()]);
        } else if s == "--setvc" {
            let p = need_n(argv, &mut i, 3);
            a.setvc = Some([p[0].clone(), p[1].clone(), p[2].clone()]);
        } else if s == "--setsrange" {
            let p = need_n(argv, &mut i, 2);
            a.setsrange = Some([p[0].clone(), p[1].clone()]);
        } else if s == "--setmrange" {
            let p = need_n(argv, &mut i, 2);
            a.setmrange = Some([p[0].clone(), p[1].clone()]);
        } else if s == "--setextremum" {
            let p = need_n(argv, &mut i, 3);
            a.setextremum = Some([p[0].clone(), p[1].clone(), p[2].clone()]);
        } else if s == "--setfan" {
            a.setfan = Some(need_one(argv, &mut i));
        } else if s == "--setperflevel" {
            a.setperf = Some(need_one(argv, &mut i));
        } else if s == "--setoverdrive" {
            a.setod = Some(need_one(argv, &mut i));
        } else if s == "--setmemoverdrive" {
            a.setmod = Some(need_one(argv, &mut i));
        } else if s == "--setpoweroverdrive" {
            a.setpower = Some(need_one(argv, &mut i));
        } else if s == "--setprofile" {
            a.setprofile = Some(need_one(argv, &mut i));
        } else if s == "--setperfdeterminism" {
            a.setperfdet = true;
            a.perfdet_mhz = c_strtoull(&need_one(argv, &mut i), 0);
        } else if s == "--setcomputepartition" {
            a.setcpart = Some(need_one(argv, &mut i));
        } else if s == "--setmemorypartition" {
            a.setmpart = Some(need_one(argv, &mut i));
        } else if s == "--rasenable" {
            let p = need_n(argv, &mut i, 2);
            a.ras_en = Some([p[0].clone(), p[1].clone()]);
        } else if s == "--rasdisable" {
            let p = need_n(argv, &mut i, 2);
            a.ras_dis = Some([p[0].clone(), p[1].clone()]);
        } else if s == "--rasinject" {
            a.ras_inj = Some(need_one(argv, &mut i));
        } else if s == "--load" {
            a.load_path = Some(need_one(argv, &mut i));
        } else if s == "--save" {
            a.save_path = Some(need_one(argv, &mut i));
        } else if s == "--autorespond" {
            ctx().autorespond = Some(need_one(argv, &mut i));
        } else if s == "--loglevel" {
            let lv = need_one(argv, &mut i);
            if str_ieq(&lv, "debug") {
                ctx().loglevel = 10;
            } else if str_ieq(&lv, "info") {
                ctx().loglevel = 20;
            } else if str_ieq(&lv, "warning") {
                ctx().loglevel = 30;
            } else if str_ieq(&lv, "error") {
                ctx().loglevel = 40;
            } else if str_ieq(&lv, "critical") {
                ctx().loglevel = 50;
            } else {
                eprintln!("rocm-smi: invalid log level {lv}");
                std::process::exit(2);
            }
        } else {
            eprintln!("usage: rocm-smi: unrecognized arguments: {s}");
            std::process::exit(2);
        }
        i += 1;
    }
    a
}

fn wants_sudo(a: &Args) -> bool {
    a.setsclk.first().copied().unwrap_or(0) != 0
        || !a.setsclk.is_empty()
        || !a.setmclk.is_empty()
        || !a.setpcie.is_empty()
        || a.resetfans
        || a.setfan.is_some()
        || a.setperf.is_some()
        || a.load_path.is_some()
        || a.resetclocks
        || a.setprofile.is_some()
        || a.resetprofile
        || a.setod.is_some()
        || a.setmod.is_some()
        || a.setpower.is_some()
        || a.resetpower
        || a.ras_en.is_some()
        || a.ras_dis.is_some()
        || a.ras_inj.is_some()
        || a.gpureset
        || a.setperfdet
        || a.setslevel.is_some()
        || a.setmlevel.is_some()
        || a.setvc.is_some()
        || a.setsrange.is_some()
        || a.setextremum.is_some()
        || a.setmrange.is_some()
        || a.setclock_type.is_some()
        || a.setcpart.is_some()
        || a.setmpart.is_some()
        || a.resetcpart
        || a.resetmpart
}

fn any_action(a: &Args) -> bool {
    a.showhw
        || a.showall
        || a.showid
        || a.showvbios
        || a.showdriver
        || a.showtempgraph
        || a.showmclkrange
        || a.showmemvendor
        || a.showsclkrange
        || a.showproduct
        || a.showserial
        || a.showuid
        || a.showvrange
        || a.showbus
        || a.showpages
        || a.showpending
        || a.showretired
        || a.showunres
        || a.showfan
        || a.showpower
        || a.showtemp
        || a.showuse
        || a.showmemuse
        || a.showvoltage
        || a.showbw
        || a.showclocks
        || a.showgpuclocks
        || a.showprofile
        || a.showmaxpower
        || a.showmod
        || a.showod
        || a.showperf
        || a.showclkvolt
        || a.showclkfrq
        || a.showreplay
        || a.showvc
        || a.showxgmi
        || a.showtopo
        || a.showaccess
        || a.showweight
        || a.showhops
        || a.showtype
        || a.shownuma
        || a.showenergy
        || a.shownodes
        || a.showcpart
        || a.showmpart
        || a.showmetrics
        || a.have_events
        || a.have_fw
        || !a.meminfo.is_empty()
        || a.have_ras
        || a.have_pids
        || a.have_pidgpus
        || a.resetclocks
        || a.resetfans
        || a.resetprofile
        || a.resetpower
        || a.resetxgmi
        || a.resetperfdet
        || a.gpureset
        || a.setfan.is_some()
        || a.setperf.is_some()
        || !a.setsclk.is_empty()
        || !a.setmclk.is_empty()
        || !a.setpcie.is_empty()
        || a.setclock_type.is_some()
        || a.setod.is_some()
        || a.setmod.is_some()
        || a.setpower.is_some()
        || a.setprofile.is_some()
        || a.load_path.is_some()
        || a.save_path.is_some()
        || a.setcpart.is_some()
        || a.setmpart.is_some()
        || a.setslevel.is_some()
        || a.setmlevel.is_some()
        || a.setvc.is_some()
        || a.setsrange.is_some()
        || a.setmrange.is_some()
        || a.setextremum.is_some()
        || a.setperfdet
        || a.ras_en.is_some()
        || a.ras_dis.is_some()
        || a.ras_inj.is_some()
        || a.resetcpart
        || a.resetmpart
}

fn concise_requested(argc: usize, a: &Args) -> bool {
    let extra = a.alldevices as i32 + if ctx().json || ctx().csv { 1 } else { 0 };
    if any_action(a) {
        return false;
    }
    if argc == 1 {
        return true;
    }
    if !a.dev_given && extra != 0 && argc <= 3 {
        return true;
    }
    false
}

fn reexec_sudo() -> ! {
    let mut owned = Vec::new();
    owned.push(CString::new("sudo").unwrap());
    for arg in std::env::args() {
        owned.push(CString::new(arg).unwrap_or_else(|_| CString::new("").unwrap()));
    }
    let mut ptrs: Vec<*const libc::c_char> = owned.iter().map(|s| s.as_ptr()).collect();
    ptrs.push(std::ptr::null());
    unsafe { libc::execvp(owned[0].as_ptr(), ptrs.as_ptr()) };
    log_err(&format!("root privileges are required (sudo exec failed: {})", errno_str()));
    std::process::exit(1);
}

fn main() {
    let argv: Vec<String> = std::env::args().collect();
    let argc = argv.len();
    let mut a = parse_args(&argv);
    if a.help {
        usage();
        return;
    }
    if wants_sudo(&a) && unsafe { libc::geteuid() } != 0 {
        reexec_sudo();
    }
    if !driver_live() {
        log_err("Driver not initialized (amdgpu not found in modules)");
        return;
    }
    if discover() != 0 {
        log_err("ROCm SMI returned no devices (the expected value is 0)");
        if ndev() == 0 {
            print_log(-1, Some("ERROR: No DRM devices available. Exiting"), None);
            std::process::exit(1);
        }
    }
    if a.version {
        show_version();
        shut_down();
        return;
    }
    let mut devs: Vec<i32> = Vec::new();
    if a.dev_given {
        for &id in &a.devs {
            if id < 0 || id as usize >= ndev() {
                log_warn(&format!("No such device card{id}"));
                return;
            }
            devs.push(id);
        }
    } else {
        for i in 0..ndev() {
            devs.push(i as i32);
        }
    }
    if devs.is_empty() {
        print_log(-1, Some("ERROR: No DRM devices available. Exiting"), None);
        std::process::exit(1);
    }
    if a.showall {
        a.showid = true;
        a.showvbios = true;
        a.showdriver = true;
        a.showfan = true;
        a.showpower = true;
        a.showtemp = true;
        a.showuse = true;
        a.showmemuse = true;
        a.showvoltage = true;
        a.showclocks = true;
        a.showmaxpower = true;
        a.showod = true;
        a.showmod = true;
        a.showperf = true;
        a.showproduct = true;
        a.showserial = true;
        a.showuid = true;
        a.showbus = true;
        a.showpages = true;
        a.showmemvendor = true;
        a.showsclkrange = true;
        a.showmclkrange = true;
        a.showvrange = true;
        a.showvc = true;
        a.showenergy = true;
        a.showreplay = true;
        a.showcpart = true;
        a.showmpart = true;
        a.showmetrics = true;
        a.have_fw = true;
        a.have_pids = true;
        a.have_pidgpus = true;
        a.fw.clear();
        if !ctx().json {
            a.showprofile = true;
            a.showclkfrq = true;
            a.showclkvolt = true;
        }
    }
    let concise = concise_requested(argc, &a);
    if !ctx().json {
        println!();
    }
    if !concise {
        print_spacer(Some(HEADER), "=", 0);
    }
    if a.gpureset {
        if !a.dev_given || a.devs.len() != 1 {
            log_err("No device specified. One device must be specified for GPU reset");
            print_spacer(None, "=", 0);
            std::process::exit(1);
        }
        do_reset_gpu(devs[0]);
        shut_down();
        return;
    }
    if concise {
        show_concise(&devs);
    }
    if a.showhw {
        show_hw(&devs);
    }
    if a.showdriver {
        show_driver(&devs);
    }
    if a.showtempgraph {
        show_tempgraph(&devs);
    }
    if a.showid {
        show_id(&devs);
    }
    if a.showuid {
        show_uid(&devs);
    }
    if a.showvbios {
        show_vbios(&devs);
    }
    if a.have_events {
        show_events(&devs, &a.events);
    }
    if a.resetclocks {
        reset_clocks(&devs);
    }
    if a.showtemp {
        show_temp(&devs);
    }
    if a.showclocks {
        show_clocks_current(&devs, None);
    }
    if a.showgpuclocks {
        show_clocks_current(&devs, Some("sclk"));
    }
    if a.showfan {
        show_fan(&devs);
    }
    if a.showperf {
        show_perf(&devs);
    }
    if a.showod {
        show_overdrive(&devs, "sclk");
    }
    if a.showmod {
        show_overdrive(&devs, "mclk");
    }
    if a.showmaxpower {
        show_maxpower(&devs);
    }
    if a.showprofile {
        show_profile(&devs);
    }
    if a.showpower {
        show_power(&devs);
    }
    if a.showclkfrq {
        show_clocks_supported(&devs);
    }
    if a.showuse {
        show_use(&devs);
    }
    if a.showmemuse {
        show_memuse(&devs);
    }
    if a.showmemvendor {
        show_memvendor(&devs);
    }
    if a.showbw {
        show_bw(&devs);
    }
    if a.showreplay {
        show_replay(&devs);
    }
    if a.showserial {
        show_serial(&devs);
    }
    if a.have_pids {
        show_pids(&a.pids_verbose);
    }
    if a.have_pidgpus {
        show_pidgpus(&a.pidgpus);
    }
    if a.showclkvolt {
        show_clkvolt(&devs);
    }
    if a.showvoltage {
        show_voltage(&devs);
    }
    if a.showbus {
        show_bus(&devs);
    }
    if !a.meminfo.is_empty() {
        show_meminfo(&devs, &a.meminfo);
    }
    if a.have_ras {
        show_ras(&devs, &a.ras);
    }
    if a.have_fw {
        show_fw(&devs, &a.fw);
    }
    if a.showproduct {
        show_product(&devs);
    }
    if a.showxgmi {
        show_xgmi(&devs);
    }
    if a.shownodes {
        show_nodes_bw(&devs);
    }
    if a.showtopo {
        show_topo_weight(&devs);
        if !ctx().json {
            println!();
        }
        show_topo_hops(&devs);
        if !ctx().json {
            println!();
        }
        show_topo_type(&devs);
        if !ctx().json {
            println!();
        }
        show_numa(&devs);
    }
    if a.showaccess {
        show_topo_access(&devs);
    }
    if a.showweight {
        show_topo_weight(&devs);
    }
    if a.showhops {
        show_topo_hops(&devs);
    }
    if a.showtype {
        show_topo_type(&devs);
    }
    if a.shownuma {
        show_numa(&devs);
    }
    if a.showpages {
        show_pages(&devs, "all");
    }
    if a.showretired {
        show_pages(&devs, "reserved");
    }
    if a.showpending {
        show_pages(&devs, "pending");
    }
    if a.showunres {
        show_pages(&devs, "unreservable");
    }
    if a.showsclkrange {
        show_range(&devs, "sclk");
    }
    if a.showmclkrange {
        show_range(&devs, "mclk");
    }
    if a.showvrange {
        show_range(&devs, "voltage");
    }
    if a.showvc {
        show_vc(&devs);
    }
    if a.showenergy {
        show_energy(&devs);
    }
    if a.showcpart {
        show_partitions(&devs, 0);
    }
    if a.showmpart {
        show_partitions(&devs, 1);
    }
    if a.showmetrics {
        show_metrics(&devs);
    }
    if let (Some(ty), Some(lv)) = (&a.setclock_type, &a.setclock_level) {
        let level = c_atoi(lv);
        set_clocks(&devs, ty, &[level]);
    }
    if !a.setsclk.is_empty() {
        set_clocks(&devs, "sclk", &a.setsclk);
    }
    if !a.setmclk.is_empty() {
        set_clocks(&devs, "mclk", &a.setmclk);
    }
    if !a.setpcie.is_empty() {
        set_clocks(&devs, "pcie", &a.setpcie);
    }
    if let Some(s) = &a.setslevel {
        set_slevel(&devs, "sclk", &s[1]);
    }
    if let Some(s) = &a.setmlevel {
        set_slevel(&devs, "mclk", &s[1]);
    }
    if a.resetfans {
        reset_fans(&devs);
    }
    if let Some(v) = &a.setfan {
        set_fan(&devs, v);
    }
    if let Some(v) = &a.setperf {
        set_perf(&devs, v);
    }
    if let Some(v) = &a.setod {
        set_overdrive(&devs, v);
    }
    if a.setmod.is_some() {
        print_spacer(Some(" Set Memory OverDrive "), "=", 0);
        print_log(-1, Some("GPU Memory OverDrive is not supported on this system"), None);
        print_spacer(None, "=", 0);
    }
    if let Some(v) = &a.setpower {
        set_power_od(&devs, Some(v), false);
    }
    if a.resetpower {
        set_power_od(&devs, None, true);
    }
    if let Some(v) = &a.setprofile {
        set_profile(&devs, v);
    }
    if let Some(v) = &a.setvc {
        set_vc(&devs, &v[0], &v[1], &v[2]);
    }
    if let Some(v) = &a.setextremum {
        set_extremum(&devs, &v[0], &v[1], &v[2]);
    }
    if let Some(v) = &a.setsrange {
        set_range(&devs, "sclk", &v[0], &v[1]);
    }
    if let Some(v) = &a.setmrange {
        set_range(&devs, "mclk", &v[0], &v[1]);
    }
    if a.setperfdet {
        set_perfdet(&devs, a.perfdet_mhz);
    }
    if let Some(v) = &a.setcpart {
        set_comppart(&devs, v);
    }
    if let Some(v) = &a.setmpart {
        set_mempart(&devs, v);
    }
    if a.resetprofile {
        reset_profile(&devs);
    }
    if a.resetxgmi {
        reset_xgmi(&devs);
    }
    if a.resetperfdet {
        reset_perfdet(&devs);
    }
    if a.resetcpart {
        print_spacer(Some(" Reset Compute Partition "), "=", 0);
        print_log(-1, Some("Reset to boot compute partition is not supported on the given system"), None);
        print_spacer(None, "=", 0);
    }
    if a.resetmpart {
        print_spacer(Some(" Reset Memory Partition "), "=", 0);
        print_log(-1, Some("Reset to boot memory partition is not supported on the given system"), None);
        print_spacer(None, "=", 0);
    }
    if a.ras_en.is_some() || a.ras_dis.is_some() || a.ras_inj.is_some() {
        print_spacer(Some(" RAS "), "=", 0);
        print_log(-1, Some("RAS enable/disable/inject is not supported on the given system"), None);
        print_spacer(None, "=", 0);
    }
    if let Some(p) = &a.load_path {
        load_settings(p);
    }
    if let Some(p) = &a.save_path {
        save_settings(&devs, p);
    }
    if ctx().loglevel >= 30 {
        ctx().retcode = 0;
    }
    if ctx().json && !concise {
        if json_empty() {
            log_warn("No JSON data to report");
        } else if !ctx().csv {
            json_dump();
        } else {
            csv_dump(&devs);
        }
    }
    if !concise {
        print_spacer(Some(FOOTER), "=", 0);
    }
    let rc = ctx().retcode;
    shut_down();
    std::process::exit(rc);
}
