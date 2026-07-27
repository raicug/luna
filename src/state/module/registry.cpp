// clang-format off
#include "state/module/registry.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace Luna::Detail {

std::string_view ModuleLoadStatusText(ModuleLoadStatus Status) noexcept {
  switch (Status) {
  case ModuleLoadStatus::Loadable:
    return "loadable";
  case ModuleLoadStatus::AlreadyLoaded:
    return "already-loaded";
  case ModuleLoadStatus::InvalidManifest:
    return "invalid-manifest";
  case ModuleLoadStatus::ConflictingDefinition:
    return "conflicting-definition";
  case ModuleLoadStatus::ConflictingVersion:
    return "conflicting-version";
  }
  return "invalid";
}

std::string ModuleLoadDecision::Message() const {
  std::string Text("module load ");
  Text.append(ModuleLoadStatusText(Status));
  if (!Identity.empty()) {
    Text.append(" for '");
    Text.append(Identity);
    Text.push_back('\'');
  }
  if (!Detail.empty()) {
    Text.append(": ");
    Text.append(Detail);
  }
  return Text;
}

std::string_view
ModuleLifecycleStatusText(ModuleLifecycleStatus Status) noexcept {
  switch (Status) {
  case ModuleLifecycleStatus::UnsupportedUnload:
    return "unsupported-unload";
  case ModuleLifecycleStatus::UnsupportedReplacement:
    return "unsupported-replacement";
  }
  return "invalid";
}

std::string ModuleLifecycleDecision::Message() const {
  std::string Text("module lifecycle ");
  Text.append(ModuleLifecycleStatusText(Status));
  if (!Identity.empty()) {
    Text.append(" for '");
    Text.append(Identity);
    Text.push_back('\'');
  }
  if (!Detail.empty()) {
    Text.append(": ");
    Text.append(Detail);
  }
  return Text;
}

ModuleLoadDecision
ModuleRegistry::ClassifyLoad(const ModuleManifest &Manifest) const {
  ModuleLoadDecision Decision;
  Decision.Identity = Manifest.Identity();

  if (!Manifest.IsValid()) {
    Decision.Status = ModuleLoadStatus::InvalidManifest;
    Decision.Detail = std::string("the manifest is ")
                          .append(ModuleManifestStatusText(Manifest.Status()));
    return Decision;
  }

  const auto Loaded = Modules.find(Manifest.Identity());
  if (Loaded == Modules.end()) {
    Decision.Status = ModuleLoadStatus::Loadable;
    return Decision;
  }

  const ModuleManifest &Existing = Loaded->second;
  if (!Existing.Version().HasSamePrecedence(Manifest.Version())) {
    Decision.Status = ModuleLoadStatus::ConflictingVersion;
    Decision.Detail = std::string("version ")
                          .append(Existing.Version().ToString())
                          .append(" is already loaded and replacement is not "
                                  "supported in the load-only milestone");
    return Decision;
  }

  if (Existing == Manifest) {
    Decision.Status = ModuleLoadStatus::AlreadyLoaded;
    Decision.Detail = "the normalized definition is identical, so the "
                      "registration callbacks are not rerun";
    return Decision;
  }

  Decision.Status = ModuleLoadStatus::ConflictingDefinition;
  Decision.Detail =
      std::string("version ")
          .append(Existing.Version().ToString())
          .append(" is loaded with a different normalized definition");
  return Decision;
}

bool ModuleRegistry::Record(ModuleManifest Manifest) {
  const ModuleLoadDecision Decision = ClassifyLoad(Manifest);
  if (Decision.Status != ModuleLoadStatus::Loadable)
    return false;
  std::string Identity = Manifest.Identity();
  Modules.insert_or_assign(std::move(Identity), std::move(Manifest));
  return true;
}

const ModuleManifest *
ModuleRegistry::Find(std::string_view Identity) const noexcept {
  const auto Loaded = Modules.find(Identity);
  if (Loaded == Modules.end())
    return nullptr;
  return &Loaded->second;
}

bool ModuleRegistry::IsLoaded(std::string_view Identity) const noexcept {
  return Modules.find(Identity) != Modules.end();
}

std::vector<const ModuleManifest *> ModuleRegistry::LoadedModules() const {
  std::vector<const ModuleManifest *> Loaded;
  Loaded.reserve(Modules.size());
  for (const auto &[Identity, Manifest] : Modules)
    Loaded.push_back(&Manifest);
  return Loaded;
}

std::vector<ModulePin> ModuleRegistry::Pins() const {
  std::vector<ModulePin> Pinned;
  Pinned.reserve(Modules.size());
  for (const auto &[Identity, Manifest] : Modules) {
    ModulePin Pin;
    Pin.Identity = Identity;
    Pin.Version = Manifest.Version();
    Pinned.push_back(std::move(Pin));
  }
  return Pinned;
}

ModuleLifecycleDecision
ModuleRegistry::RequestUnload(std::string_view Identity) const {
  ModuleLifecycleDecision Decision;
  Decision.Status = ModuleLifecycleStatus::UnsupportedUnload;
  Decision.Identity = std::string(Identity);
  Decision.Detail = "this milestone is load-only, so unloading is not "
                    "supported and nothing was changed";
  return Decision;
}

ModuleLifecycleDecision
ModuleRegistry::RequestReplacement(const ModuleManifest &Manifest) const {
  ModuleLifecycleDecision Decision;
  Decision.Status = ModuleLifecycleStatus::UnsupportedReplacement;
  Decision.Identity = Manifest.Identity();
  Decision.Detail = "this milestone is load-only, so replacement is not "
                    "supported and nothing was changed";
  return Decision;
}

} // namespace Luna::Detail
