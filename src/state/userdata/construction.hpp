#pragma once

// clang-format off
#include <luna/binding/class_construction.hpp>
#include <luna/type/stable_type_key.hpp>

#include <string>
// clang-format on

struct lua_State;

namespace Luna::Detail {

class TypeGeneration;

enum class InstancePublicationStatus {
  Published,

  MissingObject,

  UnavailableClass,

  UnavailableContext,

  RefusedExposure
};

struct InstancePublication final {
  InstancePublicationStatus Status = InstancePublicationStatus::MissingObject;
  int PublishedCount = 0;

  void *Storage = nullptr;

  bool EstablishedOwner = false;

  std::string Diagnostic;

  [[nodiscard]] bool IsSuccess() const noexcept {
    return Status == InstancePublicationStatus::Published;
  }
};

[[nodiscard]] InstancePublication
PublishConstructedInstance(lua_State *State, const TypeGeneration &Types,
                           const StableTypeKey &Class,
                           const ConstructedInstance &Produced) noexcept;

[[nodiscard]] bool ReleasePublishedInstance(lua_State *State,
                                            void *Storage) noexcept;

} // namespace Luna::Detail
