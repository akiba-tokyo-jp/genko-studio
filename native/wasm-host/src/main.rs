//! `genko-plugin-host`: runs one WebAssembly filter plug-in for Genko over its plug-in protocol 2 on stdin and stdout
//! (src/protocol.rs, docs/PLUGIN_WASM.md). Genko starts it for each run; it is not meant to be started by hand.
#![forbid(unsafe_code)]

use std::io::{BufWriter, Write};

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.first().map(String::as_str) == Some("--version") {
        println!("{}", genko_plugin_host::protocol::host_name());
        return;
    }
    if !args.is_empty() {
        let _ = writeln!(std::io::stderr(), "usage: genko-plugin-host [--version] (Genko starts it and speaks to it over stdin and stdout)");
        std::process::exit(2);
    }
    let input = std::io::stdin().lock();
    let output = BufWriter::new(std::io::stdout().lock());
    match genko_plugin_host::protocol::serve(input, output) {
        Ok(code) => std::process::exit(code),
        Err(e) => {
            let _ = writeln!(std::io::stderr(), "genko-plugin-host: {e}");
            std::process::exit(2);
        }
    }
}
