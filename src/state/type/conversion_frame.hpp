#pragma once

// The private bridge behind the public conversion boundary.
//
// A `ConversionFrame` is the transient conversion scope Luna opens around one
// converter callback. It owns an immutable copy of the value shape under
// conversion, hands out `Luna::ValueView` and `Luna::ConversionContext` tokens
// that name nodes inside that copy, and answers every public accessor those
// tokens expose. Because the shape is copied before any converter sees it, a
// view can never reach virtual-machine storage, and because the tokens are
// plain Luna-owned numbers there is nothing in them to turn back into a
// pointer, a stack index, or a registry reference.
//
// The frame is also where the boundary's own guards live:
//
//   * A probe receives `const ConversionContext&`, so the type system already
//     denies it every mutating operation. If a probe reaches one anyway - by
//     casting away constness - the frame records the violation, refuses the
//     operation, and publishes nothing.
//   * A view or context outliving its frame answers as an inert value and the
//     attempt is counted, so retention is detectable instead of dangerous.
//
// This header names no virtual-machine type at all: reading Luau values into an
// owning `Luna::OwnedValue` happens in the converters, not here.

// clang-format off
#include <luna/binding/conversion.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

class ConversionFrame final {
public:
  // Sentinel parent of the root node. Node indices are frame-local positions in
  // an owned vector, never addresses.
  static constexpr std::uint32_t InvalidNode = 0xffffffffu;

  ConversionFrame(Luna::ConversionDirection Direction, std::string Callable,
                  std::size_t Position);
  ~ConversionFrame();

  ConversionFrame(const ConversionFrame &) = delete;
  ConversionFrame &operator=(const ConversionFrame &) = delete;
  ConversionFrame(ConversionFrame &&) = delete;
  ConversionFrame &operator=(ConversionFrame &&) = delete;

  // Copy one owning value into the frame and return the transient view naming
  // it. The copy is what every view reads, so no view ever aliases the source.
  [[nodiscard]] ValueView Open(const OwnedValue &Source);

  [[nodiscard]] ValueView Root() const noexcept;

  // The committing context a selected converter receives.
  [[nodiscard]] ConversionContext CommitContext() const noexcept;

  // The probing context a viability and rank probe receives.
  [[nodiscard]] ConversionContext ProbeContext() const noexcept;

  [[nodiscard]] std::uint64_t Token() const noexcept { return TokenValue; }

  [[nodiscard]] bool IsActive() const noexcept { return ActiveValue; }

  // End the frame. Every outstanding view and context becomes inert.
  void Deactivate() noexcept;

  [[nodiscard]] Luna::ConversionDirection Direction() const noexcept {
    return DirectionValue;
  }

  [[nodiscard]] std::string_view Callable() const noexcept {
    return CallableValue;
  }

  [[nodiscard]] std::size_t Position() const noexcept { return PositionValue; }

  // Shape queries the public accessors answer with.
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

  [[nodiscard]] ValueView ViewOf(std::uint32_t Node) const noexcept;

  // Complete nested path of one node, such as `argument 2[4].Key`.
  [[nodiscard]] std::string PathOf(std::uint32_t Node) const;

  // One deterministic diagnostic naming the callable, the position, the
  // complete nested path, and the reason.
  [[nodiscard]] std::string Describe(std::uint32_t Node,
                                     std::string_view Reason) const;

  // Copy one node out of the frame, which is the only supported retention.
  [[nodiscard]] OwnedValue OwnedFrom(std::uint32_t Node) const;

  // Writer side. Resources are reserved and validated first; publication is
  // atomic and happens at most once.
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

  // Luna-owned record of one attempted probe violation.
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

  // Shared preconditions of every writer operation.
  [[nodiscard]] std::optional<WriteResult>
  RejectWriterOperation(bool IsProbe, std::uint32_t Node,
                        std::string_view Operation) const;

  // Fully qualified: `Luna::Detail` also declares a `ConversionDirection`
  // (argument/return) for nested diagnostics, and this is the public
  // read/write direction of the frame.
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

// Resolve one frame token. An unknown or ended token resolves to nothing, which
// is what makes a retained view inert instead of dangerous.
[[nodiscard]] ConversionFrame *
FindConversionFrame(std::uint64_t Token) noexcept;

// Luna-owned boundary guards. They record what the type system cannot forbid
// outright, so tests can assert that a violating converter was detected.
void RecordExpiredConversionAccess() noexcept;
void RecordConversionProbeViolation(std::string_view Reason) noexcept;
[[nodiscard]] std::size_t ExpiredConversionAccessCount() noexcept;
[[nodiscard]] std::size_t ProbeViolationCount() noexcept;
[[nodiscard]] const std::vector<std::string> &
RecordedProbeViolations() noexcept;
void ResetConversionBoundaryDiagnostics() noexcept;

} // namespace Luna::Detail
