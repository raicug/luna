#pragma once

// Collision-safe identity resolution. An identity is the version-pinned digest
// of a complete canonical descriptor, so equal descriptors always resolve to
// the same identity across States, executions, insertion orders, and container
// permutations. Resolution compares the complete stored descriptor after every
// identity match: unequal descriptors sharing one identity are rejected with a
// deterministic Internal diagnostic and never receive an order-dependent
// fallback identity.

// clang-format off
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/identity/digest.hpp"
#include "state/identity/symbol_descriptor.hpp"

#include <cstddef>
#include <map>
#include <optional>
// clang-format on

namespace Luna::Detail {

// Private hook that forces the next resolutions to a chosen identity. It exists
// so collision handling is exercised deterministically without waiting for a
// real 256-bit digest collision, and it is never reachable from the public API.
class IdentityCollisionInjector final {
public:
  // Reserved identity used when no explicit identity is supplied.
  [[nodiscard]] static CanonicalDigest::Storage SharedIdentityBytes() noexcept;

  void Inject(std::size_t Count = 1) noexcept;
  void InjectIdentity(CanonicalDigest::Storage Bytes,
                      std::size_t Count = 1) noexcept;
  void Clear() noexcept;

  [[nodiscard]] std::optional<CanonicalDigest::Storage> Consume() noexcept;
  [[nodiscard]] std::size_t Pending() const noexcept { return PendingCount; }

private:
  CanonicalDigest::Storage ForcedBytes = SharedIdentityBytes();
  std::size_t PendingCount = 0;
};

template <class Identity> struct IdentityResolution final {
  std::optional<Identity> Value;
  std::optional<ErrorDiagnostic> Diagnostic;

  [[nodiscard]] bool IsSuccess() const noexcept { return Value.has_value(); }
};

using TypeIdentityResolution = IdentityResolution<TypeId>;
using SymbolIdentityResolution = IdentityResolution<SymbolId>;

class TypeIdentityRegistry final {
public:
  // Digest of one complete canonical descriptor, with no registry state
  // involved.
  [[nodiscard]] static std::optional<TypeId>
  ComputeIdentity(const TypeDescriptor &Descriptor);

  [[nodiscard]] TypeIdentityResolution
  Resolve(const TypeDescriptor &Descriptor);

  [[nodiscard]] const TypeDescriptor *Find(TypeId Identity) const noexcept;

  // Post-match verification: an identity match alone is never treated as proof
  // of equal canonical identity.
  [[nodiscard]] bool Matches(TypeId Identity,
                             const TypeDescriptor &Descriptor) const;

  [[nodiscard]] std::size_t Size() const noexcept {
    return DescriptorsByIdentity.size();
  }

  [[nodiscard]] IdentityCollisionInjector &CollisionInjection() noexcept {
    return Injector;
  }

private:
  std::map<TypeId, TypeDescriptor> DescriptorsByIdentity;
  IdentityCollisionInjector Injector;
};

class SymbolIdentityRegistry final {
public:
  [[nodiscard]] static std::optional<SymbolId>
  ComputeIdentity(const SymbolDescriptor &Descriptor);

  [[nodiscard]] SymbolIdentityResolution
  Resolve(const SymbolDescriptor &Descriptor);

  [[nodiscard]] const SymbolDescriptor *Find(SymbolId Identity) const noexcept;

  [[nodiscard]] bool Matches(SymbolId Identity,
                             const SymbolDescriptor &Descriptor) const;

  [[nodiscard]] std::size_t Size() const noexcept {
    return DescriptorsByIdentity.size();
  }

  [[nodiscard]] IdentityCollisionInjector &CollisionInjection() noexcept {
    return Injector;
  }

private:
  std::map<SymbolId, SymbolDescriptor> DescriptorsByIdentity;
  IdentityCollisionInjector Injector;
};

} // namespace Luna::Detail
