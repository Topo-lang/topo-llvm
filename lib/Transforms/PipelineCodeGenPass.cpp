// @category: INFRA
#include "topo/Transforms/PipelineCodeGenPass.h"
#include "topo/Analysis/PipelineAnalysis.h"

#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace topo {

namespace {

/// Check if a function body contains a call to pipeline_placeholder.
/// Detection strategy:
///   1. Primary: check for demangled name containing
///      "topo::detail::pipeline_placeholder"
///   2. This covers both annotation-based and name-based detection.
bool isPipelineStub(llvm::Function& func) {
    for (auto& BB : func) {
        for (auto& I : BB) {
            auto* call = llvm::dyn_cast<llvm::CallInst>(&I);
            if (!call) continue;

            auto* callee = call->getCalledFunction();
            if (!callee) continue;

            std::string name = llvm::demangle(callee->getName().str());
            // C++ pipeline placeholder
            if (name.find("topo::detail::pipeline_placeholder") != std::string::npos) return true;
            // Rust pipeline placeholder
            if (name.find("topo::pipeline::placeholder") != std::string::npos) return true;
        }
    }
    return false;
}

/// Resolve a qualified function name to an IR function using the mapping.
llvm::Function* resolveFunction(const std::string& qualifiedName, const SymbolMapping& mapping) {
    auto it = mapping.matched.find(qualifiedName);
    if (it != mapping.matched.end()) return it->second;
    return nullptr;
}

// Pipeline DAG utilities delegated to topo-core/Analysis/PipelineAnalysis.
using analysis::findUpstreamNodes;
using analysis::groupNodesByStage;
using analysis::isSourceNode;

/// Match pipeline function parameters to a source node's callee parameters
/// by type. Simple positional matching for source nodes — the pipeline
/// function's parameters are matched to the callee's parameters by type
/// compatibility.
/// Returns empty vector on failure.
std::vector<llvm::Value*> matchSourceNodeArgs(llvm::Function& pipelineFunc,
                                              llvm::Function& calleeFunc,
                                              const std::string& pipelineName,
                                              const std::string& nodeName) {
    std::vector<llvm::Value*> args;

    // For each callee parameter, find a matching pipeline parameter by type
    for (unsigned i = 0; i < calleeFunc.arg_size(); ++i) {
        auto* paramTy = calleeFunc.getArg(i)->getType();
        bool found = false;

        for (auto& pipelineArg : pipelineFunc.args()) {
            if (pipelineArg.getType() == paramTy) {
                // Check if this pipeline arg was already used
                bool alreadyUsed = false;
                for (auto* existing : args) {
                    if (existing == &pipelineArg) {
                        alreadyUsed = true;
                        break;
                    }
                }
                if (!alreadyUsed) {
                    args.push_back(&pipelineArg);
                    found = true;
                    break;
                }
            }
        }

        if (!found) {
            // Fallback: try positional matching
            if (i < pipelineFunc.arg_size()) {
                args.push_back(pipelineFunc.getArg(i));
            } else {
                llvm::errs() << "error: pipeline '" << pipelineName << "': source node '" << nodeName << "' parameter "
                             << i << " has no matching pipeline argument\n";
                return {};
            }
        }
    }

    return args;
}

/// Generate the pipeline function body based on the DAG analysis.
bool generatePipelineBody(llvm::Function& pipelineFunc,
                          const LogicBlockEntry& logicBlock,
                          const PipelineAnalysis& analysis,
                          const SymbolMapping& mapping,
                          const SymbolTable& /*symbols*/) {
    auto& ctx = pipelineFunc.getContext();

    // Clear existing function body
    pipelineFunc.deleteBody();

    // Create entry basic block
    auto* entryBB = llvm::BasicBlock::Create(ctx, "entry", &pipelineFunc);
    llvm::IRBuilder<> builder(entryBB);

    // Storage for node outputs: nodeName -> call result value
    std::unordered_map<std::string, llvm::Value*> nodeOutputs;

    // Process nodes in stage order
    auto stageGroups = groupNodesByStage(analysis);

    for (const auto& [stage, nodes] : stageGroups) {
        for (const auto& nodeName : nodes) {
            // Resolve node name to qualified callee via analysis utility
            std::string qualifiedCallee = analysis::resolveNodeCallee(nodeName, logicBlock.calledFunctions);
            if (qualifiedCallee.empty()) {
                // Try the node name as-is (might already be qualified)
                qualifiedCallee = nodeName;
            }

            auto* calleeFunc = resolveFunction(qualifiedCallee, mapping);
            if (!calleeFunc) continue;

            // Collect arguments
            std::vector<llvm::Value*> args;

            if (isSourceNode(nodeName, analysis.sourceNodes)) {
                // Source node: match from pipeline parameters
                args = matchSourceNodeArgs(pipelineFunc, *calleeFunc, logicBlock.qualifiedName, nodeName);
                if (args.empty() && calleeFunc->arg_size() > 0) return false;
            } else {
                // Non-source node: collect from upstream outputs
                auto upstream = findUpstreamNodes(nodeName, logicBlock.edges);

                // Build available values from upstream outputs
                // For each upstream, extract fields from its return value
                std::vector<std::pair<llvm::Type*, llvm::Value*>> availableValues;

                for (const auto& upName : upstream) {
                    auto outputIt = nodeOutputs.find(upName);
                    if (outputIt == nodeOutputs.end()) continue;

                    llvm::Value* upOutput = outputIt->second;
                    auto* upTy = upOutput->getType();

                    if (auto* sty = llvm::dyn_cast<llvm::StructType>(upTy)) {
                        // Extract each field from the struct
                        for (unsigned fi = 0; fi < sty->getNumElements(); ++fi) {
                            auto* extracted = builder.CreateExtractValue(upOutput, {fi});
                            availableValues.emplace_back(sty->getElementType(fi), extracted);
                        }
                    } else {
                        // Scalar output
                        availableValues.emplace_back(upTy, upOutput);
                    }
                }

                // Match available values to callee parameters by type
                std::vector<bool> used(availableValues.size(), false);
                for (unsigned pi = 0; pi < calleeFunc->arg_size(); ++pi) {
                    auto* paramTy = calleeFunc->getArg(pi)->getType();

                    // Handle sret parameter
                    if (calleeFunc->getArg(pi)->hasStructRetAttr()) {
                        // Create alloca for sret return
                        auto* sretTy = calleeFunc->getArg(pi)->getParamStructRetType();
                        auto* alloca = builder.CreateAlloca(sretTy);
                        args.push_back(alloca);
                        continue;
                    }

                    bool matched = false;
                    for (unsigned vi = 0; vi < availableValues.size(); ++vi) {
                        if (!used[vi] && availableValues[vi].first == paramTy) {
                            args.push_back(availableValues[vi].second);
                            used[vi] = true;
                            matched = true;
                            break;
                        }
                    }

                    if (!matched) {
                        // Try pipeline parameters as fallback
                        bool fromPipeline = false;
                        for (auto& pipelineArg : pipelineFunc.args()) {
                            if (pipelineArg.getType() == paramTy) {
                                args.push_back(&pipelineArg);
                                fromPipeline = true;
                                break;
                            }
                        }
                        if (!fromPipeline) {
                            llvm::errs() << "error: pipeline '" << logicBlock.qualifiedName << "': node '" << nodeName
                                         << "' parameter " << pi
                                         << " has no matching upstream output "
                                            "or pipeline argument\n";
                            return false;
                        }
                    }
                }
            }

            // Handle sret-style callee: check if first param is sret
            bool calleeSret = calleeFunc->arg_size() > 0 && calleeFunc->getArg(0)->hasStructRetAttr();
            llvm::Value* sretAlloca = nullptr;

            if (calleeSret && !args.empty() && llvm::isa<llvm::AllocaInst>(args[0])) {
                sretAlloca = args[0];
            }

            // Create the call
            auto* callResult = builder.CreateCall(calleeFunc, args);

            // Store the output
            if (calleeSret && sretAlloca) {
                // For sret, load the result from the alloca
                auto* sretTy = calleeFunc->getArg(0)->getParamStructRetType();
                auto* loaded = builder.CreateLoad(sretTy, sretAlloca);
                nodeOutputs[nodeName] = loaded;
            } else {
                nodeOutputs[nodeName] = callResult;
            }
        }
    }

    // Generate return from the terminal node's output
    auto terminalIt = nodeOutputs.find(analysis.terminalNode);
    if (terminalIt == nodeOutputs.end()) {
        auto* retTy = pipelineFunc.getReturnType();
        if (retTy->isVoidTy()) {
            builder.CreateRetVoid();
        } else {
            llvm::errs() << "error: pipeline '" << logicBlock.qualifiedName << "': terminal node '"
                         << analysis.terminalNode
                         << "' produced no output but pipeline expects "
                            "a return value\n";
            return false;
        }
        return true;
    }

    llvm::Value* terminalResult = terminalIt->second;
    auto* pipelineRetTy = pipelineFunc.getReturnType();

    if (pipelineRetTy->isVoidTy()) {
        builder.CreateRetVoid();
    } else if (terminalResult->getType() == pipelineRetTy) {
        // Direct match
        builder.CreateRet(terminalResult);
    } else if (auto* sty = llvm::dyn_cast<llvm::StructType>(terminalResult->getType())) {
        // Terminal returns a struct but pipeline returns a scalar —
        // extract the appropriate field
        for (unsigned i = 0; i < sty->getNumElements(); ++i) {
            if (sty->getElementType(i) == pipelineRetTy) {
                auto* extracted = builder.CreateExtractValue(terminalResult, {i});
                builder.CreateRet(extracted);
                return true;
            }
        }
        llvm::errs() << "error: pipeline '" << logicBlock.qualifiedName << "': terminal node '" << analysis.terminalNode
                     << "' returns a struct with no field matching "
                        "the pipeline return type\n";
        return false;
    } else {
        llvm::errs() << "error: pipeline '" << logicBlock.qualifiedName << "': terminal node '" << analysis.terminalNode
                     << "' output type does not match pipeline return type\n";
        return false;
    }

    return true;
}

} // anonymous namespace

int PipelineCodeGenPass::run(llvm::Module& /*module*/, const SymbolTable& symbols, const SymbolMapping& mapping) {
    int generated = 0;

    for (const auto& [name, logicBlock] : symbols.logicBlocks()) {
        if (!logicBlock.isPipeline || !logicBlock.pipelineAnalysis) continue;

        const auto& analysis = *logicBlock.pipelineAnalysis;

        auto* pipelineFunc = resolveFunction(logicBlock.qualifiedName, mapping);
        if (!pipelineFunc) continue;

        if (pipelineFunc->isDeclaration()) continue;

        if (!isPipelineStub(*pipelineFunc)) continue;

        if (generatePipelineBody(*pipelineFunc, logicBlock, analysis, mapping, symbols)) {
            ++generated;
        } else {
            return -1;
        }
    }

    return generated;
}

} // namespace topo
