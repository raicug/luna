#pragma once

// clang-format off
#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>
// clang-format on

namespace Luna {

class LifetimeHandle;

namespace Detail {

class LifetimeRecord final {
public:
  LifetimeRecord() noexcept = default;

  LifetimeRecord(const LifetimeRecord &) = delete;
  LifetimeRecord &operator=(const LifetimeRecord &) = delete;
  LifetimeRecord(LifetimeRecord &&) = delete;
  LifetimeRecord &operator=(LifetimeRecord &&) = delete;
  ~LifetimeRecord() = default;

  [[nodiscard]] std::uint64_t Generation() const noexcept {
    return Current.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool IsLive() const noexcept {
    const std::uint64_t Observed = Current.load(std::memory_order_acquire);
    return (Observed & 1U) == 1U;
  }

  std::uint64_t Invalidate() noexcept {
    std::uint64_t Observed = Current.load(std::memory_order_acquire);
    while ((Observed & 1U) == 1U) {
      const std::uint64_t Advanced = Observed + 1;
      if (Current.compare_exchange_weak(Observed, Advanced,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire))
        return Advanced;
    }
    return Observed;
  }

private:
  std::atomic<std::uint64_t> Current{1};
};

struct LifetimeHandleAccess final {
  [[nodiscard]] static std::shared_ptr<const LifetimeRecord>
  Retain(const LifetimeHandle &Handle) noexcept;
  [[nodiscard]] static const void *
  Identity(const LifetimeHandle &Handle) noexcept;
  [[nodiscard]] static std::uint64_t
  Generation(const LifetimeHandle &Handle) noexcept;
};

} // namespace Detail

class LifetimeHandle final {
public:
  LifetimeHandle() : Record(std::make_shared<Detail::LifetimeRecord>()) {}

  [[nodiscard]] static LifetimeHandle Undeclared() noexcept {
    return LifetimeHandle(nullptr);
  }

  LifetimeHandle(const LifetimeHandle &) = default;
  LifetimeHandle &operator=(const LifetimeHandle &) = default;
  LifetimeHandle(LifetimeHandle &&) noexcept = default;
  LifetimeHandle &operator=(LifetimeHandle &&) noexcept = default;

  ~LifetimeHandle() = default;

  [[nodiscard]] bool IsDeclared() const noexcept { return Record != nullptr; }

  [[nodiscard]] bool IsValid() const noexcept {
    return Record != nullptr && Record->IsLive();
  }

  [[nodiscard]] std::uint64_t Generation() const noexcept {
    return Record ? Record->Generation() : 0;
  }

  void Invalidate() noexcept {
    if (Record)
      static_cast<void>(Record->Invalidate());
  }

  [[nodiscard]] bool RefersToSame(const LifetimeHandle &Other) const noexcept {
    return Record == Other.Record;
  }

private:
  friend struct Detail::LifetimeHandleAccess;

  explicit LifetimeHandle(std::nullptr_t) noexcept : Record(nullptr) {}

  std::shared_ptr<Detail::LifetimeRecord> Record;
};

namespace Detail {

inline std::shared_ptr<const LifetimeRecord>
LifetimeHandleAccess::Retain(const LifetimeHandle &Handle) noexcept {
  return Handle.Record;
}

inline const void *
LifetimeHandleAccess::Identity(const LifetimeHandle &Handle) noexcept {
  const LifetimeRecord *Held = Handle.Record.get();
  return static_cast<const void *>(Held);
}

inline std::uint64_t
LifetimeHandleAccess::Generation(const LifetimeHandle &Handle) noexcept {
  return Handle.Generation();
}

} // namespace Detail

} // namespace Luna
