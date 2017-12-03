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
        this->pointToSet.insert(ptr);
        this->blockMap.insert(pair<Pointer *, Value *>(ptr, iV));
    }
    void pointToPointSet(set<Pointer *> pSet, Value *iV) {
        set<Pointer *>::iterator it;
        for (it = pSet.begin(); it != pSet.end(); ++it) {
            this->pointToPointer(*it, iV);
        }
    }
    void copyPointToSet(Pointer *ptr) {
        set<Pointer *> ptrSet = ptr->getPointerSet();
        this->pointToPointSet(ptrSet, this->value);
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
    map<Pointer *, Pointer *> returnMap;

public:
    void insertReturn(Pointer *func, Pointer *ret) {
        returnMap.insert(pair<Pointer *, Pointer *>(func, ret));
    }
    Pointer* getReturnByFunc(Pointer *func) {
        if (returnMap.find(func) != returnMap.end())
            return returnMap[func];
        else 
            return NULL;
    }
};

class StructPropertyManager {
    // struct value -> map
    // map: offset -> property value pointer
    map<Value *, map<int, set<Pointer *>>> structMap;
    map<Pointer *, Value *> ptrMap;// property ptr -> store inst

    Value* getStruct(Value *getInst) {
        assert(isa<GetElementPtrInst>(getInst));
        return dyn_cast<GetElementPtrInst>(getInst)->getPointerOperand();
    }
    int getOffset(Value *getInst) {
        assert(isa<GetElementPtrInst>(getInst));
        return dyn_cast<GetElementPtrInst>(getInst)->getNumIndices();
    }
    bool isStructExist(Value *value) {
        return this->structMap.find(value) != this->structMap.end();
    }
    set<Pointer *> getPointerSet(Value *structV, int offset) {
        if (this->isStructExist(structV)) {
            map<int, set<Pointer *>> offsetMap = structMap[structV];
            return offsetMap[offset];
        }
        else {
            set<Pointer *> pSet;
            return pSet;
        }
    }
    void generatePtrMap(Pointer *ptr, Value *value) {
        this->ptrMap.insert(pair<Pointer *, Value *>(ptr, value));
    }
    void insertExistStructPointer(StoreInst *storeInst) {
        // getelementptr
        Value *getInst = storeInst->getPointerOperand();
        Value *source = storeInst->getValueOperand();
        Value *structV = this->getStruct(getInst);
        int offset = this->getOffset(getInst);

        // basic block of the new source
        BasicBlock *block = dyn_cast<Instruction>(storeInst)->getParent();

        // get the offset map and the property value set
        set<Pointer *> originSet = structMap[structV][offset];
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
        Pointer *sourcePtr = new Pointer(source);
        newSet.insert(sourcePtr);
        this->generatePtrMap(sourcePtr, storeInst);

        // update the set
        this->structMap[structV][offset] = newSet;
    }
    void insertNotExistStructPointer(StoreInst *storeInst) {
        // getelementptr
        Value *getInst = storeInst->getPointerOperand();
        Value *source = storeInst->getValueOperand();
        Value *structV = this->getStruct(getInst);
        int offset = this->getOffset(getInst);

        // construct a property value set
        set<Pointer *> propertyValueSet;
        Pointer *sourcePtr = new Pointer(source);
        propertyValueSet.insert(sourcePtr);
        this->generatePtrMap(sourcePtr, storeInst);

        // construct a offset map
        map<int, set<Pointer *>> offsetMap;
        offsetMap.insert(pair<int, set<Pointer *>>(offset, propertyValueSet));

        // insert into structMap
        this->structMap.insert(pair<Value *, map<int, set<Pointer *>>>(structV, offsetMap));
    }
public:
    /*
    %p_fptr5 = getelementptr inbounds %struct.fptr, %struct.fptr* %t_fptr, i32 0, i32 0, !dbg !60
    %2 = load i32 (i32, i32)*, i32 (i32, i32)** %p_fptr5, align 8, !dbg !60
    %call = call i32 %2(i32 1, i32 2), !dbg !62
    */
    /*
    第一个0是数组计算符，并不会改变返回的类型，因为，我们任何一个指针都可以作为一个数组来使用，
        进行对应的指针计算，所以这个0并不会省略。
    第二个0是结构体的计算地址，表示的是结构体的第0个元素的地址，这时，会根据结构体指针的类型，
        选取其中的元素长度，进行计算，最后返回的则是结构体成员的指针。
    */
    set<Pointer *> getPointerSetFromLoadInst(Value *loadInst) {
        assert(isa<LoadInst>(loadInst));

        // get getelementptr instruction
        Value *getInst = dyn_cast<LoadInst>(loadInst)->getPointerOperand();

        // get struct, offset
        Value *structV = this->getStruct(getInst);
        int offset = this->getOffset(getInst);

        // property set
        return this->getPointerSet(structV, offset);
    }
    /*
    store i32 (i32, i32)* @plus, i32 (i32, i32)** %p_fptr, align 8, !dbg !51
    */
    void insertPointerFromStoreInst(Value *stInst) {
        assert(isa<StoreInst>(stInst));
        StoreInst *storeInst = dyn_cast<StoreInst>(stInst);

        // if this struct exist in structMap
        if (this->isStructExist(this->getStruct(storeInst->getPointerOperand()))) {
            this->insertExistStructPointer(storeInst);
        }
        // not exist
        else {
            this->insertNotExistStructPointer(storeInst);
        }
    }
};

class PointerManager {
    map<Value *, Pointer *> pointerMap;
    ReturnManager retManager;
    StructPropertyManager structPropertyManager;
    
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
    // struct
    set<Pointer*> getPointerSetFromLoadInst(Value *loadInst) {
        return this->structPropertyManager.getPointerSetFromLoadInst(loadInst);
    }
    void insertStructProperty(Value *stInst) {
        this->structPropertyManager.insertPointerFromStoreInst(stInst);
    }

    // return
    Pointer* getReturnByFunc(Value *func) {
        Pointer *funcPtr = this->getPointerFromValue(func);
        return this->retManager.getReturnByFunc(funcPtr);
    }
    void insertReturn(Value *func, Value *ret) {
        Pointer *retPtr = this->getPointerFromValue(ret);
        Pointer *funcPtr = this->getPointerFromValue(func);

        retManager.insertReturn(funcPtr, retPtr);
    }

    // get
    Pointer* getPointerFromValue(Value *value) {
        if (isPointerExist(value)) {
            return this->getPointerByValue(value);
        }
        // when map keys not include this value
        // generate a new pointer
        // and add it to the pointer map
        else {
            Pointer *newPointer = new Pointer(value);
            pointerMap.insert(pair<Value *, Pointer *>(value, newPointer));
            return newPointer;
        }
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

///!TODO TO BE COMPLETED BY YOU FOR ASSIGNMENT 3
struct FuncPtrPass : public ModulePass {
    PointerManager manager;
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
                    // Store Instruction
                    if (isa<StoreInst>(&I)) {
                        this->dealStoreInst(&I);
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

    void dealStoreInst(Value *v) {
        assert(isa<StoreInst>(v));
        StoreInst *storeInst = dyn_cast<StoreInst>(v);

        Value *des = storeInst->getPointerOperand();
        Value *source = storeInst->getValueOperand();

        if (isa<GetElementPtrInst>(des))
            this->manager.insertStructProperty(storeInst);
        else if (isa<BitCastInst>(des)) {
            Pointer *desPtr = this->manager.getPointerFromValue(des);
            Pointer *sourcePtr = this->manager.getPointerFromValue(source);
            desPtr->pointToPointer(sourcePtr, v);
        }
    }
    void dealCallInst(Value *v) {
        CallInst *callInst = dyn_cast<CallInst>(v);
                        
        // line infomation
        DILocation *loc = callInst->getDebugLoc();
        unsigned line = loc->getLine();
        // called value
        Value *calledValue = callInst->getCalledValue();
        Pointer *calledPtr = this->manager.getPointerFromValue(calledValue);
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
        else if (isa<LoadInst>(calledValue))
            this->dealCallLoadInst(callInst, calledValue);
        // a function pointer
        else if (isFunctionPointer(calledValue)) {
            // a phi of function pointers
            if (isa<PHINode>(calledValue))        
                this->dealCallPHI(callInst, calledValue);
            else
                this->dealCallFunctionPointer(callInst, calledValue);
        }
    }
    void dealCallLoadInst(Value *call, Value *lInst) {
        assert(isa<LoadInst>(lInst));
        Value *value = dyn_cast<LoadInst>(lInst)->getPointerOperand();

        Pointer *loadInstPtr = this->manager.getPointerFromValue(lInst);
        Pointer *operandPtr = this->manager.getPointerFromValue(value);
        loadInstPtr->pointToPointer(operandPtr, lInst);

        if (isa<GetElementPtrInst>(value)) {
            this->dealCallStructLoad(call, lInst);
        }
        else if (isa<BitCastInst>(value)) {
            this->dealCallFunctionPointer(call, value);
        }
    }
    void dealCallStructLoad(Value *call, Value *lInst) {
        set<Pointer *> ptrSet = this->manager.getPointerSetFromLoadInst(lInst);

        Pointer *loadInstPrt = this->manager.getPointerFromValue(lInst);
        loadInstPrt->pointToPointSet(ptrSet, lInst);
    }
    void dealCallPHI(Value *call, Value *p) {
        /*
        PHINode *phi = dyn_cast<PHINode>(p);
        Pointer *phiPtr = this->manager.getPointerFromValue(phi);

        set<Pointer *> phiSet = phiPtr->getBasePointerSet();
        set<Pointer *>::iterator it;
        for (it = phiSet.begin(); it != phiSet.end(); ++it) {
            this->dealCallFunction(call, (*it)->getValue());
        }
        */
        this->dealCallFunctionPointer(call, p);
    }
    void dealCallFunctionPointer(Value *call, Value *fptr) {
        Pointer *funcPtr = this->manager.getPointerFromValue(fptr);
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
            this->manager.insertReturn(func, this->getReturnValue(func));

            // bind the callinst and the return value
            Pointer *callPtr = this->manager.getPointerFromValue(call);
            Pointer *retPtr = this->manager.getReturnByFunc(func);
            callPtr->pointToPointer(retPtr, call);
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
        Pointer *argPtr = this->manager.getPointerFromValue(arg);
        Pointer *realVPtr = this->manager.getPointerFromValue(realV);
        argPtr->pointToPointer(realVPtr, call);

        set<Pointer *> s = argPtr->getBasePointerSet();
    }
    Value* getReturnValue(Value *func) {
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
    void dealPHI(Value *value) {
        PHINode *phi = dyn_cast<PHINode>(value);
        Pointer *phiPtr = this->manager.getPointerFromValue(phi);

        Use *u_ptr = phi->incoming_values().begin();
        while (u_ptr != phi->incoming_values().end()) {
            // include the null
            // phi pointer to its values
            Pointer *ptr = this->manager.getPointerFromValue(u_ptr->get());
            phiPtr->pointToPointer(ptr, phi);

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