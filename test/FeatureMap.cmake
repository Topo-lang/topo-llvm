# topo-llvm/test/FeatureMap.cmake — Feature dimension labels for topo-llvm
# unit tests (IR passes) and integration tests (Builder/Verifier/JIT/Adaptive).
#
# Mechanism: For each suite prefix in the map, all matching GTest test names
# (SuitePrefix.CaseName) receive the specified feature labels. CTest silently
# ignores set_tests_properties calls for non-existent test names.
#
# Usage: ctest --test-dir build -L visibility
#        ctest --test-dir build -L parallel

# ======================================================================
# topo-llvm unit: IR passes
# ======================================================================

# Parallel — IR pass
set_tests_properties(
    TopoParallelPassTest.DisabledConfigNoChanges
    TopoParallelPassTest.HighThresholdSkipsParallelization
    TopoParallelPassTest.MinTasksTooHighSkips
    TopoParallelPassTest.RuntimeCostsOverrideTTI
    TopoParallelPassTest.SeparateThresholdsTTIvsNS
    TopoParallelPassTest.ExcludeListSkipsFunctions
    TopoParallelPassTest.PriorityReducesThreshold
    PROPERTIES LABELS "toolchain;ir;parallel")

# Adaptive — IR passes
set_tests_properties(
    AdaptiveParallelTest.RuntimeCostsSkipCheapFunction
    AdaptiveParallelTest.RuntimeCostsBelowThresholdSkips
    AdaptiveParallelTest.RuntimeCostsBothAboveThresholdParallelizes
    AdaptiveParallelTest.NoRuntimeCostsHighThresholdSkips
    PROPERTIES LABELS "toolchain;ir;adaptive")

set_tests_properties(
    AdaptiveDispatchPassTest.DisabledConfigNoChanges
    AdaptiveDispatchPassTest.EnabledInsertsDispatch
    AdaptiveDispatchPassTest.JitPtrGlobalCreated
    AdaptiveDispatchPassTest.AtomicLoadInEntry
    AdaptiveDispatchPassTest.GlobalCtorRegistered
    AdaptiveDispatchPassTest.CostInstrumentationInserted
    PROPERTIES LABELS "toolchain;ir;adaptive")

# Adaptive + JIT cross-feature
set_tests_properties(
    AdaptiveJITTest.PassPipelineWithAdaptive
    AdaptiveJITTest.DisabledAdaptiveNoChanges
    PROPERTIES LABELS "toolchain;ir;adaptive;jit")
set_tests_properties(AdaptiveJITTest.AdaptiveWithParallel
    PROPERTIES LABELS "toolchain;ir;adaptive;parallel")

# JIT — IR passes
set_tests_properties(
    JITSpecializeFixture.PreCodegenIR_CapturesPipelineStub
    JITSpecializeFixture.PreCodegenIR_VsPostCodegen
    JITSpecializeFixture.Metadata_IncludesCalledFunctions
    JITSpecializeFixture.Step7_RegeneratePipelineFromSerializedIR
    JITSpecializeFixture.Step7_PruneEdge_RemovesUnreachableNode
    PROPERTIES LABELS "toolchain;ir;jit")

# IR Embed (JIT infrastructure)
set_tests_properties(
    IREmbedTest.SerializeBitcodeRoundTrip
    IREmbedTest.SerializeMetadataJSON
    IREmbedTest.EmbedCreatesGlobalVariables
    PROPERTIES LABELS "toolchain;ir;jit")

# Data Layout (AoS to SoA)
set_tests_properties(
    DataLayoutPassTest.DisabledConfigNoChanges
    DataLayoutPassTest.SmallArraySkipped
    DataLayoutPassTest.DetectsTopoArrayOfStruct
    DataLayoutPassTest.FieldUsageAnalysis
    DataLayoutPassTest.TransformInsertsSoAAllocas
    DataLayoutPassTest.TransformPreservesSignature
    DataLayoutPassTest.NonPipelineFunctionUnchanged
    DataLayoutPassTest.ScalarArraySkipped
    PROPERTIES LABELS "toolchain;ir;data_layout")

# Data layout auto-select (Phase D)
set_tests_properties(
    LayoutBenchmarkTest.GenerateVariantsProducesPair
    LayoutBenchmarkTest.SoAVariantUsesHeapAllocation
    LayoutBenchmarkTest.SoAVariantVerifiesClean
    LayoutBenchmarkTest.DisabledConfigNoVariants
    LayoutBenchmarkTest.SmallArrayNoVariants
    LayoutBenchmarkTest.RandomAccessStillGeneratesVariants
    LayoutBenchmarkTest.BenchmarkRunsAndReturnsResult
    LayoutBenchmarkTest.BenchmarkSelectsWinnerInPassPipeline
    PROPERTIES LABELS "toolchain;ir;data_layout")

# Data layout cost model (Phase D cross-compilation fallback)
set_tests_properties(
    LayoutCostModelTest.EstimateReturnsResult
    LayoutCostModelTest.StreamingBiasesTowardSoA
    LayoutCostModelTest.RandomBiasesTowardAoS
    LayoutCostModelTest.GatherScatterBiasesTowardAoS
    LayoutCostModelTest.SmallCardinalityPenalizesSoA
    LayoutCostModelTest.LargeCardinalityFavorsSoA
    LayoutCostModelTest.NullFunctionsReturnNullopt
    LayoutCostModelTest.BreakdownPopulatesCosts
    LayoutCostModelTest.BreakdownCacheUtilization
    LayoutCostModelTest.BreakdownVectorWidth
    LayoutCostModelTest.BreakdownNativeNotCrossCompilation
    LayoutCostModelTest.CrossCompilationDetected
    LayoutCostModelTest.NativeNotCrossCompilation
    LayoutCostModelTest.UnknownTripleNotCrossCompilation
    LayoutCostModelTest.FormatRemarkContainsCosts
    LayoutCostModelTest.FormatRemarkNativeOmitsCrossCompilation
    LayoutCostModelTest.FallbackChainReturnsResultForNative
    PROPERTIES LABELS "toolchain;ir;data_layout")

# Indirection (smart pointer/span optimization)
set_tests_properties(
    IndirectionPassTest.Owned_NoNameMatch_Promoted
    IndirectionPassTest.Shared_NoNameMatch_Detected
    IndirectionPassTest.Weak_NoAutoDeref
    IndirectionPassTest.Owned_WithAutoDeref_Nonnull
    IndirectionPassTest.Shared_BatchElimination
    IndirectionPassTest.Disabled_NoChanges
    IndirectionPassTest.UniquePtr_Promoted
    IndirectionPassTest.UniquePtr_StoreSkip
    IndirectionPassTest.UniquePtr_MoveSkip
    IndirectionPassTest.UniquePtr_NonnullMetadata
    IndirectionPassTest.SharedPtr_ExclusiveStage
    IndirectionPassTest.SharedPtr_ConcurrentSkip
    IndirectionPassTest.SharedPtr_SequentialOpt
    IndirectionPassTest.Vector_NoResize
    IndirectionPassTest.Vector_ResizeSkip
    IndirectionPassTest.Vector_NonnullAttr
    IndirectionPassTest.PointerAttr_NonnullInferred
    IndirectionPassTest.PointerAttr_NoaliasStage
    IndirectionPassTest.PointerAttr_ExternalBlocks
    IndirectionPassTest.NonTopoFunction_Skip
    IndirectionPassTest.O0_Skip
    IndirectionPassTest.AutoMode_BenchmarkAppliesWhenBeneficial
    IndirectionPassTest.AutoMode_ForceConfigOverride
    PROPERTIES LABELS "toolchain;ir;indirection")

# Loop auto-parallel
set_tests_properties(
    LoopParallelizePassTest.DisabledConfigNoChanges
    LoopParallelizePassTest.ParallelStageFunctionsAnnotated
    LoopParallelizePassTest.SingleStageFunctionSkipped
    LoopParallelizePassTest.ExcludeListSkipsFunctions
    LoopParallelizePassTest.AccessGroupMetadataPresent
    LoopParallelizePassTest.VectorizeEnableMetadataAbsent
    PROPERTIES LABELS "toolchain;ir;loop_parallel")

# Observability — IR pass
set_tests_properties(
    ObservabilityPassTest.DisabledConfigNoChanges
    ObservabilityPassTest.EnabledInsertsSpans
    ObservabilityPassTest.InternalStagesSkipped
    ObservabilityPassTest.InternalStagesInstrumented
    ObservabilityPassTest.SpanNameContainsStageNumber
    PROPERTIES LABELS "toolchain;ir;observability")

# ======================================================================
# topo-llvm integration: Builder/Verifier
# ======================================================================

# Visibility — Verifier
set_tests_properties(
    Verifier.AllPublicPresent
    Verifier.MissingPublicSymbol
    Verifier.ProtectedMissingNoPublicError
    PROPERTIES LABELS "toolchain;ir;visibility")

set_tests_properties(
    VerifierSignature.MatchingSignaturesPasses
    VerifierSignature.WrongReturnType
    VerifierSignature.WrongParamCount
    VerifierSignature.VoidReturnMatches
    PROPERTIES LABELS "toolchain;ir;visibility")

# Stages — Verifier
set_tests_properties(
    VerifierStageOrder.StageOrderRespected
    VerifierStageOrder.StageOrderViolated
    PROPERTIES LABELS "toolchain;ir;stages")

set_tests_properties(
    VerifierStageParallel.IndependentSameStageOps
    VerifierStageParallel.DirectDataDependency
    VerifierStageParallel.DifferentStageDependencyOk
    VerifierStageParallel.SingleOpStageNoCheck
    PROPERTIES LABELS "toolchain;ir;stages")

set_tests_properties(
    Verifier.FnBlockMatchesIRCalls
    Verifier.FnBlockExtraDeclaration
    Verifier.IRCallsExtraTopoSymbol
    VerifierFnBlock.FnBlockMismatchIsError
    PROPERTIES LABELS "toolchain;ir;stages")

# Pipeline
set_tests_properties(
    BuilderPipeline.FullPipelineEndToEnd
    PROPERTIES LABELS "toolchain;ir;pipeline")
set_tests_properties(
    PipelineCodeGen.MismatchedParameterReturnsError
    PROPERTIES LABELS "toolchain;ir;pipeline")
set_tests_properties(
    VerifierPipelineEdge.PipelineTerminalCorrect
    VerifierPipelineEdge.PipelineTerminalWrong
    PROPERTIES LABELS "toolchain;ir;pipeline")

# Visibility — InternalVisibility
set_tests_properties(
    InternalVisibility.BasicInternalCollectedAsInternal
    InternalVisibility.NamespaceCollectedAsInternal
    InternalVisibility.SymbolMapperMapsInternal
    InternalVisibility.ApplierSetsInternalLinkage
    InternalVisibility.DebugInternalPreservesDebugInfo
    InternalVisibility.DefaultStripsDebugSubprogram
    PROPERTIES LABELS "toolchain;ir;visibility")

# Visibility — Const propagation and collector
set_tests_properties(
    ConstPropagation.CollectorExtractsConstFunction
    ConstPropagation.ApplierSetsMemoryReadForConstFunction
    VisibilityCollector.ComptimeIfFunctionsCollected
    VisibilityApplier.StripsO0NoInline
    PROPERTIES LABELS "toolchain;ir;visibility")

# Visibility — Builder pipeline
set_tests_properties(
    BuilderPipeline.VisibilityCollectorDemo
    BuilderPipeline.SymbolMapperMatchesFunctions
    BuilderPipeline.VisibilityApplierSetsLinkage
    PROPERTIES LABELS "toolchain;ir;visibility;pipeline")

# Visibility — Custom passes
set_tests_properties(
    CustomPass.InlinePassInlinesPrivate
    CustomPass.FlattenPassRemovesDeadPrivate
    PROPERTIES LABELS "toolchain;ir;visibility")
set_tests_properties(CustomPass.LayoutPassSetsSections
    PROPERTIES LABELS "toolchain;ir")

# Visibility — Obfuscation
set_tests_properties(
    Obfuscation.NormalModeDeterministic
    Obfuscation.SaltedModeDifferent
    Obfuscation.PublicSymbolUnchanged
    Obfuscation.ProtectedMappingGenerated
    PROPERTIES LABELS "toolchain;ir;visibility")

# Classes
set_tests_properties(
    VerifierClassMember.ClassMemberPresent
    VerifierClassMember.ClassMemberMissing
    PROPERTIES LABELS "toolchain;ir;classes")

# Templates
set_tests_properties(
    VerifierTemplate.InstantiationPresentInIR
    VerifierTemplate.InstantiationMissingFromIR
    VerifierTemplate.FunctionTemplateInstantiation
    VerifierTemplate.MultiArgInstantiation
    PROPERTIES LABELS "toolchain;ir;templates")
set_tests_properties(
    VerifierConstraint.AdaptedTypeAllMembersPresent
    VerifierConstraint.AdaptedTypeMissingFunction
    VerifierConstraint.ConstrainedInstantiationWithoutAdapt
    VerifierConstraint.InheritedConstraintMembers
    PROPERTIES LABELS "toolchain;ir;templates")

# Rust ABI
set_tests_properties(
    RustABI.RustV0ManglingDetected
    RustABI.RustRuntimeSymbolsSkipped
    PROPERTIES LABELS "toolchain;ir")

# Aggressive mode
set_tests_properties(AggressiveMode.ThinLTOPipelineRuns
    PROPERTIES LABELS "toolchain;ir")

# ======================================================================
# topo-llvm unit: topo-prof hints
# ======================================================================

# TopoProfHints (topo-prof hints subcommand)
set_tests_properties(
    TopoProfHintsTest.LegacyFlatFormatBackwardCompatible
    TopoProfHintsTest.ExtendedStructuredFormatParsed
    TopoProfHintsTest.CardinalityDeviationWarning
    TopoProfHintsTest.CardinalityWithinRangeIsOk
    TopoProfHintsTest.CardinalityBelowMinIsInfo
    TopoProfHintsTest.StreamingHighMissRateWarning
    TopoProfHintsTest.RandomLowMissRateInfo
    TopoProfHintsTest.RandomHighMissRateOk
    TopoProfHintsTest.FormatCardinalityValues
    TopoProfHintsTest.MixedFormatEntries
    TopoProfHintsTest.HintsReportJSONFormat
    PROPERTIES LABELS "toolchain;hints")
