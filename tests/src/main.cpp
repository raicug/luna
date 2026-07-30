// clang-format off
#include <array>
#include <exception>
#include <iostream>
#include <string_view>
// clang-format on

int RunAllocatorConstructionCleanupProperties();
int RunArtifactPublicationTests();
int RunAsynchronousExecutionIntegrationTests();
int RunAsynchronousInvocationTests();
int RunAsynchronousSettlementProperties();
int RunCallableModelTests();
int RunCallableRichnessIntegrationTests();
int RunCanonicalConversionRoundTripProperties();
int RunCanonicalTypeRegistryTests();
int RunCanonicalIdentityEncodingTests();
int RunCanonicalIdentityModelTests();
int RunCanonicalIdentityVectorTests();
int RunCacheOwnershipAndInvalidationTests();
int RunClassConstantTests();
int RunClassConstructionFaultTests();
int RunClassConstructionTests();
int RunClassMemberAccessTests();
int RunClassMemberBoundaryTests();
int RunClassIterationTests();
int RunClassMemberIntegrationTests();
int RunConvertedMemberTests();
int RunClassMemberTests();
int RunClassMethodTests();
int RunClassOperatorTests();
int RunClassRegistrationTests();
int RunClassRelationshipTests();
int RunClassUserdataIntegrationTests();
int RunConstantAndEnumerationTests();
int RunConversionBoundaryTests();
int RunConversionRegistryEdgeCaseTests();
int RunConversionRegistryIntegrationTests();
int RunConversionReturnValidationFaultEdgeCaseTests();
int RunConvertedOperandTests();
int RunClassOperandTests();
int RunInstanceMemberTests();
int RunInstanceOperandTests();
int RunInstanceReturnTests();
int RunVariadicUserdataTests();
int RunDeclarationGenerationTests();
int RunDeclaredParameterIntegrationTests();
int RunDeclaredParameterShapeTests();
int RunDelegateBindingTests();
int RunDescriptorPlanTests();
int RunDispatchGenerationIntegrationTests();
int RunDispatchSlotIndirectionTests();
int RunDocumentationGenerationTests();
int RunDynamicModuleLifecycleProperties();
int RunEndToEndRegistrationInvocationMatrixTests();
int RunEnumeratorObjectTests();
int RunExecutionRecoveryProperties();
int RunExecutionStackBalanceProperties();
int RunFailedRegistrationTransactionsProperties();
int RunFoundationCompatibilityMatrixTests();
int RunFreezeAndCacheFaultTests();
int RunFreezeLifecycleTests();
int RunFrozenCacheEquivalenceProperties();
int RunFrozenCacheLookupTests();
int RunFrozenStateIntegrationTests();
int RunFunctionRegistrationTests();
int RunGeneralizedTransactionRollbackProperties();
int RunGeneratorArtifactGoldenTests();
int RunGlobalNameGrammarProperties();
int RunHierarchicalRegistrationIntegrationTests();
int RunIncompatibleArgumentTypesProperties();
int RunInheritanceOperatorIntegrationTests();
int RunInheritanceAndCastPathProperties();
int RunInternalValidationFailureProperties();
int RunInvalidIntegerClassificationProperties();
int RunInvocableValidRegistrationProperties();
int RunInvocationPrimitiveTests();
int RunLifecycleBlockerMatrixTests();
int RunLifecycleClosureTests();
int RunLifecyclePublicationTests();
int RunLifecycleStagingTests();
int RunLoadOnceModuleResolutionProperties();
int RunModuleGraphIntegrationTests();
int RunModuleLifecycleIntegrationTests();
int RunModuleLoadingTests();
int RunModuleResolutionTests();
int RunMemberReceiverAndLazyCacheProperties();
int RunMultipleReturnShapeTests();
int RunNamespaceBuilderTests();
int RunNativeFailureCallbackStackProperties();
int RunNativeTrampolineTests();
int RunOverloadResolutionTests();
int RunOwningReflectionSnapshotProperties();
int RunParetoOverloadResolutionProperties();
int RunProfilingHookTests();
int RunReflectionDatabaseTests();
int RunReflectionMetadataTests();
int RunReflectionEnumerationOrderProperties();
int RunRegistrationExamplesAndEdgeCaseTests();
int RunRegistrationCaptureAndPreparationTests();
int RunRegistrationPrecheckTests();
int RunRegistrationStackBalanceProperties();
int RunRegistrationTransactionTests();
int RunResultTypesTests();
int RunRichSignatureShapeProperties();
int RunSignalDeliveryProperties();
int RunSourceExecutionTests();
int RunStableCanonicalIdentityProperties();
int RunStandardExceptionTranslationProperties();
int RunStateFacadeTests();
int RunStateOwnershipTests();
int RunStateOwnershipTransitionsProperties();
int RunStateRegistrationIsolationProperties();
int RunStructuralConverterTests();
int RunTransactionCallbackBoundaryTests();
int RunUnavailableExtensionBoundaryTests();
int RunTransactionInstallationAndPublicationTests();
int RunUnifiedTransactionIntegrationTests();
int RunUserdataAccessAndCacheTests();
int RunUserdataAllocatorProtocolTests();
int RunUserdataCollectionAndDestructionTests();
int RunUserdataOwnershipLifetimeProperties();
int RunUserdataOwnershipTransitionTests();
int RunUserdataPublicationFaultTests();
int RunSuccessfulValidationExactlyOnceProperties();
int RunSupportedValueRoundTripProperties();
int RunValidationShortCircuitingProperties();
int RunWrongArgumentCountProperties();

namespace {

struct TestCase final {
  std ::string_view Name;
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
      TestCase{"stable dispatch slot indirection",
               RunDispatchSlotIndirectionTests},
      TestCase{"dispatch generations through the real virtual machine",
               RunDispatchGenerationIntegrationTests},
      TestCase{"source execution", RunSourceExecutionTests},
      TestCase{"state ownership", RunStateOwnershipTests},
      TestCase{"registration prechecks", RunRegistrationPrecheckTests},
      TestCase{"registration transactions", RunRegistrationTransactionTests},
      TestCase{"canonical descriptor plans and generation sets",
               RunDescriptorPlanTests},
      TestCase{"transaction capture, validation, and preparation",
               RunRegistrationCaptureAndPreparationTests},
      TestCase{"transaction installation, undo, and publication",
               RunTransactionInstallationAndPublicationTests},
      TestCase{"transaction callback boundary and query isolation",
               RunTransactionCallbackBoundaryTests},
      TestCase{"unified registration transaction integration",
               RunUnifiedTransactionIntegrationTests},
      TestCase{"registration examples and edge cases",
               RunRegistrationExamplesAndEdgeCaseTests},
      TestCase{"atomic freeze preparation and frozen lifecycle",
               RunFreezeLifecycleTests},
      TestCase{"wrong-thread precedence, refused freeze, and frozen rejections",
               RunFreezeAndCacheFaultTests},
      TestCase{"frozen cache lookups, invalidation, and snapshot reads",
               RunFrozenCacheLookupTests},
      TestCase{"freeze through the real compiler and virtual machine",
               RunFrozenStateIntegrationTests},
      TestCase{"transactional namespaces and stale-safe builders",
               RunNamespaceBuilderTests},
      TestCase{"constants, scoped enumerations, aliases, and bitflags",
               RunConstantAndEnumerationTests},
      TestCase{"class types, class symbols, and metatable identity",
               RunClassRegistrationTests},
      TestCase{"borrowed, Lua-owned, and shared userdata release transitions",
               RunUserdataOwnershipTransitionTests},
      TestCase{"validated userdata access and the native identity cache",
               RunUserdataAccessAndCacheTests},
      TestCase{"cache ownership, equivalence, and exact invalidation",
               RunCacheOwnershipAndInvalidationTests},
      TestCase{"protected userdata collection and destruction ordering",
               RunUserdataCollectionAndDestructionTests},
      TestCase{"refused userdata exposure and publication accounting",
               RunUserdataPublicationFaultTests},
      TestCase{"the semantic allocator protocol and milestone-exact cleanup",
               RunUserdataAllocatorProtocolTests},
      TestCase{"reflected constructors, factories, and singleton accessors",
               RunClassConstructionTests},
      TestCase{"failure at every construction stage",
               RunClassConstructionFaultTests},
      TestCase{"reflected instance and static methods", RunClassMethodTests},
      TestCase{"reflected properties, fields, and member collisions",
               RunClassMemberTests},
      TestCase{"typed member access and lazy value caches",
               RunClassMemberAccessTests},
      TestCase{"member failure and side-effect boundaries",
               RunClassMemberBoundaryTests},
      TestCase{"explicit base edges, safe casts, and receiver adjustment",
               RunClassRelationshipTests},
      TestCase{"class operators and protected metamethods",
               RunClassOperatorTests},
      TestCase{"class userdata through the real virtual machine",
               RunClassUserdataIntegrationTests},
      TestCase{"the class member surface through the real virtual machine",
               RunClassMemberIntegrationTests},
      TestCase{"converted property and field values through TypeConverter<T>",
               RunConvertedMemberTests},
      TestCase{"converted operands for methods and operators through "
               "TypeConverter<T>",
               RunConvertedOperandTests},
      TestCase{"registered class instances as method and operator operands",
               RunClassOperandTests},
      TestCase{"native instance operands across registered classes",
               RunInstanceOperandTests},
      TestCase{"instance, table, and owned-pack return values",
               RunInstanceReturnTests},
      TestCase{"registered class instances as property and field values",
               RunInstanceMemberTests},
      TestCase{"class-scope constants on the class table",
               RunClassConstantTests},
      TestCase{"variadic parameters receiving userdata directly and nested "
               "inside tables",
               RunVariadicUserdataTests},
      TestCase{
          "inheritance, casts, and operators through the real virtual machine",
          RunInheritanceOperatorIntegrationTests},
      TestCase{"generic-for iteration of a class through its declared step",
               RunClassIterationTests},
      TestCase{"explicit function registration and overload selection",
               RunFunctionRegistrationTests},
      TestCase{"canonical overload sets and Pareto resolution",
               RunOverloadResolutionTests},
      TestCase{"optional, defaulted, and variadic parameter shapes",
               RunDeclaredParameterShapeTests},
      TestCase{"declared parameter shapes through the real virtual machine",
               RunDeclaredParameterIntegrationTests},
      TestCase{"zero, scalar, and multiple return shapes",
               RunMultipleReturnShapeTests},
      TestCase{"callable richness through the real virtual machine",
               RunCallableRichnessIntegrationTests},
      TestCase{"end-to-end registration and invocation matrix",
               RunEndToEndRegistrationInvocationMatrixTests},
      TestCase{"foundation compatibility matrix",
               RunFoundationCompatibilityMatrixTests},
      TestCase{"canonical identity and type model",
               RunCanonicalIdentityModelTests},
      TestCase{"immutable reflection database and snapshots",
               RunReflectionDatabaseTests},
      TestCase{"canonical reflection documentation surface and provenance",
               RunReflectionMetadataTests},
      TestCase{"canonical identity encoding and collision safety",
               RunCanonicalIdentityEncodingTests},
      TestCase{"pinned canonical identity vectors",
               RunCanonicalIdentityVectorTests},
      TestCase{"canonical type and conversion registry",
               RunCanonicalTypeRegistryTests},
      TestCase{"built-in and structural converters",
               RunStructuralConverterTests},
      TestCase{"public Luau-free conversion boundary",
               RunConversionBoundaryTests},
      TestCase{"conversion registry ranks, nullability, and reservations",
               RunConversionRegistryEdgeCaseTests},
      TestCase{"canonical conversion registry integration",
               RunConversionRegistryIntegrationTests},
      TestCase{"enumerators published as interned enumerator objects",
               RunEnumeratorObjectTests},
      TestCase{"semantic-version manifests and module resolution",
               RunModuleResolutionTests},
      TestCase{"transactional module loading", RunModuleLoadingTests},
      TestCase{"module graph loading through the real virtual machine",
               RunModuleGraphIntegrationTests},
      TestCase{"lifecycle closure analysis and canonical blockers",
               RunLifecycleClosureTests},
      TestCase{"every lifecycle blocker, dependency path, and userdata policy",
               RunLifecycleBlockerMatrixTests},
      TestCase{"staged and restored dynamic module lifecycle attempts",
               RunLifecycleStagingTests},
      TestCase{"atomically published compatible lifecycle generations",
               RunLifecyclePublicationTests},
      TestCase{"module unload and hot reload through the real machine",
               RunModuleLifecycleIntegrationTests},
      TestCase{"hierarchical registration through the real virtual machine",
               RunHierarchicalRegistrationIntegrationTests},
      TestCase{"deterministic documentation generation from one snapshot",
               RunDocumentationGenerationTests},
      TestCase{"deterministic Luau declaration generation from one snapshot",
               RunDeclarationGenerationTests},
      TestCase{"atomic artifact publication and destination preservation",
               RunArtifactPublicationTests},
      TestCase{"pinned golden and structural generated artifacts",
               RunGeneratorArtifactGoldenTests},
      TestCase{"refused roadmap extensions publish nothing",
               RunUnavailableExtensionBoundaryTests},
      TestCase{"delegate and signal bindings", RunDelegateBindingTests},
      TestCase{"profiling and debug-UI hook", RunProfilingHookTests},
      TestCase{"suspended coroutine and asynchronous invocation",
               RunAsynchronousInvocationTests},
      TestCase{"asynchronous invocation through the real virtual machine",
               RunAsynchronousExecutionIntegrationTests},
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
      TestCase{"property 18: stable canonical type and symbol identities",
               RunStableCanonicalIdentityProperties},
      TestCase{"property 19: permutation-invariant reflection enumeration",
               RunReflectionEnumerationOrderProperties},
      TestCase{"property 20: owning reflection snapshot generations",
               RunOwningReflectionSnapshotProperties},
      TestCase{"property 21: generalized transaction rollback",
               RunGeneralizedTransactionRollbackProperties},
      TestCase{"property 22: canonical conversion round trips",
               RunCanonicalConversionRoundTripProperties},
      TestCase{"property 23: load-once module resolution",
               RunLoadOnceModuleResolutionProperties},
      TestCase{"property 24: pareto overload resolution",
               RunParetoOverloadResolutionProperties},
      TestCase{"property 25: rich parameter and return shapes",
               RunRichSignatureShapeProperties},
      TestCase{"property 26: userdata ownership and lifetime transitions",
               RunUserdataOwnershipLifetimeProperties},
      TestCase{"property 27: construction and allocator cleanup milestones",
               RunAllocatorConstructionCleanupProperties},
      TestCase{"property 28: member receiver precedence and lazy caches",
               RunMemberReceiverAndLazyCacheProperties},
      TestCase{
          "property 29: inheritance and casts agree with unique accessible "
          "paths",
          RunInheritanceAndCastPathProperties},
      TestCase{"property 30: frozen caches equal uncached generation lookups",
               RunFrozenCacheEquivalenceProperties},
      TestCase{"property 31: module lifecycle publication follows the "
               "retained-generation state machine",
               RunDynamicModuleLifecycleProperties},
      TestCase{"property 32: suspended calls settle exactly once and resume "
               "through their retained generation",
               RunAsynchronousSettlementProperties},
      TestCase{"property 33: signal subscription and delivery follow the "
               "retained-generation handler model",
               RunSignalDeliveryProperties},
  };

  for (const auto &Test : Tests) {
    try {
      const int Result = Test.Run();
      if (Result != 0) {
        std ::cerr << "Test failed: " << Test.Name << " (code " << Result
                   << ")\n";
        return Result;
      }
    } catch (const std::exception &Error) {
      std ::cerr << "Test threw: " << Test.Name << ": " << Error.what() << '\n';
      return 1;
    } catch (...) {
      std ::cerr << "Test threw an unknown exception: " << Test.Name << '\n';
      return 1;
    }
  }

  return 0;
}
