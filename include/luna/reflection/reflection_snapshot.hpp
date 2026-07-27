#pragma once

// clang-format off
#include <luna/reflection/ids.hpp>
#include <luna/reflection/reflection_record.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
// clang-format on

namespace Luna {

namespace Detail {
class ReflectionStorage;
class ReflectionDatabase;
} // namespace Detail

class ReflectionRecordRange final {
public:
  ReflectionRecordRange() noexcept = default;

  [[nodiscard]] std::size_t Size() const noexcept { return CountValue; }
  [[nodiscard]] bool IsEmpty() const noexcept { return CountValue == 0; }
  [[nodiscard]] ReflectionRecord At(std::size_t Index) const;

private:
  friend class Detail::ReflectionStorage;

  ReflectionRecordRange(
      std::shared_ptr<const Detail::ReflectionStorage> Storage,
      const std::size_t *Indices, std::size_t Count) noexcept;

  std::shared_ptr<const Detail::ReflectionStorage> StorageValue;
  const std::size_t *IndicesValue = nullptr;
  std::size_t CountValue = 0;
};

class TypeRecordRange final {
public:
  TypeRecordRange() noexcept = default;

  [[nodiscard]] std::size_t Size() const noexcept { return CountValue; }
  [[nodiscard]] bool IsEmpty() const noexcept { return CountValue == 0; }
  [[nodiscard]] TypeRecord At(std::size_t Index) const;

private:
  friend class Detail::ReflectionStorage;

  TypeRecordRange(std::shared_ptr<const Detail::ReflectionStorage> Storage,
                  const std::size_t *Indices, std::size_t Count) noexcept;

  std::shared_ptr<const Detail::ReflectionStorage> StorageValue;
  const std::size_t *IndicesValue = nullptr;
  std::size_t CountValue = 0;
};

class ModuleRecordRange final {
public:
  ModuleRecordRange() noexcept = default;

  [[nodiscard]] std::size_t Size() const noexcept { return CountValue; }
  [[nodiscard]] bool IsEmpty() const noexcept { return CountValue == 0; }
  [[nodiscard]] ModuleRecord At(std::size_t Index) const;

private:
  friend class Detail::ReflectionStorage;

  ModuleRecordRange(std::shared_ptr<const Detail::ReflectionStorage> Storage,
                    const std::size_t *Indices, std::size_t Count) noexcept;

  std::shared_ptr<const Detail::ReflectionStorage> StorageValue;
  const std::size_t *IndicesValue = nullptr;
  std::size_t CountValue = 0;
};

class ReflectionSnapshot final {
public:
  ReflectionSnapshot() noexcept = default;

  [[nodiscard]] std::uint64_t Generation() const noexcept;
  [[nodiscard]] std::size_t Size() const noexcept;
  [[nodiscard]] bool IsEmpty() const noexcept;

  [[nodiscard]] ReflectionRecord Find(SymbolId Id) const;
  [[nodiscard]] ReflectionRecord Find(std::string_view QualifiedName) const;

  [[nodiscard]] ReflectionRecordRange Symbols() const;
  [[nodiscard]] ReflectionRecordRange Symbols(ScopeId Scope) const;
  [[nodiscard]] ReflectionRecordRange Symbols(SymbolKind Kind) const;
  [[nodiscard]] TypeRecordRange Types() const;
  [[nodiscard]] ModuleRecordRange Modules() const;
  [[nodiscard]] TypeRecord FindType(TypeId Id) const;

private:
  friend class Detail::ReflectionStorage;
  friend class Detail::ReflectionDatabase;

  explicit ReflectionSnapshot(
      std::shared_ptr<const Detail::ReflectionStorage> Storage) noexcept;

  std::shared_ptr<const Detail::ReflectionStorage> StorageValue;
};

} // namespace Luna
