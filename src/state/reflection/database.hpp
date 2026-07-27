#pragma once

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

class ReflectionGenerationBuilder final {
public:
  ReflectionGenerationBuilder() = default;

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

  [[nodiscard]] std::shared_ptr<const ReflectionStorage> Capture() const;
  [[nodiscard]] ReflectionSnapshot Snapshot() const;

  [[nodiscard]] std::uint64_t Generation() const noexcept;
  [[nodiscard]] std::size_t Count() const noexcept;

  [[nodiscard]] ReflectionGenerationBuilder BeginGeneration() const;

  [[nodiscard]] ReflectionGenerationStatus
  Prepare(const ReflectionGenerationBuilder &Candidate,
          std::shared_ptr<const ReflectionStorage> &Prepared) const;

  bool Publish(std::shared_ptr<const ReflectionStorage> Prepared) noexcept;

  [[nodiscard]] ReflectionGenerationStatus
  PublishGeneration(const ReflectionGenerationBuilder &Candidate);

private:
  std::shared_ptr<const ReflectionStorage> CurrentValue;
};

} // namespace Luna::Detail
