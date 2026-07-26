// clang-format off
#include "state/reflection/database.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
// clang-format on

namespace Luna::Detail {

ReflectionGenerationBuilder::ReflectionGenerationBuilder(
    const ReflectionStorage &Committed) {
  Records.reserve(Committed.RecordCount());
  for (std::size_t Index = 0; Index < Committed.RecordCount(); ++Index)
    Records.push_back(*Committed.RecordAt(Index));
  Types.reserve(Committed.TypeCount());
  for (std::size_t Index = 0; Index < Committed.TypeCount(); ++Index)
    Types.push_back(*Committed.TypeAt(Index));
  Modules.reserve(Committed.ModuleCount());
  for (std::size_t Index = 0; Index < Committed.ModuleCount(); ++Index)
    Modules.push_back(*Committed.ModuleAt(Index));
}

std::size_t
ReflectionGenerationBuilder::AddModule(ReflectionModuleFields Fields) {
  Modules.push_back(std::move(Fields));
  return Modules.size() - 1;
}

std::size_t ReflectionGenerationBuilder::AddType(ReflectionTypeFields Fields) {
  Types.push_back(std::move(Fields));
  return Types.size() - 1;
}

void ReflectionGenerationBuilder::AddRecord(ReflectionRecordFields Fields) {
  Records.push_back(std::move(Fields));
}

std::optional<std::size_t> ReflectionGenerationBuilder::FindModule(
    std::string_view Identity) const noexcept {
  for (std::size_t Index = 0; Index < Modules.size(); ++Index) {
    if (Modules[Index].Identity == Identity)
      return Index;
  }
  return std::nullopt;
}

ReflectionDatabase::ReflectionDatabase()
    : CurrentValue(ReflectionStorage::Empty()) {}

std::shared_ptr<const ReflectionStorage> ReflectionDatabase::Capture() const {
  return CurrentValue ? CurrentValue : ReflectionStorage::Empty();
}

ReflectionSnapshot ReflectionDatabase::Snapshot() const {
  return ReflectionStorage::MakeSnapshot(Capture());
}

std::uint64_t ReflectionDatabase::Generation() const noexcept {
  return CurrentValue ? CurrentValue->Generation() : 0;
}

std::size_t ReflectionDatabase::Count() const noexcept {
  return CurrentValue ? CurrentValue->RecordCount() : 0;
}

ReflectionGenerationBuilder ReflectionDatabase::BeginGeneration() const {
  return ReflectionGenerationBuilder(*Capture());
}

ReflectionGenerationStatus ReflectionDatabase::Prepare(
    const ReflectionGenerationBuilder &Candidate,
    std::shared_ptr<const ReflectionStorage> &Prepared) const {
  ReflectionGenerationStatus Status = ReflectionGenerationStatus::Valid;
  auto Storage =
      ReflectionStorage::Build(Generation() + 1, Candidate.Records,
                               Candidate.Types, Candidate.Modules, Status);
  if (!Storage) {
    Prepared.reset();
    return Status;
  }
  Prepared = std::move(Storage);
  return ReflectionGenerationStatus::Valid;
}

bool ReflectionDatabase::Publish(
    std::shared_ptr<const ReflectionStorage> Prepared) noexcept {
  if (!Prepared)
    return false;
  CurrentValue = std::move(Prepared);
  return true;
}

ReflectionGenerationStatus ReflectionDatabase::PublishGeneration(
    const ReflectionGenerationBuilder &Candidate) {
  std::shared_ptr<const ReflectionStorage> Prepared;
  const ReflectionGenerationStatus Status = Prepare(Candidate, Prepared);
  if (Status != ReflectionGenerationStatus::Valid)
    return Status;
  static_cast<void>(Publish(std::move(Prepared)));
  return ReflectionGenerationStatus::Valid;
}

} // namespace Luna::Detail
