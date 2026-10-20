#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

#include "quorumkv/types.hpp"

namespace quorumkv {

inline constexpr std::uint8_t kWireVersion = 1;
inline constexpr std::size_t kMaxFrameBytes = 4 * 1024 * 1024;
inline constexpr std::size_t kMaxBlobBytes = 1024 * 1024;
inline constexpr std::size_t kMaxEntries = 4096;

class CodecError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// One complete length-prefixed frame. Integers are unsigned, fixed-width, big-endian.
// Invalid tags, noncanonical booleans, trailing bytes and oversized input throw CodecError.
Bytes Encode(const Message& message);
Message Decode(std::span<const std::uint8_t> frame);

}  // namespace quorumkv
