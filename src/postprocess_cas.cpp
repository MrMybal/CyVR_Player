#include "cyvr/player/postprocess.hpp"
#include <cmath>
#include <stdexcept>
#define A_CPU 1
#include "cas/ffx_a.h"
#include "cas/ffx_cas.h"

namespace cyvr::player {
CasConstants cas_constants(Extent image, float sharpness) {
    if (!image.width || !image.height || !std::isfinite(sharpness) || sharpness < 0 || sharpness > 1)
        throw std::invalid_argument("CAS requires a valid extent and sharpness in [0,1]");
    CasConstants result{};
    CasSetup(result.con0.data(), result.con1.data(), sharpness,
             float(image.width), float(image.height), float(image.width), float(image.height));
    return result;
}
} // namespace cyvr::player
