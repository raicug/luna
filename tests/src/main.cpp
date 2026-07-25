// clang-format off
#include <array>
#include <exception>
#include <iostream>
#include <string_view>
// clang-format on

int RunCallableModelTests();
int RunConversionReturnValidationFaultEdgeCaseTests();
int RunEndToEndRegistrationInvocationMatrixTests();
int RunExecutionRecoveryProperties();
int RunExecutionStackBalanceProperties();
int RunFailedRegistrationTransactionsProperties();
int RunGlobalNameGrammarProperties();
int RunIncompatibleArgumentTypesProperties();
int RunInternalValidationFailureProperties();
int RunInvalidIntegerClassificationProperties();
int RunInvocableValidRegistrationProperties();
int RunInvocationPrimitiveTests();
int RunNativeFailureCallbackStackProperties();
int RunNativeTrampolineTests();
int RunRegistrationExamplesAndEdgeCaseTests();
int RunRegistrationPrecheckTests();
int RunRegistrationStackBalanceProperties();
int RunRegistrationTransactionTests();
int RunResultTypesTests();
int RunSourceExecutionTests();
int RunStandardExceptionTranslationProperties();
int RunStateFacadeTests();
int RunStateOwnershipTests();
int RunStateOwnershipTransitionsProperties();
int RunStateRegistrationIsolationProperties();
int RunSuccessfulValidationExactlyOnceProperties();
int RunSupportedValueRoundTripProperties();
int RunValidationShortCircuitingProperties();
int RunWrongArgumentCountProperties();

namespace {

struct TestCase final {
  std::string_view Name;
  int (*Run)();
};

} // namespace

int main() {
  constexpr std::array Tests{
      TestCase{"callable model", RunCallableModelTests},
      TestCase{"result types", RunResultTypesTests},
      TestCase{"state facade", RunStateFacadeTests},
      TestCase{"invocation primitives", RunInvocationPrimitiveTests},
      TestCase{"conversion, return, validation, and fault edge cases",
               RunConversionReturnValidationFaultEdgeCaseTests},
      TestCase{"native trampoline", RunNativeTrampolineTests},
      TestCase{"source execution", RunSourceExecutionTests},
      TestCase{"state ownership", RunStateOwnershipTests},
      TestCase{"registration prechecks", RunRegistrationPrecheckTests},
      TestCase{"registration transactions", RunRegistrationTransactionTests},
      TestCase{"registration examples and edge cases",
               RunRegistrationExamplesAndEdgeCaseTests},
      TestCase{"end-to-end registration and invocation matrix",
               RunEndToEndRegistrationInvocationMatrixTests},
      TestCase{"property 1: state ownership transitions",
               RunStateOwnershipTransitionsProperties},
      TestCase{"property 2: invocable valid registrations",
               RunInvocableValidRegistrationProperties},
      TestCase{"property 3: global name grammar",
               RunGlobalNameGrammarProperties},
      TestCase{"property 4: failed registration transactions",
               RunFailedRegistrationTransactionsProperties},
      TestCase{"property 5: supported value round trips",
               RunSupportedValueRoundTripProperties},
      TestCase{"property 6: wrong argument counts",
               RunWrongArgumentCountProperties},
      TestCase{"property 7: incompatible argument types",
               RunIncompatibleArgumentTypesProperties},
      TestCase{"property 8: invalid integer classification",
               RunInvalidIntegerClassificationProperties},
      TestCase{"property 9: validation short-circuiting",
               RunValidationShortCircuitingProperties},
      TestCase{"property 10: successful validation invokes exactly once",
               RunSuccessfulValidationExactlyOnceProperties},
      TestCase{"property 11: internal validation failures",
               RunInternalValidationFailureProperties},
      TestCase{"property 12: standard-exception translation",
               RunStandardExceptionTranslationProperties},
      TestCase{"property 13: execution recovery",
               RunExecutionRecoveryProperties},
      TestCase{"property 14: registration stack balance",
               RunRegistrationStackBalanceProperties},
      TestCase{"property 15: execution stack balance",
               RunExecutionStackBalanceProperties},
      TestCase{"property 16: native-failure callback stack restoration",
               RunNativeFailureCallbackStackProperties},
      TestCase{"property 17: state registration isolation",
               RunStateRegistrationIsolationProperties},
  };

  for (const auto &Test : Tests) {
    try {
      const int Result = Test.Run();
      if (Result != 0) {
        std::cerr << "Test failed: " << Test.Name << " (code " << Result
                  << ")\n";
        return Result;
      }
    } catch (const std::exception &Error) {
      std::cerr << "Test threw: " << Test.Name << ": " << Error.what() << '\n';
      return 1;
    } catch (...) {
      std::cerr << "Test threw an unknown exception: " << Test.Name << '\n';
      return 1;
    }
  }

  return 0;
}
