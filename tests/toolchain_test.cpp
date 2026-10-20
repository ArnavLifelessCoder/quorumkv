#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <span>
#include <version>

TEST(Toolchain, SupportsCpp20AndPortablePaths) {
  static_assert(__cplusplus >= 202002L);
  constexpr std::array values = {1, 2, 3};
  const std::span<const int> view(values);
  EXPECT_EQ(view.size(), 3U);
  const auto path = std::filesystem::path("results") / "test.json";
  EXPECT_EQ(path.filename(), std::filesystem::path("test.json"));
}
