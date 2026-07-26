#pragma once

// The load-once module registry. This milestone loads modules and never unloads
// or replaces them: re-registering one identity and version succeeds
// idempotently without rerunning callbacks only when the normalized manifest
// and its exported descriptors are identical to the loaded definition, a
// same-version unequal definition or any different version is a conflict, and
// unload or replacement requests return a deterministic unsupported result
// without mutating anything. Every enumeration is canonically ordered.

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

// Deterministic classification of one load request against the loaded set.
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

  // Only a loadable request executes registration callbacks. An idempotent
  // repeat succeeds without rerunning them.
  [[nodiscard]] bool RunsCallbacks() const noexcept {
    return Status == ModuleLoadStatus::Loadable;
  }

  [[nodiscard]] bool IsSuccess() const noexcept {
    return Status == ModuleLoadStatus::Loadable ||
           Status == ModuleLoadStatus::AlreadyLoaded;
  }

  [[nodiscard]] std::string Message() const;
};

// Deterministic classification of one lifecycle request. Both values are
// unsupported in the load-only milestone.
enum class ModuleLifecycleStatus { UnsupportedUnload, UnsupportedReplacement };

[[nodiscard]] std::string_view
ModuleLifecycleStatusText(ModuleLifecycleStatus Status) noexcept;

struct ModuleLifecycleDecision final {
  ModuleLifecycleStatus Status = ModuleLifecycleStatus::UnsupportedUnload;
  std::string Identity;
  std::string Detail;

  // The load-only milestone never supports a lifecycle request, so this is
  // always false and no request ever mutates the registry.
  [[nodiscard]] bool IsSupported() const noexcept { return false; }

  [[nodiscard]] std::string Message() const;
};

class ModuleRegistry final {
public:
  // Classifies one request without mutating the registry.
  [[nodiscard]] ModuleLoadDecision
  ClassifyLoad(const ModuleManifest &Manifest) const;

  // Records one successfully loaded module. Returns false when the request is
  // not loadable, so publication can never overwrite a loaded definition.
  [[nodiscard]] bool Record(ModuleManifest Manifest);

  [[nodiscard]] const ModuleManifest *
  Find(std::string_view Identity) const noexcept;

  [[nodiscard]] bool IsLoaded(std::string_view Identity) const noexcept;

  // Loaded manifests in canonical identity order.
  [[nodiscard]] std::vector<const ModuleManifest *> LoadedModules() const;

  // Loaded versions as resolution pins, in canonical identity order.
  [[nodiscard]] std::vector<ModulePin> Pins() const;

  [[nodiscard]] std::size_t Count() const noexcept { return Modules.size(); }

  // Load-only results. Both are const, so an unsupported request cannot
  // mutate the loaded set even by accident.
  [[nodiscard]] ModuleLifecycleDecision
  RequestUnload(std::string_view Identity) const;

  [[nodiscard]] ModuleLifecycleDecision
  RequestReplacement(const ModuleManifest &Manifest) const;

private:
  std::map<std::string, ModuleManifest, std::less<>> Modules;
};

} // namespace Luna::Detail
