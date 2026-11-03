#pragma once

namespace cyvr::player {
inline constexpr unsigned ui_atlas_width = 160;
inline constexpr unsigned preview_slots = 9;
inline constexpr unsigned preview_atlas_height = preview_slots * 90;
inline constexpr unsigned logo_atlas_y = preview_atlas_height + 10;
inline constexpr unsigned ui_atlas_height = logo_atlas_y + 160;
inline constexpr unsigned logo_size = 160;
} // namespace cyvr::player
