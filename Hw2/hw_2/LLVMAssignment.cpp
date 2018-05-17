//===- Hello.cpp - Example code from "Writing an LLVM Pass" ---------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements two versions of the LLVM "Hello World" pass described
// in docs/WritingAnLLVMPass.html
//
//===----------------------------------------------------------------------===//

#include <string>
#include <map>
#include <set>
#include <vector>
using namespace std;

#include <llvm/Support/CommandLine.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/ToolOutputFile.h>

#include <llvm/Transforms/Scalar.h>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/Pass.h>
#include <llvm/Support/raw_ostream.h>
#include "llvm/Bitcode/BitcodeWriterPass.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Transforms/IPO.h"

#if LLVM_VERSION_MAJOR >= 4
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#else
#include <llvm/Bitcode/ReaderWriter.h>
#endif
using namespace llvm;

#if LLVM_VERSION_MAJOR >= 4
static ManagedStatic<LLVMContext> GlobalContext;
static LLVMContext &getGlobalContext() { return *GlobalContext; }
#endif

/* In LLVM 5.0, when  -O0 passed to clang , the functions generated with clang will
 * have optnone attribute which would lead to some transform passes disabled, like mem2reg.
 */
#if LLVM_VERSION_MAJOR == 5
struct EnableFunctionOptPass: public FunctionPass {
    static char ID;
    EnableFunctionOptPass():FunctionPass(ID){}
    bool runOnFunction(Function & F) override{
        if(F.hasFnAttribute(Attribute::OptimizeNone))
        {
            F.removeFnAttr(Attribute::OptimizeNone);
        }
        return true;
    }
};

char EnableFunctionOptPass::ID=0;
#endif

enum ResultType { AlwaysTrue, AlwaysFalse, NotDefined};

class AlwaysTrueBlocks {
    set<Value *> alwaysTrueBlocks;

public:
    void insertAlwaysTrue(Value *v) {
        alwaysTrueBlocks.insert(v);
    }
    bool isAlwarysTrue(Value *v) {
        return alwaysTrueBlocks.find(v) != alwaysTrueBlocks.end();
    }
};

class FunctionNamesMap {
    map<Value *, set<Value *>> nMap;
    set<string> realNames;

public:
    FunctionNamesMap() {}

    void insertName(Value *alias, Value *name) {
        if (nMap.find(alias) != nMap.end()) {
            nMap[alias].insert(name);
        }
        else {
            set<Value *> v;
            v.insert(name);
            nMap.insert(pair<Value *, set<Value *>>(alias, v));
        }
    }
    void insertNames(Value *alias, set<Value *> names) {
        if (alias != NULL && names.size()!= 0) 
            nMap.insert(pair<Value *, set<Value *>>(alias, names));
    }

    bool hasKey(Value *alias) {
        return nMap.find(alias) != nMap.end();
    }
    void deleteKey(Value *key) {
        if (this->hasKey(key))
            nMap.erase(key);
    }
    
    bool keyHasName(Value *key, Value *name) {
        bool ret = false;
        if (this->hasKey(key)) {
            set<Value *> ns = this->getNames(key);
            if (ns.find(name) != ns.end()) {
                ret = true;
            }
        }
        return ret;
    }
    void deleteNameOfKey(Value *key, Value *name) {
        if (this->keyHasName(key, name)) {
            set<Value *> names = this->getNames(key);
            names.erase(name);
            this->deleteKey(key);
            this->insertNames(key, names);
        }
    }

    void clearRealNames() {
        this->realNames.clear();
    }
    set<Value *> getNames(Value *alias) {
        return nMap[alias];
    }
    set<string> getRealNames(set<Value *> fakeV) {
        set<Value *>::iterator fakeName;
        for (fakeName = fakeV.begin(); fakeName != fakeV.end(); ++fakeName) {
            if (this->hasKey(*fakeName)) {
                this->getRealNames(this->getNames(*fakeName));
            }
            else {
                realNames.insert((*fakeName)->getName());
            }
        }

        return this->realNames;
    }
    set<Value *> getRealNames(Value *key) {
        set<Value *> rSet;

        if (this->hasKey(key)) {
            set<Value *>::iterator iter;
            set<Value *> rNames = this->getNames(key);
            for (iter = rNames.begin(); iter != rNames.end(); ++iter) {
                if (this->hasKey(*iter)) {
                    set<Value *> subNames = this->getRealNames(*iter);
                    set<Value *>::iterator s_iter;
                    for (s_iter = subNames.begin(); s_iter != subNames.end(); ++s_iter) {
                        rSet.insert(*s_iter);
                    }
                }
                else
                    rSet.insert(*iter);
            }
        } 
        return rSet;
    }
};

class LineFunctions {
    map<int, set<Value *>> rMap;
    set<string> rSet;
    FunctionNamesMap *names;

public:
    LineFunctions() {}

    void setNameTable(FunctionNamesMap &names) {
        this->names = &names;
    }

    void insertLineFunction(int line, Value *funcName) {
        if (rMap.find(line) != rMap.end()) {
            rMap[line].insert(funcName);
        }
        else {
            set<Value *> v;
            v.insert(funcName);
            rMap.insert(pair<int, set<Value *>>(line, v));
        }
    }

    void output() {
        map<int, set<Value *>>::iterator iter;

        for(iter = rMap.begin(); iter != rMap.end(); ++iter) {
            // output the line number
            errs() << iter->first << " : ";

            this->names->clearRealNames();
            set<string> realNames = this->names->getRealNames(iter->second);

            // output the function names
            this->outputNameSet(realNames);
            errs() << "\n";
        }
    }
    void outputNameSet(set<string> s) {
        if (s.size() > 0) {
            set<string>::iterator iter;
            set<string>::iterator end = s.end();
            --end;
            for (iter = s.begin(); iter != end; ++iter) {
                // not null
                if (*iter != "")
                    errs() << *iter << ", ";
                // null
                else
                    continue;
            }
            errs() << *iter;
        }   
    }
    void rewriteCalleeName(FunctionNamesMap &ns) {
        this->setNameTable(ns);

        map<int, set<Value *>>::iterator iter;
        for(iter = rMap.begin(); iter != rMap.end(); ++iter) {
            // get the real names string set
            this->names->clearRealNames();
            set<string> realNames = this->names->getRealNames(iter->second);
            set<Value *> v_set = iter->second;
            
            // if only one real name
            if (realNames.size() == 1) {
                // get the calls
                set<Value *>::iterator originNameIter;
                for (originNameIter = iter->second.begin(); originNameIter != iter->second.end(); ++originNameIter) {
                    for (User *user : (*originNameIter)->users()) {
                        // get which CallInst called the originNameIter
                        if (CallInst *callInst = dyn_cast<CallInst>(user)) {
                            set<Value *> r_set = this->names->getRealNames(callInst->getCalledValue());
                            if (r_set.size() == 1) {
                                Function *f = dyn_cast<Function>(*(r_set.begin()));
                                if (f != NULL && f != callInst->getCalledValue()) {
                                    callInst->getCalledValue()->replaceAllUsesWith(f);
                                }
                            }

                            break;// important, if not add break, maybe crash
                        }
                    }
                }
            }
        }
    }
};

struct FuncPtrPass : public ModulePass {
    LineFunctions lineFuncs;
    FunctionNamesMap funcNames;
    AlwaysTrueBlocks alwaysTrues;

    static char ID; // Pass identification, replacement for typeid
    FuncPtrPass() : ModulePass(ID) {}

    bool runOnModule(Module &M) override {
        for (Function &F : M) {
            for (BasicBlock &B : F) {
                for (Instruction &I: B) {
                    // Call Instruction
                    if (isa<CallInst>(&I) && !isLLVMDBG(I)) {
                        this->dealCallInst(&I);
                    }
                    // PHI Instruction
                    if (isa<PHINode>(&I)) {
                        this->dealPHI(&I);
                    }
                    // Branch Instruction
                    if (isa<BranchInst>(&I)) {
                        this->dealBranchInst(&I);
                    }
                }
            }
        }
        // if not forward declaration
        for (Function &F : M) {
            for (BasicBlock &B : F) {
                for (Instruction &I: B) {
                    // Call Instruction
                    if (isa<CallInst>(&I) && !isLLVMDBG(I)) {
                        this->dealCallInst(&I);
                    }
                }
            }
        }
        
        // replace the certain function name in different lines
        this->lineFuncs.rewriteCalleeName(this->funcNames);

        return true;// if modified, return true
    }

    bool doFinalization(Module &M) override {
        this->lineFuncs.setNameTable(this->funcNames);
        this->lineFuncs.output();
        
        return true;
    }
    bool isLLVMDBG(Instruction &I) {
        return dyn_cast<CallInst>(&I)->getCalledValue()->getName().find("llvm.dbg") != std::string::npos;
    }
    bool isIfBlock(BasicBlock *b) {
        return b->getName().find("if.") != std::string::npos;
    }
    bool isFunctionPointer(Value *v) {
        return v->getType()->isPointerTy();
    }
    
    void dealCallInst(Value *v) {
        CallInst *callInst = dyn_cast<CallInst>(v);
                        
        // line infomation
        DILocation *loc = callInst->getDebugLoc();
        unsigned line = loc->getLine();
        // called value
        Value *calledValue = callInst->getCalledValue();
        lineFuncs.insertLineFunction(line, calledValue);

        // deal all kinds of call
        this->dealCallKind(callInst);
    }
    void dealCallKind(Value *v) {
        CallInst *callInst = dyn_cast<CallInst>(v);
        Value *calledValue = callInst->getCalledValue();

        // call a call
        if (isa<CallInst>(calledValue))        
            this->dealCallKind(calledValue);
        // a certain function
        else if (isa<Function>(calledValue))   
            this->dealCallFunction(callInst, calledValue); 
        // a function pointer
        else if (isFunctionPointer(calledValue)) {
            // a phi of function pointers
            if (isa<PHINode>(calledValue))        
                this->dealCallPHI(callInst, calledValue);
            else
                this->dealCallFunctionPointer(callInst, calledValue);
        }
    }
    void dealCallFunction(Value *call, Value *func) {
        this->bindFunctionParams(call, func, NULL);

        Function *f = dyn_cast<Function>(func);
        if (f->getReturnType()->isPointerTy())
            funcNames.insertName(call, this->getReturnValue(*f));
    }
    void dealCallFunctionPointer(Value *call, Value *fptr) {
        set<Value *> fset = this->funcNames.getRealNames(fptr);
        for (set<Value *>::iterator iter = fset.begin(); iter != fset.end(); ++iter) {
            // not null
            if ((*iter)->getName() != "")
                this->dealCallFunction(call, *iter);
        }
    }
    void dealCallPHI(Value *call, Value *p) {
        PHINode *phi = dyn_cast<PHINode>(p);
        BasicBlock **b = phi->block_begin();            // phi value blocks
        Use *u_ptr = phi->incoming_values().begin();    // phi values

        while (b != phi->block_end() && u_ptr != phi->incoming_values().end()) {
            Value *v = u_ptr->get();
            // not null
            if (v->getName() != "") {
                if (isa<Function>(v)) {
                    this->bindFunctionParams(call, v, *b);
                    funcNames.insertName(call, this->getReturnValue(*dyn_cast<Function>(v)));
                }
                else if (isa<PHINode>(v)) {
                    this->dealCallPHI(call, v);
                }
            }
            // null
            else {
                funcNames.insertName(call, v);
            }
            
            ++b;
            ++u_ptr;
        }
    }
    // block means this bindation has a block constrain
    void bindFunctionParams(Value *call, Value *f, Value *block=NULL) {
        // The real argument
        CallInst *callInst = dyn_cast<CallInst>(call);
        CallInst::op_iterator op = callInst->op_begin();    // Use *
        // The parameter
        Function *func = dyn_cast<Function>(f);
        Argument *arg = func->arg_begin();     // Argument *

        while (op != callInst->op_end() && arg != func->arg_end()) {
            Value *realV = op->get();
            if (isFunctionPointer(arg)) {
                 this->bindFuncPtrParam(arg, realV, block);
            }
            
            ++op;
            ++arg;
        }
    }
    void bindFuncPtrParam(Argument *arg, Value *realV, Value *block=NULL) {
        if (isa<PHINode>(realV)) {
            PHINode *phi = dyn_cast<PHINode>(realV);

            // no block constrain
            if (block == NULL)
                funcNames.insertName(arg, realV);
            // constrain the block of the realV
            else {
                BasicBlock **b = phi->block_begin();
                Use *u_ptr = phi->incoming_values().begin();

                while (b != phi->block_end() && u_ptr != phi->incoming_values().end()) {
                    if (*b == block) {
                        funcNames.insertName(arg, u_ptr->get());
                        break;
                    }

                    ++b;
                    ++u_ptr;
                }
            }
        }
        else {
            funcNames.insertName(arg, realV);
        } 
    }
    void dealPHI(Value *value) {
        PHINode *phi = dyn_cast<PHINode>(value);

        // all names
        for (Use *u_ptr = phi->incoming_values().begin(); u_ptr != phi->incoming_values().end(); ++u_ptr) {
            funcNames.insertName(phi, u_ptr->get());// null or not null
        }

        // deal the always true name
        Use *u_ptr = phi->incoming_values().begin();
        BasicBlock **b_ptr = phi->block_begin();
        while (u_ptr != phi->incoming_values().end() && b_ptr != phi->block_end()) {
            bool isAlwaysTrueName = alwaysTrues.isAlwarysTrue(*b_ptr);
            if (isAlwaysTrueName && isIfBlock(*b_ptr)) {
                funcNames.deleteKey(phi);
                funcNames.insertName(phi, u_ptr->get());
                break;
            }

            ++u_ptr;
            ++b_ptr;
        }
    }
    void dealBranchInst(Value *v) {
        BranchInst *branch = dyn_cast<BranchInst>(v);
        if (branch->isConditional()) {
            if (isa<ICmpInst>(branch->getCondition())) {
                ResultType resultType = resultOfICMP(dyn_cast<ICmpInst>(branch->getCondition()));

                if (resultType == AlwaysTrue)
                    alwaysTrues.insertAlwaysTrue(branch->getSuccessor(0));
                else if (resultType == AlwaysFalse)
                    alwaysTrues.insertAlwaysTrue(branch->getSuccessor(1));
            }
        }
    }
    ResultType resultOfICMP(ICmpInst *icmp) {
        ResultType result = NotDefined;

        Use *u1 = icmp->op_begin();
        Use *u2 = u1 + 1;
        if (u1 != NULL && u2 != NULL) {
            Value *v1 = u1->get();
            Value *v2 = u2->get();
            
            if (isFunctionPointer(v1)) {
                if (isa<PHINode>(v1) && v2->getName() == "") {
                    this->funcNames.deleteNameOfKey(v1, v2);
                }
            }
            
            if (isa<ConstantInt>(v1) && isa<ConstantInt>(v2)) {
                ConstantInt *c1 = dyn_cast<ConstantInt>(v1);
                ConstantInt *c2 = dyn_cast<ConstantInt>(v2);
                int i1 = c1->getLimitedValue();
                int i2 = c2->getLimitedValue();

                CmpInst::Predicate pr = icmp->getPredicate();
                switch (pr) {
                    case CmpInst::Predicate::ICMP_EQ: {
                        if (i1 == i2)
                            result = AlwaysTrue;
                        else 
                            result = AlwaysFalse;
                        break;
                    }
                    case CmpInst::Predicate::ICMP_NE: {
                        if (i1 != i2)
                            result = AlwaysTrue;
                        else
                            result = AlwaysFalse;
                        break;
                    }
                    case CmpInst::Predicate::ICMP_SGT: {
                        if (i1 > i2)
                            result = AlwaysTrue;
                        else
                            result = AlwaysFalse;
                        break;
                    }
                    case CmpInst::Predicate::ICMP_SGE: {
                        if (i1 >= i2)
                            result = AlwaysTrue;
                        else
                            result = AlwaysFalse;
                        break;
                    }
                    case CmpInst::Predicate::ICMP_SLT: {
                        if (i1 < i2)
                            result = AlwaysTrue;
                        else
                            result = AlwaysFalse;
                        break;
                    }
                    case CmpInst::Predicate::ICMP_SLE: {
                        if (i1 <= i2)
                            result = AlwaysTrue;
                        else
                            result = AlwaysFalse;
                        break;
                    }
                }
            }
        }

        return result;
    }
    Value* getReturnValue(Function &F) {
        Value *ret = NULL;
        if (F.getReturnType()->isPointerTy()) {
            for (BasicBlock &B : F) {
                for (Instruction &I : B) {
                    if (isa<ReturnInst>(&I)) {
                        ReturnInst *retInst = dyn_cast<ReturnInst>(&I);
                        ret = retInst->getReturnValue();
                    }
                }
            }
        }
        return ret;
    }
};

char FuncPtrPass::ID = 0;
static RegisterPass<FuncPtrPass> X("funcptrpass", "Print function call instruction");

static cl::opt<std::string>
InputFilename(cl::Positional,
              cl::desc("<filename>.bc"),
              cl::init(""));


int main(int argc, char **argv) {
    LLVMContext &Context = getGlobalContext();
    SMDiagnostic Err;
    // Parse the command line to read the Inputfilename
    cl::ParseCommandLineOptions(argc, argv,
                                "FuncPtrPass \n My first LLVM too which does not do much.\n");

    // Load the input module
    std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);
    if (!M) {
        Err.print(argv[0], errs());
        return 1;
    }

    llvm::legacy::PassManager Passes;
        
    ///Remove functions' optnone attribute in LLVM5.0
    #if LLVM_VERSION_MAJOR == 5
    Passes.add(new EnableFunctionOptPass());
    #endif

    ///Transform it to SSA
    Passes.add(llvm::createPromoteMemoryToRegisterPass());

    /// Your pass to print Function and Call Instructions
    Passes.add(new FuncPtrPass());

    // rewrite the bitcode file
    std::unique_ptr<tool_output_file> Out;
    std::error_code EC;
    Out.reset(new tool_output_file(InputFilename,EC, sys::fs::F_None));
    if (EC) {
        errs() << EC.message() << '\n';
        return 1;
    }
    raw_ostream *OS = &Out->os();

    // add bitcode writer pass
    Passes.add(createBitcodeWriterPass(*OS));

    // run the passes
    Passes.run(*M.get());

    // keep the file
    Out->keep();
   
    return 0;
}
