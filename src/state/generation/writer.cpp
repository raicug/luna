// clang-format off
#include "state/generation/writer.hpp"

#include "state/type/structural_types.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
// clang-format on

namespace Luna::Detail {
namespace {

constexpr char32_t ByteOrderMark = 0xfeff;

// One decoded UTF-8 scalar value together with the length it occupied. A length
// of zero reports malformed input.
struct DecodedScalar final {
  char32_t Value = 0;
  std::size_t Length = 0;
};

[[nodiscard]] DecodedScalar DecodeScalar(std::string_view Text,
                                         std::size_t Position) {
  const auto Byte = [&Text](std::size_t Index) {
    return static_cast<std::uint8_t>(Text[Index]);
  };
  const std::uint8_t Lead = Byte(Position);
  const std::size_t Available = Text.size() - Position;

  std::size_t Length = 0;
  char32_t Value = 0;
  if (Lead < 0x80) {
    return DecodedScalar{static_cast<char32_t>(Lead), 1};
  } else if ((Lead & 0xe0) == 0xc0) {
    Length = 2;
    Value = static_cast<char32_t>(Lead & 0x1f);
  } else if ((Lead & 0xf0) == 0xe0) {
    Length = 3;
    Value = static_cast<char32_t>(Lead & 0x0f);
  } else if ((Lead & 0xf8) == 0xf0) {
    Length = 4;
    Value = static_cast<char32_t>(Lead & 0x07);
  } else {
    return DecodedScalar{};
  }

  if (Available < Length)
    return DecodedScalar{};
  for (std::size_t Index = 1; Index < Length; ++Index) {
    const std::uint8_t Continuation = Byte(Position + Index);
    if ((Continuation & 0xc0) != 0x80)
      return DecodedScalar{};
    Value = static_cast<char32_t>((Value << 6) |
                                  static_cast<char32_t>(Continuation & 0x3f));
  }

  // Overlong forms, surrogate halves, and values above the Unicode range are
  // all malformed, so an artifact never carries a non-canonical encoding.
  static constexpr char32_t Minimum[5] = {0, 0, 0x80, 0x800, 0x10000};
  if (Value < Minimum[Length])
    return DecodedScalar{};
  if (Value > 0x10ffff)
    return DecodedScalar{};
  if (Value >= 0xd800 && Value <= 0xdfff)
    return DecodedScalar{};
  return DecodedScalar{Value, Length};
}

[[nodiscard]] char HexDigit(std::uint8_t Nibble) {
  return Nibble < 10 ? static_cast<char>('0' + Nibble)
                     : static_cast<char>('a' + (Nibble - 10));
}

// Escapes one control byte so a metadata value never introduces a structural
// break or an invisible difference between two artifacts.
void AppendEscape(std::string &Target, std::uint8_t Byte) {
  Target.push_back('\\');
  if (Byte == '\n') {
    Target.push_back('n');
    return;
  }
  if (Byte == '\t') {
    Target.push_back('t');
    return;
  }
  Target.push_back('x');
  Target.push_back(HexDigit(static_cast<std::uint8_t>(Byte >> 4)));
  Target.push_back(HexDigit(static_cast<std::uint8_t>(Byte & 0x0f)));
}

[[nodiscard]] bool IsControlByte(std::uint8_t Byte) {
  return Byte < 0x20 || Byte == 0x7f;
}

} // namespace

GenerationStatus ClassifyGeneratedText(std::string_view Text) {
  std::size_t Position = 0;
  while (Position < Text.size()) {
    const DecodedScalar Decoded = DecodeScalar(Text, Position);
    if (Decoded.Length == 0)
      return GenerationStatus::InvalidEncoding;
    if (Decoded.Value == ByteOrderMark)
      return GenerationStatus::ForbiddenByteOrderMark;
    Position += Decoded.Length;
  }
  return GenerationStatus::Valid;
}

std::string NormalizeLineEndings(std::string_view Text) {
  std::string Normalized;
  Normalized.reserve(Text.size());
  for (std::size_t Index = 0; Index < Text.size(); ++Index) {
    if (Text[Index] != '\r') {
      Normalized.push_back(Text[Index]);
      continue;
    }
    Normalized.push_back('\n');
    if (Index + 1 < Text.size() && Text[Index + 1] == '\n')
      ++Index;
  }
  return Normalized;
}

void GenerationWriter::Literal(std::string_view Text) {
  if (RejectedValue)
    return;
  BytesValue.append(Text);
}

void GenerationWriter::Break() {
  if (RejectedValue)
    return;
  BytesValue.push_back(LineBreak);
}

bool GenerationWriter::Inline(std::string_view Value,
                              std::string_view Context) {
  if (RejectedValue)
    return false;
  if (const GenerationStatus Reason = ClassifyGeneratedText(Value);
      Reason != GenerationStatus::Valid) {
    Reject(Reason, Context);
    return false;
  }
  const std::string Normalized = NormalizeLineEndings(Value);
  for (const char Character : Normalized) {
    const std::uint8_t Byte = static_cast<std::uint8_t>(Character);
    if (IsControlByte(Byte)) {
      AppendEscape(BytesValue, Byte);
      continue;
    }
    BytesValue.push_back(Character);
  }
  return true;
}

bool GenerationWriter::Block(std::string_view Value, std::string_view Indent,
                             std::string_view Context) {
  if (RejectedValue)
    return false;
  if (const GenerationStatus Reason = ClassifyGeneratedText(Value);
      Reason != GenerationStatus::Valid) {
    Reject(Reason, Context);
    return false;
  }
  const std::string Normalized = NormalizeLineEndings(Value);
  std::size_t Position = 0;
  while (Position <= Normalized.size()) {
    const std::size_t BreakPosition = Normalized.find(LineBreak, Position);
    const std::size_t End =
        BreakPosition == std::string::npos ? Normalized.size() : BreakPosition;
    BytesValue.append(Indent);
    for (std::size_t Index = Position; Index < End; ++Index) {
      const std::uint8_t Byte = static_cast<std::uint8_t>(Normalized[Index]);
      if (IsControlByte(Byte)) {
        AppendEscape(BytesValue, Byte);
        continue;
      }
      BytesValue.push_back(Normalized[Index]);
    }
    BytesValue.push_back(LineBreak);
    if (BreakPosition == std::string::npos)
      break;
    Position = BreakPosition + 1;
  }
  return true;
}

bool GenerationWriter::Field(std::string_view Name, std::string_view Value,
                             std::string_view Context) {
  if (RejectedValue)
    return false;
  Literal(Name);
  Literal(": ");
  if (!Inline(Value, Context))
    return false;
  Break();
  return true;
}

bool GenerationWriter::Refuse(GenerationStatus Status,
                              std::string_view Context) {
  Reject(Status == GenerationStatus::Valid ? GenerationStatus::Unspecified
                                           : Status,
         Context);
  return false;
}

GeneratedArtifact GenerationWriter::Release(std::string_view Artifact) {
  if (!RejectedValue) {
    // The published buffer is validated once as a whole, so no partial or
    // non-canonical artifact can ever reach a caller.
    if (const GenerationStatus Reason = ClassifyGeneratedText(BytesValue);
        Reason != GenerationStatus::Valid)
      Reject(Reason, "the assembled artifact");
  }
  if (RejectedValue) {
    std::string Message(Artifact);
    Message.append(" generation failed for ");
    Message.append(ContextValue);
    Message.append(": ");
    Message.append(GenerationStatusText(StatusValue));
    Message.push_back('.');
    return GeneratedArtifact::Rejected(StatusValue, std::move(Message));
  }
  return GeneratedArtifact::Complete(std::move(BytesValue));
}

void GenerationWriter::Reject(GenerationStatus Status,
                              std::string_view Context) {
  if (RejectedValue)
    return;
  RejectedValue = true;
  StatusValue = Status;
  ContextValue.assign(Context);
  BytesValue.clear();
  BytesValue.shrink_to_fit();
}

std::string TypeText(const ReflectionSnapshot &Snapshot, const TypeId &Id) {
  if (!Id.IsValid())
    return std::string();
  // `Luna::TypeRecord` is the public reflection view; the private type registry
  // owns an unrelated record of the same name.
  const Luna::TypeRecord Record = Snapshot.FindType(Id);
  if (Record.IsValid() && !Record.Name().empty())
    return std::string(Record.Name());
  return Id.ToString();
}

std::string TypeText(const ReflectionSnapshot &Snapshot, const TypeId &Id,
                     const TypeDescriptor &Descriptor) {
  const Luna::TypeRecord Record = Snapshot.FindType(Id);
  if (Record.IsValid() && !Record.Name().empty())
    return std::string(Record.Name());
  if (Descriptor.IsValid())
    return StructuralPublicName(Descriptor);
  return TypeText(Snapshot, Id);
}

std::string SymbolText(const ReflectionSnapshot &Snapshot, const SymbolId &Id) {
  if (!Id.IsValid())
    return std::string();
  const ReflectionRecord Record = Snapshot.Find(Id);
  if (Record.IsValid() && !Record.QualifiedName().empty())
    return std::string(Record.QualifiedName());
  return Id.ToString();
}

std::string ModuleKeyText(const ModuleRecord &Module) {
  if (!Module.IsValid())
    return std::string();
  std::string Key(Module.Identity());
  Key.push_back('@');
  Key.append(Module.Version());
  return Key;
}

} // namespace Luna::Detail
