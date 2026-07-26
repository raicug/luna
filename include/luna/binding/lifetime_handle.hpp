#pragma once

// The explicit lifetime handle of one borrowed native object.
//
// Luna never deletes an object it only borrows, so it also never guesses how
// long that object lives. A borrowed value is exposed with one explicit handle
// the owner of the object holds. While the handle is live, every access to the
// values exposed through it is permitted; the moment the owner invalidates it,
// every later access to every one of those values fails before any native code
// runs.
//
// Invalidation is one atomic step and it is idempotent. The handle owns a small
// Luna-owned record with a monotonic generation: a live generation is odd, an
// invalidated one is even, and invalidation advances the generation exactly
// once. Each exposed value remembers the generation it was published under, so
// a single comparison rejects every value of an invalidated handle, and a
// second invalidation changes nothing.
//
// Copies of one handle share its record, so a value exposed through any copy is
// rejected as soon as any copy is invalidated. The record is reference counted
// and Luna retains it for as long as one exposed value still needs it, which is
// why a handle may be destroyed while its values are still being cleaned up.
//
// Nothing here is a native or virtual-machine address, and nothing here needs a
// Luau type: a handle is an ownership statement, not a pointer.

// clang-format off
#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>
// clang-format on

namespace Luna {

class LifetimeHandle;

namespace Detail {

// The Luna-owned record behind one lifetime handle. The generation is the whole
// mechanism: odd means live, even means invalidated, and it only ever advances.
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

  // Advances a live generation exactly once. A record that is already
  // invalidated observes an even generation and is left exactly as it is, which
  // is what makes invalidation idempotent as well as atomic.
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

// The private bridge Luna's exposure and release paths use to retain one
// handle's record and to read its generation. A consumer never names it.
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
  // A default-constructed handle is one new live lifetime: the owner of the
  // native object holds it and decides when it ends.
  LifetimeHandle() : Record(std::make_shared<Detail::LifetimeRecord>()) {}

  // A handle that declares no lifetime at all. Exposing a borrowed value with
  // it is refused, because a borrowed value always requires an explicit
  // lifetime.
  [[nodiscard]] static LifetimeHandle Undeclared() noexcept {
    return LifetimeHandle(nullptr);
  }

  LifetimeHandle(const LifetimeHandle &) = default;
  LifetimeHandle &operator=(const LifetimeHandle &) = default;
  LifetimeHandle(LifetimeHandle &&) noexcept = default;
  LifetimeHandle &operator=(LifetimeHandle &&) noexcept = default;

  // Destroying a handle does not end the lifetime it declares: only
  // `Invalidate` does. Luna keeps the record alive for as long as an exposed
  // value still needs it.
  ~LifetimeHandle() = default;

  // Whether this handle declares a lifetime at all.
  [[nodiscard]] bool IsDeclared() const noexcept { return Record != nullptr; }

  // Whether the declared lifetime is still live.
  [[nodiscard]] bool IsValid() const noexcept {
    return Record != nullptr && Record->IsLive();
  }

  // The generation the declared lifetime is on right now, or zero when the
  // handle declares nothing. It is a Luna-local counter, never an address.
  [[nodiscard]] std::uint64_t Generation() const noexcept {
    return Record ? Record->Generation() : 0;
  }

  // Ends the declared lifetime. Every value exposed through this handle, or
  // through any copy of it, fails every later access before native code runs.
  // Calling it again changes nothing.
  void Invalidate() noexcept {
    if (Record)
      static_cast<void>(Record->Invalidate());
  }

  // Whether two handles declare exactly the same lifetime.
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
