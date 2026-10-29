#include "cyvr/player/postprocess.hpp"
#include <cmath>
#include <stdexcept>
#define A_CPU 1
#include "fsr1/ffx_a.h"
#include "fsr1/ffx_fsr1.h"

namespace cyvr::player {
EasuConstants easu_constants(Extent input, Extent output) {
    if (!input.width || !input.height || output.width < input.width || output.height < input.height)
        throw std::invalid_argument("EASU requires nonzero input and output >= input in both axes");
    EasuConstants result{};
    FsrEasuCon(result.con0.data(), result.con1.data(), result.con2.data(), result.con3.data(),
        float(input.width), float(input.height), float(input.width), float(input.height),
        float(output.width), float(output.height));
    return result;
}
RcasConstants rcas_constants(float stops) {
    if (!std::isfinite(stops) || stops < 0 || stops > 2)
        throw std::invalid_argument("RCAS attenuation must be in [0,2] stops (0 = strongest)");
    RcasConstants result{};
    FsrRcasCon(result.con.data(), stops);
    return result;
}
} // namespace cyvr::player
