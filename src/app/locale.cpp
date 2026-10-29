#include "cyvr/player/locale.hpp"
#include "cyvr/locale.hpp"
#include "cyvr/player/locale_data.hpp"
#include <algorithm>
namespace cyvr::player {
std::string_view player_translate(std::string_view language,
                                  std::string_view english) {
  if (std::string_view(cyvr::resolveLocale(language).code) != "fr")
    return english;
  const auto first = std::begin(player_messages),
             last = std::end(player_messages);
  auto found = std::lower_bound(first, last, english,
                                [](const auto &item, std::string_view key) {
                                  return item.english < key;
                                });
  return found != last && found->english == english ? found->french : english;
}
} // namespace cyvr::player
