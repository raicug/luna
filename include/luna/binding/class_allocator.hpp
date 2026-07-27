#pragma once

// clang-format off
#include <cstddef>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
// clang-format on

namespace Luna {

struct StorageRequest final {
  std::size_t ByteCount = 0;
  std::size_t Alignment = 0;

  [[nodiscard]] constexpr bool IsUsable() const noexcept {
    return ByteCount != 0 && Alignment != 0 &&
           (Alignment & (Alignment - 1)) == 0;
  }

  template <class Type>
  [[nodiscard]] static constexpr StorageRequest ForClass() noexcept {
    static_assert(sizeof(Type) > 0,
                  "A Luna storage request requires a complete class type.");

    StorageRequest Requested;
    Requested.ByteCount = sizeof(Type);
    Requested.Alignment = alignof(Type);
    return Requested;
  }

  [[nodiscard]] friend constexpr bool
  operator==(const StorageRequest &Left,
             const StorageRequest &Right) noexcept = default;
};

struct AllocatorStepResult final {
  bool Performed = false;

  [[nodiscard]] static constexpr AllocatorStepResult Done() noexcept {
    AllocatorStepResult Result;
    Result.Performed = true;
    return Result;
  }

  [[nodiscard]] static constexpr AllocatorStepResult Declined() noexcept {
    return AllocatorStepResult();
  }
};

class ClassAllocator;

namespace Detail {

class AllocatorRecord final {
public:
  using AllocateOperation =
      std::function<void *(const StorageRequest &Requested)>;
  using ConstructOperation = std::function<AllocatorStepResult(void *Storage)>;
  using DestroyOperation = std::function<AllocatorStepResult(void *Storage)>;
  using DeallocateOperation = std::function<AllocatorStepResult(
      void *Storage, const StorageRequest &Requested)>;

  AllocatorRecord(std::string Identity, StorageRequest Requested,
                  AllocateOperation Allocate, ConstructOperation Construct,
                  DestroyOperation Destroy, DeallocateOperation Deallocate)
      : IdentityText(std::move(Identity)), RequestedStorage(Requested),
        AllocateStorage(std::move(Allocate)),
        ConstructObject(std::move(Construct)),
        DestroyObject(std::move(Destroy)),
        DeallocateStorage(std::move(Deallocate)) {}

  AllocatorRecord(const AllocatorRecord &) = delete;
  AllocatorRecord &operator=(const AllocatorRecord &) = delete;
  AllocatorRecord(AllocatorRecord &&) = delete;
  AllocatorRecord &operator=(AllocatorRecord &&) = delete;
  ~AllocatorRecord() = default;

  [[nodiscard]] std::string_view PolicyIdentity() const noexcept {
    return IdentityText;
  }

  [[nodiscard]] const StorageRequest &Storage() const noexcept {
    return RequestedStorage;
  }

  [[nodiscard]] bool DeclaresAllocation() const noexcept {
    return static_cast<bool>(AllocateStorage);
  }
  [[nodiscard]] bool DeclaresConstruction() const noexcept {
    return static_cast<bool>(ConstructObject);
  }
  [[nodiscard]] bool DeclaresDestruction() const noexcept {
    return static_cast<bool>(DestroyObject);
  }
  [[nodiscard]] bool DeclaresDeallocation() const noexcept {
    return static_cast<bool>(DeallocateStorage);
  }

  [[nodiscard]] void *Allocate() const {
    return AllocateStorage ? AllocateStorage(RequestedStorage) : nullptr;
  }

  [[nodiscard]] AllocatorStepResult Construct(void *Storage) const {
    if (!ConstructObject || Storage == nullptr)
      return AllocatorStepResult::Declined();
    return ConstructObject(Storage);
  }

  [[nodiscard]] AllocatorStepResult Destroy(void *Storage) const {
    if (!DestroyObject || Storage == nullptr)
      return AllocatorStepResult::Declined();
    return DestroyObject(Storage);
  }

  [[nodiscard]] AllocatorStepResult Deallocate(void *Storage) const {
    if (!DeallocateStorage || Storage == nullptr)
      return AllocatorStepResult::Declined();
    return DeallocateStorage(Storage, RequestedStorage);
  }

private:
  std::string IdentityText;
  StorageRequest RequestedStorage;
  AllocateOperation AllocateStorage;
  ConstructOperation ConstructObject;
  DestroyOperation DestroyObject;
  DeallocateOperation DeallocateStorage;
};

struct ClassAllocatorAccess final {
  [[nodiscard]] static std::shared_ptr<const AllocatorRecord>
  Retain(const ClassAllocator &Allocator) noexcept;
  [[nodiscard]] static const AllocatorRecord *
  Observe(const ClassAllocator &Allocator) noexcept;
};

} // namespace Detail

class ClassAllocator final {
public:
  using AllocateOperation = Detail::AllocatorRecord::AllocateOperation;
  using ConstructOperation = Detail::AllocatorRecord::ConstructOperation;
  using DestroyOperation = Detail::AllocatorRecord::DestroyOperation;
  using DeallocateOperation = Detail::AllocatorRecord::DeallocateOperation;

  ClassAllocator() noexcept = default;

  ClassAllocator(const ClassAllocator &) = default;
  ClassAllocator &operator=(const ClassAllocator &) = default;
  ClassAllocator(ClassAllocator &&) noexcept = default;
  ClassAllocator &operator=(ClassAllocator &&) noexcept = default;

  ~ClassAllocator() = default;

  [[nodiscard]] static ClassAllocator Undeclared() noexcept {
    return ClassAllocator();
  }

  [[nodiscard]] static ClassAllocator
  FromOperations(std::string_view PolicyIdentity,
                 const StorageRequest &Requested, AllocateOperation Allocate,
                 ConstructOperation Construct, DestroyOperation Destroy,
                 DeallocateOperation Deallocate) {
    auto Held = std::make_shared<const Detail::AllocatorRecord>(
        std::string(PolicyIdentity), Requested, std::move(Allocate),
        std::move(Construct), std::move(Destroy), std::move(Deallocate));
    return ClassAllocator(std::move(Held));
  }

  template <class Type>
  [[nodiscard]] static ClassAllocator
  ForOwnedObject(std::string_view PolicyIdentity = "Luna.GlobalStorage") {
    static_assert(std::is_destructible_v<Type>,
                  "A Luna-owned object requires a destructible class type.");

    AllocateOperation Allocate = [](const StorageRequest &Wanted) -> void * {
      return ::operator new(Wanted.ByteCount,
                            std::align_val_t{Wanted.Alignment});
    };
    ConstructOperation Construct;
    if constexpr (std::is_default_constructible_v<Type>) {
      Construct = [](void *Storage) {
        new (Storage) Type();
        return AllocatorStepResult::Done();
      };
    }
    DeallocateOperation Deallocate = [](void *Storage,
                                        const StorageRequest &Wanted) {
      ::operator delete(Storage, std::align_val_t{Wanted.Alignment});
      return AllocatorStepResult::Done();
    };
    return FromOperations(PolicyIdentity, StorageRequest::ForClass<Type>(),
                          std::move(Allocate), std::move(Construct),
                          DestructionOf<Type>(), std::move(Deallocate));
  }

  template <class Type>
  [[nodiscard]] static ClassAllocator
  ForAdoptedObject(std::string_view PolicyIdentity = "Luna.AdoptedObject") {
    static_assert(std::is_destructible_v<Type>,
                  "An adopted object requires a destructible class type.");

    return FromOperations(PolicyIdentity, StorageRequest::ForClass<Type>(),
                          AllocateOperation(), ConstructOperation(),
                          DestructionOf<Type>(), DeallocateOperation());
  }

  template <class Type, class AllocateStorage, class DeallocateStorage>
  [[nodiscard]] static ClassAllocator
  ForStorage(std::string_view PolicyIdentity, AllocateStorage Allocate,
             DeallocateStorage Deallocate) {
    static_assert(std::is_destructible_v<Type>,
                  "A consumer storage protocol requires a destructible class "
                  "type.");

    AllocateOperation Allocation =
        [Allocate](const StorageRequest &Wanted) -> void * {
      return Allocate(Wanted);
    };
    ConstructOperation Construct;
    if constexpr (std::is_default_constructible_v<Type>) {
      Construct = [](void *Storage) {
        new (Storage) Type();
        return AllocatorStepResult::Done();
      };
    }
    DeallocateOperation Deallocation =
        [Deallocate](void *Storage, const StorageRequest &Wanted) {
          Deallocate(Storage, Wanted);
          return AllocatorStepResult::Done();
        };
    return FromOperations(PolicyIdentity, StorageRequest::ForClass<Type>(),
                          std::move(Allocation), std::move(Construct),
                          DestructionOf<Type>(), std::move(Deallocation));
  }

  [[nodiscard]] bool IsDeclared() const noexcept { return Record != nullptr; }

  [[nodiscard]] bool DeclaresAllocation() const noexcept {
    return Record && Record->DeclaresAllocation();
  }
  [[nodiscard]] bool DeclaresConstruction() const noexcept {
    return Record && Record->DeclaresConstruction();
  }
  [[nodiscard]] bool DeclaresDestruction() const noexcept {
    return Record && Record->DeclaresDestruction();
  }

  [[nodiscard]] bool OwnsStorage() const noexcept {
    return Record && Record->DeclaresDeallocation();
  }

  [[nodiscard]] StorageRequest Storage() const noexcept {
    return Record ? Record->Storage() : StorageRequest();
  }

  [[nodiscard]] std::string_view PolicyIdentity() const noexcept {
    return Record ? Record->PolicyIdentity() : std::string_view();
  }

  [[nodiscard]] bool RefersToSame(const ClassAllocator &Other) const noexcept {
    return Record == Other.Record;
  }

private:
  friend struct Detail::ClassAllocatorAccess;

  explicit ClassAllocator(
      std::shared_ptr<const Detail::AllocatorRecord> Held) noexcept
      : Record(std::move(Held)) {}

  template <class Type> [[nodiscard]] static DestroyOperation DestructionOf() {
    return [](void *Storage) {
      static_cast<Type *>(Storage)->~Type();
      return AllocatorStepResult::Done();
    };
  }

  std::shared_ptr<const Detail::AllocatorRecord> Record;
};

namespace Detail {

inline std::shared_ptr<const AllocatorRecord>
ClassAllocatorAccess::Retain(const ClassAllocator &Allocator) noexcept {
  return Allocator.Record;
}

inline const AllocatorRecord *
ClassAllocatorAccess::Observe(const ClassAllocator &Allocator) noexcept {
  return Allocator.Record.get();
}

} // namespace Detail

} // namespace Luna
