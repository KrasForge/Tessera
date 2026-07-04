//! Safe Rust wrapper over the Tessera C plugin ABI (Theme F, issue #126).
//!
//! The plugin ABI is five C symbols (`plugin_abi_version`, `plugin_init`,
//! `plugin_process_block`, `plugin_set_param`, `plugin_destroy`); see
//! `docs/plugin-abi.md` and `include/plugin_abi.h`. Writing them by hand in Rust
//! means `#[no_mangle] extern "C"`, raw pointers, and a manually-managed static
//! instance. This crate hides all of that: an author implements the safe
//! [`Plugin`] trait over slices, and the [`tessera_plugin!`] macro generates the
//! five exports, the pointer/length marshalling, and the instance lifecycle.
//!
//! The library is `no_std` (a plugin is a freestanding object with no libc);
//! `std` is pulled in only for `cargo test` on the host. A real plugin crate is
//! built as a `cdylib` for the `aarch64` bare-metal target and supplies its own
//! `#[panic_handler]`.
//!
//! ```ignore
//! use tessera_plugin::{Plugin, tessera_plugin};
//!
//! struct Gain { g: f32 }
//! impl Plugin for Gain {
//!     fn init(_sr: u32, _bs: u32) -> Option<Self> { Some(Gain { g: 1.0 }) }
//!     fn process(&mut self, il: &[f32], ir: &[f32], ol: &mut [f32], or: &mut [f32]) {
//!         for i in 0..ol.len() { ol[i] = il[i] * self.g; or[i] = ir[i] * self.g; }
//!     }
//!     fn set_param(&mut self, id: u32, v: f32) { if id == 0 { self.g = v; } }
//! }
//! tessera_plugin!(Gain);
//! ```

#![cfg_attr(not(test), no_std)]

/// ABI major version this SDK targets (matches `TESSERA_PLUGIN_ABI_VERSION_MAJOR`).
pub const ABI_VERSION_MAJOR: u32 = 1;
/// ABI minor version this SDK targets (matches `TESSERA_PLUGIN_ABI_VERSION_MINOR`).
pub const ABI_VERSION_MINOR: u32 = 1;
/// Packed ABI version, as returned by `plugin_abi_version()`.
pub const ABI_VERSION: u32 = (ABI_VERSION_MAJOR << 16) | ABI_VERSION_MINOR;

/// `plugin_init` return codes (mirror the `TESSERA_PLUGIN_*` C constants).
pub const OK: i32 = 0;
pub const E_VERSION: i32 = -1;
pub const E_NOMEM: i32 = -2;
pub const E_INVAL: i32 = -3;

/// A Tessera audio plugin, implemented over safe slices.
///
/// The generated C exports own a single instance: `init` constructs it,
/// `process`/`set_param` borrow it mutably (only ever from the audio thread,
/// serially), and `destroy` drops it.
pub trait Plugin: Sized {
    /// One-time setup at load. `sample_rate` (Hz) and `block_size` (frames per
    /// `process` call) are fixed for the plugin's lifetime. Return `None` to
    /// reject the configuration (reported to the host as `E_VERSION`).
    fn init(sample_rate: u32, block_size: u32) -> Option<Self>;

    /// Process one block of de-interleaved stereo audio. All four slices have
    /// the same length (`n_frames`). REAL-TIME SAFE: no allocation, no locks,
    /// no syscalls.
    fn process(&mut self, in_l: &[f32], in_r: &[f32], out_l: &mut [f32], out_r: &mut [f32]);

    /// Update a parameter by id. Real-time-safe; unknown ids must be ignored.
    /// The default ignores all parameters.
    fn set_param(&mut self, _param_id: u32, _value: f32) {}
}

/// Generate the five `extern "C"` ABI exports for a type implementing [`Plugin`].
///
/// Invoke once at the crate root: `tessera_plugin!(MyPlugin);`.
#[macro_export]
macro_rules! tessera_plugin {
    ($t:ty) => {
        // Single instance, owned by the C exports. The audio host calls these
        // serially from one thread, so a plain static is sufficient; access is
        // confined to these generated functions.
        static mut __TESSERA_INSTANCE: ::core::option::Option<$t> = ::core::option::Option::None;

        #[no_mangle]
        pub extern "C" fn plugin_abi_version() -> u32 {
            $crate::ABI_VERSION
        }

        #[no_mangle]
        pub extern "C" fn plugin_init(sample_rate: u32, block_size: u32) -> i32 {
            match <$t as $crate::Plugin>::init(sample_rate, block_size) {
                ::core::option::Option::Some(p) => {
                    unsafe { *::core::ptr::addr_of_mut!(__TESSERA_INSTANCE) =
                        ::core::option::Option::Some(p); }
                    $crate::OK
                }
                ::core::option::Option::None => $crate::E_VERSION,
            }
        }

        #[no_mangle]
        pub extern "C" fn plugin_process_block(
            in_l: *const f32, in_r: *const f32,
            out_l: *mut f32, out_r: *mut f32, n_frames: u32,
        ) {
            unsafe {
                // Borrow the instance through a raw pointer (no reference to the
                // static itself), then dispatch to the safe trait method.
                let inst = &mut *::core::ptr::addr_of_mut!(__TESSERA_INSTANCE);
                if let ::core::option::Option::Some(p) = inst.as_mut() {
                    let n = n_frames as usize;
                    // Null buffers or a zero count degrade to a no-op rather than
                    // constructing an invalid slice.
                    if n == 0 || in_l.is_null() || in_r.is_null()
                        || out_l.is_null() || out_r.is_null() {
                        return;
                    }
                    let il = ::core::slice::from_raw_parts(in_l, n);
                    let ir = ::core::slice::from_raw_parts(in_r, n);
                    let ol = ::core::slice::from_raw_parts_mut(out_l, n);
                    let orr = ::core::slice::from_raw_parts_mut(out_r, n);
                    <$t as $crate::Plugin>::process(p, il, ir, ol, orr);
                }
            }
        }

        #[no_mangle]
        pub extern "C" fn plugin_set_param(param_id: u32, value: f32) {
            unsafe {
                let inst = &mut *::core::ptr::addr_of_mut!(__TESSERA_INSTANCE);
                if let ::core::option::Option::Some(p) = inst.as_mut() {
                    <$t as $crate::Plugin>::set_param(p, param_id, value);
                }
            }
        }

        #[no_mangle]
        pub extern "C" fn plugin_destroy() {
            unsafe { *::core::ptr::addr_of_mut!(__TESSERA_INSTANCE) =
                ::core::option::Option::None; }
        }
    };
}
