#include <gtest/gtest.h>

#include <iostream>
#include <string_view>
#include <vector>

#include "seed_config.hpp"

auto main(int argc, char** argv) -> int try {
  std::vector<std::string_view> options;
  int remaining = 1;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument(argv[i]);
    if (argument.starts_with("--seed")) {
      options.push_back(argument);
    } else {
      argv[remaining++] = argv[i];
    }
  }
  argc = remaining;
  argv[argc] = nullptr;
  try {
    quorumkv::test::seed_config = quorumkv::test::ParseSeedOptions(options);
  } catch (const std::invalid_argument& error) {
    std::cerr << error.what() << '\n';
    return 2;
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
} catch (...) {
  return 2;
}
