# topo-llvm/runtime/test/FeatureMap.cmake — Feature dimension labels for
# topo-runtime-tests (C ABI runtime libraries).
#
# Mechanism: For each suite prefix in the map, all matching GTest test names
# (SuitePrefix.CaseName) receive the specified feature labels. CTest silently
# ignores set_tests_properties calls for non-existent test names.
#
# Usage: ctest --test-dir build -L parallel
#        ctest --test-dir build -L cabi

# ======================================================================
# topo-runtime unit tests
# ======================================================================

# Parallel runtime
set_tests_properties(
    ParallelRuntimeTest.SpawnAndAwaitSingleTask
    ParallelRuntimeTest.SpawnNTasksAwaitAll
    ParallelRuntimeTest.SpawnRetReturnsResult
    ParallelRuntimeTest.WorkStealingImbalanced
    ParallelRuntimeTest.CostSamplingHeavyGtLight
    ParallelRuntimeTest.ResetCostSamples
    ParallelRuntimeLazyInit.EnsureInitDoesNotCrash
    PROPERTIES LABELS "toolchain;parallel")
set_tests_properties(ParallelRuntimeTest.SpawnRetPriHighPriority
    PROPERTIES LABELS "toolchain;priority;parallel")

# JIT runtime
set_tests_properties(
    JITContextTest.PruneEdgeAccumulates
    JITContextTest.NarrowReturnsAccumulates
    JITContextTest.SetParams
    JITContextTest.AvailableReturnsBool
    JITEdgeCaseTest.SpecializeWithMalformedNameDoesNotCrash
    JITEdgeCaseTest.SpecializeWithEmptyNameReturnsNull
    JITEdgeCaseTest.SpecializeWithContradictoryConstraintsDegradesCleanly
    JITEdgeCaseTest.PendingSpecializeOutlivesContext
    JITEdgeCaseTest.ManyContextsDoNotAccumulateState
    JITEdgeCaseTest.DumpIRFallbackOnUnknownSymbol
    JITEdgeCaseTest.ConcurrentSpecializeFromMultipleThreads
    JITEdgeCaseTest.RepeatedAvailableQueriesAreIdempotent
    PROPERTIES LABELS "toolchain;jit")

# Adaptive runtime
set_tests_properties(
    AdaptiveMonitorTest.InitAndShutdown
    AdaptiveMonitorTest.DoubleInitIsSafe
    AdaptiveMonitorTest.ShutdownWithoutInitIsSafe
    AdaptiveMonitorTest.RegisterPipeline
    AdaptiveMonitorTest.StatsAfterInit
    AdaptiveMonitorTest.MaxVersionsRespected
    AdaptiveMonitorTest.RapidInitShutdownCycles
    AdaptiveMonitorTest.RegisterBeforeInitIsSafe
    AdaptiveMonitorTest.CallsAfterShutdownAreSafe
    AdaptiveMonitorTest.ManyRegisteredPipelines
    AdaptiveMonitorTest.MonitorStartStopRace
    AdaptiveMonitorTest.StatsSnapshotDuringActiveMonitoring
    PROPERTIES LABELS "toolchain;adaptive")

# Observability — runtime
set_tests_properties(
    ObserveRuntimeTest.InitAndShutdown
    ObserveRuntimeTest.DoubleInitIsSafe
    ObserveRuntimeTest.ShutdownWithoutInitIsSafe
    ObserveRuntimeTest.SpanBeginEnd
    ObserveRuntimeTest.NestedSpans
    ObserveRuntimeTest.SpanEndWithoutBeginIsSafe
    ObserveRuntimeTest.ZeroSamplingRateSkipsAll
    ObserveRuntimeTest.ThreadSafety
    ObserveRuntimeTest.ConcurrentSpanStressEightThreads
    ObserveRuntimeTest.DeepNestingFiveHundredLayers
    ObserveRuntimeTest.SpanEndOnEmptyStackConcurrent
    ObserveRuntimeTest.CallsBeforeInitDoNotCrash
    ObserveRuntimeTest.CallsAfterShutdownDoNotCrash
    ObserveRuntimeTest.ZeroSamplingRateConcurrentStress
    ObserveRuntimeTest.RapidInitShutdownCycles
    ObserveRuntimeTest.VeryLongSpanName
    PROPERTIES LABELS "toolchain;observability")

# Arena runtime
set_tests_properties(
    ArenaRuntimeTest.CreateAndDestroy
    ArenaRuntimeTest.AllocAndUse
    ArenaRuntimeTest.MultipleAllocations
    ArenaRuntimeTest.ResetFreesUsage
    ArenaRuntimeTest.AlignmentRespected
    ArenaRuntimeTest.NullArenaHandled
    ArenaRuntimeTest.ZeroSizeReturnsNull
    PROPERTIES LABELS "toolchain;lifetime")

# C ABI contract tests
set_tests_properties(
    CABIContract.ParallelSymbols
    CABIContract.ParallelSignatures
    PROPERTIES LABELS "toolchain;parallel;cabi")
set_tests_properties(
    CABIContract.AdaptiveSymbols
    CABIContract.AdaptiveSignatures
    PROPERTIES LABELS "toolchain;adaptive;cabi")
set_tests_properties(
    CABIContract.JitEngineSignatureTypes
    CABIContract.JitEngineConstants
    PROPERTIES LABELS "toolchain;jit;cabi")
set_tests_properties(
    CABIContract.ObserveSymbols
    CABIContract.ObserveSignatures
    PROPERTIES LABELS "toolchain;observability;cabi")
set_tests_properties(
    CABIContract.ArenaSymbols
    CABIContract.ArenaSignatures
    PROPERTIES LABELS "toolchain;lifetime;cabi")
set_tests_properties(
    CABIContract.HeadersCompileTogetherWithoutConflict
    CABIContract.NamingConvention
    PROPERTIES LABELS "toolchain;cabi")
