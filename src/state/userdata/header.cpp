// clang-format off
#include "state/userdata/header.hpp"

#include "state/userdata/identity.hpp"

#include <cstddef>
// clang-format on

namespace Luna::Detail {

UserdataHeader
MakeUserdataHeader(const UserdataHeaderRequest &Request) noexcept {
  UserdataHeader Header;
  Header.Magic = UserdataMagic;
  Header.LayoutVersion = UserdataLayoutVersion;
  Header.Ownership = Request.Ownership;

  Header.Lifetime = LifetimeState::Allocated;
  Header.Access = Request.Access;
  Header.Origin = Request.Origin;
  Header.DynamicType = Request.DynamicType;

  Header.DeclaredViewType = Request.DeclaredViewType.IsValid()
                                ? Request.DeclaredViewType
                                : Request.DynamicType;
  Header.ClassSymbol = Request.ClassSymbol;
  Header.Metatable = Request.Metatable;
  Header.DispatchGeneration = Request.DispatchGeneration;
  return Header;
}

const UserdataHeader *InspectUserdataHeader(const void *Block,
                                            std::size_t ByteCount) noexcept {
  if (Block == nullptr || ByteCount < sizeof(UserdataHeader))
    return nullptr;

  const auto *Candidate = static_cast<const UserdataHeader *>(Block);
  if (!Candidate->HasCanonicalLayout())
    return nullptr;
  return Candidate;
}

} // namespace Luna::Detail
