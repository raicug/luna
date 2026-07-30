#pragma once

// clang-format off
#include <luna/binding/conversion.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

class ConversionFrame final {
public:
  static constexpr std::uint32_t InvalidNode = 0xffffffffu;

  ConversionFrame(Luna::ConversionDirection Direction, std::string Callable,
                  std::size_t Position);
  ~ConversionFrame();

  ConversionFrame(const ConversionFrame &) = delete;
  ConversionFrame &operator=(const ConversionFrame &) = delete;
  ConversionFrame(ConversionFrame &&) = delete;
  ConversionFrame &operator=(ConversionFrame &&) = delete;

  [[nodiscard]] ValueView Open(const OwnedValue &Source);

  [[nodiscard]] ValueView Root() const noexcept;

  [[nodiscard]] ConversionContext CommitContext() const noexcept;

  [[nodiscard]] ConversionContext ProbeContext() const noexcept;

  [[nodiscard]] std::uint64_t Token() const noexcept { return TokenValue; }

  [[nodiscard]] bool IsActive() const noexcept { return ActiveValue; }

  void Deactivate() noexcept;

  [[nodiscard]] Luna::ConversionDirection Direction() const noexcept {
    return DirectionValue;
  }

  [[nodiscard]] std::string_view Callable() const noexcept {
    return CallableValue;
  }

  [[nodiscard]] std::size_t Position() const noexcept { return PositionValue; }

  [[nodiscard]] bool HasNode(std::uint32_t Node) const noexcept;
  [[nodiscard]] ValueCategory CategoryOf(std::uint32_t Node) const noexcept;
  [[nodiscard]] std::optional<bool>
  BooleanOf(std::uint32_t Node) const noexcept;
  [[nodiscard]] std::optional<double>
  NumberOf(std::uint32_t Node) const noexcept;
  [[nodiscard]] std::optional<std::string> TextOf(std::uint32_t Node) const;
  [[nodiscard]] std::size_t ByteCountOf(std::uint32_t Node) const noexcept;
  [[nodiscard]] std::size_t ElementCountOf(std::uint32_t Node) const noexcept;
  [[nodiscard]] std::uint32_t ElementNode(std::uint32_t Node,
                                          std::size_t Index) const noexcept;
  [[nodiscard]] std::size_t FieldCountOf(std::uint32_t Node) const noexcept;
  [[nodiscard]] std::string_view FieldNameOf(std::uint32_t Node,
                                             std::size_t Index) const noexcept;
  [[nodiscard]] std::uint32_t FieldNode(std::uint32_t Node,
                                        std::string_view Name) const noexcept;

  [[nodiscard]] std::string_view
  UserdataClassNameOf(std::uint32_t Node) const noexcept;
  [[nodiscard]] std::string_view
  UserdataTextOf(std::uint32_t Node) const noexcept;
  [[nodiscard]] const CapturedUserdataTarget *
  UserdataTargetOf(std::uint32_t Node) const noexcept;

  [[nodiscard]] ValueView ViewOf(std::uint32_t Node) const noexcept;

  [[nodiscard]] std::string PathOf(std::uint32_t Node) const;

  [[nodiscard]] std::string Describe(std::uint32_t Node,
                                     std::string_view Reason) const;

  [[nodiscard]] OwnedValue OwnedFrom(std::uint32_t Node) const;

  [[nodiscard]] bool HasReservation() const noexcept {
    return HasReservationValue;
  }

  [[nodiscard]] ValueReservation Reservation() const noexcept {
    return ReservationValue;
  }

  [[nodiscard]] bool IsPublished() const noexcept { return PublishedValue; }

  [[nodiscard]] const std::optional<OwnedValue> &
  PublishedResult() const noexcept {
    return PublishedResultValue;
  }

  [[nodiscard]] const std::optional<ValuePack> &PublishedPack() const noexcept {
    return PublishedPackValue;
  }

  [[nodiscard]] WriteResult Reserve(const ValueReservation &Request,
                                    bool IsProbe, std::uint32_t Node);
  [[nodiscard]] WriteResult Publish(const OwnedValue &Source, bool IsProbe,
                                    std::uint32_t Node);
  [[nodiscard]] WriteResult PublishPack(const ValuePack &Source, bool IsProbe,
                                        std::uint32_t Node);

  void RecordProbeViolation(std::string_view Reason) const;

  [[nodiscard]] const std::vector<std::string> &
  ProbeViolations() const noexcept {
    return ProbeViolationsValue;
  }

private:
  struct ValueNode final {
    ValueCategory Category = ValueCategory::Nil;
    bool Boolean = false;
    double Number = 0.0;
    std::string Text;
    // A userdata node keeps the class name in `Text` and the value's own
    // rendered text here, so neither displaces the other on the way back out.
    std::string UserdataText;
    std::shared_ptr<CapturedUserdataTarget> Userdata;
    std::vector<std::uint32_t> Elements;
    std::vector<std::string> FieldNames;
    std::vector<std::uint32_t> FieldValues;
    std::uint32_t Parent = InvalidNode;
    std::string Segment;
  };

  [[nodiscard]] std::uint32_t Insert(const OwnedValue &Source,
                                     std::uint32_t Parent, std::string Segment);

  [[nodiscard]] const ValueNode *NodeAt(std::uint32_t Node) const noexcept;

  [[nodiscard]] std::string PositionText() const;

  [[nodiscard]] std::optional<WriteResult>
  RejectWriterOperation(bool IsProbe, std::uint32_t Node,
                        std::string_view Operation) const;

  Luna::ConversionDirection DirectionValue = Luna::ConversionDirection::Read;
  std::string CallableValue;
  std::size_t PositionValue = 0;
  std::uint64_t TokenValue = 0;
  bool ActiveValue = true;

  std::vector<ValueNode> NodesValue;

  bool HasReservationValue = false;
  ValueReservation ReservationValue;
  bool PublishedValue = false;
  std::optional<OwnedValue> PublishedResultValue;
  std::optional<ValuePack> PublishedPackValue;

  mutable std::vector<std::string> ProbeViolationsValue;
};

[[nodiscard]] ConversionFrame *
FindConversionFrame(std::uint64_t Token) noexcept;

void RecordExpiredConversionAccess() noexcept;
void RecordConversionProbeViolation(std::string_view Reason) noexcept;
[[nodiscard]] std::size_t ExpiredConversionAccessCount() noexcept;
[[nodiscard]] std::size_t ProbeViolationCount() noexcept;
[[nodiscard]] const std::vector<std::string> &
RecordedProbeViolations() noexcept;
void ResetConversionBoundaryDiagnostics() noexcept;

} // namespace Luna::Detail
