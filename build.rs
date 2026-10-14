fn main() {
    let rocm = std::env::var("ROCM").unwrap_or_else(|_| "/opt/rocm".to_string());
    println!("cargo:rerun-if-env-changed=ROCM");
    println!("cargo:rerun-if-changed={rocm}/include/amd_smi/amdsmi.h");
    println!("cargo:rustc-link-search=native={rocm}/lib");
    println!("cargo:rustc-link-lib=dylib=amd_smi");
    println!("cargo:rustc-link-arg=-Wl,-rpath,{rocm}/lib");

    let out = std::path::PathBuf::from(std::env::var("OUT_DIR").unwrap());
    let bindings = bindgen::Builder::default()
        .header(format!("{rocm}/include/amd_smi/amdsmi.h"))
        .clang_arg(format!("-I{rocm}/include"))
        .clang_arg("-x")
        .clang_arg("c")
        .allowlist_function("amdsmi_.*")
        .allowlist_type("amdsmi_.*")
        .allowlist_var("AMDSMI_.*")
        .default_enum_style(bindgen::EnumVariation::ModuleConsts)
        .layout_tests(false)
        .generate()
        .expect("failed to generate amdsmi bindings");
    bindings
        .write_to_file(out.join("amdsmi.rs"))
        .expect("failed to write amdsmi bindings");
}
