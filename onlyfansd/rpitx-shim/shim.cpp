// C-callable shim around librpitx::ookbursttiming. See shim.h.
//
// We deliberately keep the surface tiny: one entry point that mirrors what
// the upstream `sendook` CLI does after argument parsing. Errors from
// librpitx propagate via std::exception (it occasionally throws); we catch
// them and return non-zero.

#include "shim.h"

#include <librpitx/librpitx.h>

#include <unistd.h>
#include <vector>
#include <new>

extern "C" int rpitx_send_ook_timing(uint64_t freq_hz,
                                     const uint8_t *values,
                                     const uint32_t *durations_us,
                                     size_t n_samples,
                                     uint32_t total_duration_us,
                                     uint32_t repeat,
                                     uint32_t pause_us)
{
    if (n_samples == 0 || values == nullptr || durations_us == nullptr) {
        return 1;
    }

    try {
        ookbursttiming sender(freq_hz, total_duration_us);

        std::vector<ookbursttiming::SampleOOKTiming> message(n_samples);
        for (size_t i = 0; i < n_samples; ++i) {
            message[i].value    = values[i];
            message[i].duration = durations_us[i];
        }

        for (uint32_t r = 0; r < (repeat == 0 ? 1 : repeat); ++r) {
            sender.SendMessage(message.data(), n_samples);
            if (r + 1 < repeat && pause_us > 0) {
                usleep(pause_us);
            }
        }
        return 0;
    } catch (const std::bad_alloc &) {
        return 2;
    } catch (...) {
        return 3;
    }
}
