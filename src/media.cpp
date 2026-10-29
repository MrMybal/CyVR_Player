#include "cyvr/player/media.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace cyvr::player {
UvRect eye_rect(StereoLayout layout, Eye eye, bool swap) noexcept {
    const bool second = (eye == Eye::right) != swap;
    if (layout == StereoLayout::side_by_side)
        return {second ? 0.5f : 0.0f, 0.0f, 0.5f, 1.0f};
    if (layout == StereoLayout::top_bottom)
        return {0.0f, second ? 0.5f : 0.0f, 1.0f, 0.5f};
    return {0.0f, 0.0f, 1.0f, 1.0f};
}
Uv apply_rect(Uv uv, UvRect rect) noexcept {
    return {rect.x + uv.u * rect.width, rect.y + uv.v * rect.height};
}
std::optional<Uv> equirect_uv(Direction d, Projection projection) noexcept {
    if (projection == Projection::flat || !std::isfinite(d.x) ||
        !std::isfinite(d.y) || !std::isfinite(d.z)) return std::nullopt;
    // Normalize in double precision to avoid overflow for finite float inputs.
    const double length = std::sqrt(double(d.x)*d.x + double(d.y)*d.y + double(d.z)*d.z);
    if (length == 0.0) return std::nullopt;
    constexpr double pi = 3.14159265358979323846;
    // Longitude is undefined at the poles. Choose the forward meridian so
    // exact up/down rays stay on the valid boundary of a VR180 hemisphere.
    const double azimuth = (d.x == 0.0f && d.z == 0.0f)
        ? 0.0 : std::atan2(double(d.x), -double(d.z));
    if (projection == Projection::equirect180 && std::abs(azimuth) > pi * 0.5)
        return std::nullopt;
    const double span = projection == Projection::equirect180 ? pi : 2.0 * pi;
    return Uv{float(azimuth / span + 0.5),
              float(0.5 - std::asin(std::clamp(double(d.y) / length, -1.0, 1.0)) / pi)};
}
double eye_aspect(std::uint32_t w, std::uint32_t h, StereoLayout layout, double par) {
    if (!w || !h || !std::isfinite(par) || par <= 0.0)
        throw std::invalid_argument("Invalid video dimensions or pixel aspect ratio");
    double result = double(w) * par / h;
    if (layout == StereoLayout::side_by_side) result *= 0.5;
    if (layout == StereoLayout::top_bottom) result *= 2.0;
    return result;
}
} // namespace cyvr::player
