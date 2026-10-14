// arena.hpp — 线性分配器（bump allocator）：批量分配、整块回收，
// 供编译器各模块做短生命周期对象的分配。
#pragma once

#include <cstddef>
#include <new>
#include <utility>

namespace vx::util {

class Arena {
public:
  // block_size：普通块的最小容量（字节）
  explicit Arena(std::size_t block_size = 64 * 1024);
  ~Arena();

  Arena(const Arena &) = delete;
  Arena &operator=(const Arena &) = delete;

  // 分配指定对齐（必须为 2 的幂）的未初始化内存。
  // 单次需求超过当前块容量时会开辟新块。
  void *allocate(std::size_t bytes,
                 std::size_t alignment = alignof(std::max_align_t));

  // 在 arena 上原地构造 T
  template <class T, class... Args> T *create(Args &&...args) {
    void *mem = allocate(sizeof(T), alignof(T));
    return ::new (mem) T(std::forward<Args>(args)...);
  }

  // 回收全部已分配内存（底层块保留复用）
  void reset() noexcept;

  std::size_t block_size() const noexcept { return block_size_; }

private:
  struct Block;

  void new_block(std::size_t bytes, std::size_t alignment);

  Block *head_ = nullptr;    // 最新块在前
  Block *current_ = nullptr; // 当前 bump 块
  std::size_t block_size_;
};

} // namespace vx::util
