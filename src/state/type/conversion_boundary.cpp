// clang-format off
#include <luna/binding/conversion.hpp>

#include "state/type/conversion_frame.hpp"
#include "state/type/conversion_outcome.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
// clang-format on

namespace Luna {

namespace {

[[nodiscard]] Detail::ConversionFrame *
ResolveFrame(std::uint64_t Token) noexcept {
  Detail::ConversionFrame *Frame = Detail::FindConversionFrame(Token);
  if (Frame != nullptr && Frame->IsActive())
    return Frame;
  if (Token != 0)
    Detail::RecordExpiredConversionAccess();
  return nullptr;
}

[[nodiscard]] WriteResult InactiveWrite() {
  WriteResult Result;
  Result.Status = WriteStatus::InactiveContext;
  Result.Diagnostic = "The conversion frame has already ended.";
  return Result;
}

} // namespace

std::size_t MaximumConversionStringBytes() noexcept {
  return Detail::MaximumInvocationStringBytes;
}

bool ValueView::IsActive() const noexcept {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  return Frame != nullptr && Frame->HasNode(NodeIndexValue);
}

ValueCategory ValueView::Kind() const noexcept {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  return Frame ? Frame->CategoryOf(NodeIndexValue) : ValueCategory::None;
}

bool ValueView::IsNil() const noexcept { return Kind() == ValueCategory::Nil; }

bool ValueView::IsTable() const noexcept {
  return Kind() == ValueCategory::Table;
}

bool ValueView::IsUserdata() const noexcept {
  return Kind() == ValueCategory::Userdata;
}

std::string_view ValueView::UserdataClassName() const noexcept {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  return Frame ? Frame->UserdataClassNameOf(NodeIndexValue)
               : std::string_view();
}

std::string_view ValueView::UserdataText() const noexcept {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  return Frame ? Frame->UserdataTextOf(NodeIndexValue) : std::string_view();
}

bool ValueView::UserdataIsLive() const noexcept {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  if (!Frame)
    return false;
  const Detail::CapturedUserdataTarget *Target =
      Frame->UserdataTargetOf(NodeIndexValue);
  return Target != nullptr && Target->IsLive();
}

TypeId ValueView::UserdataType() const noexcept {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  if (!Frame)
    return TypeId();
  const Detail::CapturedUserdataTarget *Target =
      Frame->UserdataTargetOf(NodeIndexValue);
  return Target ? Target->CapturedType() : TypeId();
}

void *ValueView::UserdataStorage() const noexcept {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  if (!Frame)
    return nullptr;
  const Detail::CapturedUserdataTarget *Target =
      Frame->UserdataTargetOf(NodeIndexValue);
  return Target ? Target->Storage() : nullptr;
}

bool ValueView::UserdataPermitsMutation() const noexcept {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  if (!Frame)
    return false;
  const Detail::CapturedUserdataTarget *Target =
      Frame->UserdataTargetOf(NodeIndexValue);
  return Target != nullptr && Target->PermitsMutation();
}

std::optional<bool> ValueView::ToBoolean() const noexcept {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  if (!Frame)
    return std::nullopt;
  return Frame->BooleanOf(NodeIndexValue);
}

std::optional<double> ValueView::ToNumber() const noexcept {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  if (!Frame)
    return std::nullopt;
  return Frame->NumberOf(NodeIndexValue);
}

std::optional<std::string> ValueView::ToText() const {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  if (!Frame)
    return std::nullopt;
  return Frame->TextOf(NodeIndexValue);
}

std::size_t ValueView::ByteCount() const noexcept {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  return Frame ? Frame->ByteCountOf(NodeIndexValue) : 0;
}

std::size_t ValueView::Size() const noexcept {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  return Frame ? Frame->ElementCountOf(NodeIndexValue) : 0;
}

ValueView ValueView::Element(std::size_t Index) const noexcept {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  if (!Frame)
    return ValueView();
  return Frame->ViewOf(Frame->ElementNode(NodeIndexValue, Index));
}

std::size_t ValueView::FieldCount() const noexcept {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  return Frame ? Frame->FieldCountOf(NodeIndexValue) : 0;
}

std::string_view ValueView::FieldName(std::size_t Index) const noexcept {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  if (!Frame)
    return {};
  return Frame->FieldNameOf(NodeIndexValue, Index);
}

bool ValueView::HasField(std::string_view Name) const noexcept {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  if (!Frame)
    return false;
  return Frame->FieldNode(NodeIndexValue, Name) !=
         Detail::ConversionFrame::InvalidNode;
}

ValueView ValueView::Field(std::string_view Name) const noexcept {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  if (!Frame)
    return ValueView();
  return Frame->ViewOf(Frame->FieldNode(NodeIndexValue, Name));
}

std::string ValueView::Path() const {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  if (!Frame)
    return {};
  return Frame->PathOf(NodeIndexValue);
}

OwnedValue ValueView::ToOwned() const {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  if (!Frame)
    return OwnedValue();
  return Frame->OwnedFrom(NodeIndexValue);
}

bool ConversionContext::IsActive() const noexcept {
  return ResolveFrame(FrameTokenValue) != nullptr;
}

ConversionDirection ConversionContext::Direction() const noexcept {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  return Frame ? Frame->Direction() : ConversionDirection::Read;
}

bool ConversionContext::IsProbing() const noexcept { return ProbingValue; }

ValueCategory ConversionContext::Kind() const noexcept {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  return Frame ? Frame->CategoryOf(NodeIndexValue) : ValueCategory::None;
}

bool ConversionContext::IsNil() const noexcept {
  return Kind() == ValueCategory::Nil;
}

std::size_t ConversionContext::Size() const noexcept {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  return Frame ? Frame->ElementCountOf(NodeIndexValue) : 0;
}

ValueView ConversionContext::Element(std::size_t Index) const noexcept {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  if (!Frame)
    return ValueView();
  return Frame->ViewOf(Frame->ElementNode(NodeIndexValue, Index));
}

ValueView ConversionContext::Field(std::string_view Name) const noexcept {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  if (!Frame)
    return ValueView();
  return Frame->ViewOf(Frame->FieldNode(NodeIndexValue, Name));
}

ValueView ConversionContext::Source() const noexcept {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  if (!Frame)
    return ValueView();
  return Frame->ViewOf(NodeIndexValue);
}

std::string_view ConversionContext::Callable() const noexcept {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  if (!Frame)
    return {};
  return Frame->Callable();
}

std::size_t ConversionContext::Position() const noexcept {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  return Frame ? Frame->Position() : 0;
}

std::string ConversionContext::Path() const {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  if (!Frame)
    return {};
  return Frame->PathOf(NodeIndexValue);
}

std::string ConversionContext::Describe(std::string_view Reason) const {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  if (!Frame)
    return std::string(Reason);
  return Frame->Describe(NodeIndexValue, Reason);
}

bool ConversionContext::HasReservation() const noexcept {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  return Frame != nullptr && Frame->HasReservation();
}

bool ConversionContext::IsPublished() const noexcept {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  return Frame != nullptr && Frame->IsPublished();
}

ValueReservation ConversionContext::Reservation() const noexcept {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  if (!Frame)
    return ValueReservation();
  return Frame->Reservation();
}

WriteResult ConversionContext::Reserve(const ValueReservation &Request) {
  Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  if (!Frame)
    return InactiveWrite();
  return Frame->Reserve(Request, ProbingValue, NodeIndexValue);
}

WriteResult ConversionContext::Publish(const OwnedValue &Published) {
  Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  if (!Frame)
    return InactiveWrite();
  return Frame->Publish(Published, ProbingValue, NodeIndexValue);
}

WriteResult ConversionContext::PublishPack(const ValuePack &Published) {
  Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  if (!Frame)
    return InactiveWrite();
  return Frame->PublishPack(Published, ProbingValue, NodeIndexValue);
}

void ConversionContext::ReportProbeViolation(std::string_view Reason) const {
  const Detail::ConversionFrame *Frame = ResolveFrame(FrameTokenValue);
  if (!Frame) {
    Detail::RecordConversionProbeViolation(Reason);
    return;
  }
  Frame->RecordProbeViolation(Reason);
}

} // namespace Luna
