#include "cyvr/player/video_layout.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>
namespace cyvr::player {
namespace {
std::vector<std::string> words(std::string_view input) {
  input = input.substr(0, input.find_first_of("?#"));
  // SAF URIs can percent-encode the original document name and separators.
  std::string decoded;
  decoded.reserve(std::min<std::size_t>(input.size(), 8192));
  auto hex = [](char c) {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < input.size() && i < 8192; ++i) {
    if (input[i] == '%' && i + 2 < input.size() && hex(input[i + 1]) >= 0 &&
        hex(input[i + 2]) >= 0) {
      decoded += char(hex(input[i + 1]) * 16 + hex(input[i + 2]));
      i += 2;
    } else
      decoded += input[i];
  }
  const auto slash = decoded.find_last_of("/\\");
  if (slash != std::string::npos)
    decoded.erase(0, slash + 1);
  std::vector<std::string> result;
  std::string token;
  for (unsigned char c : decoded) {
    if (c < 128 && std::isalnum(c))
      token += char(std::tolower(c));
    else if (!token.empty()) {
      result.push_back(token);
      token.clear();
    }
  }
  if (!token.empty())
    result.push_back(token);
  return result;
}
} // namespace
VideoLayoutHints named_video_layout(std::string_view name) {
  const auto tokens = words(name);
  auto has = [&](std::initializer_list<std::string_view> names) {
    for (auto name : names)
      if (std::find(tokens.begin(), tokens.end(), name) != tokens.end())
        return true;
    return false;
  };
  auto sequence = [&](std::initializer_list<std::string_view> names) {
    return std::search(tokens.begin(), tokens.end(), names.begin(),
                       names.end()) != tokens.end();
  };
  VideoLayoutHints out;
  const bool sbs = has({"sbs", "hsbs", "fsbs", "lr", "rl", "3dh"}) ||
                   sequence({"side", "by", "side"}) ||
                   sequence({"left", "right"}) || sequence({"right", "left"}),
             tb = has({"tb", "bt", "ou", "uo", "hou", "htb", "fou", "ftb",
                       "3dv"}) ||
                  sequence({"top", "bottom"}) || sequence({"bottom", "top"}) ||
                  sequence({"over", "under"});
  if (sbs != tb) {
    out.stereo = sbs ? 1 : 2;
    out.swap = has({"rl", "bt", "uo"}) || sequence({"right", "left"}) ||
               sequence({"bottom", "top"});
    if (has({"hsbs", "hou", "htb", "half"}))
      out.half = true;
    if (has({"fsbs", "fou", "ftb", "full"}))
      out.half = false;
  } else if (has({"mono", "2d"}) && !sbs && !tb)
    out.stereo = 0;
  const bool half = has({"180", "vr180", "180x180"}),
             full = has({"360", "vr360", "equirectangular"});
  if (half != full)
    out.projection = half ? 1 : 2;
  return out;
}
DetectedVideoLayout detect_video_layout(std::string_view location,
                                        std::string_view title,
                                        const VideoLayoutHints &metadata,
                                        int width, int height,
                                        double pixel_aspect, bool multiview) {
  auto hints = named_video_layout(title),
       fallback = named_video_layout(location);
  if (!hints.stereo) {
    hints.stereo = fallback.stereo;
    hints.half = fallback.half;
    hints.swap = fallback.swap;
  }
  if (!hints.projection)
    hints.projection = fallback.projection;
  DetectedVideoLayout out;
  if (hints.stereo || hints.projection)
    out.evidence = 1;
  if (metadata.stereo || metadata.projection || metadata.unsupported_projection)
    out.evidence = 2;
  if (metadata.stereo) {
    if (hints.stereo != metadata.stereo)
      hints.half.reset();
    hints.stereo = metadata.stereo;
    hints.swap = metadata.swap.value_or(false);
  }
  if (metadata.half)
    hints.half = metadata.half;
  if (metadata.swap)
    hints.swap = metadata.swap;
  if (metadata.projection)
    hints.projection = metadata.projection;
  if (metadata.unsupported_projection) {
    hints.projection = 0;
    out.ambiguous = true;
  }
  out.stereo = std::clamp(hints.stereo.value_or(0), 0, 2);
  out.projection = std::clamp(hints.projection.value_or(0), 0, 2);
  out.swap = out.stereo && hints.swap.value_or(false);
  out.half = out.stereo && hints.half.value_or(false);
  // Infer only packed cinema sampling, never spherical projection from aspect.
  // A non-square SAR may already describe half-width/height content correctly.
  if (out.stereo && !out.projection && !hints.half && width > 0 && height > 0 &&
      std::isfinite(pixel_aspect) && pixel_aspect > 0) {
    const double aspect = double(width) * pixel_aspect / height;
    out.half = out.stereo == 1 ? aspect < 2.5 : aspect > 1.3;
  }
  if (multiview) {
    out.stereo = 1;
    out.half = false;
    out.swap = false;
    out.evidence = 3;
  }
  return out;
}
} // namespace cyvr::player
