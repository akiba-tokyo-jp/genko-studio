//! Genko's plug-in protocol 2 on the host's side (docs/PLUGIN_WASM.md, docs/PLUGIN_NATIVE.md).
//!
//! Over stdin and stdout, each message one line of JSON, then its bytes:
//!
//! 1. Genko: `{"protocol": 2, "module": N, "limits"?: {…}}` and the module's N bytes (the very bytes Genko checked
//!    against the person's choice). The host: `{"ok": true, "manifest": {…}, "limits": {…}, "host": "…"}`, or
//!    `{"ok": false, "error": "…", "code": "…"}` and it ends. `limits` may only lower the host's limits (tests).
//! 2. Genko, for each band: `{"protocol": 2, "width", "height", "channels", "format", "params"}` and the band's
//!    `width × height × channels` little-endian `f32` samples. The host runs `pc_filter` on them in a fresh instance and
//!    answers `{"ok": true, "width", "height", "channels"}` and the samples as they came back (Genko cleans them: NaN,
//!    the range, the selection), or `{"ok": false, …}` and ends. `params` are the person's settings, checked against the
//!    manifest here (PhotoCraft's `resolve_params`), with Genko's `_image` (where the band is) kept as given.
//! 3. Genko closes stdin: the host ends.
//!
//! One wall-clock budget covers the whole run (every band), as PhotoCraft's `apply`.

use std::io::{BufRead, Write};

use serde_json::{Map, Value, json};

use crate::{Deadline, Error, Limits, Plugin};

/// The protocol this host speaks.
pub const PROTOCOL: i64 = 2;
/// The longest line of JSON read (a band's settings are small: a manifest's 64 parameters at most).
pub const MAX_LINE: usize = 1 << 20;

/// What the host says it is.
pub fn host_name() -> String {
    format!("genko-plugin-host {} (wasmi 2.0.0)", env!("CARGO_PKG_VERSION"))
}

/// Reads one line (without its line break), at most `max` bytes; `None` at the end of the input.
fn read_line<R: BufRead>(input: &mut R, max: usize) -> std::io::Result<Option<Vec<u8>>> {
    let mut line = Vec::new();
    loop {
        let buf = input.fill_buf()?;
        if buf.is_empty() {
            return Ok(if line.is_empty() { None } else { Some(line) });
        }
        if let Some(i) = buf.iter().position(|b| *b == b'\n') {
            line.extend_from_slice(&buf[..i]);
            input.consume(i + 1);
            return Ok(Some(line));
        }
        let n = buf.len();
        line.extend_from_slice(buf);
        input.consume(n);
        if line.len() > max {
            return Err(std::io::Error::new(std::io::ErrorKind::InvalidData, "a line longer than the protocol allows"));
        }
    }
}

fn write_reply<W: Write>(out: &mut W, header: &Value, data: &[u8]) -> std::io::Result<()> {
    let mut line = serde_json::to_vec(header).map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e))?;
    line.push(b'\n');
    out.write_all(&line)?;
    out.write_all(data)?;
    out.flush()
}

fn refuse<W: Write>(out: &mut W, e: &Error) -> std::io::Result<i32> {
    write_reply(out, &json!({"ok": false, "error": e.to_string(), "code": e.code()}), &[])?;
    Ok(1)
}

fn object(line: &[u8]) -> Result<Map<String, Value>, Error> {
    match serde_json::from_slice::<Value>(line) {
        Ok(Value::Object(m)) => Ok(m),
        _ => Err(Error::Io("the request is not a JSON object".into())),
    }
}

fn protocol_of(m: &Map<String, Value>) -> Result<(), Error> {
    if m.get("protocol").and_then(Value::as_i64) != Some(PROTOCOL) {
        return Err(Error::Io(format!("this host speaks protocol {PROTOCOL}")));
    }
    Ok(())
}

fn count(m: &Map<String, Value>, key: &str) -> Result<u64, Error> {
    m.get(key).and_then(Value::as_u64).ok_or_else(|| Error::Io(format!("`{key}` must be a whole number")))
}

/// The host's limits, each lowered to what Genko asks (never raised).
pub fn limits_with(asked: Option<&Value>) -> Limits {
    let mut l = Limits::default();
    let Some(Value::Object(m)) = asked else { return l };
    let get = |k: &str| m.get(k).and_then(Value::as_u64);
    if let Some(v) = get("max_module_bytes") {
        l.max_module_bytes = l.max_module_bytes.min(v as usize);
    }
    if let Some(v) = get("max_memory_bytes") {
        l.max_memory_bytes = l.max_memory_bytes.min(v as usize);
    }
    if let Some(v) = get("band_bytes") {
        l.band_bytes = l.band_bytes.min(v as usize);
    }
    if let Some(v) = get("fuel_base") {
        l.fuel_base = l.fuel_base.min(v);
    }
    if let Some(v) = get("fuel_per_sample") {
        l.fuel_per_sample = l.fuel_per_sample.min(v);
    }
    if let Some(v) = get("max_recursion_depth") {
        l.max_recursion_depth = l.max_recursion_depth.min(v as usize);
    }
    if let Some(v) = get("wall_time_ms") {
        l.wall_time_ms = l.wall_time_ms.min(v);
    }
    l
}

fn limits_json(l: &Limits) -> Value {
    json!({
        "max_module_bytes": l.max_module_bytes, "max_memory_bytes": l.max_memory_bytes, "band_bytes": l.band_bytes,
        "fuel_base": l.fuel_base, "fuel_per_sample": l.fuel_per_sample, "max_recursion_depth": l.max_recursion_depth,
        "wall_time_ms": l.wall_time_ms,
    })
}

/// Serves one run: the module, then its bands, until the input ends. Returns the process's exit code (0: the input
/// ended; 1: a refusal was sent).
pub fn serve<R: BufRead, W: Write>(mut input: R, mut out: W) -> std::io::Result<i32> {
    // 1. the module
    let Some(line) = read_line(&mut input, MAX_LINE)? else { return Ok(0) };
    let hello = match object(&line).and_then(|m| protocol_of(&m).map(|()| m)) {
        Ok(m) => m,
        Err(e) => return refuse(&mut out, &e),
    };
    let limits = limits_with(hello.get("limits"));
    let size = match count(&hello, "module") {
        Ok(n) => n,
        Err(e) => return refuse(&mut out, &e),
    };
    if size > limits.max_module_bytes as u64 {
        return refuse(&mut out, &Error::Module(format!("module is {size} bytes (limit {})", limits.max_module_bytes)));
    }
    let mut bytes = vec![0u8; size as usize];
    input.read_exact(&mut bytes)?;
    let plugin = match std::panic::catch_unwind(|| Plugin::load(&bytes, limits.clone())) {
        Ok(Ok(p)) => p,
        Ok(Err(e)) => return refuse(&mut out, &e),
        Err(_) => return refuse(&mut out, &Error::Trap("the runtime panicked".into())),
    };
    drop(bytes);
    let manifest = serde_json::to_value(plugin.manifest()).unwrap_or(Value::Null);
    write_reply(&mut out, &json!({"ok": true, "manifest": manifest, "limits": limits_json(plugin.limits()), "host": host_name()}), &[])?;

    // 2. the bands, under one deadline
    let deadline = Deadline::new(plugin.limits());
    // (half the plug-in's memory is for the band: the module needs room of its own, as PhotoCraft's apply leaves it)
    let room = plugin.limits().max_memory_bytes / 2;
    loop {
        let Some(line) = read_line(&mut input, MAX_LINE)? else { return Ok(0) };
        let band = match object(&line).and_then(|m| protocol_of(&m).map(|()| m)) {
            Ok(m) => m,
            Err(e) => return refuse(&mut out, &e),
        };
        let geometry = (|| -> Result<(u32, u32, u32, u32), Error> {
            let side = |k: &str| -> Result<u32, Error> {
                let v = count(&band, k)?;
                u32::try_from(v).ok().filter(|v| *v > 0 && *v <= 1 << 20).ok_or_else(|| Error::Io(format!("`{k}` is out of range")))
            };
            let (w, h, ch) = (side("width")?, side("height")?, side("channels")?);
            let format = u32::try_from(count(&band, "format")?).map_err(|_| Error::Io("`format` is out of range".into()))?;
            Ok((w, h, ch, format))
        })();
        let (w, h, ch, format) = match geometry {
            Ok(g) => g,
            Err(e) => return refuse(&mut out, &e),
        };
        let samples = (w as usize).checked_mul(h as usize).and_then(|n| n.checked_mul(ch as usize));
        let Some(nbytes) = samples.and_then(|n| n.checked_mul(4)).filter(|b| *b <= room) else {
            return refuse(&mut out, &Error::Limit("memory budget (a band does not fit in the plug-in's memory)".into()));
        };
        let mut raw = vec![0u8; nbytes];
        input.read_exact(&mut raw)?;
        let mut data: Vec<f32> = raw.chunks_exact(4).map(|c| f32::from_le_bytes([c[0], c[1], c[2], c[3]])).collect();
        drop(raw);
        // the settings checked against the manifest (PhotoCraft's resolve_params), Genko's `_image` kept
        let mut given = match band.get("params") {
            None | Some(Value::Null) => Map::new(),
            Some(Value::Object(m)) => m.clone(),
            Some(_) => return refuse(&mut out, &Error::Params("plug-in parameters must be a JSON object".into())),
        };
        let image = given.remove("_image");
        let mut resolved = match plugin.manifest().resolve_params(&Value::Object(given)) {
            Ok(r) => r,
            Err(e) => return refuse(&mut out, &e),
        };
        if let Some(image) = image {
            resolved.insert("_image".into(), image);
        }
        let pbytes = serde_json::to_vec(&Value::Object(resolved)).unwrap_or_default();
        let run = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| plugin.filter_buffer(&mut data, w, h, ch, format, &pbytes, &deadline)));
        match run {
            Ok(Ok(())) => {}
            Ok(Err(e)) => return refuse(&mut out, &e),
            Err(_) => return refuse(&mut out, &Error::Trap("the runtime panicked".into())),
        }
        let mut back = Vec::with_capacity(nbytes);
        for v in &data {
            back.extend_from_slice(&v.to_le_bytes());
        }
        write_reply(&mut out, &json!({"ok": true, "width": w, "height": h, "channels": ch}), &back)?;
    }
}
