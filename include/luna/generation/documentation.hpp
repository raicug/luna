#pragma once

// clang-format off
#include <luna/generation/artifact.hpp>
#include <luna/reflection/reflection_snapshot.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
// clang-format on

namespace Luna {

class DocumentationOptions final {
public:
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

[[nodiscard]] GeneratedArtifact
GenerateDocumentation(const ReflectionSnapshot &Snapshot,
                      const DocumentationOptions &Options);

} // namespace Luna
