#pragma once
#include "cyvr/player/application.hpp"
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
namespace cyvr::player {
struct PlayerApplication::PreviewWorker {
  Platform platform;
  std::mutex mutex;
  std::condition_variable wake;
  std::vector<std::string> requests;
  std::map<std::string, std::vector<std::uint8_t>> cache;
  std::vector<std::string> lru;
  std::vector<std::uint8_t> atlas;
  std::array<bool, 7> present{};
  std::uint64_t revision{}, generation{};
  bool pending{}, quit{};
  std::shared_ptr<Cancellation> cancellation = std::make_shared<Cancellation>();
  std::thread worker;
  explicit PreviewWorker(Platform);
  ~PreviewWorker();
  void request(std::vector<std::string>);
  void run();
};
} // namespace cyvr::player
