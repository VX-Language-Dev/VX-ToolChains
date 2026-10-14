#include "vx/util/arena.hpp"

#include <cstdint>

namespace vx::util {

namespace {

// 向上对齐（alignment 必须为 2 的幂）
template <class T> constexpr T align_up(T value, std::size_t alignment) {
  return (value + static_cast<T>(alignment - 1)) &
         ~static_cast<T>(alignment - 1);
}

} // namespace

struct Arena::Block {
  Block *next;
  std::size_t capacity; // 数据区容量（字节）
  std::size_t used;     // 数据区已用字节

  // 数据区起始偏移：块头之后向上对齐到 max_align，保证数据区对齐
  static std::size_t data_offset() noexcept {
    const std::size_t header = (sizeof(Block) + alignof(std::max_align_t) - 1) &
                               ~(alignof(std::max_align_t) - 1);
    return header;
  }

  std::byte *data() noexcept {
    return reinterpret_cast<std::byte *>(this) + data_offset();
  }
};

Arena::Arena(std::size_t block_size) : block_size_(block_size) {}

Arena::~Arena() {
  Block *b = head_;
  while (b) {
    Block *next = b->next;
    ::operator delete(static_cast<void *>(b));
    b = next;
  }
}

void Arena::new_block(std::size_t bytes, std::size_t alignment) {
  // 容量需覆盖：新块内 bump（含对齐填充，最多 alignment-1 字节）
  const std::size_t needed = Block::data_offset() + bytes + alignment;
  const std::size_t capacity = needed > block_size_ ? needed : block_size_;

  void *mem = ::operator new(sizeof(Block) + capacity);
  auto *block = ::new (mem) Block{head_, capacity, 0};
  head_ = block;
  current_ = block;
}

void *Arena::allocate(std::size_t bytes, std::size_t alignment) {
  if (bytes == 0) {
    bytes = 1;
  }

  auto fits = [bytes, alignment](Block *b) -> bool {
    const auto base = reinterpret_cast<std::uintptr_t>(b->data());
    return align_up(base + b->used, alignment) + bytes <= base + b->capacity;
  };

  if (!current_ || !fits(current_)) {
    new_block(bytes, alignment);
  }

  const auto base = reinterpret_cast<std::uintptr_t>(current_->data());
  const auto addr = align_up(base + current_->used, alignment);
  current_->used = static_cast<std::size_t>(addr - base + bytes);
  return reinterpret_cast<void *>(addr);
}

void Arena::reset() noexcept {
  for (Block *b = head_; b; b = b->next) {
    b->used = 0;
  }
  current_ = head_;
}

} // namespace vx::util
