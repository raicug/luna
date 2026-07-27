#pragma once

// clang-format off
#include <luna/module/module_manifest.hpp>

#include "state/module/resolution.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

enum class ModuleLoadStatus {
  Loadable,
  AlreadyLoaded,
  InvalidManifest,
  ConflictingDefinition,
  ConflictingVersion
};

[[nodiscard]] std::string_view
ModuleLoadStatusText(ModuleLoadStatus Status) noexcept;

struct ModuleLoadDecision final {
  ModuleLoadStatus Status = ModuleLoadStatus::Loadable;
  std::string Identity;
  std::string Detail;

  [[nodiscard]] bool RunsCallbacks() const noexcept {
    return Status == ModuleLoadStatus::Loadable;
  }

  [[nodiscard]] bool IsSuccess() const noexcept {
    return Status == ModuleLoadStatus::Loadable ||
           Status == ModuleLoadStatus::AlreadyLoaded;
  }

  [[nodiscard]] std::string Message() const;
};

enum class ModuleLifecycleStatus { UnsupportedUnload, UnsupportedReplacement };

[[nodiscard]] std::string_view
ModuleLifecycleStatusText(ModuleLifecycleStatus Status) noexcept;

struct ModuleLifecycleDecision final {
  ModuleLifecycleStatus Status = ModuleLifecycleStatus::UnsupportedUnload;
  std::string Identity;
  std::string Detail;

  [[nodiscard]] bool IsSupported() const noexcept { return false; }

  [[nodiscard]] std::string Message() const;
};

class ModuleRegistry final {
public:
  [[nodiscard]] ModuleLoadDecision
  ClassifyLoad(const ModuleManifest &Manifest) const;

  [[nodiscard]] bool Record(ModuleManifest Manifest);

  [[nodiscard]] bool Publish(const std::vector<ModuleManifest> &Graph) noexcept;

  [[nodiscard]] const ModuleManifest *
  Find(std::string_view Identity) const noexcept;

  [[nodiscard]] bool IsLoaded(std::string_view Identity) const noexcept;

  [[nodiscard]] std::vector<const ModuleManifest *> LoadedModules() const;

  [[nodiscard]] std::vector<ModulePin> Pins() const;

  [[nodiscard]] std::size_t Count() const noexcept { return Modules.size(); }

  [[nodiscard]] ModuleLifecycleDecision
  RequestUnload(std::string_view Identity) const;

  [[nodiscard]] ModuleLifecycleDecision
  RequestReplacement(const ModuleManifest &Manifest) const;

private:
  std::map<std::string, ModuleManifest, std::less<>> Modules;
};

} // namespace Luna::Detail
