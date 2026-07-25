#pragma once

// clang-format off
#include "state/binding/record.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
// clang-format on

namespace Luna::Detail {

class FaultInjector;

class BindingStore final {
public:
  [[nodiscard]] std::size_t Count() const noexcept { return Records.size(); }

  [[nodiscard]] std::size_t PendingCount() const noexcept {
    std::size_t Result = 0;
    for (const auto &[Name, Record] : Records) {
      static_cast<void>(Name);
      if (!Record->IsCommitted())
        ++Result;
    }
    return Result;
  }

  [[nodiscard]] bool Contains(std::string_view GlobalName) const noexcept {
    return Find(GlobalName) != nullptr;
  }

  [[nodiscard]] BindingRecord *Find(std::string_view GlobalName) noexcept {
    for (auto &[StoredName, Record] : Records) {
      if (StoredName == GlobalName)
        return Record.get();
    }
    return nullptr;
  }

  [[nodiscard]] const BindingRecord *
  Find(std::string_view GlobalName) const noexcept {
    for (const auto &[StoredName, Record] : Records) {
      if (StoredName == GlobalName)
        return Record.get();
    }
    return nullptr;
  }

  [[nodiscard]] BindingRecord *Prepare(std::string GlobalName,
                                       ErasedCallableDescriptor Descriptor,
                                       FaultInjector &Faults) {
    Records.reserve(Records.size() + 1);
    auto Record = std::make_unique<BindingRecord>(
        std::move(GlobalName), std::move(Descriptor), Faults);
    auto *Address = Record.get();
    const auto [Position, Inserted] =
        Records.try_emplace(Address->GlobalName(), std::move(Record));
    static_cast<void>(Position);
    return Inserted ? Address : nullptr;
  }

  void Commit(BindingRecord &Record) noexcept {
    Record.State = BindingRecordState::Committed;
  }

  [[nodiscard]] bool Rollback(std::string_view GlobalName,
                              const BindingRecord *Expected) noexcept {
    for (auto Iterator = Records.begin(); Iterator != Records.end();
         ++Iterator) {
      if (Iterator->first == GlobalName && Iterator->second.get() == Expected &&
          !Iterator->second->IsCommitted()) {
        Records.erase(Iterator);
        return true;
      }
    }
    return false;
  }

private:
  std::unordered_map<std::string, std::unique_ptr<BindingRecord>> Records;
};

} // namespace Luna::Detail
