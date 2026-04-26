/* C-callable shim around librpitx::ookbursttiming.
 *
 * Mirrors what `sendook` does internally — constructs an ookbursttiming,
 * fills a SampleOOKTiming[] from caller-supplied parallel arrays of bit
 * values + per-bit durations (in microseconds), and calls SendMessage().
 *
 * Returns 0 on success, non-zero on failure. Failure currently just means
 * "construction threw / no /dev/mem" — the underlying C++ has limited
 * error reporting.
 */
#ifndef ONLYFANSD_RPITX_SHIM_H
#define ONLYFANSD_RPITX_SHIM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

int rpitx_send_ook_timing(uint64_t freq_hz,
                          const uint8_t *values,
                          const uint32_t *durations_us,
                          size_t n_samples,
                          uint32_t total_duration_us,
                          uint32_t repeat,
                          uint32_t pause_us);

#ifdef __cplusplus
}
#endif

#endif
