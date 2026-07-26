#pragma once

// The semantic storage protocol of one registered class.
//
// Luna creates native objects on behalf of Luau, so it has to know four things
// and nothing more: how to obtain suitably aligned storage, how to construct
// one object in it, how to destroy an object it knows is constructed, and how
// to give the storage back. Those four steps are this protocol, and they are
// semantic: none of them names a virtual machine, a Luau type, a Luau pointer,
// a stack index, or a registry reference. An allocator is a statement about
// memory, not about scripting.
//
// The protocol prescribes no member names. A consumer supplies whichever of the
// four steps it wants to own as an ordinary callable - a lambda over an arena,
// a function, a functor - and Luna erases every one of them, together with
// whatever state the callable captured, into one immutable record. That record
// is reference counted and Luna retains it until the last userdata that depends
// on it has finished its cleanup, which is why an arena a value was allocated
// from is still reachable while that value is being destroyed.
//
// The steps a protocol declares decide the cleanup a value can receive, and
// nothing else does:
//
//   * A protocol with no deallocation step means Luna does not own the storage,
//     so it never deallocates it.
//   * A protocol with no destruction step means Luna never destroys the object,
//     which is exactly what a borrowed object requires.
//   * A protocol with no allocation step cannot create an object at all; it can
//     only describe one that already exists.
//
// Luna performs each declared step at most once per value, in one fixed order,
// and never destroys storage nothing was constructed in. A step that declines
// reports it, and a step that throws is contained at Luna's boundary rather
// than escaping through a garbage collector or a State destructor.

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

// How much suitably aligned storage one object of one class needs. It is
// declared where the class type is still complete and carried through the
// backend, which never sees that type.
struct StorageRequest final {
  std::size_t ByteCount = 0;
  std::size_t Alignment = 0;

  // The request names a storage size and one power-of-two alignment.
  [[nodiscard]] constexpr bool IsUsable() const noexcept {
    return ByteCount != 0 && Alignment != 0 &&
           (Alignment & (Alignment - 1)) == 0;
  }

  // The storage one complete class type needs.
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

// What one semantic step of the protocol reports. A step that declined to do
// its work says so instead of leaving Luna to guess; throwing means the same
// thing and is contained.
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

// The immutable erased protocol of one class's storage, together with whatever
// state its operations captured. Luna retains it through the final cleanup step
// of the last value that depends on it, so nothing a destructor or a
// deallocation needs can be gone by the time it runs.
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

  // The reflected identity of this policy. It is a consumer-supplied name, not
  // an address, so it is safe to reflect and to persist.
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

  // The four semantic steps, each run exactly as the consumer supplied it.
  // Whatever they throw is contained by the Luna boundary that calls them, and
  // a step this protocol never declared is never called at all.
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

// The private bridge Luna's construction and release paths use to retain one
// protocol and to name the immutable record behind it. A consumer never names
// it.
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

  // A default-constructed allocator names no protocol at all: it is what an
  // object Luna neither creates nor releases is exposed with.
  ClassAllocator() noexcept = default;

  ClassAllocator(const ClassAllocator &) = default;
  ClassAllocator &operator=(const ClassAllocator &) = default;
  ClassAllocator(ClassAllocator &&) noexcept = default;
  ClassAllocator &operator=(ClassAllocator &&) noexcept = default;

  // Destroying the last copy of an allocator does not end any object created
  // through it: Luna retains the protocol for as long as one value still needs
  // it.
  ~ClassAllocator() = default;

  [[nodiscard]] static ClassAllocator Undeclared() noexcept {
    return ClassAllocator();
  }

  // One protocol assembled from exactly the steps the consumer supplies. An
  // absent step is a statement in its own right: Luna never performs it.
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

  // The ordinary protocol of one complete class type: suitably aligned storage
  // from the global allocator, default construction in place, ordinary
  // destruction, and deallocation of exactly that storage.
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

  // The protocol of one object Luna did not allocate and will not deallocate,
  // but does destroy exactly once.
  template <class Type>
  [[nodiscard]] static ClassAllocator
  ForAdoptedObject(std::string_view PolicyIdentity = "Luna.AdoptedObject") {
    static_assert(std::is_destructible_v<Type>,
                  "An adopted object requires a destructible class type.");

    return FromOperations(PolicyIdentity, StorageRequest::ForClass<Type>(),
                          AllocateOperation(), ConstructOperation(),
                          DestructionOf<Type>(), DeallocateOperation());
  }

  // One class's storage supplied by the consumer, with Luna keeping the
  // construction and destruction of `Type` because only the consumer's
  // translation unit knows that type. The two operations are ordinary callables
  // and may capture whatever state they need; Luna retains that state with the
  // protocol.
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

  // Whether this value names a protocol at all.
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

  // Whether Luna owns the storage of a value created through this protocol, and
  // therefore deallocates it exactly once. Owning storage means declaring the
  // step that gives it back.
  [[nodiscard]] bool OwnsStorage() const noexcept {
    return Record && Record->DeclaresDeallocation();
  }

  [[nodiscard]] StorageRequest Storage() const noexcept {
    return Record ? Record->Storage() : StorageRequest();
  }

  [[nodiscard]] std::string_view PolicyIdentity() const noexcept {
    return Record ? Record->PolicyIdentity() : std::string_view();
  }

  // Whether two values name exactly the same protocol.
  [[nodiscard]] bool RefersToSame(const ClassAllocator &Other) const noexcept {
    return Record == Other.Record;
  }

private:
  friend struct Detail::ClassAllocatorAccess;

  explicit ClassAllocator(
      std::shared_ptr<const Detail::AllocatorRecord> Held) noexcept
      : Record(std::move(Held)) {}

  // The destruction step of one complete class type. It destroys an object the
  // caller already knows is constructed, which is the only object Luna ever
  // asks it about.
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
