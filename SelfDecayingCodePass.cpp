#include "SelfDecayingCodePass.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

llvm::cl::opt<bool> SelfDecayingCodeEnable("obfselfdecayingcode", llvm::cl::init(true), llvm::cl::desc("Master Toggle"));

static constexpr bool SkipLoops = true;

enum : uint8_t
{
    OpcodeInvalid = 0,
    OpcodeAdd = 1,
    OpcodeSub = 2,
    OpcodeXor = 3,
    OpcodeMul = 4,
    OpcodeAnd = 5,
    OpcodeOr = 6,
    OpcodeShl = 7,
    OpcodeLShr = 8,
    OpcodeAShr = 9,
    OpcodeUDiv = 10,
    OpcodeSDiv = 11,
    OpcodeURem = 12,
    OpcodeSRem = 13,
    OpcodeCount = 14,
};

static bool OpNeedsSextA(uint8_t op)
{
    return op == OpcodeAShr || op == OpcodeSDiv || op == OpcodeSRem;
}

static bool OpNeedsSextB(uint8_t op)
{
    return op == OpcodeSDiv || op == OpcodeSRem;
}

static uint8_t PickOpcode(unsigned op)
{
    switch (op)
    {
        case Instruction::Add: return OpcodeAdd;
        case Instruction::Sub: return OpcodeSub;
        case Instruction::Xor: return OpcodeXor;
        case Instruction::Mul: return OpcodeMul;
        case Instruction::And: return OpcodeAnd;
        case Instruction::Or: return OpcodeOr;
        case Instruction::Shl: return OpcodeShl;
        case Instruction::LShr: return OpcodeLShr;
        case Instruction::AShr: return OpcodeAShr;
        case Instruction::UDiv: return OpcodeUDiv;
        case Instruction::SDiv: return OpcodeSDiv;
        case Instruction::URem: return OpcodeURem;
        case Instruction::SRem: return OpcodeSRem;
        default: return OpcodeInvalid;
    }
}

static std::string MakeSiteName()
{
    static std::mt19937_64 rng{std::random_device{}()};
    static const char hex[] = "0123456789abcdef";
    std::string s = "__selfdecay_site_";
    uint64_t v = rng();
    for (int i = 0; i < 10; ++i)
    {
        s.push_back(hex[v & 0xf]);
        v >>= 4;
    }
    return s;
}

static bool HasAnnotationTag(Function& f, const char* tag)
{
    Module* m = f.getParent();
    GlobalVariable* ga = m->getGlobalVariable("llvm.global.annotations");
    if (!ga) return false;

    ConstantArray* ca = dyn_cast<ConstantArray>(ga->getOperand(0));
    if (!ca) return false;

    for (unsigned i = 0; i < ca->getNumOperands(); ++i)
    {
        ConstantStruct* cs = dyn_cast<ConstantStruct>(ca->getOperand(i));
        if (!cs) continue;
        if (cs->getOperand(0)->stripPointerCasts() != &f) continue;

        auto* strgv = dyn_cast<GlobalVariable>(cs->getOperand(1)->stripPointerCasts());
        if (!strgv || !strgv->hasInitializer()) continue;

        auto* cds = dyn_cast<ConstantDataSequential>(strgv->getInitializer());
        if (!cds || !cds->isString()) continue;

        if (cds->getAsCString() == tag) return true;
    }
    return false;
}

static bool HasSelfDecayAnnotation(Function& f)
{
    return HasAnnotationTag(f, "selfdecay");
}

static bool HasNoSelfDecayAnnotation(Function& f)
{
    return HasAnnotationTag(f, "noselfdecay");
}

static bool WantsSelfDecay(Function& f)
{
    if (HasNoSelfDecayAnnotation(f)) return false;
    return HasSelfDecayAnnotation(f);
}

static bool HasFuncletBody(Function& f)
{
    // Inserting a plain CallInst inside a catchpad or cleanuppad without a funclet operand bundle passes the verifier on MSVC EH personality functions but silently miscompiles at codegen because the backend emits the call in the wrong funclet section; this pass does not attach funclet bundles so any F with funclet body IR is skipped outright
    for (BasicBlock& bb : f)
    {
        for (Instruction& i : bb)
        {
            if (isa<FuncletPadInst>(&i)) return true;
            if (isa<CatchReturnInst>(&i)) return true;
            if (isa<CleanupReturnInst>(&i)) return true;
        }
    }
    return false;
}

static Function* GetOrCreateDispatcher(Module& m)
{
    const char* name = "__selfdecay_exec";
    if (Function* existing = m.getFunction(name)) return existing;

    LLVMContext& c = m.getContext();
    Type* i8 = Type::getInt8Ty(c);
    Type* i64 = Type::getInt64Ty(c);
    PointerType* ptrty = PointerType::get(c, 0);

    FunctionType* fty = FunctionType::get(i64, {ptrty, i64, i64}, false);
    Function* dispatcher = Function::Create(fty, GlobalValue::InternalLinkage, name, m);

    // NoInline keeps later passes from inlining the dispatcher back into sites which would reexpose the opcode per site; atomicrmw Xchg + GV write naturally defeat CSE/GVN across call sites so OptimizeNone is not needed and that attribute has a track record of confusing backend lowering
    dispatcher->addFnAttr(Attribute::NoInline);
    dispatcher->addFnAttr(Attribute::NoUnwind);

    Value* descarg = dispatcher->getArg(0);
    Value* aarg = dispatcher->getArg(1);
    Value* barg = dispatcher->getArg(2);
    descarg->setName("Desc");
    aarg->setName("A");
    barg->setName("B");

    BasicBlock* entrybb = BasicBlock::Create(c, "Entry", dispatcher);
    BasicBlock* retbb = BasicBlock::Create(c, "Ret", dispatcher);
    BasicBlock* badbb = BasicBlock::Create(c, "Bad", dispatcher);

    IRBuilder<> eb(entrybb);
    AtomicRMWInst* op = eb.CreateAtomicRMW(AtomicRMWInst::Xchg, descarg, ConstantInt::get(i8, 0), MaybeAlign(1), AtomicOrdering::Monotonic);
    op->setName("Op");

    SwitchInst* sw = eb.CreateSwitch(op, badbb, OpcodeCount - 1);

    IRBuilder<> rb(retbb);
    // use IRBuilder's CreatePHI instead of PHINode::Create(..., BasicBlock*) LLVM 19's InsertPosition API deprecates the BasicBlock* overload
    PHINode* phi = rb.CreatePHI(i64, OpcodeCount - 1, "Result");
    rb.CreateRet(phi);

    auto emitcase = [&](uint8_t opcode, const char* bbname, Value* (*build)(IRBuilder<>&, Value*, Value*))
    {
        BasicBlock* bb = BasicBlock::Create(c, bbname, dispatcher);
        IRBuilder<> cb(bb);
        Value* r = build(cb, aarg, barg);
        cb.CreateBr(retbb);
        phi->addIncoming(r, bb);
        sw->addCase(eb.getInt8(opcode), bb);
    };

    emitcase(OpcodeAdd, "Add", [](IRBuilder<>& b, Value* a, Value* v) { return b.CreateAdd(a, v); });
    emitcase(OpcodeSub, "Sub", [](IRBuilder<>& b, Value* a, Value* v) { return b.CreateSub(a, v); });
    emitcase(OpcodeXor, "Xor", [](IRBuilder<>& b, Value* a, Value* v) { return b.CreateXor(a, v); });
    emitcase(OpcodeMul, "Mul", [](IRBuilder<>& b, Value* a, Value* v) { return b.CreateMul(a, v); });
    emitcase(OpcodeAnd, "And", [](IRBuilder<>& b, Value* a, Value* v) { return b.CreateAnd(a, v); });
    emitcase(OpcodeOr, "Or", [](IRBuilder<>& b, Value* a, Value* v) { return b.CreateOr(a, v); });
    emitcase(OpcodeShl, "Shl", [](IRBuilder<>& b, Value* a, Value* v) { return b.CreateShl(a, v); });
    emitcase(OpcodeLShr, "LShr", [](IRBuilder<>& b, Value* a, Value* v) { return b.CreateLShr(a, v); });
    emitcase(OpcodeAShr, "AShr", [](IRBuilder<>& b, Value* a, Value* v) { return b.CreateAShr(a, v); });
    emitcase(OpcodeUDiv, "UDiv", [](IRBuilder<>& b, Value* a, Value* v) { return b.CreateUDiv(a, v); });
    emitcase(OpcodeSDiv, "SDiv", [](IRBuilder<>& b, Value* a, Value* v) { return b.CreateSDiv(a, v); });
    emitcase(OpcodeURem, "URem", [](IRBuilder<>& b, Value* a, Value* v) { return b.CreateURem(a, v); });
    emitcase(OpcodeSRem, "SRem", [](IRBuilder<>& b, Value* a, Value* v) { return b.CreateSRem(a, v); });

    IRBuilder<> ub(badbb);
    Function* trap = Intrinsic::getDeclaration(&m, Intrinsic::trap);
    ub.CreateCall(trap);
    ub.CreateUnreachable();

    return dispatcher;
}

static void ProcessFunction(Function& f, Function* dispatcher)
{
    if (f.isDeclaration()) return;
    if (&f == dispatcher) return;
    if (!WantsSelfDecay(f)) return;
    if (f.hasFnAttribute(Attribute::Naked)) return;
    if (HasFuncletBody(f)) return;

    LLVMContext& c = f.getContext();
    Module& m = *f.getParent();
    Type* i8 = Type::getInt8Ty(c);
    Type* i64 = Type::getInt64Ty(c);

  
    DominatorTree dt;
    dt.recalculate(f);
    LoopInfo li;
    li.analyze(dt);

    // Collect candidates up front. The per-op rewrite calls eraseFromParent on the BinaryOperator so iterating the function while rewriting would invalidate the instruction iterator
    std::vector<std::pair<BinaryOperator*, uint8_t>> targets;

    for (BasicBlock& bb : f)
    {
        bool inloop = SkipLoops && (li.getLoopFor(&bb) != nullptr);
        for (Instruction& i : bb)
        {
            auto* bo = dyn_cast<BinaryOperator>(&i);
            if (!bo) continue;

            uint8_t op = PickOpcode(bo->getOpcode());
            if (op == OpcodeInvalid) continue;

            Type* ty = bo->getType();
            if (!ty->isIntegerTy() || ty->getIntegerBitWidth() > 64) continue;
            if (inloop) continue;

            targets.emplace_back(bo, op);
        }
    }

    for (auto& [bo, op] : targets)
    {
        std::string gname = MakeSiteName();
        Constant* descinit = ConstantInt::get(i8, op);
        GlobalVariable* gv = new GlobalVariable(m, i8, false, GlobalValue::PrivateLinkage, descinit, gname);
        gv->setAlignment(Align(1));
        // UnnamedAddr::None keeps the per site descriptor out of merging and constant pools. The GV must stay unique per site and must remain writable for the xchg to decay a single site rather than every site that happens to share its initial byte
        gv->setUnnamedAddr(GlobalValue::UnnamedAddr::None);

        IRBuilder<> b(bo);
        Value* aorig = bo->getOperand(0);
        Value* borig = bo->getOperand(1);
        unsigned bw = bo->getType()->getIntegerBitWidth();

        // AShr's data operand and both SDiv/SRem operands need SExt. ZExt would zero fill the upper half and the low-bit truncation would silently return a wrong signed value
        bool sexta = OpNeedsSextA(op);
        bool sextb = OpNeedsSextB(op);
        Value* a64 = (bw == 64) ? aorig : (sexta ? b.CreateSExt(aorig, i64) : b.CreateZExt(aorig, i64));
        Value* b64 = (bw == 64) ? borig : (sextb ? b.CreateSExt(borig, i64) : b.CreateZExt(borig, i64));

        CallInst* call = b.CreateCall(dispatcher, {gv, a64, b64});
        call->setDoesNotThrow();

        Value* result = (bw == 64) ? static_cast<Value*>(call) : b.CreateTrunc(call, bo->getType());

        bo->replaceAllUsesWith(result);
        bo->eraseFromParent();
    }
}

bool RunSelfDecayingCode(Module& m)
{
    if (!SelfDecayingCodeEnable) return false;

    bool anywants = false;
    for (Function& f : m)
    {
        if (f.isDeclaration()) continue;
        if (WantsSelfDecay(f))
        {
            anywants = true;
            break;
        }
    }
    if (!anywants) return false;

    Function* dispatcher = GetOrCreateDispatcher(m);

    for (Function& f : m)
    {
        if (f.isDeclaration()) continue;
        if (&f == dispatcher) continue;
        if (!WantsSelfDecay(f)) continue;
        ProcessFunction(f, dispatcher);
    }

    return true;
}

PreservedAnalyses SelfDecayingCodePass::run(Module& m, ModuleAnalysisManager&)
{
    bool mutated = RunSelfDecayingCode(m);
    return mutated ? PreservedAnalyses::none() : PreservedAnalyses::all();
}