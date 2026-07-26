#pragma once

// Deterministic human-readable documentation generation. Generation reads one
// explicitly captured immutable `ReflectionSnapshot` and one immutable options
// value, and nothing else: no State, no virtual machine, no table, no dispatch
// target, and no native object is consulted, so a snapshot retained across
// later registrations, a freeze, a State move, or destruction of the
// originating State still generates exactly the material it captured.
//
// The traversal is canonical - module provenance, then scope qualified name,
// symbol kind, declaration signature, and stable identity - so equivalent
// snapshot content and equal options always produce byte-identical UTF-8
// output without a byte-order mark and with LF line endings, independent of
// registration order, addresses, locale, hash iteration, and process-random
// values.

// clang-format off
#include <luna/generation/artifact.hpp>
#include <luna/reflection/reflection_snapshot.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
// clang-format on

namespace Luna {

// Immutable documentation generator options. Every option is a value: an
// options object is never rebound after generation begins, and two equal
// options always select the same output.
class DocumentationOptions final {
public:
  // The canonical title used when a caller states none.
  static constexpr std::string_view DefaultTitle = "Luna API Reference";

  DocumentationOptions() = default;

  [[nodiscard]] static DocumentationOptions Create(std::string Title,
                                                   bool IncludeIdentities,
                                                   bool IncludeAttributes,
                                                   bool IncludeExamples) {
    DocumentationOptions Options;
    Options.TitleValue = std::move(Title);
    Options.IdentitiesValue = IncludeIdentities;
    Options.AttributesValue = IncludeAttributes;
    Options.ExamplesValue = IncludeExamples;
    return Options;
  }

  // The document title. An empty title selects `DefaultTitle`, so the heading
  // is never absent.
  [[nodiscard]] std::string_view Title() const noexcept {
    return TitleValue.empty() ? DefaultTitle : std::string_view(TitleValue);
  }

  [[nodiscard]] bool IncludesIdentities() const noexcept {
    return IdentitiesValue;
  }

  [[nodiscard]] bool IncludesAttributes() const noexcept {
    return AttributesValue;
  }

  [[nodiscard]] bool IncludesExamples() const noexcept { return ExamplesValue; }

  [[nodiscard]] DocumentationOptions WithTitle(std::string Title) const {
    DocumentationOptions Options = *this;
    Options.TitleValue = std::move(Title);
    return Options;
  }

  [[nodiscard]] DocumentationOptions WithIdentities(bool Include) const {
    DocumentationOptions Options = *this;
    Options.IdentitiesValue = Include;
    return Options;
  }

  [[nodiscard]] DocumentationOptions WithAttributes(bool Include) const {
    DocumentationOptions Options = *this;
    Options.AttributesValue = Include;
    return Options;
  }

  [[nodiscard]] DocumentationOptions WithExamples(bool Include) const {
    DocumentationOptions Options = *this;
    Options.ExamplesValue = Include;
    return Options;
  }

  [[nodiscard]] friend bool operator==(const DocumentationOptions &Left,
                                       const DocumentationOptions &Right) {
    return Left.Title() == Right.Title() &&
           Left.IdentitiesValue == Right.IdentitiesValue &&
           Left.AttributesValue == Right.AttributesValue &&
           Left.ExamplesValue == Right.ExamplesValue;
  }

private:
  std::string TitleValue;
  bool IdentitiesValue = true;
  bool AttributesValue = true;
  bool ExamplesValue = true;
};

// Generates the complete documentation of one captured reflection generation.
// The whole artifact is built and validated in an owned buffer first: an
// accepted artifact is canonical UTF-8 with LF line endings, and a rejected
// artifact carries one deterministic diagnostic and no bytes.
[[nodiscard]] GeneratedArtifact
GenerateDocumentation(const ReflectionSnapshot &Snapshot,
                      const DocumentationOptions &Options);

} // namespace Luna
