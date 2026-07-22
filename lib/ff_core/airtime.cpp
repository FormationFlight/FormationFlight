#include "airtime.h"

#include <cmath>

namespace ff {

double loraAirtimeMs(const LoraParams& p, size_t payload_bytes) {
    const double sf = static_cast<double>(p.spreading_factor);
    const double bw = static_cast<double>(p.bandwidth_hz);

    // Symbol duration (seconds).
    const double t_sym = std::pow(2.0, sf) / bw;

    // Preamble duration.
    const double t_preamble = (static_cast<double>(p.preamble_symbols) + 4.25) * t_sym;

    // Payload symbol count (Semtech AN1200.13).
    const int crc = p.crc_on ? 1 : 0;
    const int ih = p.explicit_header ? 0 : 1;   // implicit header => 1
    const int de = p.low_data_rate_optimize ? 1 : 0;
    const int cr = static_cast<int>(p.coding_rate_denom) - 4;  // 4/5..4/8 => 1..4

    const double numerator =
        8.0 * static_cast<double>(payload_bytes) - 4.0 * sf + 28.0 + 16.0 * crc - 20.0 * ih;
    const double denominator = 4.0 * (sf - 2.0 * de);

    double payload_symb = std::ceil(numerator / denominator) * (cr + 4);
    if (payload_symb < 0.0) {
        payload_symb = 0.0;
    }
    payload_symb += 8.0;

    const double t_payload = payload_symb * t_sym;
    return (t_preamble + t_payload) * 1000.0;
}

}  // namespace ff
