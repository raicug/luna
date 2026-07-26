// clang-format off
#include "state/type/conversion_frame.hpp"

#include "state/type/conversion_outcome.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
// clang-format on

namespace Luna::Detail {

namespace {

// Every live frame of the current thread, keyed by its Luna-owned token. The
// token is issued monotonically and never reused, so an ended frame's token
// resolves to nothing instead of to some later frame.
struct FrameTable final {
  std::unordered_map<std::uint64_t, ConversionFrame *> Frames;
  std::uint64_t NextToken = 1;
  std::size_t ExpiredAccessCount = 0;
  std::size_t ProbeViolationCount = 0;
  std::vector<std::string> ProbeViolations;
};

[[nodiscard]] FrameTable &Table() noexcept {
  static thread_local FrameTable Frames;
  return Frames;
}

[[nodiscard]] std::string ByteCountText(std::size_t Received,
                                        std::size_t Permitted) {
  return "received " + std::to_string(Received) + " string bytes; maximum is " +
         std::to_string(Permitted);
}

// An aggregate is complete only when every field it publishes is named.
[[nodiscard]] bool IsCompleteAggregate(const OwnedValue &Source) {
  for (std::size_t Index = 0; Index < Source.FieldCount(); ++Index) {
    if (Source.FieldName(Index).empty())
      return false;
    if (!IsCompleteAggregate(Source.Field(Source.FieldName(Index))))
      return false;
  }
  for (std::size_t Index = 0; Index < Source.Size(); ++Index) {
    if (!IsCompleteAggregate(Source.Element(Index)))
      return false;
  }
  return true;
}

// Which reserved resource a request exceeds, if any.
[[nodiscard]] std::optional<std::string>
ExceededResource(const ValueReservation &Required,
                 const ValueReservation &Reserved) {
  if (Required.ValueCount > Reserved.ValueCount)
    return "values (" + std::to_string(Required.ValueCount) + " needed, " +
           std::to_string(Reserved.ValueCount) + " reserved)";
  if (Required.ElementCount > Reserved.ElementCount)
    return "elements (" + std::to_string(Required.ElementCount) + " needed, " +
           std::to_string(Reserved.ElementCount) + " reserved)";
  if (Required.FieldCount > Reserved.FieldCount)
    return "fields (" + std::to_string(Required.FieldCount) + " needed, " +
           std::to_string(Reserved.FieldCount) + " reserved)";
  if (Required.TableCount > Reserved.TableCount)
    return "tables (" + std::to_string(Required.TableCount) + " needed, " +
           std::to_string(Reserved.TableCount) + " reserved)";
  if (Required.UserdataCount > Reserved.UserdataCount)
    return "userdata (" + std::to_string(Required.UserdataCount) + " needed, " +
           std::to_string(Reserved.UserdataCount) + " reserved)";
  if (Required.ByteCount > Reserved.ByteCount)
    return "bytes (" + std::to_string(Required.ByteCount) + " needed, " +
           std::to_string(Reserved.ByteCount) + " reserved)";
  return std::nullopt;
}

} // namespace

ConversionFrame::ConversionFrame(Luna::ConversionDirection Direction,
                                 std::string Callable, std::size_t Position)
    : DirectionValue(Direction), CallableValue(std::move(Callable)),
      PositionValue(Position) {
  FrameTable &Frames = Table();
  TokenValue = Frames.NextToken++;
  Frames.Frames.emplace(TokenValue, this);
}

ConversionFrame::~ConversionFrame() { Deactivate(); }

void ConversionFrame::Deactivate() noexcept {
  if (!ActiveValue)
    return;
  ActiveValue = false;
  Table().Frames.erase(TokenValue);
}

ValueView ConversionFrame::Open(const OwnedValue &Source) {
  NodesValue.clear();
  static_cast<void>(Insert(Source, InvalidNode, std::string()));
  return Root();
}

ValueView ConversionFrame::Root() const noexcept { return ViewOf(0); }

ConversionContext ConversionFrame::CommitContext() const noexcept {
  return ConversionContext(TokenValue, 0, false);
}

ConversionContext ConversionFrame::ProbeContext() const noexcept {
  return ConversionContext(TokenValue, 0, true);
}

std::uint32_t ConversionFrame::Insert(const OwnedValue &Source,
                                      std::uint32_t Parent,
                                      std::string Segment) {
  const std::uint32_t Index = static_cast<std::uint32_t>(NodesValue.size());
  NodesValue.emplace_back();
  {
    ValueNode &Created = NodesValue[Index];
    Created.Category = Source.Kind();
    Created.Boolean = Source.ToBoolean().value_or(false);
    Created.Number = Source.ToNumber().value_or(0.0);
    Created.Text = std::string(Source.TextBytes());
    Created.Parent = Parent;
    Created.Segment = std::move(Segment);
  }

  for (std::size_t Position = 0; Position < Source.Size(); ++Position) {
    const std::uint32_t Child =
        Insert(Source.Element(Position), Index,
               "[" + std::to_string(Position + 1) + "]");
    NodesValue[Index].Elements.push_back(Child);
  }

  for (std::size_t Position = 0; Position < Source.FieldCount(); ++Position) {
    const std::string Name(Source.FieldName(Position));
    const std::uint32_t Child = Insert(Source.Field(Name), Index, "." + Name);
    NodesValue[Index].FieldNames.push_back(Name);
    NodesValue[Index].FieldValues.push_back(Child);
  }

  return Index;
}

const ConversionFrame::ValueNode *
ConversionFrame::NodeAt(std::uint32_t Node) const noexcept {
  if (Node >= NodesValue.size())
    return nullptr;
  return &NodesValue[Node];
}

bool ConversionFrame::HasNode(std::uint32_t Node) const noexcept {
  return NodeAt(Node) != nullptr;
}

ValueCategory ConversionFrame::CategoryOf(std::uint32_t Node) const noexcept {
  const ValueNode *Found = NodeAt(Node);
  return Found ? Found->Category : ValueCategory::None;
}

std::optional<bool>
ConversionFrame::BooleanOf(std::uint32_t Node) const noexcept {
  const ValueNode *Found = NodeAt(Node);
  if (!Found || Found->Category != ValueCategory::Boolean)
    return std::nullopt;
  return Found->Boolean;
}

std::optional<double>
ConversionFrame::NumberOf(std::uint32_t Node) const noexcept {
  const ValueNode *Found = NodeAt(Node);
  if (!Found || Found->Category != ValueCategory::Number)
    return std::nullopt;
  return Found->Number;
}

std::optional<std::string> ConversionFrame::TextOf(std::uint32_t Node) const {
  const ValueNode *Found = NodeAt(Node);
  if (!Found || Found->Category != ValueCategory::String)
    return std::nullopt;
  return Found->Text;
}

std::size_t ConversionFrame::ByteCountOf(std::uint32_t Node) const noexcept {
  const ValueNode *Found = NodeAt(Node);
  return Found ? Found->Text.size() : 0;
}

std::size_t ConversionFrame::ElementCountOf(std::uint32_t Node) const noexcept {
  const ValueNode *Found = NodeAt(Node);
  return Found ? Found->Elements.size() : 0;
}

std::uint32_t ConversionFrame::ElementNode(std::uint32_t Node,
                                           std::size_t Index) const noexcept {
  const ValueNode *Found = NodeAt(Node);
  if (!Found || Index >= Found->Elements.size())
    return InvalidNode;
  return Found->Elements[Index];
}

std::size_t ConversionFrame::FieldCountOf(std::uint32_t Node) const noexcept {
  const ValueNode *Found = NodeAt(Node);
  return Found ? Found->FieldNames.size() : 0;
}

std::string_view
ConversionFrame::FieldNameOf(std::uint32_t Node,
                             std::size_t Index) const noexcept {
  const ValueNode *Found = NodeAt(Node);
  if (!Found || Index >= Found->FieldNames.size())
    return {};
  return Found->FieldNames[Index];
}

std::uint32_t ConversionFrame::FieldNode(std::uint32_t Node,
                                         std::string_view Name) const noexcept {
  const ValueNode *Found = NodeAt(Node);
  if (!Found)
    return InvalidNode;
  for (std::size_t Index = 0; Index < Found->FieldNames.size(); ++Index) {
    if (Found->FieldNames[Index] == Name)
      return Found->FieldValues[Index];
  }
  return InvalidNode;
}

ValueView ConversionFrame::ViewOf(std::uint32_t Node) const noexcept {
  if (Node == InvalidNode)
    return ValueView();
  return ValueView(TokenValue, Node);
}

std::string ConversionFrame::PositionText() const {
  if (PositionValue == 0)
    return DirectionValue == Luna::ConversionDirection::Read ? "argument"
                                                             : "return value";
  const std::string Prefix = DirectionValue == Luna::ConversionDirection::Read
                                 ? "argument "
                                 : "return ";
  return Prefix + std::to_string(PositionValue);
}

std::string ConversionFrame::PathOf(std::uint32_t Node) const {
  std::vector<std::string> Segments;
  std::uint32_t Current = Node;
  while (const ValueNode *Found = NodeAt(Current)) {
    if (!Found->Segment.empty())
      Segments.push_back(Found->Segment);
    if (Found->Parent == InvalidNode)
      break;
    Current = Found->Parent;
  }

  std::string Path = PositionText();
  for (std::size_t Index = Segments.size(); Index > 0; --Index)
    Path += Segments[Index - 1];
  return Path;
}

std::string ConversionFrame::Describe(std::uint32_t Node,
                                      std::string_view Reason) const {
  std::string Text;
  if (!CallableValue.empty())
    Text += "Callable '" + CallableValue + "' ";
  Text += PathOf(Node);
  Text += " ";
  Text += Reason;
  if (Text.back() != '.')
    Text += ".";
  return Text;
}

OwnedValue ConversionFrame::OwnedFrom(std::uint32_t Node) const {
  const ValueNode *Found = NodeAt(Node);
  if (!Found)
    return OwnedValue();

  OwnedValue Copied;
  switch (Found->Category) {
  case ValueCategory::Boolean:
    Copied = OwnedValue::Boolean(Found->Boolean);
    break;
  case ValueCategory::Number:
    Copied = OwnedValue::Number(Found->Number);
    break;
  case ValueCategory::String:
    Copied = OwnedValue::Text(Found->Text);
    break;
  case ValueCategory::Table:
    Copied = OwnedValue::Table();
    break;
  default:
    return OwnedValue();
  }

  for (const std::uint32_t Element : Found->Elements)
    Copied.Append(OwnedFrom(Element));
  for (std::size_t Index = 0; Index < Found->FieldNames.size(); ++Index)
    Copied.SetField(Found->FieldNames[Index],
                    OwnedFrom(Found->FieldValues[Index]));
  return Copied;
}

std::optional<WriteResult>
ConversionFrame::RejectWriterOperation(bool IsProbe, std::uint32_t Node,
                                       std::string_view Operation) const {
  WriteResult Result;
  if (!ActiveValue) {
    Result.Status = WriteStatus::InactiveContext;
    Result.Diagnostic = Describe(Node, "used a conversion context whose frame "
                                       "had already ended");
    return Result;
  }
  if (IsProbe) {
    RecordProbeViolation(Operation);
    Result.Status = WriteStatus::ProbeViolation;
    Result.Diagnostic =
        Describe(Node, std::string("a viability probe attempted to ") +
                           std::string(Operation));
    return Result;
  }
  if (DirectionValue != Luna::ConversionDirection::Write) {
    Result.Status = WriteStatus::WrongDirection;
    Result.Diagnostic =
        Describe(Node, "cannot publish a value while reading a value");
    return Result;
  }
  if (PublishedValue) {
    Result.Status = WriteStatus::AlreadyPublished;
    Result.Diagnostic = Describe(Node, "has already published its value");
    return Result;
  }
  return std::nullopt;
}

WriteResult ConversionFrame::Reserve(const ValueReservation &Request,
                                     bool IsProbe, std::uint32_t Node) {
  if (const std::optional<WriteResult> Rejected =
          RejectWriterOperation(IsProbe, Node, "reserve writer resources"))
    return *Rejected;

  if (Request.ByteCount > MaximumInvocationStringBytes) {
    WriteResult Result;
    Result.Status = WriteStatus::PolicyExceeded;
    Result.Diagnostic = Describe(
        Node, "reserved " + std::to_string(Request.ByteCount) +
                  " string bytes; maximum is " +
                  std::to_string(MaximumInvocationStringBytes) + " per string");
    return Result;
  }

  ReservationValue = Request;
  HasReservationValue = true;

  WriteResult Result;
  Result.Status = WriteStatus::Success;
  return Result;
}

WriteResult ConversionFrame::Publish(const OwnedValue &Source, bool IsProbe,
                                     std::uint32_t Node) {
  if (const std::optional<WriteResult> Rejected =
          RejectWriterOperation(IsProbe, Node, "publish a value"))
    return *Rejected;

  WriteResult Result;
  if (!HasReservationValue) {
    Result.Status = WriteStatus::ReservationMissing;
    Result.Diagnostic =
        Describe(Node, "published a value without reserving its resources");
    return Result;
  }
  if (!IsCompleteAggregate(Source)) {
    Result.Status = WriteStatus::IncompleteAggregate;
    Result.Diagnostic = Describe(Node, "published an aggregate with an unnamed "
                                       "field");
    return Result;
  }
  if (const std::size_t Largest = Source.LargestStringByteCount();
      Largest > MaximumInvocationStringBytes) {
    Result.Status = WriteStatus::PolicyExceeded;
    Result.Diagnostic =
        Describe(Node, ByteCountText(Largest, MaximumInvocationStringBytes));
    return Result;
  }
  if (const std::optional<std::string> Exceeded =
          ExceededResource(Source.RequiredReservation(), ReservationValue)) {
    Result.Status = WriteStatus::ReservationExceeded;
    Result.Diagnostic =
        Describe(Node, "needs more resources than were reserved: " + *Exceeded);
    return Result;
  }

  // Everything is validated, so publication is a single visible step.
  PublishedResultValue = Source;
  PublishedValue = true;
  Result.Status = WriteStatus::Success;
  return Result;
}

WriteResult ConversionFrame::PublishPack(const ValuePack &Source, bool IsProbe,
                                         std::uint32_t Node) {
  if (const std::optional<WriteResult> Rejected =
          RejectWriterOperation(IsProbe, Node, "publish a return pack"))
    return *Rejected;

  WriteResult Result;
  if (!HasReservationValue) {
    Result.Status = WriteStatus::ReservationMissing;
    Result.Diagnostic = Describe(
        Node, "published a return pack without reserving its resources");
    return Result;
  }
  for (std::size_t Index = 0; Index < Source.Size(); ++Index) {
    if (IsCompleteAggregate(Source.At(Index)))
      continue;
    Result.Status = WriteStatus::IncompleteAggregate;
    Result.Diagnostic =
        Describe(Node, "published a return pack whose value " +
                           std::to_string(Index + 1) + " has an unnamed field");
    return Result;
  }
  if (const std::size_t Largest = Source.LargestStringByteCount();
      Largest > MaximumInvocationStringBytes) {
    Result.Status = WriteStatus::PolicyExceeded;
    Result.Diagnostic =
        Describe(Node, ByteCountText(Largest, MaximumInvocationStringBytes));
    return Result;
  }
  if (const std::optional<std::string> Exceeded =
          ExceededResource(Source.RequiredReservation(), ReservationValue)) {
    Result.Status = WriteStatus::ReservationExceeded;
    Result.Diagnostic =
        Describe(Node, "needs more resources than were reserved: " + *Exceeded);
    return Result;
  }

  PublishedPackValue = Source;
  PublishedValue = true;
  Result.Status = WriteStatus::Success;
  return Result;
}

void ConversionFrame::RecordProbeViolation(std::string_view Reason) const {
  ProbeViolationsValue.push_back(std::string(Reason));
  RecordConversionProbeViolation(Reason);
}

ConversionFrame *FindConversionFrame(std::uint64_t Token) noexcept {
  if (Token == 0)
    return nullptr;
  FrameTable &Frames = Table();
  const auto Found = Frames.Frames.find(Token);
  if (Found == Frames.Frames.end())
    return nullptr;
  return Found->second;
}

void RecordExpiredConversionAccess() noexcept { ++Table().ExpiredAccessCount; }

void RecordConversionProbeViolation(std::string_view Reason) noexcept {
  FrameTable &Frames = Table();
  ++Frames.ProbeViolationCount;
  Frames.ProbeViolations.push_back(std::string(Reason));
}

std::size_t ExpiredConversionAccessCount() noexcept {
  return Table().ExpiredAccessCount;
}

std::size_t ProbeViolationCount() noexcept {
  return Table().ProbeViolationCount;
}

const std::vector<std::string> &RecordedProbeViolations() noexcept {
  return Table().ProbeViolations;
}

void ResetConversionBoundaryDiagnostics() noexcept {
  FrameTable &Frames = Table();
  Frames.ExpiredAccessCount = 0;
  Frames.ProbeViolationCount = 0;
  Frames.ProbeViolations.clear();
}

} // namespace Luna::Detail
