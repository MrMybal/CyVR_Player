#pragma once
#include <cstdint>
#include <optional>

namespace cyvr::player {
enum class StereoLayout { mono, side_by_side, top_bottom };
enum class Eye { left, right };
enum class Projection { flat, equirect180, equirect360 };

struct Uv { float u, v; };
struct Direction { float x, y, z; };
struct UvRect { float x, y, width, height; };

// Texture origin: top left. OpenXR view axes: +X right, +Y up, -Z forward.
UvRect eye_rect(StereoLayout layout, Eye eye, bool swap_eyes = false) noexcept;
Uv apply_rect(Uv uv, UvRect rect) noexcept;
// Only for equirectangular projections. Back-facing VR180 rays return nullopt.
std::optional<Uv> equirect_uv(Direction ray, Projection projection) noexcept;
// Coded image dimensions include both eyes. Pixel aspect ratio is independent
// of packing (full/half SBS must be resolved from metadata or user settings).
double eye_aspect(std::uint32_t width, std::uint32_t height,
                  StereoLayout layout, double pixel_aspect = 1.0);
} // namespace cyvr::player
