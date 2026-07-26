// clang-format off
#include "state/userdata/access.hpp"

#include "state/userdata/class_relationships.hpp"
#include "state/userdata/header.hpp"
#include "state/userdata/identity.hpp"

#include <cstddef>
#include <string_view>
// clang-format on

namespace Luna::Detail {
namespace {

// The lifetime half of the gate, in the documented order: a null payload, then
// the explicit handle a borrowed value requires, then the handle's generation,
// then the release state itself.
[[nodiscard]] UserdataAccessFailure
CheckLifetime(const UserdataHeader &Header,
              const UserdataAccessRequest &Request) noexcept {
  if (!Header.Payload.HasStorage())
    return UserdataAccessFailure::NullPayload;
  if (!Header.HasRequiredLifetimeHandle())
    return UserdataAccessFailure::MissingLifetimeHandle;

  // A handle whose generation moved on was invalidated, so every access taken
  // against the recorded generation is expired.
  if (Header.Handle.IsDeclared() && Request.HandleProbe != nullptr &&
      Request.HandleProbe(Header.Handle.Record) != Header.Handle.Generation)
    return UserdataAccessFailure::ExpiredLifetimeHandle;

  switch (Header.Lifetime) {
  case LifetimeState::Published:
    return UserdataAccessFailure::None;
  case LifetimeState::Allocated:
  case LifetimeState::Constructed:
    return UserdataAccessFailure::Unpublished;
  case LifetimeState::Invalid:
    return UserdataAccessFailure::Invalidated;
  case LifetimeState::Destroyed:
    return UserdataAccessFailure::Destroyed;
  case LifetimeState::SharedReleased:
  case LifetimeState::Released:
    return UserdataAccessFailure::Released;
  }
  return UserdataAccessFailure::Invalidated;
}

// How the value reaches the requested view: its own dynamic type, one
// registered accessible base path from it, one registered safe downcast policy
// to it, or the declared view it was exposed as. A view Luna never recorded a
// path for is a mismatch rather than a guess.
[[nodiscard]] ClassConversion
ResolveRequestedType(const UserdataHeader &Header,
                     const UserdataAccessRequest &Request) noexcept {
  ClassConversion Resolved;
  if (Header.DynamicType == Request.RequestedType) {
    Resolved.Kind = ClassConversionKind::Identity;
    return Resolved;
  }
  if (Request.Relationships != nullptr) {
    Resolved = Request.Relationships->Resolve(Header.DynamicType,
                                              Request.RequestedType);
    if (Resolved.IsViable())
      return Resolved;
  }
  if (Header.DeclaredViewType == Request.RequestedType)
    Resolved.Kind = ClassConversionKind::Identity;
  return Resolved;
}

// The value carries the metatable identity of the requested class, or the
// identity the relationship graph records for the class the value actually is.
// Metatable equality alone is never proof of type, so the type question is
// still asked separately.
[[nodiscard]] bool
CarriesKnownMetatable(const UserdataHeader &Header,
                      const UserdataAccessRequest &Request) noexcept {
  if (Header.CarriesMetatable(Request.Metatable))
    return true;
  if (Request.Relationships == nullptr)
    return false;
  const MetatableId Declared =
      Request.Relationships->MetatableOf(Header.DynamicType);
  return Declared.IsValid() && Header.CarriesMetatable(Declared) &&
         Request.Relationships->Contains(Request.RequestedType);
}

[[nodiscard]] UserdataAccessResult Refuse(UserdataAccessFailure Failure,
                                          const UserdataHeader *Header) {
  UserdataAccessResult Result;
  Result.Failure = Failure;
  Result.Header = Header;
  return Result;
}

} // namespace

std::string_view
UserdataAccessFailureText(UserdataAccessFailure Failure) noexcept {
  switch (Failure) {
  case UserdataAccessFailure::None:
    return "none";
  case UserdataAccessFailure::UnavailableRequest:
    return "unavailable_request";
  case UserdataAccessFailure::MissingValue:
    return "missing_value";
  case UserdataAccessFailure::ForeignLayout:
    return "foreign_layout";
  case UserdataAccessFailure::ForeignState:
    return "foreign_state";
  case UserdataAccessFailure::MetatableMismatch:
    return "metatable_mismatch";
  case UserdataAccessFailure::NullPayload:
    return "null_payload";
  case UserdataAccessFailure::MissingLifetimeHandle:
    return "missing_lifetime_handle";
  case UserdataAccessFailure::ExpiredLifetimeHandle:
    return "expired_lifetime_handle";
  case UserdataAccessFailure::Unpublished:
    return "unpublished";
  case UserdataAccessFailure::Invalidated:
    return "invalidated";
  case UserdataAccessFailure::Destroyed:
    return "destroyed";
  case UserdataAccessFailure::Released:
    return "released";
  case UserdataAccessFailure::TypeMismatch:
    return "type_mismatch";
  case UserdataAccessFailure::IncompatibleObject:
    return "incompatible_object";
  case UserdataAccessFailure::ConstViolation:
    return "const_violation";
  }
  return "unavailable_request";
}

UserdataAccessResult
ValidateUserdataAccess(const UserdataHeader &Header,
                       const UserdataAccessRequest &Request) noexcept {
  if (!Request.IsComplete())
    return Refuse(UserdataAccessFailure::UnavailableRequest, nullptr);

  // The layout question is asked before any other field of the block is
  // trusted.
  if (!Header.HasCanonicalLayout())
    return Refuse(UserdataAccessFailure::ForeignLayout, nullptr);
  if (!Header.BelongsTo(Request.Origin))
    return Refuse(UserdataAccessFailure::ForeignState, &Header);

  // A value that names no complete class identity cannot carry the requested
  // metatable, which is exactly what the metatable check reports.
  if (!Header.IdentifiesClass() || !CarriesKnownMetatable(Header, Request))
    return Refuse(UserdataAccessFailure::MetatableMismatch, &Header);

  const UserdataAccessFailure Lifetime = CheckLifetime(Header, Request);
  if (Lifetime != UserdataAccessFailure::None)
    return Refuse(Lifetime, &Header);

  const ClassConversion Resolved = ResolveRequestedType(Header, Request);
  if (!Resolved.IsViable())
    return Refuse(UserdataAccessFailure::TypeMismatch, &Header);

  // A downcast decides compatibility before anything is adjusted, converted, or
  // invoked, and the decision never mutates the object.
  if (ProbeClassConversion(Resolved, Header.Payload.Storage) == nullptr)
    return Refuse(UserdataAccessFailure::IncompatibleObject, &Header);

  void *const Adjusted = ApplyClassConversion(Resolved, Header.Payload.Storage);
  if (Adjusted == nullptr)
    return Refuse(UserdataAccessFailure::IncompatibleObject, &Header);
  if (Request.RequiresMutation && !Header.PermitsMutation())
    return Refuse(UserdataAccessFailure::ConstViolation, &Header);

  UserdataAccessResult Result;
  Result.Failure = UserdataAccessFailure::None;
  Result.Header = &Header;
  Result.Storage = Adjusted;
  Result.Conversion = Resolved.Kind;
  Result.PermitsMutation = Header.PermitsMutation();
  return Result;
}

UserdataAccessResult
InspectUserdataAccess(const void *Block, std::size_t ByteCount,
                      const UserdataAccessRequest &Request) noexcept {
  if (!Request.IsComplete())
    return Refuse(UserdataAccessFailure::UnavailableRequest, nullptr);
  if (Block == nullptr)
    return Refuse(UserdataAccessFailure::MissingValue, nullptr);

  const UserdataHeader *Header = InspectUserdataHeader(Block, ByteCount);
  if (Header == nullptr)
    return Refuse(UserdataAccessFailure::ForeignLayout, nullptr);
  return ValidateUserdataAccess(*Header, Request);
}

} // namespace Luna::Detail
