//! Integration test for the Rust plugin SDK (Theme F, issue #126): a gain
//! plugin defined through the safe `Plugin` trait, driven through the generated
//! C ABI exports exactly as the host would call them.
//!
//! Run via:  cargo test  (in sdk/rust/tessera-plugin)

use tessera_plugin::{tessera_plugin, Plugin, ABI_VERSION, E_VERSION, OK};

struct Gain {
    g: f32,
}

impl Plugin for Gain {
    fn init(sample_rate: u32, block_size: u32) -> Option<Self> {
        // Reject a nonsensical configuration to exercise the error path.
        if sample_rate == 0 || block_size == 0 {
            return None;
        }
        Some(Gain { g: 1.0 })
    }

    fn process(&mut self, in_l: &[f32], in_r: &[f32], out_l: &mut [f32], out_r: &mut [f32]) {
        for i in 0..out_l.len() {
            out_l[i] = in_l[i] * self.g;
            out_r[i] = in_r[i] * self.g;
        }
    }

    fn set_param(&mut self, param_id: u32, value: f32) {
        if param_id == 0 {
            self.g = value;
        }
        // any other id is ignored, per the ABI contract
    }
}

// Generates plugin_abi_version / plugin_init / plugin_process_block /
// plugin_set_param / plugin_destroy at this module's root; the tests below call
// them directly, driving the plugin exactly through its C ABI surface.
tessera_plugin!(Gain);

#[test]
fn abi_version_matches_sdk() {
    assert_eq!(plugin_abi_version(), ABI_VERSION);
    assert_eq!(ABI_VERSION, (1u32 << 16) | 1u32); // v1.1
}

#[test]
fn init_rejects_bad_config_and_accepts_good() {
    assert_eq!(plugin_init(0, 64), E_VERSION);
    assert_eq!(plugin_init(48_000, 64), OK);
    plugin_destroy();
}

#[test]
fn process_applies_gain_through_the_c_abi() {
    assert_eq!(plugin_init(48_000, 4), OK);

    let il = [1.0f32, 2.0, 3.0, 4.0];
    let ir = [-1.0f32, -2.0, -3.0, -4.0];
    let mut ol = [0.0f32; 4];
    let mut or = [0.0f32; 4];

    // Default gain is unity.
    plugin_process_block(il.as_ptr(), ir.as_ptr(), ol.as_mut_ptr(), or.as_mut_ptr(), 4);
    assert_eq!(ol, il);
    assert_eq!(or, ir);

    // Set param 0 (gain) to 0.5 and reprocess.
    plugin_set_param(0, 0.5);
    plugin_process_block(il.as_ptr(), ir.as_ptr(), ol.as_mut_ptr(), or.as_mut_ptr(), 4);
    assert_eq!(ol, [0.5, 1.0, 1.5, 2.0]);
    assert_eq!(or, [-0.5, -1.0, -1.5, -2.0]);

    // An unknown param id is ignored (gain unchanged).
    plugin_set_param(99, 0.0);
    plugin_process_block(il.as_ptr(), ir.as_ptr(), ol.as_mut_ptr(), or.as_mut_ptr(), 4);
    assert_eq!(ol, [0.5, 1.0, 1.5, 2.0]);

    plugin_destroy();
}

#[test]
fn null_or_empty_block_is_a_noop() {
    assert_eq!(plugin_init(48_000, 4), OK);
    let mut ol = [7.0f32; 4];
    let mut or = [7.0f32; 4];
    // Zero frames: nothing is written.
    plugin_process_block(core::ptr::null(), core::ptr::null(), ol.as_mut_ptr(), or.as_mut_ptr(), 0);
    assert_eq!(ol, [7.0; 4]);
    // Null input pointers: guarded, no write, no UB.
    plugin_process_block(core::ptr::null(), core::ptr::null(), ol.as_mut_ptr(), or.as_mut_ptr(), 4);
    assert_eq!(ol, [7.0; 4]);
    plugin_destroy();
}
