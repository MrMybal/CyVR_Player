#pragma once
#include <array>
#include <cstdint>

namespace cyvr::player {
struct Extent { std::uint32_t width, height; };
// Match the shaders' push constant blocks exactly (uvec4 groups, 16-byte aligned).
struct alignas(16) CasConstants { std::array<std::uint32_t, 4> con0, con1; };
struct alignas(16) EasuConstants { std::array<std::uint32_t, 4> con0, con1, con2, con3; };
struct alignas(16) RcasConstants { std::array<std::uint32_t, 4> con; };

// One eye per image. CAS: linear RGB [0,1], same input/output resolution.
// FSR1: EASU then RCAS, perceptual RGB [0,1], after HDR tone mapping.
// No implicit transfer-function conversion is performed by these kernels.
CasConstants cas_constants(Extent image, float sharpness);
EasuConstants easu_constants(Extent input, Extent output);
RcasConstants rcas_constants(float attenuation_stops);
} // namespace cyvr::player
