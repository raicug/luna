// clang-format off
#include <luna/binding/delegate.hpp>

#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Notify = Luna::Delegate<void(int)>;
using Rate = Luna::Delegate<double(std::string)>;

static_assert(std::is_default_constructible_v<Notify>,
              "A delegate must be default constructible.");
static_assert(std::is_copy_constructible_v<Notify>,
              "Delegate copies must share one subscribed handler.");
static_assert(std::is_same_v<Notify::ResultType, void>,
              "A delegate must publish its result type.");
static_assert(std::is_same_v<Rate::ResultType, double>,
              "A delegate must publish its declared result type.");

static_assert(
    Luna::DelegateStatusText(Luna::DelegateStatus::Ready) == "ready" &&
        Luna::DelegateStatusText(Luna::DelegateStatus::Released) ==
            "released" &&
        Luna::DelegateStatusText(Luna::DelegateStatus::ForeignThread) ==
            "foreign_thread" &&
        Luna::DelegateStatusText(Luna::DelegateStatus::HandlerFailed) ==
            "handler_failed" &&
        Luna::DelegateStatusText(Luna::DelegateStatus::ResultMismatch) ==
            "result_mismatch",
    "Every delegate status must format canonically.");

static_assert(Luna::Detail::DelegateSignatureShape<void(int)>::IsSupported &&
                  Luna::Detail::DelegateSignatureShape<
                      bool(std::string, double)>::IsSupported,
              "A canonical delegate shape must be supported.");
static_assert(
    !Luna::Detail::DelegateSignatureShape<void(unsigned int)>::IsSupported,
    "A delegate outside the canonical value domain must be refused.");

} // namespace

void VerifyDelegateHeaderCompilesStandalone() {
  const Luna::DelegateShape Declared = Notify::Shape();
  const Luna::DelegateShape Same = Notify::Shape();

  Notify Unsubscribed;
  const Luna::DelegateCallResult Refused = Unsubscribed.Invoke(1);

  const Luna::DelegateCallResult Delivered =
      Luna::DelegateCallResult::Delivered(Luna::Value(2));

  static_cast<void>(Declared == Same);
  static_cast<void>(Declared.Parameters.size());
  static_cast<void>(Declared.Result.has_value());
  static_cast<void>(Unsubscribed.IsValid());
  static_cast<void>(Unsubscribed.Identity());
  static_cast<void>(Unsubscribed.Target());
  Unsubscribed.Release();
  static_cast<void>(Refused.Status());
  static_cast<void>(Refused.Diagnostic().empty());
  static_cast<void>(Delivered.IsSuccess());
  static_cast<void>(Delivered.Produced() != nullptr);
}
