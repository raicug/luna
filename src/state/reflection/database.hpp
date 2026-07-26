#pragma once

// One logical reflection database per State. The database holds exactly one
// committed immutable generation at a time: a candidate generation is prepared
// and validated separately, publication is a single atomic pointer swap, and a
// rejected candidate leaves the committed generation untouched. Because every
// capture copies the shared generation pointer first, a reader can never mix
// records from two generations.

// clang-format off
#include <luna/reflection/reflection_snapshot.hpp>

#include "state/reflection/storage.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

// Mutable staging area for one candidate generation. It is private to Luna and
// never observable through a public snapshot.
class ReflectionGenerationBuilder final {
public:
  ReflectionGenerationBuilder() = default;

  // Seeds the candidate with every record, type, and module of one committed
  // generation so a later transaction can publish an extended generation.
  explicit ReflectionGenerationBuilder(const ReflectionStorage &Committed);

  std::size_t AddModule(ReflectionModuleFields Fields);
  std::size_t AddType(ReflectionTypeFields Fields);
  void AddRecord(ReflectionRecordFields Fields);

  [[nodiscard]] std::size_t RecordCount() const noexcept {
    return Records.size();
  }
  [[nodiscard]] std::size_t TypeCount() const noexcept { return Types.size(); }
  [[nodiscard]] std::size_t ModuleCount() const noexcept {
    return Modules.size();
  }

  // The candidate index of one module identity, whether the module was seeded
  // from the committed generation or added by this candidate. Preparation
  // resolves the module provenance of a declaration through it, so a record
  // never carries an index the candidate does not describe.
  [[nodiscard]] std::optional<std::size_t>
  FindModule(std::string_view Identity) const noexcept;

private:
  friend class ReflectionDatabase;

  std::vector<ReflectionRecordFields> Records;
  std::vector<ReflectionTypeFields> Types;
  std::vector<ReflectionModuleFields> Modules;
};

class ReflectionDatabase final {
public:
  ReflectionDatabase();

  ReflectionDatabase(const ReflectionDatabase &) = delete;
  ReflectionDatabase &operator=(const ReflectionDatabase &) = delete;
  ReflectionDatabase(ReflectionDatabase &&) noexcept = default;
  ReflectionDatabase &operator=(ReflectionDatabase &&) noexcept = default;
  ~ReflectionDatabase() = default;

  // Captures the committed generation exactly once per query.
  [[nodiscard]] std::shared_ptr<const ReflectionStorage> Capture() const;
  [[nodiscard]] ReflectionSnapshot Snapshot() const;

  [[nodiscard]] std::uint64_t Generation() const noexcept;
  [[nodiscard]] std::size_t Count() const noexcept;

  [[nodiscard]] ReflectionGenerationBuilder BeginGeneration() const;

  // Validates and materializes a candidate generation without publishing it.
  [[nodiscard]] ReflectionGenerationStatus
  Prepare(const ReflectionGenerationBuilder &Candidate,
          std::shared_ptr<const ReflectionStorage> &Prepared) const;

  // Publication is the only visibility point of a prepared generation.
  bool Publish(std::shared_ptr<const ReflectionStorage> Prepared) noexcept;

  [[nodiscard]] ReflectionGenerationStatus
  PublishGeneration(const ReflectionGenerationBuilder &Candidate);

private:
  std::shared_ptr<const ReflectionStorage> CurrentValue;
};

} // namespace Luna::Detail
