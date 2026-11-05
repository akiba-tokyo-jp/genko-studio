//! Genko's WebAssembly plug-in host (`genko-plugin-host`, docs/PLUGIN_WASM.md).
//!
//! A filter plug-in is a WebAssembly module of PhotoCraft's plug-in ABI v1. This process loads one module and runs it
//! on the bands of pixels Genko sends it, over Genko's plug-in protocol 2 (one JSON line, then the samples), with the
//! sandbox of PhotoCraft v0.2.0's `crates/plugins` (MIT, native/third_party/photocraft/NOTICE.md): no host imports, an
//! instruction budget, a wall-clock budget, a cap on its memory and on the depth of its calls, a fresh instance for each
//! band. `manifest.rs` and `runtime.rs` are PhotoCraft's; `protocol.rs` is Genko's side of the pipe (PhotoCraft's
//! `apply.rs` is done by Genko itself: it cuts the bands, cleans and blends what comes back).
//!
//! The host runs as a process of its own so that a fault in it (the interpreter is not Genko's code) never reaches the
//! application or the book; it has no files, no network and no clock to give a plug-in, and reads nothing but its pipe.
#![forbid(unsafe_code)]
#![deny(clippy::unwrap_used, clippy::expect_used, clippy::panic, clippy::unimplemented, clippy::todo, clippy::unreachable)]

pub mod manifest;
pub mod protocol;
mod runtime;

pub use manifest::{Area, Kind, Manifest, ParamSpec};
pub use runtime::{ABI_VERSION, Deadline, Limits, Plugin};

/// A plug-in error. Every way a module can misbehave ends here, never in a panic.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum Error {
    #[error("not a valid plug-in module: {0}")]
    Module(String),
    #[error("plug-in ABI: {0}")]
    Abi(String),
    #[error("plug-in manifest: {0}")]
    Manifest(String),
    #[error("plug-in trapped: {0}")]
    Trap(String),
    #[error("plug-in exceeded its {0}")]
    Limit(String),
    #[error("plug-in parameters: {0}")]
    Params(String),
    #[error("plug-in failed: {0}")]
    Failed(String),
    #[error("no plug-in {0:?} is installed")]
    NotFound(String),
    #[error("{0}")]
    Io(String),
}

impl Error {
    /// The kind of the error as the protocol names it (Genko words its message by it).
    pub fn code(&self) -> &'static str {
        match self {
            Error::Module(_) => "module",
            Error::Abi(_) => "abi",
            Error::Manifest(_) => "manifest",
            Error::Trap(_) => "trap",
            Error::Limit(_) => "limit",
            Error::Params(_) => "params",
            Error::Failed(_) => "failed",
            Error::NotFound(_) => "not_found",
            Error::Io(_) => "io",
        }
    }
}

pub type Result<T> = std::result::Result<T, Error>;
