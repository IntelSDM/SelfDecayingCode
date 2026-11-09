#pragma once

#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/CommandLine.h"

struct SelfDecayingCodePass : llvm::PassInfoMixin<SelfDecayingCodePass>
{
    llvm::PreservedAnalyses run(llvm::Module& m, llvm::ModuleAnalysisManager&);
};

bool RunSelfDecayingCode(llvm::Module& m);

extern llvm::cl::opt<bool> SelfDecayingCodeEnable;