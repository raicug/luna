// clang-format off
#include <luna/binding/class_allocator.hpp>

#include <cstddef>
#include <memory>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>
// clang-format on

namespace {

struct StandaloneObject final {
  int Value = 0;
};

static_assert(std::is_trivially_copyable_v<Luna::StorageRequest>,
              "A storage request stays a plain consumer value.");
static_assert(Luna::StorageRequest::ForClass<StandaloneObject>().IsUsable(),
              "The storage of a complete class type is usable.");
static_assert(!Luna::StorageRequest().IsUsable(),
              "An empty storage request names no usable storage.");
static_assert(Luna::AllocatorStepResult::Done().Performed &&
                  !Luna::AllocatorStepResult::Declined().Performed,
              "A semantic step reports whether it performed its work.");

class StandaloneArena final {
public:
  [[nodiscard]] void *Reserve(std::size_t ByteCount, std::size_t Alignment) {
    ++Reservations;
    return ::operator new(ByteCount, std::align_val_t{Alignment});
  }

  void Return(void *Storage, std::size_t Alignment) {
    ++Returns;
    ::operator delete(Storage, std::align_val_t{Alignment});
  }

  [[nodiscard]] std::size_t Balance() const noexcept {
    return Reservations - Returns;
  }

private:
  std::size_t Reservations = 0;
  std::size_t Returns = 0;
};

[[nodiscard]] Luna::ClassAllocator StandaloneArenaProtocol() {
  const std::shared_ptr<StandaloneArena> Arena =
      std::make_shared<StandaloneArena>();
  Luna::ClassAllocator::AllocateOperation Allocate =
      [Arena](const Luna::StorageRequest &Wanted) -> void * {
    return Arena->Reserve(Wanted.ByteCount, Wanted.Alignment);
  };
  Luna::ClassAllocator::ConstructOperation Construct = [](void *Storage) {
    static_cast<void>(new (Storage) StandaloneObject());
    return Luna::AllocatorStepResult::Done();
  };
  Luna::ClassAllocator::DestroyOperation Destroy = [](void *Storage) {
    static_cast<StandaloneObject *>(Storage)->~StandaloneObject();
    return Luna::AllocatorStepResult::Done();
  };
  Luna::ClassAllocator::DeallocateOperation Deallocate =
      [Arena](void *Storage, const Luna::StorageRequest &Wanted) {
        Arena->Return(Storage, Wanted.Alignment);
        return Luna::AllocatorStepResult::Done();
      };
  return Luna::ClassAllocator::FromOperations(
      "standalone.arena", Luna::StorageRequest::ForClass<StandaloneObject>(),
      std::move(Allocate), std::move(Construct), std::move(Destroy),
      std::move(Deallocate));
}

static_assert(
    std::is_same_v<decltype(StandaloneArenaProtocol()), Luna::ClassAllocator>,
    "An assembled protocol stays a consumer value.");
static_assert(
    std::is_same_v<
        decltype(Luna::ClassAllocator::ForOwnedObject<StandaloneObject>()),
        Luna::ClassAllocator>,
    "The ordinary protocol of a class stays a consumer value.");
static_assert(
    std::is_same_v<
        decltype(Luna::ClassAllocator::ForAdoptedObject<StandaloneObject>()),
        Luna::ClassAllocator>,
    "The protocol of an adopted object stays a consumer value.");
static_assert(
    std::is_same_v<
        decltype(std::declval<const Luna::ClassAllocator &>().OwnsStorage()),
        bool>,
    "Whether Luna owns the storage stays a plain query.");
static_assert(
    std::is_same_v<
        decltype(std::declval<const Luna::ClassAllocator &>().PolicyIdentity()),
        std::string_view>,
    "A policy identity stays an ordinary string view.");
static_assert(
    std::is_same_v<
        decltype(std::declval<const Luna::ClassAllocator &>().RefersToSame(
            std::declval<const Luna::ClassAllocator &>())),
        bool>,
    "Comparing two protocols stays a plain query.");

} // namespace
