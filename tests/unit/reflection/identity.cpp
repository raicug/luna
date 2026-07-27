// clang-format off
#include <luna/detail/canonical_type.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/type/stable_type_key.hpp>
#include <luna/type/type_descriptor.hpp>

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
// clang-format on

namespace {

enum class Color { Red, Green };
struct Widget {
  int Field = 0;
};

using KeyList = std::vector<Luna::StableTypeKey>;

static_assert(!std::is_same_v<Luna::TypeId, Luna::SymbolId>);
static_assert(Luna::TypeId::BitCount == 256);
static_assert(Luna::TypeId::ByteCount == 32);
static_assert(Luna::TypeId::TextLength == 64);
static_assert(std::is_copy_constructible_v<Luna::TypeDescriptor>);
static_assert(std::is_copy_assignable_v<Luna::TypeDescriptor>);
static_assert(std::is_move_constructible_v<Luna::TypeDescriptor>);

[[nodiscard]] Luna::TypeId::Storage BytesWith(std::uint8_t Last) {
  Luna::TypeId::Storage Bytes{};
  Bytes[Luna::TypeId::ByteCount - 1] = Last;
  return Bytes;
}

template <class Type>
[[nodiscard]] Luna::TypeDescriptor Canonical(const KeyList &Keys = {}) {
  return Luna::Detail::CanonicalDescriptorFor<Type>(Keys);
}

[[nodiscard]] bool VerifyStableKeyGrammar() {
  const std::array Valid{"studio.physics.Vector3", "_x", "a1.b_2", "Widget",
                         "a._"};
  for (const auto *Text : Valid) {
    if (!Luna::StableTypeKey::IsValidText(Text))
      return false;
    if (!Luna::StableTypeKey(Text).IsValid())
      return false;
  }

  const std::array<std::pair<const char *, Luna::StableTypeKeyStatus>, 7>
      Rejected{{
          {"", Luna::StableTypeKeyStatus::Empty},
          {".a", Luna::StableTypeKeyStatus::EmptySegment},
          {"a.", Luna::StableTypeKeyStatus::EmptySegment},
          {"a..b", Luna::StableTypeKeyStatus::EmptySegment},
          {"1abc", Luna::StableTypeKeyStatus::InvalidLeadingCharacter},
          {"a-b", Luna::StableTypeKeyStatus::InvalidCharacter},
          {"luna.void", Luna::StableTypeKeyStatus::ReservedPrefix},
      }};
  for (const auto &[Text, Status] : Rejected) {
    if (Luna::StableTypeKey::Classify(Text) != Status)
      return false;
    if (Luna::StableTypeKey(Text).IsValid())
      return false;
  }

  const std::string TooLong(Luna::StableTypeKey::MaximumLength + 1, 'a');
  if (Luna::StableTypeKey::Classify(TooLong) !=
      Luna::StableTypeKeyStatus::TooLong)
    return false;
  const std::string LongestAllowed(Luna::StableTypeKey::MaximumLength, 'a');
  if (!Luna::StableTypeKey::IsValidText(LongestAllowed))
    return false;

  const Luna::StableTypeKey First("studio.physics.Vector3");
  const Luna::StableTypeKey Same("studio.physics.Vector3");
  const Luna::StableTypeKey Other("studio.physics.Vector2");
  if (!(First == Same) || First.Hash() != Same.Hash())
    return false;
  if (First == Other || !(Other < First))
    return false;
  return true;
}

[[nodiscard]] bool VerifyReservedFixedKeys() {
  constexpr std::array FixedKeys{
      Luna::FixedTypeKey::Void,       Luna::FixedTypeKey::Boolean,
      Luna::FixedTypeKey::Int32,      Luna::FixedTypeKey::Float,
      Luna::FixedTypeKey::Double,     Luna::FixedTypeKey::String,
      Luna::FixedTypeKey::StringView, Luna::FixedTypeKey::CString,
      Luna::FixedTypeKey::Null,       Luna::FixedTypeKey::Value,
      Luna::FixedTypeKey::ValuePack,
  };
  std::vector<std::string_view> SeenTexts;
  for (const auto Key : FixedKeys) {
    const std::string_view Text = Luna::FixedTypeKeyText(Key);
    if (Text.substr(0, Luna::StableTypeKey::ReservedKeyPrefix.size()) !=
        Luna::StableTypeKey::ReservedKeyPrefix)
      return false;
    if (Luna::StableTypeKey::Classify(Text) !=
        Luna::StableTypeKeyStatus::ReservedPrefix)
      return false;
    for (const auto Seen : SeenTexts) {
      if (Seen == Text)
        return false;
    }
    SeenTexts.push_back(Text);
  }
  return true;
}

[[nodiscard]] bool VerifyIdentityFormatting() {
  const Luna::TypeId Unresolved;
  if (Unresolved.IsValid())
    return false;
  if (Unresolved.ToString() != std::string(Luna::TypeId::TextLength, '0'))
    return false;

  Luna::TypeId::Storage Bytes{};
  for (std::size_t Index = 0; Index < Luna::TypeId::ByteCount; ++Index)
    Bytes[Index] = static_cast<std::uint8_t>(Index * 8 + 10);
  const Luna::TypeId Identity = Luna::TypeId::FromBytes(Bytes);
  const std::string Text = Identity.ToString();
  if (Text.size() != Luna::TypeId::TextLength || !Identity.IsValid())
    return false;
  for (const char Character : Text) {
    const bool IsCanonicalDigit = (Character >= '0' && Character <= '9') ||
                                  (Character >= 'a' && Character <= 'f');
    if (!IsCanonicalDigit)
      return false;
  }
  if (Text.substr(0, 4) != "0a12")
    return false;

  const auto Parsed = Luna::TypeId::Parse(Text);
  if (!Parsed || *Parsed != Identity || Parsed->Bytes() != Bytes)
    return false;
  if (Luna::TypeId::Parse(Text.substr(1)) || Luna::TypeId::Parse(Text + "0") ||
      Luna::TypeId::Parse(std::string(Luna::TypeId::TextLength, 'A')) ||
      Luna::TypeId::Parse(std::string(Luna::TypeId::TextLength, 'g')))
    return false;

  const Luna::SymbolId Low = Luna::SymbolId::FromBytes(BytesWith(1));
  const Luna::SymbolId High = Luna::SymbolId::FromBytes(BytesWith(2));
  const Luna::SymbolId LowAgain = Luna::SymbolId::FromBytes(BytesWith(1));
  if (!(Low == LowAgain) || Low.Hash() != LowAgain.Hash())
    return false;
  if (!(Low < High) || !(High > Low) || Low == High)
    return false;
  if (Low.Hash() == High.Hash())
    return false;

  std::unordered_map<Luna::TypeId, int, Luna::CanonicalHash> Index;
  Index.emplace(Identity, 7);
  const auto Found = Index.find(Luna::TypeId::FromBytes(Bytes));
  return Found != Index.end() && Found->second == 7;
}

[[nodiscard]] bool VerifyNormalizedLeaves() {
  const Luna::TypeDescriptor Integer = Canonical<int>();
  if (Integer.Kind() != Luna::TypeKind::Fixed || !Integer.IsValid())
    return false;
  if (!Integer.FixedKey() || *Integer.FixedKey() != Luna::FixedTypeKey::Int32)
    return false;
  if (Integer.Qualification() != Luna::CvQualification::None)
    return false;

  if (Canonical<const int>() != Integer ||
      Canonical<const int &>() != Integer || Canonical<int &&>() != Integer ||
      Canonical<volatile int>() != Integer)
    return false;
  if (Canonical<const std::string &>() != Canonical<std::string>())
    return false;
  if (Canonical<std::string>().Hash() !=
      Canonical<const std::string &>().Hash())
    return false;

  if (Canonical<void>().FixedKey() != Luna::FixedTypeKey::Void ||
      Canonical<bool>().FixedKey() != Luna::FixedTypeKey::Boolean ||
      Canonical<float>().FixedKey() != Luna::FixedTypeKey::Float ||
      Canonical<double>().FixedKey() != Luna::FixedTypeKey::Double ||
      Canonical<std::string_view>().FixedKey() !=
          Luna::FixedTypeKey::StringView ||
      Canonical<std::nullptr_t>().FixedKey() != Luna::FixedTypeKey::Null ||
      Canonical<Luna::Value>().FixedKey() != Luna::FixedTypeKey::Value)
    return false;
  if (Canonical<const char *>().FixedKey() != Luna::FixedTypeKey::CString ||
      Canonical<char *>().FixedKey() != Luna::FixedTypeKey::CString)
    return false;

  if (Canonical<long>().IsValid() || Canonical<unsigned int>().IsValid() ||
      Canonical<char>().IsValid())
    return false;
  return true;
}

[[nodiscard]] bool VerifyStructuralNormalization() {
  const Luna::TypeDescriptor Pointer = Canonical<int *>();
  const Luna::TypeDescriptor ConstPointee = Canonical<const int *>();
  if (Pointer.Kind() != Luna::TypeKind::Pointer || Pointer.PointerDepth() != 1)
    return false;
  if (Pointer == ConstPointee)
    return false;
  if (ConstPointee.Children().front().Qualification() !=
      Luna::CvQualification::Const)
    return false;
  if (Canonical<int *const>() != Pointer)
    return false;
  if (Canonical<int **>().PointerDepth() != 2 || Canonical<int **>() == Pointer)
    return false;

  const Luna::TypeDescriptor Fixed4 = Canonical<int[4]>();
  if (Fixed4.Kind() != Luna::TypeKind::Array || Fixed4.ArrayExtent() != 4)
    return false;
  if (Fixed4 == Canonical<int[3]>())
    return false;
  if (Canonical<std::array<int, 4>>() != Fixed4)
    return false;
  if (Canonical<const int[4]>() != Fixed4)
    return false;
  const Luna::TypeDescriptor PointerToConstArray =
      Canonical<const int (*)[4]>();
  if (PointerToConstArray.Kind() != Luna::TypeKind::Pointer ||
      PointerToConstArray.Children().front().Qualification() !=
          Luna::CvQualification::Const ||
      PointerToConstArray == Canonical<int (*)[4]>())
    return false;

  const Luna::TypeDescriptor Sequence = Canonical<std::vector<int>>();
  if (Sequence.Kind() != Luna::TypeKind::Sequence ||
      Sequence.ChildCount() != 1 ||
      Sequence.Children().front() != Canonical<int>())
    return false;
  if (Canonical<std::deque<int>>() != Sequence ||
      Canonical<std::list<int>>() != Sequence)
    return false;
  if (Canonical<std::vector<double>>() == Sequence)
    return false;

  const Luna::TypeDescriptor Mapping = Canonical<std::map<std::string, int>>();
  if (Mapping.Kind() != Luna::TypeKind::Map || Mapping.ChildCount() != 2)
    return false;
  if (Mapping.Children()[0] != Canonical<std::string>() ||
      Mapping.Children()[1] != Canonical<int>())
    return false;
  if (Canonical<std::unordered_map<std::string, int>>() != Mapping)
    return false;
  if (Canonical<std::map<int, std::string>>() == Mapping)
    return false;

  if (Canonical<std::optional<int>>() == Canonical<int>() ||
      Canonical<std::optional<int>>().Kind() != Luna::TypeKind::Optional)
    return false;
  if (Canonical<std::pair<int, double>>() ==
      Canonical<std::tuple<int, double>>())
    return false;
  if (Canonical<std::tuple<int, double>>() ==
      Canonical<std::tuple<double, int>>())
    return false;
  if (Canonical<std::tuple<>>().Kind() != Luna::TypeKind::Tuple ||
      !Canonical<std::tuple<>>().IsValid())
    return false;
  if (Canonical<std::reference_wrapper<int>>().Kind() !=
      Luna::TypeKind::BorrowedReference)
    return false;

  const Luna::TypeDescriptor Nested =
      Canonical<std::vector<std::optional<std::pair<int, std::string>>>>();
  if (!Nested.IsValid() || Nested.Kind() != Luna::TypeKind::Sequence)
    return false;
  const Luna::TypeDescriptor &Optional = Nested.Children().front();
  const Luna::TypeDescriptor &Pair = Optional.Children().front();
  if (Optional.Kind() != Luna::TypeKind::Optional ||
      Pair.Kind() != Luna::TypeKind::Pair || Pair.ChildCount() != 2)
    return false;
  return Nested ==
         Canonical<std::vector<std::optional<std::pair<int, std::string>>>>();
}

[[nodiscard]] bool VerifyUserDefinedLeafKeys() {
  static_assert(Luna::Detail::UserDefinedLeafCount<int> == 0);
  static_assert(Luna::Detail::UserDefinedLeafCount<Widget> == 1);
  static_assert(Luna::Detail::UserDefinedLeafCount<const Widget &> == 1);
  static_assert(Luna::Detail::UserDefinedLeafCount<std::vector<Widget>> == 1);
  static_assert(Luna::Detail::UserDefinedLeafCount<std::map<Color, Widget>> ==
                2);
  static_assert(Luna::Detail::UserDefinedLeafCount<std::shared_ptr<Widget>> ==
                1);

  const KeyList WidgetKey{Luna::StableTypeKey("studio.ui.Widget")};
  const Luna::TypeDescriptor Class = Canonical<Widget>(WidgetKey);
  if (Class.Kind() != Luna::TypeKind::Class || !Class.IsValid())
    return false;
  if (Class.Key() != Luna::StableTypeKey("studio.ui.Widget"))
    return false;
  if (Canonical<const Widget &>(WidgetKey) != Class)
    return false;

  const KeyList ColorKey{Luna::StableTypeKey("studio.ui.Color")};
  const Luna::TypeDescriptor Enumeration = Canonical<Color>(ColorKey);
  if (Enumeration.Kind() != Luna::TypeKind::Enumeration ||
      !Enumeration.IsValid())
    return false;
  if (Canonical<Color>(WidgetKey) == Class)
    return false;

  const KeyList Ordered{Luna::StableTypeKey("studio.ui.Color"),
                        Luna::StableTypeKey("studio.ui.Widget")};
  const Luna::TypeDescriptor Mapping =
      Canonical<std::map<Color, Widget>>(Ordered);
  if (!Mapping.IsValid() || Mapping.Children()[0] != Enumeration ||
      Mapping.Children()[1] != Class)
    return false;
  const KeyList Swapped{Luna::StableTypeKey("studio.ui.Widget"),
                        Luna::StableTypeKey("studio.ui.Color")};
  if (Canonical<std::map<Color, Widget>>(Swapped) == Mapping)
    return false;

  if (Canonical<Widget>().IsValid())
    return false;
  if (Canonical<std::map<Color, Widget>>(ColorKey).IsValid())
    return false;
  if (Canonical<Widget>(KeyList{Luna::StableTypeKey("luna.void")}).IsValid())
    return false;
  if (Canonical<Widget>(KeyList{Luna::StableTypeKey("1invalid")}).IsValid())
    return false;
  if (Canonical<Widget>(Ordered).IsValid())
    return false;

  const Luna::TypeDescriptor Shared =
      Canonical<std::shared_ptr<Widget>>(WidgetKey);
  if (Shared.Kind() != Luna::TypeKind::SharedOwnership || !Shared.IsValid())
    return false;
  if (Shared == Class)
    return false;

  const KeyList RebuiltKey{
      Luna::StableTypeKey(std::string("studio.ui.") + "Widget")};
  return Canonical<Widget>(RebuiltKey) == Class &&
         Canonical<Widget>(RebuiltKey).Hash() == Class.Hash();
}

[[nodiscard]] bool VerifyDescriptorConstruction() {
  if (Luna::TypeDescriptor().IsValid() ||
      Luna::TypeDescriptor().Kind() != Luna::TypeKind::Unsupported)
    return false;
  if (Luna::TypeDescriptor::Unsupported() != Luna::TypeDescriptor())
    return false;

  std::vector<Luna::TypeDescriptor> One;
  One.push_back(Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::Int32));
  if (Luna::TypeDescriptor::ForStructure(Luna::TypeKind::Pair, One).IsValid())
    return false;
  if (Luna::TypeDescriptor::ForStructure(Luna::TypeKind::Unsupported, {})
          .IsValid())
    return false;
  if (!Luna::TypeDescriptor::ForStructure(Luna::TypeKind::Optional, One)
           .IsValid())
    return false;

  if (Luna::TypeDescriptor::ForClass(Luna::StableTypeKey("1invalid")).IsValid())
    return false;
  if (Luna::TypeDescriptor::ForEnumeration(Luna::StableTypeKey()).IsValid())
    return false;

  std::vector<Luna::TypeDescriptor> WithUnsupported;
  WithUnsupported.push_back(Luna::TypeDescriptor::Unsupported());
  if (Luna::TypeDescriptor::ForStructure(Luna::TypeKind::Sequence,
                                         WithUnsupported)
          .IsValid())
    return false;

  const Luna::TypeDescriptor Fixed =
      Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::Double);
  if (Fixed != Canonical<double>() ||
      Fixed.Hash() != Canonical<double>().Hash())
    return false;
  if (Luna::TypeKindText(Luna::TypeKind::Map) != "map" ||
      Luna::SymbolKindText(Luna::SymbolKind::OverloadSet) != "overload_set" ||
      Luna::CvQualificationText(Luna::CvQualification::ConstVolatile) !=
          "const_volatile" ||
      Luna::StableTypeKeyStatusText(Luna::StableTypeKeyStatus::Valid) !=
          "valid")
    return false;

  const Luna::TypeDescriptor Integer = Canonical<int>();
  const Luna::TypeDescriptor Sequence = Canonical<std::vector<int>>();
  if (!(Integer < Sequence) || !(Sequence > Integer) || Integer == Sequence)
    return false;
  return true;
}

} // namespace

int RunCanonicalIdentityModelTests() {
  if (!VerifyStableKeyGrammar())
    return 1;
  if (!VerifyReservedFixedKeys())
    return 2;
  if (!VerifyIdentityFormatting())
    return 3;
  if (!VerifyNormalizedLeaves())
    return 4;
  if (!VerifyStructuralNormalization())
    return 5;
  if (!VerifyUserDefinedLeafKeys())
    return 6;
  if (!VerifyDescriptorConstruction())
    return 7;
  return 0;
}
