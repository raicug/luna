#pragma once

// clang-format off
#include <luna/generation/artifact.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/reflection/reflection_record.hpp>
#include <luna/reflection/reflection_snapshot.hpp>
#include <luna/type/type_descriptor.hpp>

#include <cstddef>
#include <string>
#include <string_view>
// clang-format on

namespace Luna::Detail {

[[nodiscard]] GenerationStatus ClassifyGeneratedText(std::string_view Text);

[[nodiscard]] std::string NormalizeLineEndings(std::string_view Text);

class GenerationWriter final {
public:
  static constexpr char LineBreak = '\n';

  void Literal(std::string_view Text);

  void Break();

  bool Inline(std::string_view Value, std::string_view Context);

  bool Block(std::string_view Value, std::string_view Indent,
             std::string_view Context);

  bool Field(std::string_view Name, std::string_view Value,
             std::string_view Context);

  bool Refuse(GenerationStatus Status, std::string_view Context);

  [[nodiscard]] bool IsRejected() const noexcept { return RejectedValue; }

  [[nodiscard]] GenerationStatus Status() const noexcept { return StatusValue; }

  [[nodiscard]] const std::string &Bytes() const noexcept { return BytesValue; }

  [[nodiscard]] GeneratedArtifact Release(std::string_view Artifact);

private:
  void Reject(GenerationStatus Status, std::string_view Context);

  GenerationStatus StatusValue = GenerationStatus::Valid;
  bool RejectedValue = false;
  std::string BytesValue;
  std::string ContextValue;
};

[[nodiscard]] std::string TypeText(const ReflectionSnapshot &Snapshot,
                                   const TypeId &Id);

[[nodiscard]] std::string TypeText(const ReflectionSnapshot &Snapshot,
                                   const TypeId &Id,
                                   const TypeDescriptor &Descriptor);

[[nodiscard]] std::string SymbolText(const ReflectionSnapshot &Snapshot,
                                     const SymbolId &Id);

[[nodiscard]] std::string ModuleKeyText(const ModuleRecord &Module);

} // namespace Luna::Detail
