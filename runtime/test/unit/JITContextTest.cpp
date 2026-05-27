#include <gtest/gtest.h>
#include <topo/jit.h>

#include <string>
#include <vector>

using namespace topo::jit;

TEST(JITContextTest, PruneEdgeAccumulates) {
    Context ctx;
    ctx.prune_edge("shadow", "composite");
    ctx.prune_edge("light", "composite");

    ASSERT_EQ(ctx.prunedEdges().size(), 2u);
    EXPECT_EQ(ctx.prunedEdges()[0].source, "shadow");
    EXPECT_EQ(ctx.prunedEdges()[0].target, "composite");
    EXPECT_EQ(ctx.prunedEdges()[1].source, "light");
    EXPECT_EQ(ctx.prunedEdges()[1].target, "composite");
}

TEST(JITContextTest, NarrowReturnsAccumulates) {
    Context ctx;
    ctx.narrow_returns("prepare", {"position", "normal"});
    ctx.narrow_returns("enhance", {"color"});

    ASSERT_EQ(ctx.narrowedReturns().size(), 2u);
    EXPECT_EQ(ctx.narrowedReturns()[0].func, "prepare");
    EXPECT_EQ(ctx.narrowedReturns()[0].fields.size(), 2u);
    EXPECT_EQ(ctx.narrowedReturns()[1].func, "enhance");
    EXPECT_EQ(ctx.narrowedReturns()[1].fields.size(), 1u);
}

TEST(JITContextTest, SetParams) {
    Context ctx;
    ctx.set("quality", "high");
    ctx.set("mode", "fast");

    ASSERT_EQ(ctx.params().size(), 2u);
    EXPECT_EQ(ctx.params()[0].first, "quality");
    EXPECT_EQ(ctx.params()[0].second, "high");
}

TEST(JITContextTest, AvailableReturnsBool) {
    // Just verify it returns without crashing
    // Actual availability depends on platform
    bool avail = topo::jit::available();
    (void)avail; // May or may not be true depending on test environment
}

TEST(JITContextTest, WildcardPruneEdge) {
    // Verify that wildcard source pruning works as the adaptive monitor
    // would use it: prune_edge("*", "cold_stage") marks all edges
    // targeting cold_stage for removal by the engine.
    Context ctx;
    ctx.prune_edge("*", "shadow");
    ctx.prune_edge("*", "ambient");
    ctx.set("aot_tti_cost", "4200");
    ctx.set("runtime_cost_ns", "15000");

    ASSERT_EQ(ctx.prunedEdges().size(), 2u);
    EXPECT_EQ(ctx.prunedEdges()[0].source, "*");
    EXPECT_EQ(ctx.prunedEdges()[0].target, "shadow");
    EXPECT_EQ(ctx.prunedEdges()[1].source, "*");
    EXPECT_EQ(ctx.prunedEdges()[1].target, "ambient");

    // Context params carry cost metadata for the engine
    ASSERT_EQ(ctx.params().size(), 2u);
    EXPECT_EQ(ctx.params()[0].first, "aot_tti_cost");
    EXPECT_EQ(ctx.params()[0].second, "4200");
    EXPECT_EQ(ctx.params()[1].first, "runtime_cost_ns");
    EXPECT_EQ(ctx.params()[1].second, "15000");
}

TEST(JITContextTest, CombinedConstraints) {
    // Verify that mixed constraint types accumulate correctly,
    // simulating a scenario where the adaptive monitor detects both
    // cold stages (for pruning) and unused return fields (for narrowing).
    Context ctx;
    ctx.prune_edge("*", "cold_pass");
    ctx.narrow_returns("prepare", {"position"});
    ctx.set("aot_tti_cost", "1000");

    EXPECT_EQ(ctx.prunedEdges().size(), 1u);
    EXPECT_EQ(ctx.narrowedReturns().size(), 1u);
    EXPECT_EQ(ctx.params().size(), 1u);
}
