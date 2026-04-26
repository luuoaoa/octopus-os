// Direct librpitx binding via a small C++ shim. Replaces the previous
// `Command::new("sendook")` shell-out — same RF behaviour, no fork/exec
// cost, and no dependency on the rpitx CLI tools at runtime.

use std::os::raw::{c_int, c_uchar};

extern "C" {
    fn rpitx_send_ook_timing(
        freq_hz: u64,
        values: *const c_uchar,
        durations_us: *const u32,
        n_samples: usize,
        total_duration_us: u32,
        repeat: u32,
        pause_us: u32,
    ) -> c_int;
}

/// Transmit an OOK message at `freq_hz` whose chips are described by
/// `chip_bits` ('0' / '1' chars), with each chip held for `chip_us`
/// microseconds. Repeats `repeat` times with `pause_us` between repeats.
///
/// Mirrors `sendook -f $freq_hz -0 $chip_us -1 $chip_us $chip_bits`.
pub fn send_ook(
    freq_hz: u64,
    chip_bits: &str,
    chip_us: u32,
    repeat: u32,
    pause_us: u32,
) -> Result<(), String> {
    let (values, durations): (Vec<u8>, Vec<u32>) = chip_bits
        .chars()
        .filter_map(|c| match c {
            '0' => Some((0u8, chip_us)),
            '1' => Some((1u8, chip_us)),
            _ => None,
        })
        .unzip();

    if values.is_empty() {
        return Err("empty chip stream".into());
    }

    let total_us: u32 = durations.iter().copied().sum();

    let rc = unsafe {
        rpitx_send_ook_timing(
            freq_hz,
            values.as_ptr(),
            durations.as_ptr(),
            values.len(),
            total_us,
            repeat,
            pause_us,
        )
    };
    if rc == 0 {
        Ok(())
    } else {
        Err(format!("rpitx_send_ook_timing rc={}", rc))
    }
}
