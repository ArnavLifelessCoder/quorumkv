#pragma once

#include <charconv>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>

namespace quorumkv::test {

struct SeedConfig {
  std::uint64_t first{};
  std::uint64_t count{10000};
};

inline SeedConfig seed_config;
inline bool mutation_survey{};

inline auto ParseSeedOptions(std::span<const std::string_view> options) -> SeedConfig {
  SeedConfig config;
  bool saw_seed = false;
  bool saw_count = false;
  for (auto option : options) {
    const bool is_seed = option.starts_with("--seed=");
    const bool is_count = option.starts_with("--seed-count=");
    if ((!is_seed && !is_count) || (is_seed && saw_seed) || (is_count && saw_count)) {
      throw std::invalid_argument("use --seed=N and --seed-count=N at most once");
    }
    const auto number = option.substr(is_seed ? 7 : 13);
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), value);
    if (number.empty() || error != std::errc{} || end != number.data() + number.size()) {
      throw std::invalid_argument("seed options require unsigned decimal integers");
    }
    if (is_seed) {
      config.first = value;
      saw_seed = true;
    } else {
      config.count = value;
      saw_count = true;
    }
  }
  if (saw_seed && !saw_count) {
    config.count = 1;
  }
  if (config.count == 0 ||
      config.count - 1 > std::numeric_limits<std::uint64_t>::max() - config.first) {
    throw std::invalid_argument("seed range must be nonempty and must not wrap");
  }
  return config;
}

}  // namespace quorumkv::test
