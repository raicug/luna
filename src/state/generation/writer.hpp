#pragma once

// Canonical artifact writer shared by every snapshot generator. The writer owns
// one unpublished byte buffer, accepts only well-formed UTF-8 without a
// byte-order mark, normalizes every line ending to LF, and escapes control
// bytes so one metadata value can never break the structure of the artifact.
// It reads nothing but the text handed to it, so it never touches a State, a
// virtual machine, or a native object.

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

// Deterministic acceptance of one metadata fragment: well-formed UTF-8 that
// carries no byte-order mark.
[[nodiscard]] GenerationStatus ClassifyGeneratedText(std::string_view Text);

// Canonical LF form of one text: a CRLF pair and a lone CR both become one LF,
// so output never depends on the host platform or on how the metadata was
// authored.
[[nodiscard]] std::string NormalizeLineEndings(std::string_view Text);

class GenerationWriter final {
public:
  static constexpr char LineBreak = '\n';

  // Appends generator-owned canonical text. Structure literals are written by
  // the generator itself, never by a consumer, so they need no validation.
  void Literal(std::string_view Text);

  void Break();

  // Appends one metadata value as exactly one line's worth of text: line
  // endings and control bytes are escaped so the value cannot introduce a
  // structural break. Returns false and records the first deterministic
  // rejection when the value is not canonical UTF-8.
  bool Inline(std::string_view Value, std::string_view Context);

  // Appends one metadata value as a block of LF-separated lines, each prefixed
  // by `Indent`. Control bytes other than the line breaks are escaped.
  bool Block(std::string_view Value, std::string_view Indent,
             std::string_view Context);

  // Appends `Name: Value` as one line.
  bool Field(std::string_view Name, std::string_view Value,
             std::string_view Context);

  // Records one deterministic rejection the generator itself decided, such as
  // metadata the target artifact language cannot declare. The first rejection
  // latches and discards the unpublished buffer, so a later one never changes
  // the reported reason and no partial bytes survive. Always returns false, so
  // a mapping step can `return Writer.Refuse(...)`.
  bool Refuse(GenerationStatus Status, std::string_view Context);

  [[nodiscard]] bool IsRejected() const noexcept { return RejectedValue; }

  [[nodiscard]] GenerationStatus Status() const noexcept { return StatusValue; }

  [[nodiscard]] const std::string &Bytes() const noexcept { return BytesValue; }

  // Validates the accumulated buffer once more and publishes it, or returns the
  // first deterministic rejection. A rejected artifact exposes no bytes.
  [[nodiscard]] GeneratedArtifact Release(std::string_view Artifact);

private:
  void Reject(GenerationStatus Status, std::string_view Context);

  GenerationStatus StatusValue = GenerationStatus::Valid;
  bool RejectedValue = false;
  std::string BytesValue;
  std::string ContextValue;
};

// Canonical display text of one referenced type: the canonical name the
// captured generation published for it, or its stable identity when the
// generation carries no record of that type.
[[nodiscard]] std::string TypeText(const ReflectionSnapshot &Snapshot,
                                   const TypeId &Id);

// The same text, with the canonical name of the descriptor the record captured
// as the fallback. A fixed or structural type is named by its canonical model
// even when the generation publishes a record only for declared types.
[[nodiscard]] std::string TypeText(const ReflectionSnapshot &Snapshot,
                                   const TypeId &Id,
                                   const TypeDescriptor &Descriptor);

// Canonical display text of one referenced symbol: its qualified name, or its
// stable identity when the captured generation carries no record of it.
[[nodiscard]] std::string SymbolText(const ReflectionSnapshot &Snapshot,
                                     const SymbolId &Id);

// Canonical `identity@version` key of one module. The key both orders and names
// module provenance, so grouping never depends on load order.
[[nodiscard]] std::string ModuleKeyText(const ModuleRecord &Module);

} // namespace Luna::Detail
