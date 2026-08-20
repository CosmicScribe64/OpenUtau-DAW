#include "openutau_vst/audio_ring_buffer.hpp"

#include <array>
#include <cassert>
#include <stdexcept>

using openutau::vst::AudioRingBuffer;

int main() {
  bool rejected = false;
  try { AudioRingBuffer invalid(3, 2); } catch (const std::invalid_argument&) { rejected = true; }
  assert(rejected);

  AudioRingBuffer buffer(4, 2);
  const std::array<float, 6> first{1, 2, 3, 4, 5, 6};
  assert(buffer.write(first.data(), 3) == 3);
  assert(buffer.readableFrames() == 3);

  std::array<float, 4> head{};
  assert(buffer.read(head.data(), 2) == 2);
  assert((head == std::array<float, 4>{1, 2, 3, 4}));

  const std::array<float, 6> wrapped{7, 8, 9, 10, 11, 12};
  assert(buffer.write(wrapped.data(), 3) == 3);
  std::array<float, 8> tail{};
  assert(buffer.read(tail.data(), 4) == 4);
  assert((tail == std::array<float, 8>{5, 6, 7, 8, 9, 10, 11, 12}));

  assert(buffer.write(first.data(), 3) == 3);
  buffer.clear();
  assert(buffer.readableFrames() == 0);

  AudioRingBuffer tagged(8, 2);
  assert(tagged.writeTagged(first.data(), 3, 4, 100) == 3);
  assert(tagged.writeTagged(wrapped.data(), 3, 5, 200) == 3);
  std::array<float, 6> taggedOut{};
  assert(tagged.readTagged(taggedOut.data(), 3, 5, 200) == 3);
  assert(taggedOut == wrapped);

  AudioRingBuffer gap(8, 2);
  assert(gap.writeTagged(first.data(), 3, 9, 301) == 3);
  assert(gap.readTagged(taggedOut.data(), 3, 9, 300) == 0);
  return 0;
}
