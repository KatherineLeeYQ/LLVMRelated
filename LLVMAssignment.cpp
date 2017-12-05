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

#include <set>
#include <map>

#include <llvm/Support/CommandLine.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/ToolOutputFile.h>

#if LLVM_VERSION_MAJOR >= 4
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#else
#include <llvm/Bitcode/ReaderWriter.h>
#endif

#include <llvm/Transforms/Scalar.h>
#include "Liveness.h"
#include "llvm/IR/Function.h"
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace std;

#if LLVM_VERSION_MAJOR >= 4
static ManagedStatic<LLVMContext> GlobalContext;
static LLVMContext &getGlobalContext() { return *GlobalContext; }
#endif

/* In LLVM 5.0, when  -O0 passed to clang , the functions generated with clang will
* have optnone attribute which would lead to some transform passes disabled, like mem2reg.
*/
#if LLVM_VERSION_MAJOR == 5
struct EnableFunctionOptPass : public FunctionPass {
    static char ID;
    EnableFunctionOptPass() :FunctionPass(ID) {}
    bool runOnFunction(Function & F) override {
        if (F.hasFnAttribute(Attribute::OptimizeNone))
        {
            F.removeFnAttr(Attribute::OptimizeNone);
        }
        return true;
    }
};
char EnableFunctionOptPass::ID = 0;
#endif

#define IS_DEBUG true

class Pointer {
    set<Pointer *> pointToSet;
    map<Pointer *, Value *> blockMap;
    Value *value;

    void setInsertSet(set<Pointer *> &des, set<Pointer *> &source) {
        set<Pointer *>::iterator it;
        for (it = source.begin(); it != source.end(); ++it) {
            des.insert(*it); 
        }
    }
public:
    // constructor
    Pointer() {
        this->value = NULL;
    }
    Pointer(Value *value) {
        this->value = value;
    }

    // point & delete
    void pointToPointer(Pointer *ptr, Value *iV) {
        assert(isa<Instruction>(iV));
        Instruction *inst = dyn_cast<Instruction>(iV);

        #if IS_DEBUG
        errs() << "=== pointToPointer ===\n";
        this->value->dump();
        inst->dump();
        #endif
        
        set<Pointer *>::iterator it;
        for (it = this->pointToSet.begin(); it != this->pointToSet.end(); ++it) {
            Value *oldInstV = this->blockMap[*it];
            assert(isa<Instruction>(oldInstV));
            Instruction *oldInst = dyn_cast<Instruction>(oldInstV);

            // only store inst erase
            // if in the same basic block
            if (isa<StoreInst>(inst) && inst->getParent() == oldInst->getParent()) {
                this->pointToSet.erase(*it);
                
                #if IS_DEBUG
                errs() << "erased!\n";
                #endif
            }
        }
        this->pointToSet.insert(ptr);
        this->blockMap.insert(pair<Pointer *, Value *>(ptr, iV));

        #if IS_DEBUG
        this->output();
        errs() << "+++++++++++\n";
        #endif
    }
    void pointToPointSet(set<Pointer *> pSet, Value *iV) {
        set<Pointer *>::iterator it;
        for (it = pSet.begin(); it != pSet.end(); ++it) {
            this->pointToPointer(*it, iV);
        }
    }
    void copyPointToSet(Pointer *ptr, Value *iV) {
        set<Pointer *> ptrSet = ptr->getPointerSet();
        if (ptrSet.size() != 0)
            this->pointToPointSet(ptrSet, iV);
        else
            this->pointToPointer(ptr, iV);
    }
    void deletePointedPointer(Pointer *ptr) {
        this->pointToSet.erase(ptr);
    }

    // get
    Value* getValue() {
        return this->value;
    }
    set<Pointer *> getPointerSet() {
        return this->pointToSet;
    }
    set<Pointer *> getBasePointerSet() {
        set<Pointer *> basePointers;

        if (pointToSet.size() != 0) {
            set<Pointer *>::iterator it;
            for (it = pointToSet.begin(); it != pointToSet.end(); ++it) {
                set<Pointer *> source = (*it)->getBasePointerSet();
                this->setInsertSet(basePointers, source);
            }
        }
        else {
            if (isa<Function>(this->value)) {
                basePointers.insert(this);
            }
        }

        return basePointers;
    }

    // !TODO delete
    void output() {
        set<Pointer *> ptrSet = this->getBasePointerSet();
        set<Pointer *>::iterator it;
        for (it = ptrSet.begin(); it != ptrSet.end(); ++it) {
            errs() << (*it)->getValue()->getName() << " + ";
        }
        errs() << "\n";
    }
};

class ReturnManager {
public:
    Value* getReturnValueByFuncValue(Value *func) {
        if (!isa<Function>(func))
            return NULL;
        
        Value *ret = NULL;
        Function &F = *(dyn_cast<Function>(func));
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

class LineFunctionPtr {
    map<int, Pointer *> lineMap;

public:
    LineFunctionPtr() {}

    void insertLineFunctionPtr(int line, Pointer *ptr) {
        // line not add to map yet
        if (lineMap.find(line) == lineMap.end()) {
            lineMap.insert(pair<int, Pointer *>(line, ptr));
        }
    }
    void outputFuncNames(set<Pointer *> pointToSet) {
        if (pointToSet.size() != 0) {
            set<Pointer *>::iterator it;
            for (it = pointToSet.begin(); it != --pointToSet.end(); ++it) {
                errs() << (*it)->getValue()->getName() << ", ";
            }
            errs() << (*it)->getValue()->getName() << "\n";
        }
    }
    void output() {
        map<int, Pointer *>::iterator it;
        for (it = lineMap.begin(); it != lineMap.end(); ++it) {
            errs() << it->first << " : ";
            set<Pointer *> ptrs = it->second->getBasePointerSet();
            this->outputFuncNames(ptrs);
        }
    }
};

class PointerManager {
    map<Value *, Pointer *> pointerMap;
    
    bool isPointerExist(Value *value) {
        return pointerMap.find(value) != pointerMap.end();
    }
    Pointer* getPointerByValue(Value *value) {
        if (this->isPointerExist(value)) {
            return pointerMap[value];
        }
        return NULL;
    }
public:
    Pointer* getPointerFromValue(Value *value) {
        if (isPointerExist(value)) {
            return this->getPointerByValue(value);
        }
        else {
            Pointer *newPointer = new Pointer(value);
            pointerMap.insert(pair<Value *, Pointer *>(value, newPointer));
            return newPointer;
        }
    }
};

PointerManager pointerManager;

class PropertyManager {
    // struct value -> map
    // map: offset -> property value pointer
    map<Value *, map<int, set<Pointer *>>> ownerMap;
    PointerManager ptrManager;
    map<Pointer *, Value *> ptrMap;// property ptr -> store inst

    void generatePtrMap(Pointer *ptr, Value *value) {
        this->ptrMap.insert(pair<Pointer *, Value *>(ptr, value));
    }
    bool isValueExist(Value *value) {
        return this->ownerMap.find(value) != this->ownerMap.end();
    }
    Value* getOwner(Value *getInst) {
        assert(isa<GetElementPtrInst>(getInst));
        return dyn_cast<GetElementPtrInst>(getInst)->getPointerOperand();
    }
    int getOffset(Value *getInst) {
        assert(isa<GetElementPtrInst>(getInst));
        GetElementPtrInst *getInstr = dyn_cast<GetElementPtrInst>(getInst);

        #if IS_DEBUG
        errs() << "=== Get Offset ===\n";
        getInstr->dump();
        #endif

        int count = 0;
        for (Use *u = getInstr->idx_begin(); u != getInstr->idx_end(); ++u) {
            ++count;

            if (count == 2 && isa<ConstantInt>(u->get())) {
                ConstantInt *c = dyn_cast<ConstantInt>(u->get());

                #if IS_DEBUG
                errs() << "Offset is : " << c->getLimitedValue() << "\n";
                #endif

                return c->getLimitedValue();
            }
        }

        return 0;
    }
    set<Pointer *> pointerSetWithVariableAndOffset(Value *value, int offset) {
        if (this->isValueExist(value)) {
            map<int, set<Pointer *>> offsetMap = this->ownerMap[value];
            return offsetMap[offset];
        }
        else {
            set<Pointer *> pSet;
            return pSet;
        }
    }
    void insertOwnerPointer(StoreInst *storeInst, Value *getInst, 
                            Value *source, Value*owner, int offset) {
        #if IS_DEBUG
        errs() << "Owner Exist\n";
        #endif
        
        // basic block of the new source
        BasicBlock *block = dyn_cast<Instruction>(storeInst)->getParent();

        // get the offset map and the property value set
        set<Pointer *> originSet = this->ownerMap[owner][offset];
        set<Pointer *> newSet;
        set<Pointer *>::iterator it;
        for (it = originSet.begin(); it != originSet.end(); ++it) {
            Value *v = this->ptrMap[(*it)];
            assert(isa<Instruction>(v));
            BasicBlock *b = dyn_cast<Instruction>(v)->getParent();

            // delete the old pointer in the same basic block
            // delete the old pointer that has the same value // !TODO
            if (b != block && (*it)->getValue() != source)
                newSet.insert(*it);
        }

        // insert new value into property value set
        Pointer *sourcePtr = pointerManager.getPointerFromValue(source);
        newSet.insert(sourcePtr);
        this->generatePtrMap(sourcePtr, storeInst);

        // update the set
        this->ownerMap[owner][offset] = newSet;
    }
public:
    /*
    r_fptr[1] = q_fptr[0];
    %arrayidx12 = getelementptr inbounds [1 x i32 (i32, i32)*], [1 x i32 (i32, i32)*]* %q_fptr, i64 0, i64 0, !dbg !78
    %1 = load i32 (i32, i32)*, i32 (i32, i32)** %arrayidx12, align 8, !dbg !78
    %arrayidx13 = getelementptr inbounds [2 x i32 (i32, i32)*], [2 x i32 (i32, i32)*]* %r_fptr, i64 0, i64 1, !dbg !79
    store i32 (i32, i32)* %1, i32 (i32, i32)** %arrayidx13, align 8, !dbg !80
    */
    set<Pointer *> propertyPointerSet(Value *getInst) {
        assert(isa<GetElementPtrInst>(getInst));

        // get varaiable, offset
        Value *owner = this->getOwner(getInst);
        int offset = this->getOffset(getInst);

        #if IS_DEBUG
        errs() << "=== propertyPointerSet === \nGetElementPtrInst:\n";
        getInst->dump();

        set<Pointer *> ptrSet = this->pointerSetWithVariableAndOffset(owner, offset);
        errs() << "Set size : " << ptrSet.size() << "\n";
        set<Pointer *>::iterator it;
        for (it = ptrSet.begin(); it != ptrSet.end(); ++it) {
            (*it)->getValue()->dump();
        }
        #endif 

        // property set
        return this->pointerSetWithVariableAndOffset(owner, offset);
    }
    void insertPropertyPointer(Value *getInst, Value *source, Value *stInst) {
        assert(isa<GetElementPtrInst>(getInst));
        assert(isa<StoreInst>(stInst));

        #if IS_DEBUG
        errs() << "=== insertPropertyPointer === \n";
        errs() << "GetELementPtrInst:\n";
        getInst->dump();
        errs() << "Source:\n";
        source->dump();
        #endif

        // getelementptr
        Value *owner = this->getOwner(getInst);
        int offset = this->getOffset(getInst);

        // if this value exist in ownerMap
        StoreInst *storeInst = dyn_cast<StoreInst>(stInst);
        this->insertOwnerPointer(storeInst, getInst, source, owner, offset);
    }
    void initProperty(Value *getInst) {
        assert(isa<GetElementPtrInst>(getInst));

        // get owner and offset
        Value *owner = this->getOwner(getInst);
        int offset = this->getOffset(getInst);

        if (!this->isValueExist(owner)) {
            // construct a property value set
            set<Pointer *> propertyValueSet;

            // construct a offset map
            map<int, set<Pointer *>> offsetMap;
            offsetMap.insert(pair<int, set<Pointer *>>(offset, propertyValueSet));

            // insert into ownerMap
            this->ownerMap.insert(pair<Value *, map<int, set<Pointer *>>>(owner, offsetMap));
        }
    }
};

///!TODO TO BE COMPLETED BY YOU FOR ASSIGNMENT 3
struct FuncPtrPass : public ModulePass {
    ReturnManager returnManager;
    PropertyManager propertyManager;
    LineFunctionPtr lineFuncs;

    static char ID; // Pass identification, replacement for typeid
    FuncPtrPass() : ModulePass(ID) {}

    bool runOnModule(Module &M) override {
        //M.dump();
        for (Function &F : M) {
            for (BasicBlock &B : F) {
                for (Instruction &I: B) {
                    // Call Instruction
                    if (isa<CallInst>(&I) && !isLLVMCall(I)) {
                        this->dealCallInst(&I);
                    }
                    // PHI Instruction
                    if (isa<PHINode>(&I)) {
                        this->dealPHI(&I);
                    }
                    // GetElementPtrInst
                    if (isa<GetElementPtrInst>(&I)) {
                        this->dealGetElementPtrInst(&I);
                    }
                    // Store Instruction
                    if (isa<StoreInst>(&I)) {
                        this->dealStoreInst(&I);
                    }
                    // Load Instruction
                    if (isa<LoadInst>(&I)) {
                        this->dealLoadInst(&I);
                    }
                }
            }
        }
        return false;
    }
    bool doFinalization(Module &M) override {
        this->lineFuncs.output();

        return true;
    }

    // tools
    bool isLLVMCall(Instruction &I) {
        CallInst *callInst = dyn_cast<CallInst>(&I);
        bool llvm_dbg = callInst->getCalledValue()->getName().find("llvm.dbg") != std::string::npos;
        bool llvm_memset = callInst->getCalledValue()->getName().find("llvm.memset") != std::string::npos;
        return llvm_dbg || llvm_memset;
    }
    bool isFunctionPointer(Value *v) {
        return v->getType()->isPointerTy();
    }

    void dealGetElementPtrInst(Value *v) {
        assert(isa<GetElementPtrInst>(v));
        GetElementPtrInst *getInst = dyn_cast<GetElementPtrInst>(v);

        Value *operandValue = getInst->getPointerOperand();

        #if IS_DEBUG
        errs() << "=== dealGetElementPtrInst ===\nGetInst:\n";
        v->dump();
        #endif

        // load
        if (isa<LoadInst>(operandValue)) {
            Pointer *loadPtr = pointerManager.getPointerFromValue(operandValue);
            set<Pointer *> ptrSet = loadPtr->getPointerSet();

            #if IS_DEBUG
            errs() << "LoadInst:\n";
            operandValue->dump();
            errs() << "Sub Value:\n";
            #endif
            set<Pointer *>::iterator it;
            for (it = ptrSet.begin(); it != ptrSet.end(); ++it) {
                Value *subValue = (*it)->getValue();

                #if IS_DEBUG
                subValue->dump();
                #endif
            }
        }
        // struct
        else {
            this->propertyManager.initProperty(v);
        }
    }
    /*
    %arrayidx13 = getelementptr inbounds [2 x i32 (i32, i32)*], [2 x i32 (i32, i32)*]* %r_fptr, i64 0, i64 1, !dbg !79
    store i32 (i32, i32)* %1, i32 (i32, i32)** %arrayidx13, align 8, !dbg !80

    %call = call noalias i8* @malloc(i64 8) #3, !dbg !71
    %0 = bitcast i8* %call to i32 (i32, i32)**, !dbg !72
    store i32 (i32, i32)* @plus, i32 (i32, i32)** %0, align 8, !dbg !79
    */
    void dealStoreInst(Value *v) {
        assert(isa<StoreInst>(v));
        StoreInst *storeInst = dyn_cast<StoreInst>(v);

        Value *des = storeInst->getPointerOperand();
        Value *source = storeInst->getValueOperand();

        if (isa<GetElementPtrInst>(des))
            this->propertyManager.insertPropertyPointer(des, source, storeInst);
        else if (isa<BitCastInst>(des)) {
            Pointer *desPtr = pointerManager.getPointerFromValue(des);
            Pointer *sourcePtr = pointerManager.getPointerFromValue(source);
            desPtr->pointToPointer(sourcePtr, v);
        }
    }
    /*
    %arrayidx12 = getelementptr inbounds [1 x i32 (i32, i32)*], [1 x i32 (i32, i32)*]* %q_fptr, i64 0, i64 0, !dbg !78
    %1 = load i32 (i32, i32)*, i32 (i32, i32)** %arrayidx12, align 8, !dbg !78

    %0 = bitcast i8* %call to i32 (i32, i32)**, !dbg !34
    store i32 (i32, i32)* @plus, i32 (i32, i32)** %0, align 8, !dbg !40
    %1 = load i32 (i32, i32)*, i32 (i32, i32)** %0, align 8, !dbg !42

    [%call1 = call i32 %1(i32 1, i32 %x), !dbg !43]
    */
    void dealLoadInst(Value *v) { 
        assert(isa<LoadInst>(v));

        Value *des = dyn_cast<LoadInst>(v)->getPointerOperand();
        Pointer *loadInstPtr = pointerManager.getPointerFromValue(v);

        #if IS_DEBUG
            errs() << "=== dealLoadInst ===\nGetElementPtrInst:\n";
            des->dump();
            errs() << "\nLoadInst:\n";
            v->dump();
            errs() << "Output:\n";
            loadInstPtr->output();
        #endif

        if (isa<GetElementPtrInst>(des)) {
            set<Pointer *> ptrSet = this->propertyManager.propertyPointerSet(des);
            loadInstPtr->pointToPointSet(ptrSet, v);
        }
        else if (isa<BitCastInst>(des)) {
            Pointer *desPtr = pointerManager.getPointerFromValue(des);
            loadInstPtr->copyPointToSet(desPtr, v);
        }
    }
    void dealCallInst(Value *v) {
        CallInst *callInst = dyn_cast<CallInst>(v);
                        
        // line infomation
        DILocation *loc = callInst->getDebugLoc();
        unsigned line = loc->getLine();
        // called value
        Value *calledValue = callInst->getCalledValue();
        Pointer *calledPtr = pointerManager.getPointerFromValue(calledValue);
        lineFuncs.insertLineFunctionPtr(line, calledPtr);

        // deal all kinds of call
        this->dealCallKind(callInst);
    }
    void dealCallKind(CallInst *callInst) {
        Value *calledValue = callInst->getCalledValue();

        // call a call
        if (isa<CallInst>(calledValue))        
            this->dealCallKind(dyn_cast<CallInst>(calledValue));
        // a certain function
        else if (isa<Function>(calledValue))   
            this->dealCallFunction(callInst, calledValue); 
        // a function pointer, include phi and normal function ptr
        else if (isFunctionPointer(calledValue)) {
            this->dealCallFunctionPointer(callInst, calledValue);
        }
        /*
        else if (isa<LoadInst>(calledValue))
            this->dealCallLoadInst(callInst, calledValue);
        */
    }
    void dealCallFunctionPointer(Value *call, Value *fptr) {
        Pointer *funcPtr = pointerManager.getPointerFromValue(fptr);
        set<Pointer *> pSet = funcPtr->getBasePointerSet();
        set<Pointer *>::iterator it;
        for (it = pSet.begin(); it != pSet.end(); ++it) {
            this->dealCallFunction(call, (*it)->getValue());
        }
    }
    void dealCallFunction(Value *call, Value *func) {
        // null
        if (func->getName() == "")
            return;

        // bind the parameters
        this->bindFunctionParams(call, func);

        // if return pointer value
        if (dyn_cast<Function>(func)->getReturnType()->isPointerTy() && func->getName() != "malloc") {
            Value *ret = this->returnManager.getReturnValueByFuncValue(func);
            Pointer *retPtr = pointerManager.getPointerFromValue(ret);
            Pointer *funcPtr = pointerManager.getPointerFromValue(func);

            // bind the callinst and the return value
            Pointer *callPtr = pointerManager.getPointerFromValue(call);
            callPtr->copyPointToSet(retPtr, call);
        }
    }
    // block means this bindation has a block constrain
    void bindFunctionParams(Value *call, Value *f) {
        // The real argument
        CallInst *callInst = dyn_cast<CallInst>(call);
        // The parameter: Function
        Function *func = dyn_cast<Function>(f);

        CallInst::op_iterator op = callInst->op_begin();    // Operand Use *
        Argument *arg = func->arg_begin();                  // Argument *
        while (op != callInst->op_end() && arg != func->arg_end()) {
            Value *realV = op->get();

            // if the argument is of pointer type
            if (isFunctionPointer(arg)) {
                 this->bindFuncPtrParam(call, arg, realV);
            }
            
            ++op;
            ++arg;
        }
    }
    void bindFuncPtrParam(Value *call, Argument *arg, Value *realV) {
        Pointer *argPtr = pointerManager.getPointerFromValue(arg);
        Pointer *realVPtr = pointerManager.getPointerFromValue(realV);
        argPtr->copyPointToSet(realVPtr, call);
    }
    void dealPHI(Value *value) {
        PHINode *phi = dyn_cast<PHINode>(value);
        Pointer *phiPtr = pointerManager.getPointerFromValue(phi);

        Use *u_ptr = phi->incoming_values().begin();
        while (u_ptr != phi->incoming_values().end()) {
            // include the null
            // phi pointer to its values
            Pointer *ptr = pointerManager.getPointerFromValue(u_ptr->get());
            phiPtr->copyPointToSet(ptr, phi);

            ++u_ptr;
        }
    }
};


char FuncPtrPass::ID = 0;
static RegisterPass<FuncPtrPass> X("funcptrpass", "Print function call instruction");

char Liveness::ID = 0;
static RegisterPass<Liveness> Y("liveness", "Liveness Dataflow Analysis");

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
#if LLVM_VERSION_MAJOR == 5
   Passes.add(new EnableFunctionOptPass());
#endif
   ///Transform it to SSA
   Passes.add(llvm::createPromoteMemoryToRegisterPass());

   /// Your pass to print Function and Call Instructions
   //Passes.add(new Liveness());
   Passes.add(new FuncPtrPass());
   Passes.run(*M.get());
   /*
#ifndef NDEBUG
   system("pause");
#endif
*/
}