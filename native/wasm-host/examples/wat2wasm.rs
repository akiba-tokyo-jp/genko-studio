//! Compiles WebAssembly text to `.wasm` for Genko's test modules (native/tests/data/plugins), once, on a developer's
//! machine: `cargo run --example wat2wasm -- in.wat out.wasm [--any]`. Without `--any` the module must load as a
//! plug-in (it is checked); the hostile and broken ones are made with `--any`. From PhotoCraft v0.2.0's
//! `crates/plugins/examples/wat2wasm.rs` (MIT, native/third_party/photocraft/NOTICE.md).

use genko_plugin_host::{Limits, Plugin};

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let any = args.iter().any(|a| a == "--any");
    let files: Vec<&String> = args.iter().filter(|a| *a != "--any").collect();
    let (Some(input), Some(output)) = (files.first(), files.get(1)) else {
        eprintln!("usage: wat2wasm <in.wat> <out.wasm> [--any]");
        std::process::exit(2);
    };
    let bytes = match wat::parse_file(input) {
        Ok(b) => b,
        Err(e) => {
            eprintln!("{e}");
            std::process::exit(1);
        }
    };
    match Plugin::load(&bytes, Limits::default()) {
        Ok(p) => println!("{} ({}): {} bytes, params {}", p.manifest().name, p.id(), bytes.len(), p.manifest().params_notation()),
        Err(e) if any => println!("{input}: {} bytes, not a plug-in as it should not be: {e}", bytes.len()),
        Err(e) => {
            eprintln!("not a valid plug-in: {e}");
            std::process::exit(1);
        }
    }
    if let Err(e) = std::fs::write(output, &bytes) {
        eprintln!("{output}: {e}");
        std::process::exit(1);
    }
}
