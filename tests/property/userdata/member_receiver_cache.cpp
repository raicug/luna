// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/class_member.hpp>
#include <luna/binding/value.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/core/results/execution_result.hpp>
#include <luna/core/results/registration_result.hpp>
#include <luna/state/state.hpp>
#include <luna/type/stable_type_key.hpp>

#include "state/testing/fault_point.hpp"
#include "state/testing/test_hooks.hpp"
#include "state/userdata/header.hpp"
#include "state/userdata/lazy_cache.hpp"
#include "state/userdata/member_access.hpp"
#include "state/userdata/member_dispatch.hpp"
#include "state/userdata/ownership.hpp"

#include <rapidcheck.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;
using Luna::Detail::ConstAccess;
using Luna::Detail::LazyCacheCounters;
using Luna::Detail::MemberDispatchStageText;
using Luna::Detail::OwnershipModel;
using Luna::Detail::StateFaultPoint;

class ByteCursor final {
public:
  explicit ByteCursor(const std::vector<std::uint8_t> &Bytes) noexcept
      : BytesValue(&Bytes) {}

  [[nodiscard]] std::uint8_t Next() noexcept {
    const std::size_t Index = IndexValue++;
    if (BytesValue->empty())
      return static_cast<std::uint8_t>(Index * 41U + 7U);
    return (*BytesValue)[Index % BytesValue->size()];
  }

  [[nodiscard]] std::size_t Pick(std::size_t Count) noexcept {
    return Count == 0 ? 0 : static_cast<std::size_t>(Next()) % Count;
  }

private:
  const std::vector<std::uint8_t> *BytesValue;
  std::size_t IndexValue = 0;
};

std::size_t LevelReads = 0;
std::size_t LevelWrites = 0;
std::size_t WeightReads = 0;
std::size_t ExpensiveReads = 0;
std::size_t TunedReads = 0;
std::size_t TunedWrites = 0;
std::size_t HiddenWrites = 0;
std::size_t MarkedWrites = 0;
std::size_t GrowCalls = 0;

void ResetAccessorCounters() {
  LevelReads = 0;
  LevelWrites = 0;
  WeightReads = 0;
  ExpensiveReads = 0;
  TunedReads = 0;
  TunedWrites = 0;
  HiddenWrites = 0;
  MarkedWrites = 0;
  GrowCalls = 0;
}

struct Gadget final {
  int Charge = 3;
  const int Serial = 42;
  int Trace = 0;

  bool LazyGetterRefuses = false;
  bool MarkingSetterRefuses = false;

  [[nodiscard]] int Level() const {
    ++LevelReads;
    return Charge * 2;
  }

  void SetLevel(int Value) {
    ++LevelWrites;
    Charge = Value;
  }

  [[nodiscard]] double Weight() {
    ++WeightReads;
    return static_cast<double>(Charge) + 0.5;
  }

  [[nodiscard]] int Expensive() const {
    ++ExpensiveReads;
    if (LazyGetterRefuses)
      throw std::runtime_error("the expensive getter refused");
    return Charge + 100;
  }

  [[nodiscard]] int Tuned() const {
    ++TunedReads;
    return Charge + 200;
  }

  void SetTuned(int Value) {
    ++TunedWrites;
    Charge = Value;
  }

  void SetHidden(int Value) {
    ++HiddenWrites;
    Charge = Value;
  }

  void SetMarked(int Value) {
    ++MarkedWrites;
    Trace = Value;
    if (MarkingSetterRefuses)
      throw std::runtime_error("the marking setter refused after mutating");
  }

  [[nodiscard]] int Grow(int By) {
    ++GrowCalls;
    Charge += By;
    return Charge;
  }
};

[[nodiscard]] Luna::RegistrationResult
RegisterGadget(Luna::BindingRegistry &Registry) {
  Luna::ClassBuilder<Gadget> Class = Registry.RegisterClass<Gadget>(
      "Gadget", Luna::StableTypeKey("Studio.PropertyGadget"));
  Luna::ClassBuilder<Gadget> &WithLevel =
      Class.Property("Level", &Gadget::Level, &Gadget::SetLevel);
  Luna::ClassBuilder<Gadget> &WithWeight =
      WithLevel.Property("Weight", &Gadget::Weight);
  Luna::ClassBuilder<Gadget> &WithExpensive = WithWeight.Property(
      "Expensive", Luna::PropertyPolicy::Lazy(), &Gadget::Expensive);
  Luna::ClassBuilder<Gadget> &WithTuned =
      WithExpensive.Property("Tuned", Luna::PropertyPolicy::LazyReadWrite(),
                             &Gadget::Tuned, &Gadget::SetTuned);
  Luna::ClassBuilder<Gadget> &WithHidden = WithTuned.Property(
      "Hidden", Luna::PropertyPolicy::WriteOnly(), &Gadget::SetHidden);
  Luna::ClassBuilder<Gadget> &WithMarked = WithHidden.Property(
      "Marked", Luna::PropertyPolicy::WriteOnly(), &Gadget::SetMarked);
  Luna::ClassBuilder<Gadget> &WithCharge =
      WithMarked.Field("Charge", &Gadget::Charge);
  Luna::ClassBuilder<Gadget> &WithSerial =
      WithCharge.Field("Serial", &Gadget::Serial);
  Luna::ClassBuilder<Gadget> &WithGrow =
      WithSerial.Method("Grow", &Gadget::Grow);
  return WithGrow.Commit();
}

enum class MemberSlot : std::uint8_t {
  Level,
  Weight,
  Expensive,
  Tuned,
  Hidden,
  Marked,
  Charge,
  Serial,
  Missing
};

struct MemberTraits final {
  const char *Name;
  bool Known;
  bool Readable;
  bool Writable;
  bool Lazy;
  bool ReadNeedsMutableReceiver;
  bool ProducesDouble;
};

constexpr MemberTraits DeclaredMembers[] = {
    {"Level", true, true, true, false, false, false},
    {"Weight", true, true, false, false, true, true},
    {"Expensive", true, true, false, true, false, false},
    {"Tuned", true, true, true, true, false, false},
    {"Hidden", true, false, true, false, false, false},
    {"Marked", true, false, true, false, false, false},
    {"Charge", true, true, true, false, false, false},
    {"Serial", true, true, false, false, false, false},
    {"Missing", false, false, false, false, false, false}};

[[nodiscard]] const MemberTraits &TraitsOf(MemberSlot Slot) noexcept {
  return DeclaredMembers[static_cast<std::size_t>(Slot)];
}

[[nodiscard]] std::string NameOf(MemberSlot Slot) {
  return std::string(TraitsOf(Slot).Name);
}

enum class ModelFailure : std::uint8_t {
  None,
  UnknownMember,
  RefusedReceiver,
  UnreadableMember,
  UnwritableMember,
  IncompatibleValue,
  ContainedException
};

[[nodiscard]] std::string_view FailureText(ModelFailure Failure) noexcept {
  switch (Failure) {
  case ModelFailure::None:
    return "none";
  case ModelFailure::UnknownMember:
    return "unknown_member";
  case ModelFailure::RefusedReceiver:
    return "refused_receiver";
  case ModelFailure::UnreadableMember:
    return "unreadable_member";
  case ModelFailure::UnwritableMember:
    return "unwritable_member";
  case ModelFailure::IncompatibleValue:
    return "incompatible_value";
  case ModelFailure::ContainedException:
    break;
  }
  return "contained_exception";
}

[[nodiscard]] std::string_view StageTextOf(ModelFailure Failure) noexcept {
  switch (Failure) {
  case ModelFailure::None:
    return "published";
  case ModelFailure::UnknownMember:
    return "unknown_member";
  case ModelFailure::RefusedReceiver:
    return "receiver";
  case ModelFailure::UnreadableMember:
  case ModelFailure::UnwritableMember:
    return "direction";
  case ModelFailure::IncompatibleValue:
    return "value";
  case ModelFailure::ContainedException:
    break;
  }
  return "target";
}

[[nodiscard]] std::string_view BoundaryTextOf(ModelFailure Failure) noexcept {
  return Failure == ModelFailure::ContainedException ? "after_user_code"
                                                     : "before_user_code";
}

struct ModelEntry final {
  MemberSlot Member = MemberSlot::Expensive;
  int Cached = 0;
};

struct ModelCache final {
  bool HasNode = false;
  std::uint64_t NodeGeneration = 0;
  std::vector<ModelEntry> Entries;
  bool SlotPopulated = false;
  std::uint64_t SlotGeneration = 0;
};

struct ModelObject final {
  bool IsConst = false;
  bool Expired = false;
  bool Retired = false;
  bool LazyGetterRefuses = false;
  bool MarkingSetterRefuses = false;
  int Charge = 3;
  int Trace = 0;
  ModelCache Cache;
};

struct ModelAccessorCounters final {
  std::size_t LevelReads = 0;
  std::size_t LevelWrites = 0;
  std::size_t WeightReads = 0;
  std::size_t ExpensiveReads = 0;
  std::size_t TunedReads = 0;
  std::size_t TunedWrites = 0;
  std::size_t HiddenWrites = 0;
  std::size_t MarkedWrites = 0;
  std::size_t GrowCalls = 0;
};

struct ModelWorld final {
  std::vector<ModelObject> Objects;
  std::uint64_t Generation = 0;
  ModelAccessorCounters Accessors;
  LazyCacheCounters Cache;
  std::size_t PendingPublicationFaults = 0;
};

[[nodiscard]] std::string_view
ModelReceiverRefusal(const ModelObject &Object,
                     bool RequiresMutation) noexcept {
  if (Object.Expired)
    return "expired_lifetime_handle";
  if (Object.Retired)
    return "invalidated";
  if (Object.IsConst && RequiresMutation)
    return "const_violation";
  return "none";
}

[[nodiscard]] const ModelEntry *FindEntry(const ModelCache &Cache,
                                          MemberSlot Member) noexcept {
  for (const ModelEntry &Entry : Cache.Entries) {
    if (Entry.Member == Member)
      return &Entry;
  }
  return nullptr;
}

[[nodiscard]] std::size_t EntryCountOf(const ModelObject &Object) noexcept {
  return Object.Cache.Entries.size();
}

[[nodiscard]] int ProducedIntegerOf(const ModelObject &Object,
                                    MemberSlot Member) noexcept {
  switch (Member) {
  case MemberSlot::Level:
    return Object.Charge * 2;
  case MemberSlot::Expensive:
    return Object.Charge + 100;
  case MemberSlot::Tuned:
    return Object.Charge + 200;
  case MemberSlot::Charge:
    return Object.Charge;
  case MemberSlot::Serial:
    return 42;
  default:
    break;
  }
  return 0;
}

struct ModelReadOutcome final {
  ModelFailure Failure = ModelFailure::None;
  std::string Receiver = "none";
  bool ServedFromCache = false;
  bool Recorded = false;
  int Produced = 0;
  double ProducedDouble = 0.0;
};

[[nodiscard]] ModelReadOutcome
ModelRead(ModelWorld &World, std::size_t ObjectIndex, MemberSlot Member) {
  ModelObject &Object = World.Objects[ObjectIndex];
  const MemberTraits &Traits = TraitsOf(Member);

  ModelReadOutcome Outcome;
  if (!Traits.Known) {
    Outcome.Failure = ModelFailure::UnknownMember;
    return Outcome;
  }

  const std::string_view Refusal =
      ModelReceiverRefusal(Object, Traits.ReadNeedsMutableReceiver);
  if (Refusal != "none") {
    Outcome.Failure = ModelFailure::RefusedReceiver;
    Outcome.Receiver = std::string(Refusal);
    return Outcome;
  }

  if (!Traits.Readable) {
    Outcome.Failure = ModelFailure::UnreadableMember;
    return Outcome;
  }

  if (Traits.Lazy) {
    if (!Object.Cache.SlotPopulated || !Object.Cache.HasNode) {
      ++World.Cache.Miss;
    } else if (Object.Cache.NodeGeneration != World.Generation ||
               Object.Cache.SlotGeneration != World.Generation) {
      ++World.Cache.GenerationMismatch;
    } else if (const ModelEntry *Entry = FindEntry(Object.Cache, Member)) {
      ++World.Cache.Hit;
      Outcome.ServedFromCache = true;
      Outcome.Produced = Entry->Cached;
      return Outcome;
    } else {
      ++World.Cache.Miss;
    }
  }

  switch (Member) {
  case MemberSlot::Level:
    ++World.Accessors.LevelReads;
    break;
  case MemberSlot::Weight:
    ++World.Accessors.WeightReads;
    break;
  case MemberSlot::Expensive:
    ++World.Accessors.ExpensiveReads;
    break;
  case MemberSlot::Tuned:
    ++World.Accessors.TunedReads;
    break;
  default:
    break;
  }

  if (Member == MemberSlot::Expensive && Object.LazyGetterRefuses) {
    Outcome.Failure = ModelFailure::ContainedException;
    return Outcome;
  }

  if (Traits.ProducesDouble)
    Outcome.ProducedDouble = static_cast<double>(Object.Charge) + 0.5;
  else
    Outcome.Produced = ProducedIntegerOf(Object, Member);

  if (!Traits.Lazy)
    return Outcome;

  ModelCache &Cache = Object.Cache;
  if (!Cache.HasNode) {
    Cache.HasNode = true;
    Cache.NodeGeneration = World.Generation;
    Cache.Entries.clear();
  } else if (Cache.NodeGeneration != World.Generation) {
    Cache.Entries.clear();
    Cache.NodeGeneration = World.Generation;
  }

  bool Replaced = false;
  for (ModelEntry &Entry : Cache.Entries) {
    if (Entry.Member != Member)
      continue;
    Entry.Cached = Outcome.Produced;
    Replaced = true;
    break;
  }
  if (Replaced) {
    ++World.Cache.Replace;
  } else {
    Cache.Entries.push_back(ModelEntry{Member, Outcome.Produced});
    ++World.Cache.Store;
  }
  Cache.SlotPopulated = true;
  Cache.SlotGeneration = World.Generation;
  Outcome.Recorded = true;
  return Outcome;
}

struct ModelWriteOutcome final {
  ModelFailure Failure = ModelFailure::None;
  std::string Receiver = "none";
  std::size_t Invalidated = 0;
};

[[nodiscard]] std::size_t ModelInvalidateOwner(ModelWorld &World,
                                               ModelObject &Object) {
  if (!Object.Cache.HasNode)
    return 0;

  const std::size_t Removed = Object.Cache.Entries.size();
  Object.Cache.Entries.clear();
  if (Removed != 0)
    ++World.Cache.Invalidate;
  Object.Cache.SlotPopulated = false;
  Object.Cache.SlotGeneration = 0;
  return Removed;
}

[[nodiscard]] ModelWriteOutcome ModelWrite(ModelWorld &World,
                                           std::size_t ObjectIndex,
                                           MemberSlot Member, bool Mistyped,
                                           int Written) {
  ModelObject &Object = World.Objects[ObjectIndex];
  const MemberTraits &Traits = TraitsOf(Member);

  ModelWriteOutcome Outcome;
  if (!Traits.Known) {
    Outcome.Failure = ModelFailure::UnknownMember;
    return Outcome;
  }

  const std::string_view Refusal = ModelReceiverRefusal(Object, true);
  if (Refusal != "none") {
    Outcome.Failure = ModelFailure::RefusedReceiver;
    Outcome.Receiver = std::string(Refusal);
    return Outcome;
  }

  if (!Traits.Writable) {
    Outcome.Failure = ModelFailure::UnwritableMember;
    return Outcome;
  }

  if (Mistyped) {
    Outcome.Failure = ModelFailure::IncompatibleValue;
    return Outcome;
  }

  switch (Member) {
  case MemberSlot::Level:
    ++World.Accessors.LevelWrites;
    Object.Charge = Written;
    break;
  case MemberSlot::Tuned:
    ++World.Accessors.TunedWrites;
    Object.Charge = Written;
    break;
  case MemberSlot::Hidden:
    ++World.Accessors.HiddenWrites;
    Object.Charge = Written;
    break;
  case MemberSlot::Charge:
    Object.Charge = Written;
    break;
  case MemberSlot::Marked:
    ++World.Accessors.MarkedWrites;
    Object.Trace = Written;
    if (Object.MarkingSetterRefuses) {
      Outcome.Failure = ModelFailure::ContainedException;
      return Outcome;
    }
    break;
  default:
    break;
  }

  Outcome.Invalidated = ModelInvalidateOwner(World, Object);
  return Outcome;
}

void ModelRetire(ModelWorld &World, ModelObject &Object) {
  if (Object.Cache.HasNode) {
    ++World.Cache.Drop;
    Object.Cache.HasNode = false;
    Object.Cache.NodeGeneration = 0;
    Object.Cache.Entries.clear();
  }
  Object.Cache.SlotPopulated = false;
  Object.Cache.SlotGeneration = 0;
  Object.Retired = true;
}

} // namespace

namespace {

[[nodiscard]] std::string PathOf(std::size_t Index) {
  return "Value_" + std::to_string(Index);
}

[[nodiscard]] std::string ExposeGadget(Luna::State &Owner,
                                       const std::string &Path, Gadget &Object,
                                       const std::uint64_t *Generation,
                                       ConstAccess Access) {
  Luna::Detail::ClassExposureRequest Request;
  Request.QualifiedName = "Gadget";
  Request.Path = Path;
  Request.Storage = &Object;
  Request.Ownership = OwnershipModel::Borrowed;
  Request.Access = Access;
  Request.LifetimeGeneration = Generation;
  return Hooks::ExposeClassUserdata(Owner, Request).Status;
}

[[nodiscard]] Luna::Detail::ClassMemberAccessObservation
ReadMember(Luna::State &Owner, const std::string &Path,
           const std::string &Member) {
  Luna::Detail::ClassMemberAccessRequest Request;
  Request.QualifiedName = "Gadget";
  Request.Member = Member;
  Request.Path = Path;
  return Hooks::ReadClassMemberValue(Owner, Request);
}

[[nodiscard]] Luna::Detail::ClassMemberAccessObservation
WriteMember(Luna::State &Owner, const std::string &Path,
            const std::string &Member, Luna::Value Incoming) {
  Luna::Detail::ClassMemberAccessRequest Request;
  Request.QualifiedName = "Gadget";
  Request.Member = Member;
  Request.Path = Path;
  Request.Incoming = std::move(Incoming);
  return Hooks::WriteClassMemberValue(Owner, Request);
}

[[nodiscard]] bool
HoldsInteger(const Luna::Detail::ClassMemberAccessObservation &Observed,
             int Expected) {
  if (!Observed.Produced)
    return false;
  const int *Held = std::get_if<int>(&*Observed.Produced);
  return Held != nullptr && *Held == Expected;
}

[[nodiscard]] bool
HoldsDouble(const Luna::Detail::ClassMemberAccessObservation &Observed,
            double Expected) {
  if (!Observed.Produced)
    return false;
  const double *Held = std::get_if<double>(&*Observed.Produced);
  if (Held == nullptr)
    return false;
  const double Difference = *Held - Expected;
  return Difference > -0.000001 && Difference < 0.000001;
}

void VerifyWorld(const Luna::State &Owner, const ModelWorld &World,
                 const std::vector<Gadget *> &Objects) {
  RC_ASSERT(LevelReads == World.Accessors.LevelReads);
  RC_ASSERT(LevelWrites == World.Accessors.LevelWrites);
  RC_ASSERT(WeightReads == World.Accessors.WeightReads);
  RC_ASSERT(ExpensiveReads == World.Accessors.ExpensiveReads);
  RC_ASSERT(TunedReads == World.Accessors.TunedReads);
  RC_ASSERT(TunedWrites == World.Accessors.TunedWrites);
  RC_ASSERT(HiddenWrites == World.Accessors.HiddenWrites);
  RC_ASSERT(MarkedWrites == World.Accessors.MarkedWrites);
  RC_ASSERT(GrowCalls == World.Accessors.GrowCalls);

  std::size_t Nodes = 0;
  std::size_t Entries = 0;
  std::size_t Live = 0;
  for (std::size_t Index = 0; Index < World.Objects.size(); ++Index) {
    const ModelObject &Object = World.Objects[Index];
    if (Object.Cache.HasNode) {
      ++Nodes;
      Entries += Object.Cache.Entries.size();
      if (Object.Cache.NodeGeneration == World.Generation)
        Live += Object.Cache.Entries.size();
    }

    RC_ASSERT(Objects[Index]->Charge == Object.Charge);
    RC_ASSERT(Objects[Index]->Trace == Object.Trace);
    RC_ASSERT(Hooks::LazyMemberCacheEntryCountOf(Owner, Objects[Index]) ==
              EntryCountOf(Object));
    RC_ASSERT(Hooks::LazyMemberCacheGenerationOf(Owner, Objects[Index]) ==
              (Object.Cache.HasNode ? Object.Cache.NodeGeneration : 0));
    RC_ASSERT(Hooks::ClassUserdataNamesLazyEntries(Owner, PathOf(Index)) ==
              Object.Cache.SlotPopulated);
    RC_ASSERT(Hooks::ClassUserdataLazyGeneration(Owner, PathOf(Index)) ==
              Object.Cache.SlotGeneration);
  }

  RC_ASSERT(Hooks::LazyMemberCacheNodeCount(Owner) == Nodes);
  RC_ASSERT(Hooks::LazyMemberCacheEntryCount(Owner) == Entries);
  RC_ASSERT(Hooks::LiveLazyMemberCacheEntryCount(Owner) == Live);

  const LazyCacheCounters Counted = Hooks::LazyMemberCacheCounters(Owner);
  RC_ASSERT(Counted.Hit == World.Cache.Hit);
  RC_ASSERT(Counted.Miss == World.Cache.Miss);
  RC_ASSERT(Counted.GenerationMismatch == World.Cache.GenerationMismatch);
  RC_ASSERT(Counted.Store == World.Cache.Store);
  RC_ASSERT(Counted.Replace == World.Cache.Replace);
  RC_ASSERT(Counted.Invalidate == World.Cache.Invalidate);
  RC_ASSERT(Counted.Drop == World.Cache.Drop);
}

enum class GateActionKind : std::uint8_t {
  Read,
  Write,
  MistypedWrite,
  Invalidate,
  Retire,
  Expire,
  AdvanceGeneration
};

struct GateAction final {
  GateActionKind Kind = GateActionKind::Read;
  std::size_t Object = 0;
  MemberSlot Member = MemberSlot::Level;
  int Written = 0;
};

[[nodiscard]] MemberSlot ReadableSlot(std::size_t Choice) noexcept {
  switch (Choice % 10) {
  case 0:
    return MemberSlot::Level;
  case 1:
    return MemberSlot::Weight;
  case 2:
  case 3:
    return MemberSlot::Expensive;
  case 4:
  case 5:
    return MemberSlot::Tuned;
  case 6:
    return MemberSlot::Hidden;
  case 7:
    return MemberSlot::Charge;
  case 8:
    return MemberSlot::Serial;
  default:
    break;
  }
  return MemberSlot::Missing;
}

[[nodiscard]] MemberSlot WritableSlot(std::size_t Choice) noexcept {
  switch (Choice % 8) {
  case 0:
    return MemberSlot::Level;
  case 1:
    return MemberSlot::Charge;
  case 2:
    return MemberSlot::Tuned;
  case 3:
    return MemberSlot::Hidden;
  case 4:
    return MemberSlot::Marked;
  case 5:
    return MemberSlot::Serial;
  case 6:
    return MemberSlot::Weight;
  default:
    break;
  }
  return MemberSlot::Missing;
}

[[nodiscard]] GateAction GenerateGateAction(ByteCursor &Cursor,
                                            std::size_t ObjectCount) {
  GateAction Action;
  switch (Cursor.Pick(14)) {
  case 0:
  case 1:
  case 2:
  case 3:
  case 4:
    Action.Kind = GateActionKind::Read;
    break;
  case 5:
  case 6:
  case 7:
  case 8:
    Action.Kind = GateActionKind::Write;
    break;
  case 9:
    Action.Kind = GateActionKind::MistypedWrite;
    break;
  case 10:
    Action.Kind = GateActionKind::Invalidate;
    break;
  case 11:
    Action.Kind = GateActionKind::Expire;
    break;
  case 12:
    Action.Kind = GateActionKind::Retire;
    break;
  default:
    Action.Kind = GateActionKind::AdvanceGeneration;
    break;
  }

  Action.Object = Cursor.Pick(ObjectCount);
  Action.Member = Action.Kind == GateActionKind::Read
                      ? ReadableSlot(Cursor.Pick(10))
                      : WritableSlot(Cursor.Pick(8));
  Action.Written = static_cast<int>(Cursor.Pick(40)) + 1;
  return Action;
}

void VerifyGateDrivenMemberSequence(ByteCursor &Cursor) {
  ResetAccessorCounters();

  const std::size_t Count = 1 + Cursor.Pick(3);

  std::vector<std::unique_ptr<Gadget>> Storage;
  std::vector<std::unique_ptr<std::uint64_t>> Lifetimes;
  std::vector<Gadget *> Objects;
  Storage.reserve(Count);
  Lifetimes.reserve(Count);
  Objects.reserve(Count);

  ModelWorld World;
  World.Objects.resize(Count);
  for (std::size_t Index = 0; Index < Count; ++Index) {
    ModelObject &Object = World.Objects[Index];
    Object.IsConst = Cursor.Pick(4) == 0;
    Object.LazyGetterRefuses = Cursor.Pick(4) == 0;
    Object.MarkingSetterRefuses = Cursor.Pick(3) == 0;

    Storage.push_back(std::make_unique<Gadget>());
    Storage.back()->LazyGetterRefuses = Object.LazyGetterRefuses;
    Storage.back()->MarkingSetterRefuses = Object.MarkingSetterRefuses;
    Objects.push_back(Storage.back().get());
    Lifetimes.push_back(std::make_unique<std::uint64_t>(1 + Index));
  }

  const std::size_t ActionCount = 3 + Cursor.Pick(10);
  std::vector<GateAction> Actions;
  Actions.reserve(ActionCount);
  for (std::size_t Index = 0; Index < ActionCount; ++Index)
    Actions.push_back(GenerateGateAction(Cursor, Count));

  std::size_t Successes = 0;
  std::size_t CacheHits = 0;
  std::size_t Invalidations = 0;

  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    RC_ASSERT(RegisterGadget(Registry).IsSuccess());
    for (const MemberTraits &Declared : DeclaredMembers) {
      RC_ASSERT(Hooks::ClassMemberIsRegistered(
                    Owner, "Gadget", Declared.Name) == Declared.Known);
    }

    const auto Observed = Hooks::LifecycleGenerationOf(Owner);
    RC_ASSERT(Observed.has_value());
    World.Generation = *Observed;

    for (std::size_t Index = 0; Index < Count; ++Index) {
      RC_ASSERT(ExposeGadget(Owner, PathOf(Index), *Objects[Index],
                             Lifetimes[Index].get(),
                             World.Objects[Index].IsConst
                                 ? ConstAccess::Const
                                 : ConstAccess::Mutable) == "created");
    }
    RC_ASSERT(Hooks::LazyMemberCacheEntryCount(Owner) == 0);

    const int EntryDepth = Hooks::ObserveRootStackDepth(Owner).value_or(-1);
    VerifyWorld(Owner, World, Objects);

    for (const GateAction &Action : Actions) {
      const std::string Path = PathOf(Action.Object);
      const int ChargeBefore = Objects[Action.Object]->Charge;
      const int TraceBefore = Objects[Action.Object]->Trace;

      switch (Action.Kind) {
      case GateActionKind::Read: {
        const ModelReadOutcome Predicted =
            ModelRead(World, Action.Object, Action.Member);
        const auto Taken = ReadMember(Owner, Path, NameOf(Action.Member));

        RC_ASSERT(Taken.Failure == FailureText(Predicted.Failure));
        RC_ASSERT(Taken.Reached == (Predicted.Failure == ModelFailure::None));
        RC_ASSERT(Taken.Receiver == Predicted.Receiver);
        if (Predicted.Failure != ModelFailure::UnknownMember)
          RC_ASSERT(Taken.Boundary == BoundaryTextOf(Predicted.Failure));

        if (Predicted.Failure == ModelFailure::None) {
          ++Successes;
          RC_ASSERT(Taken.ServedFromCache == Predicted.ServedFromCache);
          RC_ASSERT(Taken.Recorded == Predicted.Recorded);
          if (Predicted.ServedFromCache)
            ++CacheHits;
          if (TraitsOf(Action.Member).ProducesDouble)
            RC_ASSERT(HoldsDouble(Taken, Predicted.ProducedDouble));
          else
            RC_ASSERT(HoldsInteger(Taken, Predicted.Produced));
        } else {
          RC_ASSERT(!Taken.Produced.has_value());
          RC_ASSERT(!Taken.ServedFromCache);
          RC_ASSERT(!Taken.Recorded);
        }

        RC_ASSERT(Objects[Action.Object]->Charge == ChargeBefore);
        RC_ASSERT(Objects[Action.Object]->Trace == TraceBefore);
        break;
      }

      case GateActionKind::Write:
      case GateActionKind::MistypedWrite: {
        const bool Mistyped = Action.Kind == GateActionKind::MistypedWrite;
        const ModelWriteOutcome Predicted = ModelWrite(
            World, Action.Object, Action.Member, Mistyped, Action.Written);
        const auto Taken = Mistyped
                               ? WriteMember(Owner, Path, NameOf(Action.Member),
                                             std::string("mistyped"))
                               : WriteMember(Owner, Path, NameOf(Action.Member),
                                             Action.Written);

        RC_ASSERT(Taken.Failure == FailureText(Predicted.Failure));
        RC_ASSERT(Taken.Reached == (Predicted.Failure == ModelFailure::None));
        RC_ASSERT(Taken.Receiver == Predicted.Receiver);
        if (Predicted.Failure != ModelFailure::UnknownMember)
          RC_ASSERT(Taken.Boundary == BoundaryTextOf(Predicted.Failure));
        RC_ASSERT(Taken.Invalidated == Predicted.Invalidated);
        if (Predicted.Failure == ModelFailure::None) {
          ++Successes;
          Invalidations += Predicted.Invalidated;
        }

        if (Predicted.Failure == ModelFailure::ContainedException) {
          RC_ASSERT(Objects[Action.Object]->Trace == Action.Written);
          RC_ASSERT(Objects[Action.Object]->Charge == ChargeBefore);
        } else if (Predicted.Failure != ModelFailure::None) {
          RC_ASSERT(Objects[Action.Object]->Charge == ChargeBefore);
          RC_ASSERT(Objects[Action.Object]->Trace == TraceBefore);
        }
        break;
      }

      case GateActionKind::Invalidate: {
        ModelObject &Object = World.Objects[Action.Object];
        const std::size_t Predicted = ModelInvalidateOwner(World, Object);
        RC_ASSERT(Hooks::InvalidateClassMemberCache(Owner, Path) == Predicted);
        Invalidations += Predicted;
        break;
      }

      case GateActionKind::Retire: {
        ModelObject &Object = World.Objects[Action.Object];
        const bool AlreadyRetired = Object.Retired;
        const bool Retired =
            Hooks::RetireClassUserdata(Owner, Objects[Action.Object]);
        if (AlreadyRetired) {
          RC_ASSERT(!Retired);
        } else {
          RC_ASSERT(Retired);
          ModelRetire(World, Object);
        }
        break;
      }

      case GateActionKind::Expire:
        ++*Lifetimes[Action.Object];
        World.Objects[Action.Object].Expired = true;
        break;

      case GateActionKind::AdvanceGeneration: {
        RC_ASSERT(Hooks::AdvanceLifecycleGeneration(Owner));
        ++World.Generation;
        RC_ASSERT(Hooks::LifecycleGenerationOf(Owner) ==
                  std::optional<std::uint64_t>(World.Generation));
        break;
      }
      }

      VerifyWorld(Owner, World, Objects);
      RC_ASSERT(Hooks::ObserveRootStackDepth(Owner) == EntryDepth);
    }

    RC_ASSERT(Owner.IsReady());
    RC_ASSERT(Owner.Execute("Recovered = 11").IsSuccess());
    RC_ASSERT(Hooks::ObserveIntegerGlobal(Owner, "Recovered") ==
              std::optional<int>(11));
    RC_ASSERT(Hooks::UserdataReleaseCounters(Owner).IncompleteMetadata == 0);
  }

  if (Successes == 0)
    RC_TAG("gate: every access refused");
  else if (CacheHits != 0 && Invalidations != 0)
    RC_TAG("gate: values recorded, reused, and invalidated");
  else if (CacheHits != 0)
    RC_TAG("gate: values recorded and reused");
  else if (Invalidations != 0)
    RC_TAG("gate: values recorded and invalidated");
  else
    RC_TAG("gate: accesses succeeded without any reuse");
}

} // namespace

namespace {

[[nodiscard]] std::string Refusal(Luna::State &Owner,
                                  const std::string &Source) {
  const Luna::ExecutionResult Result = Owner.Execute(Source);
  if (Result.IsSuccess())
    return std::string();
  const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic();
  return Diagnostic ? Diagnostic->Message() : std::string("<no diagnostic>");
}

[[nodiscard]] bool Contains(std::string_view Text, std::string_view Needle) {
  return Text.find(Needle) != std::string_view::npos;
}

[[nodiscard]] std::string ExpectedNeedle(ModelFailure Failure, bool Reading,
                                         MemberSlot Member) {
  const std::string Qualified = "Gadget." + NameOf(Member);
  switch (Failure) {
  case ModelFailure::UnknownMember:
    return "Class 'Gadget' declares no member '" + NameOf(Member) + "'.";
  case ModelFailure::RefusedReceiver:
    return "Member '" + Qualified + "' receiver";
  case ModelFailure::UnreadableMember:
    return "Member '" + Qualified + "' permits no read.";
  case ModelFailure::UnwritableMember:
    return "Member '" + Qualified + "' permits no write.";
  case ModelFailure::IncompatibleValue:
    return "Member '" + Qualified + "' value";
  case ModelFailure::ContainedException:
    return "member '" + Qualified +
           (Reading ? "' getter threw:" : "' setter threw:");
  case ModelFailure::None:
    break;
  }
  return std::string();
}

[[nodiscard]] bool RestoredCheckpoint(const Luna::State &Owner) {
  const auto Observation = Hooks::ObserveLastCallbackStackRestoration(Owner);
  return Observation.has_value() &&
         Observation->EntryDepth == Observation->RestoredDepth &&
         Observation->ErrorDepth == Observation->RestoredDepth + 1;
}

void VerifyDispatch(const Luna::State &Owner, bool Reading, MemberSlot Member,
                    std::string_view Stage, std::string_view Boundary,
                    std::string_view Receiver, bool Succeeded,
                    int PublishedCount) {
  const auto Observed = Hooks::ObserveLastClassMemberDispatch(Owner);
  RC_ASSERT(Observed.has_value());
  RC_ASSERT(Observed->Attempted);
  RC_ASSERT(Observed->Reading == Reading);
  RC_ASSERT(Observed->Succeeded == Succeeded);
  RC_ASSERT(MemberDispatchStageText(Observed->Stage) == Stage);
  RC_ASSERT(Luna::Detail::MemberSideEffectBoundaryText(Observed->Boundary) ==
            Boundary);
  RC_ASSERT(Luna::Detail::UserdataAccessFailureText(Observed->Receiver) ==
            Receiver);
  RC_ASSERT(Observed->ClassName == "Gadget");
  RC_ASSERT(Observed->MemberName == NameOf(Member));
  RC_ASSERT(Observed->PublishedCount == PublishedCount);

  if (!Succeeded)
    RC_ASSERT(Observed->EntryDepth == Observed->RestoredDepth);
}

enum class VmActionKind : std::uint8_t {
  Read,
  Write,
  MistypedWrite,
  ColonCall,
  SpellingProbe,
  MissingReceiver,
  ForeignReceiver,
  InjectPublicationFault,
  Expire,
  Invalidate
};

struct VmAction final {
  VmActionKind Kind = VmActionKind::Read;
  std::size_t Object = 0;
  MemberSlot Member = MemberSlot::Level;
  int Written = 0;
};

[[nodiscard]] VmAction GenerateVmAction(ByteCursor &Cursor,
                                        std::size_t ObjectCount) {
  VmAction Action;
  switch (Cursor.Pick(16)) {
  case 0:
  case 1:
  case 2:
  case 3:
  case 4:
    Action.Kind = VmActionKind::Read;
    break;
  case 5:
  case 6:
  case 7:
    Action.Kind = VmActionKind::Write;
    break;
  case 8:
    Action.Kind = VmActionKind::MistypedWrite;
    break;
  case 9:
  case 10:
    Action.Kind = VmActionKind::ColonCall;
    break;
  case 11:
    Action.Kind = VmActionKind::SpellingProbe;
    break;
  case 12:
    Action.Kind = VmActionKind::MissingReceiver;
    break;
  case 13:
    Action.Kind = VmActionKind::ForeignReceiver;
    break;
  case 14:
    Action.Kind = VmActionKind::InjectPublicationFault;
    break;
  default:
    Action.Kind =
        Cursor.Pick(2) == 0 ? VmActionKind::Expire : VmActionKind::Invalidate;
    break;
  }

  Action.Object = Cursor.Pick(ObjectCount);
  Action.Member = Action.Kind == VmActionKind::Read
                      ? ReadableSlot(Cursor.Pick(10))
                      : WritableSlot(Cursor.Pick(8));
  Action.Written = static_cast<int>(Cursor.Pick(30)) + 1;
  return Action;
}

void VerifyVirtualMachineMemberSequence(ByteCursor &Cursor) {
  ResetAccessorCounters();

  const std::size_t Count = 1 + Cursor.Pick(3);

  std::vector<std::unique_ptr<Gadget>> Storage;
  std::vector<std::unique_ptr<std::uint64_t>> Lifetimes;
  std::vector<Gadget *> Objects;
  Storage.reserve(Count);
  Lifetimes.reserve(Count);
  Objects.reserve(Count);

  ModelWorld World;
  World.Objects.resize(Count);
  for (std::size_t Index = 0; Index < Count; ++Index) {
    ModelObject &Object = World.Objects[Index];
    Object.IsConst = Cursor.Pick(4) == 0;
    Object.LazyGetterRefuses = Cursor.Pick(4) == 0;
    Object.MarkingSetterRefuses = Cursor.Pick(3) == 0;

    Storage.push_back(std::make_unique<Gadget>());
    Storage.back()->LazyGetterRefuses = Object.LazyGetterRefuses;
    Storage.back()->MarkingSetterRefuses = Object.MarkingSetterRefuses;
    Objects.push_back(Storage.back().get());
    Lifetimes.push_back(std::make_unique<std::uint64_t>(7 + Index));
  }

  const std::size_t ActionCount = 3 + Cursor.Pick(10);
  std::vector<VmAction> Actions;
  Actions.reserve(ActionCount);
  for (std::size_t Index = 0; Index < ActionCount; ++Index)
    Actions.push_back(GenerateVmAction(Cursor, Count));

  std::size_t Published = 0;
  std::size_t Refusals = 0;
  std::size_t Unpublishable = 0;
  std::size_t IdenticalSpellings = 0;

  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    RC_ASSERT(RegisterGadget(Registry).IsSuccess());

    const auto Generation = Hooks::LifecycleGenerationOf(Owner);
    RC_ASSERT(Generation.has_value());
    World.Generation = *Generation;

    for (std::size_t Index = 0; Index < Count; ++Index) {
      RC_ASSERT(ExposeGadget(Owner, PathOf(Index), *Objects[Index],
                             Lifetimes[Index].get(),
                             World.Objects[Index].IsConst
                                 ? ConstAccess::Const
                                 : ConstAccess::Mutable) == "created");
    }

    const int EntryDepth = Hooks::ObserveRootStackDepth(Owner).value_or(-1);
    VerifyWorld(Owner, World, Objects);

    for (const VmAction &Action : Actions) {
      const std::string Path = PathOf(Action.Object);
      const std::string Name = NameOf(Action.Member);
      const int ChargeBefore = Objects[Action.Object]->Charge;
      const int TraceBefore = Objects[Action.Object]->Trace;
      Hooks::ClearClassMemberDispatch(Owner);

      switch (Action.Kind) {
      case VmActionKind::Read: {
        const ModelReadOutcome Predicted =
            ModelRead(World, Action.Object, Action.Member);
        const std::string Message =
            Refusal(Owner, "Result = " + Path + "." + Name);

        if (Predicted.Failure != ModelFailure::None) {
          ++Refusals;
          RC_ASSERT(!Message.empty());
          RC_ASSERT(Contains(
              Message, ExpectedNeedle(Predicted.Failure, true, Action.Member)));
          VerifyDispatch(
              Owner, true, Action.Member, StageTextOf(Predicted.Failure),
              BoundaryTextOf(Predicted.Failure), Predicted.Receiver, false, 0);
          RC_ASSERT(RestoredCheckpoint(Owner));
        } else if (World.PendingPublicationFaults != 0) {
          --World.PendingPublicationFaults;
          ++Unpublishable;
          RC_ASSERT(!Message.empty());
          RC_ASSERT(Contains(Message, "could not be published"));
          RC_ASSERT(Contains(Message, "Gadget." + Name));
          VerifyDispatch(Owner, true, Action.Member, "publication",
                         Predicted.ServedFromCache ? "before_user_code"
                                                   : "after_user_code",
                         "none", false, 0);
          RC_ASSERT(RestoredCheckpoint(Owner));
        } else {
          ++Published;
          RC_ASSERT(Message.empty());
          VerifyDispatch(Owner, true, Action.Member, "published",
                         "before_user_code", "none", true, 1);
          if (!TraitsOf(Action.Member).ProducesDouble) {
            RC_ASSERT(Hooks::ObserveIntegerGlobal(Owner, "Result") ==
                      std::optional<int>(Predicted.Produced));
          }
        }

        RC_ASSERT(Objects[Action.Object]->Charge == ChargeBefore);
        RC_ASSERT(Objects[Action.Object]->Trace == TraceBefore);
        break;
      }

      case VmActionKind::Write:
      case VmActionKind::MistypedWrite: {
        const bool Mistyped = Action.Kind == VmActionKind::MistypedWrite;
        const ModelWriteOutcome Predicted = ModelWrite(
            World, Action.Object, Action.Member, Mistyped, Action.Written);
        const std::string Source = Path + "." + Name + " = " +
                                   (Mistyped ? std::string("'mistyped'")
                                             : std::to_string(Action.Written));
        const std::string Message = Refusal(Owner, Source);

        if (Predicted.Failure != ModelFailure::None) {
          ++Refusals;
          RC_ASSERT(!Message.empty());
          RC_ASSERT(Contains(Message, ExpectedNeedle(Predicted.Failure, false,
                                                     Action.Member)));
          RC_ASSERT(RestoredCheckpoint(Owner));
        } else {
          ++Published;
          RC_ASSERT(Message.empty());
        }
        VerifyDispatch(Owner, false, Action.Member,
                       Predicted.Failure == ModelFailure::None
                           ? "published"
                           : StageTextOf(Predicted.Failure),
                       BoundaryTextOf(Predicted.Failure), Predicted.Receiver,
                       Predicted.Failure == ModelFailure::None, 0);

        if (Predicted.Failure == ModelFailure::ContainedException) {
          RC_ASSERT(Objects[Action.Object]->Trace == Action.Written);
          RC_ASSERT(Objects[Action.Object]->Charge == ChargeBefore);
        } else if (Predicted.Failure != ModelFailure::None) {
          RC_ASSERT(Objects[Action.Object]->Charge == ChargeBefore);
          RC_ASSERT(Objects[Action.Object]->Trace == TraceBefore);
        }
        break;
      }

      case VmActionKind::ColonCall: {
        ModelObject &Object = World.Objects[Action.Object];
        const std::string_view ReceiverRefusal =
            ModelReceiverRefusal(Object, true);
        const std::string Message =
            Refusal(Owner, "Result = " + Path + ":Grow(" +
                               std::to_string(Action.Written) + ")");
        if (ReceiverRefusal != "none") {
          ++Refusals;
          RC_ASSERT(!Message.empty());
          RC_ASSERT(Contains(Message, "receiver"));
          RC_ASSERT(Objects[Action.Object]->Charge == ChargeBefore);
        } else {
          ++Published;
          RC_ASSERT(Message.empty());
          ++World.Accessors.GrowCalls;
          Object.Charge += Action.Written;
          RC_ASSERT(Hooks::ObserveIntegerGlobal(Owner, "Result") ==
                    std::optional<int>(Object.Charge));
        }

        RC_ASSERT(!Hooks::ObserveLastClassMemberDispatch(Owner).has_value());
        break;
      }

      case VmActionKind::SpellingProbe: {
        ModelObject &Object = World.Objects[Action.Object];
        const bool Permitted = ModelReceiverRefusal(Object, true) == "none";
        const std::string Argument = std::to_string(Action.Written);
        const std::string Colon =
            Refusal(Owner, "Result = " + Path + ":Grow(" + Argument + ")");
        const std::string Dot =
            Refusal(Owner, "Result = " + Path + ".Grow(" + Path + ", " +
                               Argument + ")");
        const std::string Scoped = Refusal(
            Owner, "Result = Gadget.Grow(" + Path + ", " + Argument + ")");

        RC_ASSERT(Colon == Dot);
        RC_ASSERT(Dot == Scoped);
        ++IdenticalSpellings;

        if (Permitted) {
          Published += 3;
          RC_ASSERT(Colon.empty());
          World.Accessors.GrowCalls += 3;
          Object.Charge += 3 * Action.Written;
          RC_ASSERT(Hooks::ObserveIntegerGlobal(Owner, "Result") ==
                    std::optional<int>(Object.Charge));
        } else {
          Refusals += 3;
          RC_ASSERT(Contains(Colon, "receiver"));
          RC_ASSERT(Objects[Action.Object]->Charge == ChargeBefore);
        }
        RC_ASSERT(!Hooks::ObserveLastClassMemberDispatch(Owner).has_value());
        break;
      }

      case VmActionKind::MissingReceiver: {
        const std::string Message = Refusal(Owner, "Result = Gadget.Grow()");
        ++Refusals;
        RC_ASSERT(Contains(Message, "receiver"));
        RC_ASSERT(!Contains(Message, "argument 1"));
        RC_ASSERT(RestoredCheckpoint(Owner));
        RC_ASSERT(!Hooks::ObserveLastClassMemberDispatch(Owner).has_value());
        break;
      }

      case VmActionKind::ForeignReceiver: {
        const std::string Message =
            Refusal(Owner, "Result = Gadget.Grow('text', 1)");
        ++Refusals;
        RC_ASSERT(Contains(Message, "receiver"));
        RC_ASSERT(RestoredCheckpoint(Owner));
        RC_ASSERT(!Hooks::ObserveLastClassMemberDispatch(Owner).has_value());
        break;
      }

      case VmActionKind::InjectPublicationFault:
        Hooks::InjectFault(Owner, StateFaultPoint::MemberValuePublication, 1);
        World.PendingPublicationFaults = 1;
        break;

      case VmActionKind::Expire:
        ++*Lifetimes[Action.Object];
        World.Objects[Action.Object].Expired = true;
        break;

      case VmActionKind::Invalidate: {
        ModelObject &Object = World.Objects[Action.Object];
        const std::size_t Predicted = ModelInvalidateOwner(World, Object);
        RC_ASSERT(Hooks::InvalidateClassMemberCache(Owner, Path) == Predicted);
        break;
      }
      }

      VerifyWorld(Owner, World, Objects);
      RC_ASSERT(Hooks::ObserveRootStackDepth(Owner) == EntryDepth);
      RC_ASSERT(Hooks::PendingFaults(Owner,
                                     StateFaultPoint::MemberValuePublication) ==
                World.PendingPublicationFaults);
    }

    RC_ASSERT(Owner.IsReady());
    RC_ASSERT(Owner.Execute("Recovered = 5").IsSuccess());
    RC_ASSERT(Hooks::ObserveIntegerGlobal(Owner, "Recovered") ==
              std::optional<int>(5));
    RC_ASSERT(Hooks::UserdataReleaseCounters(Owner).IncompleteMetadata == 0);
  }

  if (Published == 0)
    RC_TAG("virtual machine: every access refused");
  else if (Unpublishable != 0)
    RC_TAG("virtual machine: a produced value could not be published");
  else if (IdenticalSpellings != 0)
    RC_TAG("virtual machine: colon, dot, and class spellings agreed");
  else if (Refusals == 0)
    RC_TAG("virtual machine: every access published");
  else
    RC_TAG("virtual machine: accesses published and refused");
}

} // namespace

int RunMemberReceiverAndLazyCacheProperties() {

  const bool Passed = rc::check(

      "Member access follows receiver precedence and lazy-cache generations",
      [](const std::vector<std::uint8_t> &GateBytes,
         const std::vector<std::uint8_t> &DispatchBytes) {
        ByteCursor Gate(GateBytes);
        VerifyGateDrivenMemberSequence(Gate);

        ByteCursor Dispatch(DispatchBytes);
        VerifyVirtualMachineMemberSequence(Dispatch);
      });
  return Passed ? 0 : 1;
}
