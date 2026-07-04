# tessera-plugin — Rust plugin SDK

A thin, safe Rust wrapper over the Tessera C plugin ABI (v1.1), so plugins can be
written in Rust as well as C (Theme F, issue #126). It is the Rust counterpart of
the C SDK in [`../../`](../..); the binary contract is identical (the five
`plugin_*` symbols in [`include/plugin_abi.h`](../../../include/plugin_abi.h)).

## Writing a plugin

Implement the safe [`Plugin`](src/lib.rs) trait over slices, then invoke the
`tessera_plugin!` macro once to emit the five `extern "C"` ABI exports:

```rust
#![no_std]
use tessera_plugin::{Plugin, tessera_plugin};

struct Gain { g: f32 }

impl Plugin for Gain {
    fn init(_sample_rate: u32, _block_size: u32) -> Option<Self> {
        Some(Gain { g: 1.0 })
    }
    fn process(&mut self, il: &[f32], ir: &[f32], ol: &mut [f32], or: &mut [f32]) {
        for i in 0..ol.len() {
            ol[i] = il[i] * self.g;
            or[i] = ir[i] * self.g;
        }
    }
    fn set_param(&mut self, id: u32, v: f32) {
        if id == 0 { self.g = v; }
    }
}

tessera_plugin!(Gain);
```

The macro handles the pointer/length marshalling, the single-instance lifecycle
(`init` constructs, `destroy` drops), and guards against null/zero-length blocks —
the author only ever touches safe slices.

## What the SDK does *not* hide

The real-time contract is unchanged: `process` and `set_param` run on the audio
path, so no allocation, no locks, no syscalls. The DSP building blocks and effects
suite currently live in the C `libtessera.a`; a Rust plugin implements its DSP in
Rust (or calls the C library via FFI).

## Building

The library is `no_std` and target-agnostic. A plugin crate is built as a
`cdylib` for the bare-metal aarch64 target and links with the plugin linker script
(see [`../../plugin.ld`](../../plugin.ld) and the C SDK's
[`Makefile.template`](../../Makefile.template)); the plugin crate supplies its own
`#[panic_handler]`.

## Testing

```
cargo test
```

`tests/gain_plugin.rs` builds a gain plugin through the trait and drives it through
the generated C ABI exports — ABI-version handshake, `init` accept/reject, unity
and 0.5× gain, unknown-parameter rejection, and null/zero-length block safety.
