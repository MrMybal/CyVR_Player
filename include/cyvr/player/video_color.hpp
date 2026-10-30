#pragma once
namespace cyvr::player {
// Android color-standard/transfer identifiers, retained per decoded frame.
struct VideoColor {
  int standard{1}, transfer{3}, range{};
  float peak_nits{1000};
  bool hdr() const { return transfer == 6 || transfer == 7; }
};
}
