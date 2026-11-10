//! The plug-in ABI end to end through Genko's protocol 2: the example Invert plug-in, and hostile modules (infinite
//! loops, memory bombs, out-of-bounds access, bad manifests…), which must all fail with a refusal, quickly. The ideas
//! and the test modules are PhotoCraft v0.2.0's `crates/plugins/tests/abi.rs` (MIT, native/third_party/photocraft/
//! NOTICE.md); the pixels' cleaning, the bands' seams and the selection are Genko's (native/tests/unit/
//! test_wasm_plugins.cpp).

use std::time::{Duration, Instant};

use genko_plugin_host::protocol::serve;
use genko_plugin_host::{Limits, Plugin};
use serde_json::{Value, json};

const INVERT: &[u8] = include_bytes!("../../tests/data/plugins/invert.wasm");

/// A module implementing the ABI whose pieces can be swapped: `manifest` JSON, `version`, `filter` body (params
/// `$buf $len $w $h $ch $fmt $pp $pl`, result i32) and extra items.
fn module(manifest: &str, version: i32, filter: &str, extra: &str) -> Vec<u8> {
    let escaped = manifest.replace('\\', "\\\\").replace('"', "\\\"");
    let src = format!(
        r#"(module
  {extra}
  (memory (export "memory") 1)
  (data (i32.const 16) "{escaped}")
  (func (export "pc_abi_version") (result i32) (i32.const {version}))
  (func (export "pc_manifest") (result i64)
    (i64.or (i64.shl (i64.const {len}) (i64.const 32)) (i64.const 16)))
  (func (export "pc_alloc") (param $n i32) (result i32) (local $old i32)
    (local.set $old (memory.grow (i32.shr_u (i32.add (local.get $n) (i32.const 65535)) (i32.const 16))))
    (if (result i32) (i32.eq (local.get $old) (i32.const -1))
      (then (i32.const 0)) (else (i32.mul (local.get $old) (i32.const 65536)))))
  (func (export "pc_filter") (param $buf i32) (param $len i32) (param $w i32) (param $h i32)
    (param $ch i32) (param $fmt i32) (param $pp i32) (param $pl i32) (result i32)
    {filter})
)"#,
        len = manifest.len()
    );
    wat::parse_str(&src).unwrap_or_else(|e| panic!("bad test WAT: {e}\n{src}"))
}

const MANIFEST: &str = r#"{"id":"test.plugin","name":"Test","version":"1","kind":"filter"}"#;

fn ok_module(filter: &str) -> Vec<u8> {
    module(MANIFEST, 1, filter, "")
}

/// One band to send: width, height, channels, format, params, samples.
struct Band {
    w: u32,
    h: u32,
    ch: u32,
    format: u32,
    params: Value,
    data: Vec<f32>,
}

fn rgba_band(w: u32, h: u32) -> Band {
    let mut data = Vec::new();
    for y in 0..h {
        for x in 0..w {
            let v = ((x * 7 + y * 13) % 31) as f32 / 30.0;
            let a = if (x + y) % 9 == 0 { 0.0 } else { 0.25 + 0.75 * ((x % 4) as f32 / 3.0) };
            data.extend_from_slice(&[v, 1.0 - v, (x % 5) as f32 / 4.0, a]);
        }
    }
    Band { w, h, ch: 4, format: 8 | 1 << 8 | 3 << 16, params: json!({}), data }
}

/// What the host answered: each reply's JSON and its samples.
fn session(module: &[u8], limits: Value, bands: &[Band]) -> (i32, Vec<(Value, Vec<f32>)>) {
    let mut input = Vec::new();
    input.extend_from_slice(json!({"protocol": 2, "module": module.len(), "limits": limits}).to_string().as_bytes());
    input.push(b'\n');
    input.extend_from_slice(module);
    for b in bands {
        let head = json!({"protocol": 2, "width": b.w, "height": b.h, "channels": b.ch, "format": b.format, "params": b.params});
        input.extend_from_slice(head.to_string().as_bytes());
        input.push(b'\n');
        for v in &b.data {
            input.extend_from_slice(&v.to_le_bytes());
        }
    }
    let mut out = Vec::new();
    let code = serve(std::io::Cursor::new(input), &mut out).expect("the pipe works");
    let mut replies = Vec::new();
    let mut rest = &out[..];
    while let Some(i) = rest.iter().position(|b| *b == b'\n') {
        let head: Value = serde_json::from_slice(&rest[..i]).expect("a JSON line");
        rest = &rest[i + 1..];
        let n = if head["ok"] == json!(true) && head.get("width").is_some() {
            (head["width"].as_u64().unwrap_or(0) * head["height"].as_u64().unwrap_or(0) * head["channels"].as_u64().unwrap_or(0)) as usize
        } else {
            0
        };
        let data = rest[..n * 4].chunks_exact(4).map(|c| f32::from_le_bytes([c[0], c[1], c[2], c[3]])).collect();
        rest = &rest[n * 4..];
        replies.push((head, data));
    }
    (code, replies)
}

/// The refusal a module gets on a small image, and how long it took.
fn refusal(bytes: &[u8], limits: Value) -> (Value, Duration) {
    let t = Instant::now();
    let (code, replies) = session(bytes, limits, &[rgba_band(32, 24)]);
    let last = replies.last().expect("an answer").0.clone();
    assert_eq!(code, 1, "{last}");
    assert_eq!(last["ok"], json!(false), "hostile module must fail: {last}");
    (last, t.elapsed())
}

#[test]
fn example_invert_loads_and_reports_its_manifest() {
    let p = Plugin::load(INVERT, Limits::default()).unwrap();
    let m = p.manifest();
    assert_eq!(m.id, "org.photocraft.example.invert");
    assert_eq!(m.name, "Invert (WebAssembly)");
    assert_eq!(m.kind, genko_plugin_host::Kind::Filter);
    assert!(m.params.is_empty());
    assert!(p.size() < 4096, "the example stays tiny: {} bytes", p.size());
    // through the protocol: the manifest and the limits in the first answer
    let (code, replies) = session(INVERT, json!({}), &[]);
    assert_eq!(code, 0);
    assert_eq!(replies[0].0["manifest"]["id"], json!("org.photocraft.example.invert"));
    assert_eq!(replies[0].0["limits"]["fuel_base"], json!(50_000_000));
    assert_eq!(replies[0].0["limits"]["wall_time_ms"], json!(60_000));
}

#[test]
fn example_invert_inverts_colour_bands_in_turn() {
    let a = rgba_band(40, 30);
    let b = Band { params: json!({"_image": {"x": 0, "y": 30}}), ..rgba_band(40, 7) };
    let (code, replies) = session(INVERT, json!({}), &[a, b]);
    assert_eq!(code, 0);
    assert_eq!(replies.len(), 3);
    let a = rgba_band(40, 30);
    for (i, (orig, out)) in a.data.chunks_exact(4).zip(replies[1].1.chunks_exact(4)).enumerate() {
        if orig[3] <= 0.0 {
            assert_eq!(orig, out, "transparent pixels are left alone ({i})");
            continue;
        }
        for c in 0..3 {
            assert!((out[c] - (1.0 - orig[c])).abs() < 1e-6, "{i} c{c}: {orig:?} -> {out:?}");
        }
        assert_eq!(orig[3], out[3], "alpha is kept");
    }
    assert_eq!(replies[2].1.len(), 40 * 7 * 4);
}

#[test]
fn grey_one_channel_band_inverts() {
    // an alpha channel or a Quick Mask: one grey channel, no alpha
    let band = Band { w: 5, h: 3, ch: 1, format: 8 | 1 << 16, params: json!({}), data: (0..15).map(|i| i as f32 / 14.0).collect() };
    let (_, replies) = session(INVERT, json!({}), &[band]);
    for (i, v) in replies[1].1.iter().enumerate() {
        assert!((v - (1.0 - i as f32 / 14.0)).abs() < 1e-6);
    }
}

#[test]
fn params_and_image_context_reach_the_plugin() {
    // Writes params_len / 1000 into every sample, so the host must pass the JSON through.
    let fill = r#"(local $i i32)
      (block $done (loop $l
        (br_if $done (i32.ge_u (local.get $i) (local.get $len)))
        (f32.store (i32.add (local.get $buf) (local.get $i))
          (f32.div (f32.convert_i32_u (local.get $pl)) (f32.const 1000)))
        (local.set $i (i32.add (local.get $i) (i32.const 4)))
        (br $l)))
      (i32.const 0)"#;
    let man = r#"{"id":"test.params","name":"P","kind":"filter","params":{"amount":{"type":"number","min":0,"max":10,"default":5}}}"#;
    let bytes = module(man, 1, fill, "");
    let p = Plugin::load(&bytes, Limits::default()).unwrap();
    assert_eq!(p.manifest().params_notation(), r#"{"amount":0..10=5}"#);
    let image = json!({"x": 0, "y": 0, "width": 8, "height": 8, "overlap": 0, "canvasWidth": 8, "canvasHeight": 8,
                       "mode": "rgb", "depth": 32, "alpha": true, "channels": 4});
    let band = Band { params: json!({"amount": 3, "junk": 1, "_image": image}), format: 32 | 1 << 8 | 3 << 16, ..rgba_band(8, 8) };
    let (_, replies) = session(&bytes, json!({}), &[band]);
    let expect = json!({"amount": 3.0, "_image": image}).to_string().len() as f32;
    let v = replies[1].1[5] * 1000.0;
    assert!((v - expect).abs() < 0.5, "params JSON length {v}, expected {expect}");
    // Wrong-typed params are refused before the plug-in runs.
    let bad = Band { params: json!({"amount": "x"}), ..rgba_band(8, 8) };
    let (code, replies) = session(&bytes, json!({}), &[bad]);
    assert_eq!(code, 1);
    assert_eq!(replies[1].0["code"], json!("params"));
}

#[test]
fn nan_output_comes_back_as_written() {
    // The host hands back what the plug-in wrote; Genko keeps the original value for NaN and infinity.
    let nan = r#"(local $i i32)
      (block $done (loop $l
        (br_if $done (i32.ge_u (local.get $i) (local.get $len)))
        (f32.store (i32.add (local.get $buf) (local.get $i)) (f32.const nan))
        (local.set $i (i32.add (local.get $i) (i32.const 4)))
        (br $l)))
      (i32.const 0)"#;
    let (_, replies) = session(&ok_module(nan), json!({}), &[rgba_band(8, 8)]);
    assert!(replies[1].1.iter().all(|v| v.is_nan()));
}

#[test]
fn hostile_modules_fail_quickly() {
    let quick = Duration::from_secs(5);
    let cases: Vec<(&str, Vec<u8>, &str)> = vec![
        ("infinite loop", ok_module("(loop $l (br $l)) (i32.const 0)"), "limit"),
        ("memory.grow bomb, then trap", ok_module("(loop $l (br_if $l (i32.ne (memory.grow (i32.const 512)) (i32.const -1)))) unreachable"), "trap"),
        ("memory.grow forever", ok_module("(loop $l (drop (memory.grow (i32.const 1))) (br $l)) (i32.const 0)"), "limit"),
        ("out-of-bounds write", ok_module("(i32.store (i32.const -4) (i32.const 0)) (i32.const 0)"), "trap"),
        ("trap", ok_module("unreachable"), "trap"),
        ("division by zero", ok_module("(i32.div_u (i32.const 1) (local.get $ch))  (drop) (i32.div_u (i32.const 1) (i32.const 0))"), "trap"),
        ("deep recursion", module(MANIFEST, 1, "(call $r) (i32.const 0)", "(func $r (call $r))"), "trap"),
        ("error code", ok_module("(i32.const 7)"), "failed"),
        ("wrong ABI version", module(MANIFEST, 2, "(i32.const 0)", ""), "abi"),
        ("bad manifest", module("not json", 1, "(i32.const 0)", ""), "manifest"),
        ("manifest with a bad id", module(r#"{"id":"a b","name":"x","kind":"filter"}"#, 1, "(i32.const 0)", ""), "manifest"),
        ("manifest of the wrong kind", module(r#"{"id":"a","name":"x","kind":"panel"}"#, 1, "(i32.const 0)", ""), "manifest"),
        ("start function loops", module(MANIFEST, 1, "(i32.const 0)", "(func $spin (loop $l (br $l))) (start $spin)"), "limit"),
        (
            "WASI import",
            module(MANIFEST, 1, "(i32.const 0)", r#"(import "wasi_snapshot_preview1" "fd_write" (func (param i32 i32 i32 i32) (result i32)))"#),
            "abi",
        ),
    ];
    for (name, bytes, code) in cases {
        let (e, t) = refusal(&bytes, json!({}));
        assert!(t < quick, "{name} took {t:?}");
        assert_eq!(e["code"], json!(code), "{name}: {e}");
        eprintln!("{name}: {e} ({t:?})");
    }
}

#[test]
fn malformed_and_oversized_modules_are_refused() {
    let half = &INVERT[..INVERT.len() / 2];
    for (name, bytes) in [("empty", &b""[..]), ("garbage", &b"\0asm\x01\0\0\0\xff\xff\xff"[..]), ("not wasm", &b"MZ\x90\0"[..]), ("truncated", half)] {
        let (e, _) = refusal(bytes, json!({}));
        assert_eq!(e["code"], json!("module"), "{name}: {e}");
    }
    let (e, _) = refusal(INVERT, json!({"max_module_bytes": 100}));
    assert_eq!(e["code"], json!("module"));
    // Huge initial memory, manifest outside memory, missing exports, bogus allocations.
    let huge = wat::parse_str(r#"(module (memory (export "memory") 60000))"#).unwrap();
    assert!(Plugin::load(&huge, Limits::default()).is_err());
    let no_filter = wat::parse_str(
        r#"(module (memory (export "memory") 1) (func (export "pc_abi_version") (result i32) (i32.const 1))
           (func (export "pc_manifest") (result i64) (i64.const 0)))"#,
    )
    .unwrap();
    assert!(Plugin::load(&no_filter, Limits::default()).is_err());
    let src = r#"(module (memory (export "memory") 1) (func (export "pc_abi_version") (result i32) (i32.const 1))
           (func (export "pc_manifest") (result i64) (i64.const 0x00000064ffffff00))
           (func (export "pc_alloc") (param i32) (result i32) (i32.const 16))
           (func (export "pc_filter") (param i32 i32 i32 i32 i32 i32 i32 i32) (result i32) (i32.const 0)))"#;
    let (e, _) = refusal(&wat::parse_str(src).unwrap(), json!({}));
    assert_eq!(e["code"], json!("abi"));
    let bogus_alloc = wat::parse_str(format!(
        r#"(module (memory (export "memory") 1) (data (i32.const 16) "{}")
           (func (export "pc_abi_version") (result i32) (i32.const 1))
           (func (export "pc_manifest") (result i64) (i64.const {}))
           (func (export "pc_alloc") (param i32) (result i32) (i32.const -16))
           (func (export "pc_filter") (param i32 i32 i32 i32 i32 i32 i32 i32) (result i32) (i32.const 0)))"#,
        MANIFEST.replace('"', "\\\""),
        ((MANIFEST.len() as i64) << 32) | 16
    ))
    .unwrap();
    let (e, _) = refusal(&bogus_alloc, json!({}));
    assert_eq!(e["code"], json!("abi"), "{e}");
}

#[test]
fn budgets_stop_runaway_plugins() {
    let spin = ok_module("(loop $l (br $l)) (i32.const 0)");
    // Instruction budget.
    let (e, _) = refusal(&spin, json!({"fuel_base": 1_000_000, "fuel_per_sample": 10}));
    assert_eq!(e["error"], json!("plug-in exceeded its instruction budget"));
}

#[test]
fn wall_clock_budget_stops_runaway_plugins() {
    // Wall-clock budget: 1 ms, checked between slices of 20 million instructions.
    let spin = ok_module("(loop $l (br $l)) (i32.const 0)");
    let (e, t) = refusal(&spin, json!({"wall_time_ms": 1}));
    assert_eq!(e["error"], json!("plug-in exceeded its time budget"));
    assert!(t < Duration::from_secs(3), "{t:?}");
}

#[test]
fn limits_are_only_lowered() {
    let (_, replies) = session(INVERT, json!({"fuel_base": u64::MAX / 4, "wall_time_ms": 10_000_000, "max_memory_bytes": 1u64 << 40}), &[]);
    let l = &replies[0].0["limits"];
    assert_eq!(l["fuel_base"], json!(50_000_000));
    assert_eq!(l["wall_time_ms"], json!(60_000));
    assert_eq!(l["max_memory_bytes"], json!(512u64 << 20));
}

#[test]
fn memory_cap_limits_bands() {
    // A band that can't fit in half the plug-in's memory is refused before it is read.
    let (code, replies) = session(INVERT, json!({"max_memory_bytes": 4 << 20}), &[rgba_band(64, 8)]);
    assert_eq!(code, 0, "small bands still fit");
    assert_eq!(replies[1].0["ok"], json!(true));
    // (too little memory for the module itself: refused as it is instantiated, or its band refused)
    let (e, _) = refusal(INVERT, json!({"max_memory_bytes": 4096}));
    eprintln!("{e}");
}

#[test]
fn protocol_mistakes_are_refused() {
    let mut out = Vec::new();
    assert_eq!(serve(std::io::Cursor::new(b"{\"protocol\": 1, \"module\": 0}\n".to_vec()), &mut out).unwrap(), 1);
    let reply: Value = serde_json::from_slice(&out[..out.len() - 1]).unwrap();
    assert_eq!(reply["ok"], json!(false));
    let mut out = Vec::new();
    assert_eq!(serve(std::io::Cursor::new(b"not json\n".to_vec()), &mut out).unwrap(), 1);
    // nothing at all: the host ends quietly
    let mut out = Vec::new();
    assert_eq!(serve(std::io::Cursor::new(Vec::new()), &mut out).unwrap(), 0);
    assert!(out.is_empty());
}
