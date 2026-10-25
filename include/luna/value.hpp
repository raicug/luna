#pragma once

// Values whose shape is decided at runtime, owned or viewed.

#include <luna/fwd.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace luna {
enum class ValueCategory : std::uint8_t {
  None = 0,
  Nil,
  Boolean,
  Number,
  String,
  Table,
  Userdata,
  Function,
  Thread,
};

class Value {
public:
  Value() = default;
  [[nodiscard]] ValueCategory Category() const noexcept { return Kind; }
  [[nodiscard]] bool IsNil() const noexcept {
    return Kind == ValueCategory::Nil;
  }
  [[nodiscard]] bool IsNumber() const noexcept {
    return Kind == ValueCategory::Number;
  }
  [[nodiscard]] bool IsString() const noexcept {
    return Kind == ValueCategory::String;
  }
  [[nodiscard]] bool IsBoolean() const noexcept {
    return Kind == ValueCategory::Boolean;
  }
  [[nodiscard]] bool IsTable() const noexcept {
    return Kind == ValueCategory::Table;
  }
  [[nodiscard]] bool IsUserdata() const noexcept {
    return Kind == ValueCategory::Userdata;
  }
  [[nodiscard]] bool IsFunction() const noexcept {
    return Kind == ValueCategory::Function;
  }

  [[nodiscard]] double AsNumber(double Fallback = 0.0) const noexcept {
    return Kind == ValueCategory::Number ? Number_ : Fallback;
  }
  [[nodiscard]] bool AsBoolean(bool Fallback = false) const noexcept {
    return Kind == ValueCategory::Boolean ? Flag : Fallback;
  }

  [[nodiscard]] std::string_view AsString() const noexcept {
    return Kind == ValueCategory::String ? Text : std::string_view{};
  }

  template <class F> decltype(auto) Visit(F &&Visitor) const {
    return Visitor(*this);
  }

private:
  friend class ArgumentPack;
  ValueCategory Kind = ValueCategory::None;
  double Number_ = 0.0;
  bool Flag = false;
  std::string_view Text;
};

class OwnedValue;
struct OwnedTable;

class OwnedValue {
public:
  // A userdata, as a script handed it over. Object is set only when the host
  // shares it, which is also what lets it go back into Lua; the rest describes
  // it as it was when it was read.
  struct InstanceRef {
    std::shared_ptr<void> Object;
    const void *TypeToken = nullptr;
    std::string ClassName;
    std::string Text;
    const void *Pointer = nullptr;
    bool Live = true;
  };

  // A value with no host-side form: a function or a thread.
  struct Opaque {
    ValueCategory Category = ValueCategory::None;
  };

  using Payload =
      std::variant<std::monostate, bool, double, std::string,
                   std::shared_ptr<OwnedTable>, InstanceRef, Opaque>;

  OwnedValue() = default;
  OwnedValue(bool Value) : Data(Value) {}
  OwnedValue(double Value) : Data(Value) {}
  OwnedValue(float Value) : Data(static_cast<double>(Value)) {}
  OwnedValue(int Value) : Data(static_cast<double>(Value)) {}
  OwnedValue(long Value) : Data(static_cast<double>(Value)) {}
  OwnedValue(long long Value) : Data(static_cast<double>(Value)) {}
  OwnedValue(unsigned Value) : Data(static_cast<double>(Value)) {}
  OwnedValue(std::string Value) : Data(std::move(Value)) {}
  OwnedValue(std::string_view Value) : Data(std::string(Value)) {}
  OwnedValue(const char *Value) : Data(std::string(Value ? Value : "")) {}

  static OwnedValue Table();
  // An instance the host already holds a shared pointer to, by class token.
  static OwnedValue FromInstance(std::shared_ptr<void> Object,
                                 const void *TypeToken) {
    OwnedValue Result;
    Result.Data = InstanceRef{std::move(Object), TypeToken};
    return Result;
  }
  static OwnedValue FromUserdata(InstanceRef Described) {
    OwnedValue Result;
    Result.Data = std::move(Described);
    return Result;
  }
  static OwnedValue FromOpaque(ValueCategory Category) {
    OwnedValue Result;
    Result.Data = Opaque{Category};
    return Result;
  }
  static OwnedValue Nil() { return OwnedValue(); }
  static OwnedValue Boolean(bool Value) { return OwnedValue(Value); }
  static OwnedValue Number(double Value) { return OwnedValue(Value); }
  static OwnedValue Text(std::string_view Value) { return OwnedValue(Value); }

  template <class T> static OwnedValue Instance(T Value) {
    return Instance(std::make_shared<T>(std::move(Value)));
  }

  template <class T> static OwnedValue Instance(std::shared_ptr<T> Value) {
    OwnedValue Result;
    T *Raw = Value.get();
    Result.Data = InstanceRef{std::shared_ptr<void>(std::move(Value)),
                              static_cast<const void *>(&detail::TypeToken<T>),
                              std::string(), std::string(), Raw, true};
    return Result;
  }

  // What a value read from a script is, for code that inspects it.
  [[nodiscard]] ValueCategory Kind() const noexcept {
    if (IsNil())
      return ValueCategory::Nil;
    if (IsBoolean())
      return ValueCategory::Boolean;
    if (IsNumber())
      return ValueCategory::Number;
    if (IsString())
      return ValueCategory::String;
    if (IsTable())
      return ValueCategory::Table;
    if (IsInstance())
      return ValueCategory::Userdata;
    return std::get<Opaque>(Data).Category;
  }
  [[nodiscard]] std::optional<double> ToNumber() const noexcept {
    return IsNumber() ? std::optional<double>(AsNumber()) : std::nullopt;
  }
  [[nodiscard]] std::optional<bool> ToBoolean() const noexcept {
    return IsBoolean() ? std::optional<bool>(AsBoolean()) : std::nullopt;
  }
  // A string, number or boolean as text; nothing for the rest.
  [[nodiscard]] std::optional<std::string> ToText() const;
  [[nodiscard]] std::string_view TextBytes() const noexcept {
    return AsString();
  }

  // A table: the sequence, then the named fields. Out of range reads as nil.
  [[nodiscard]] std::size_t Size() const noexcept;
  [[nodiscard]] std::size_t FieldCount() const noexcept;
  [[nodiscard]] const OwnedValue &Element(std::size_t Index) const noexcept;
  [[nodiscard]] std::string_view FieldName(std::size_t Index) const noexcept;
  [[nodiscard]] const OwnedValue &Field(std::string_view Name) const noexcept;

  // A userdata: its class, its tostring text, whether it was usable when read,
  // and the object it wrapped.
  [[nodiscard]] bool IsUserdata() const noexcept { return IsInstance(); }
  [[nodiscard]] std::string_view UserdataClassName() const noexcept {
    const InstanceRef *Ref = AsInstance();
    return Ref ? std::string_view(Ref->ClassName) : std::string_view();
  }
  [[nodiscard]] std::string_view UserdataText() const noexcept {
    const InstanceRef *Ref = AsInstance();
    return Ref ? std::string_view(Ref->Text) : std::string_view();
  }
  [[nodiscard]] bool UserdataIsLive() const noexcept {
    const InstanceRef *Ref = AsInstance();
    return Ref && Ref->Live;
  }
  [[nodiscard]] const void *UserdataTarget() const noexcept {
    const InstanceRef *Ref = AsInstance();
    return Ref ? Ref->Pointer : nullptr;
  }

  [[nodiscard]] bool IsTable() const noexcept {
    return std::holds_alternative<std::shared_ptr<OwnedTable>>(Data);
  }
  [[nodiscard]] bool IsNil() const noexcept {
    return std::holds_alternative<std::monostate>(Data);
  }
  [[nodiscard]] bool IsInstance() const noexcept {
    return std::holds_alternative<InstanceRef>(Data);
  }
  [[nodiscard]] bool IsString() const noexcept {
    return std::holds_alternative<std::string>(Data);
  }
  [[nodiscard]] bool IsNumber() const noexcept {
    return std::holds_alternative<double>(Data);
  }
  [[nodiscard]] bool IsBoolean() const noexcept {
    return std::holds_alternative<bool>(Data);
  }

  [[nodiscard]] double AsNumber(double Fallback = 0.0) const noexcept {
    const double *Found = std::get_if<double>(&Data);
    return Found ? *Found : Fallback;
  }
  [[nodiscard]] bool AsBoolean(bool Fallback = false) const noexcept {
    const bool *Found = std::get_if<bool>(&Data);
    return Found ? *Found : Fallback;
  }
  [[nodiscard]] std::string_view AsString() const noexcept {
    const std::string *Found = std::get_if<std::string>(&Data);
    return Found ? std::string_view(*Found) : std::string_view{};
  }
  [[nodiscard]] const OwnedTable *AsTable() const noexcept {
    const std::shared_ptr<OwnedTable> *Found =
        std::get_if<std::shared_ptr<OwnedTable>>(&Data);
    return Found && *Found ? Found->get() : nullptr;
  }
  [[nodiscard]] const InstanceRef *AsInstance() const noexcept {
    return std::get_if<InstanceRef>(&Data);
  }

  OwnedValue &SetField(std::string_view Name, OwnedValue FieldValue);
  OwnedValue &Append(OwnedValue Item);
  [[nodiscard]] const Payload &Get() const noexcept { return Data; }

private:
  Payload Data;
};

struct OwnedTable {
  struct Entry {
    std::string Key;
    OwnedValue Value;
  };
  std::vector<Entry> Fields;
  std::vector<OwnedValue> Items;
};

class ValuePack {
public:
  ValuePack() = default;
  explicit ValuePack(std::vector<OwnedValue> Values)
      : Values_(std::move(Values)) {}

  ValuePack &Append(OwnedValue Value) {
    Values_.push_back(std::move(Value));
    return *this;
  }

  [[nodiscard]] std::size_t Size() const noexcept { return Values_.size(); }
  [[nodiscard]] bool Empty() const noexcept { return Values_.empty(); }

  [[nodiscard]] const OwnedValue &At(std::size_t Index) const noexcept {
    static const OwnedValue Nil;
    return Index < Values_.size() ? Values_[Index] : Nil;
  }

  [[nodiscard]] const std::vector<OwnedValue> &Values() const noexcept {
    return Values_;
  }

private:
  std::vector<OwnedValue> Values_;
};

class ArgumentPack {
public:
  ArgumentPack() = default;
  ArgumentPack(void *VM, int First, int Count) noexcept
      : VM_(VM), First_(First), Count_(Count) {}

  [[nodiscard]] std::size_t Size() const noexcept {
    return Count_ > 0 ? static_cast<std::size_t>(Count_) : 0;
  }
  [[nodiscard]] bool Empty() const noexcept { return Size() == 0; }
  [[nodiscard]] void *GetVM() const noexcept { return VM_; }

  [[nodiscard]] OwnedValue At(std::size_t Index) const;
  [[nodiscard]] Value View(std::size_t Index) const;

private:
  void *VM_ = nullptr;
  int First_ = 0;
  int Count_ = 0;
};

template <class T> struct IsOwnedValue : std::false_type {};
template <> struct IsOwnedValue<OwnedValue> : std::true_type {};

template <class T> struct IsValuePack : std::false_type {};
template <> struct IsValuePack<ValuePack> : std::true_type {};

template <class T> struct IsArgumentPack : std::false_type {};
template <> struct IsArgumentPack<ArgumentPack> : std::true_type {};

template <class T> struct IsValue : std::false_type {};
template <> struct IsValue<Value> : std::true_type {};

template <class T>
inline constexpr bool IsOwnedValueV = IsOwnedValue<std::remove_cv_t<T>>::value;
template <class T>
inline constexpr bool IsValuePackV = IsValuePack<std::remove_cv_t<T>>::value;
template <class T>
inline constexpr bool IsArgumentPackV =
    IsArgumentPack<std::remove_cv_t<T>>::value;
template <class T>
inline constexpr bool IsValueV = IsValue<std::remove_cv_t<T>>::value;
} // namespace luna
