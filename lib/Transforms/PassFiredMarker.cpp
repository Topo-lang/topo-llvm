// @category: INFRA
#include "topo/Transforms/PassFiredMarker.h"

#include <llvm/ADT/Twine.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Type.h>

#include <string>

namespace topo {

void markPassFired(llvm::Module& module, llvm::StringRef passName, unsigned count) {
    if (count == 0) return;

    auto& ctx = module.getContext();
    std::string mdName = (llvm::Twine("topo.fired.") + passName).str();

    auto* named = module.getOrInsertNamedMetadata(mdName);

    // Sum into the existing marker if present — preserves the invariant that
    // the total count reflects every transformation the pass performed, even
    // across multiple invocations in the same pipeline run.
    unsigned existing = 0;
    if (named->getNumOperands() > 0) {
        if (auto* node = named->getOperand(0)) {
            if (node->getNumOperands() > 0) {
                if (auto* cam = llvm::dyn_cast<llvm::ConstantAsMetadata>(node->getOperand(0))) {
                    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(cam->getValue())) {
                        existing = static_cast<unsigned>(ci->getZExtValue());
                    }
                }
            }
        }
        named->clearOperands();
    }

    unsigned total = existing + count;
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* countConst = llvm::ConstantInt::get(i32Ty, total);
    auto* node = llvm::MDNode::get(ctx, {llvm::ConstantAsMetadata::get(countConst)});
    named->addOperand(node);
}

} // namespace topo
