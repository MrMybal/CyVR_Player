#pragma once
#include "cyvr/player/source.hpp"
#include <array>
#include <string_view>
#include <vector>
namespace cyvr::player {
// Standard ClearKey JWK Set. Only caller-supplied keys are accepted; this does
// not acquire licenses or extract keys from proprietary DRM implementations.
std::vector<ContentKey> parse_clear_keys(std::string_view json);
std::vector<ContentKey> read_clear_keys(const MediaSource&,const Cancellation&);
}
