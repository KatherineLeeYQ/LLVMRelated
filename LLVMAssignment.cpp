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
#include <vector>

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
#include "llvm/IR/Type.h"
#include "llvm/IR/Function.h"
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/StringRef.h"

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

#define IS_DEBUG false

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

        #if IS_DEBUG
        errs() << "=== pointToPointer ===\n";
        this->value->dump();
        iV->dump();
        #endif

        Instruction *inst = dyn_cast<Instruction>(iV);
        BasicBlock *block = inst->getParent();
        Function *func = block->getParent();
        
        set<Pointer *>::iterator it;
        for (it = this->pointToSet.begin(); it != this->pointToSet.end(); ++it) {
            Value *oldInstV = this->blockMap[*it];
            assert(isa<Instruction>(oldInstV));
            Instruction *oldInst = dyn_cast<Instruction>(oldInstV);
            BasicBlock *oldBlock = oldInst->getParent();
            Function *oldFunc = oldBlock->getParent();

            // only store inst erase
            // if in the same basic block, erase the old values
            // if in different functions, erase the old values
            #if IS_DEBUG
            errs() << "\n### determine erase!\n";
            iV->dump();
            errs() << "func name:" << func->getName() << "\n";
            errs() << "old func name:" << oldFunc->getName() << "\n";
            #endif
            if (isa<StoreInst>(inst) && (block == oldBlock || func != oldFunc)) {
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
    void resetPointToSet(set<Pointer *> pSet) {
        this->pointToSet = pSet;
    }
    void pointToPointSet(set<Pointer *> pSet, Value *iV) {
        set<Pointer *>::iterator it;
        for (it = pSet.begin(); it != pSet.end(); ++it) {
            this->pointToPointer(*it, iV);
        }
    }
    void copyPointToSet(Pointer *ptr, Value *iV) {
        set<Pointer *> ptrSet = ptr->getPointerSet();

        #if IS_DEBUG
        errs() << "copy set size: " << ptrSet.size() << "\n";
        #endif

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

        if (this->pointToSet.size() != 0) {
            set<Pointer *>::iterator it;
            for (it = this->pointToSet.begin(); it != this->pointToSet.end(); ++it) {
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
        //set<Pointer *> ptrSet = this->getBasePointerSet();
        set<Pointer *>::iterator it;
        for (it = this->pointToSet.begin(); it != this->pointToSet.end(); ++it) {
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
        errs() << "\n";
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
    map<Value *, map<int, set<Pointer *>>> ownerMap;
    map<Pointer *, Value *> ptrMap;// property ptr -> store inst

    void generatePtrMap(Pointer *ptr, Value *value) {
        this->ptrMap.insert(pair<Pointer *, Value *>(ptr, value));
    }
    // insert
    void insertOwnerPointer(Value *owner, int offset, 
                            Value *source, StoreInst *storeInst) {

        // basic block of the new source
        BasicBlock *block = dyn_cast<Instruction>(storeInst)->getParent();
        Function *func = block->getParent();

        // get the offset map and the property value set
        set<Pointer *> originSet = this->ownerMap[owner][offset];
        set<Pointer *> newSet;
        set<Pointer *>::iterator it;
        for (it = originSet.begin(); it != originSet.end(); ++it) {
            Value *v = this->ptrMap[(*it)];
            assert(isa<Instruction>(v));
            BasicBlock *oldBlock = dyn_cast<Instruction>(v)->getParent();
            Function *oldFunc = oldBlock->getParent();

            if (oldFunc != func)
                break;

            // 1.delete the old pointer in the same basic block
            // 2.delete the old pointer that has the same value
            // 3.when it comes to being in different functions
            //   delete the old values
            if (oldBlock != block && (*it)->getValue() != source)
                newSet.insert(*it);
        }

        // insert new value into property value set
        Pointer *sourcePtr = pointerManager.getPointerFromValue(source);
        if (isa<LoadInst>(source)) {
            #if IS_DEBUG
            errs() << "insert load value!";
            sourcePtr->output();
            #endif

            set<Pointer *> sourcePtrSet = sourcePtr->getPointerSet();
            newSet.insert(sourcePtrSet.begin(), sourcePtrSet.end());
        }
        else {
            newSet.insert(sourcePtr);
            this->generatePtrMap(sourcePtr, storeInst);
        }

        #if IS_DEBUG
        Pointer *ownerPtr = pointerManager.getPointerFromValue(owner);
        errs() << "_owner:\n";
        owner->dump();
        errs() << "source:\n";
        source->dump();
        storeInst->dump();
        #endif

        // update the set
        this->ownerMap[owner][offset] = newSet;
    }
public:
    bool isOwnerExist(Value *value) {
        return this->ownerMap.find(value) != this->ownerMap.end();
    }
    Value* getOwner(Value *getInst) {
        assert(isa<GetElementPtrInst>(getInst));
        return dyn_cast<GetElementPtrInst>(getInst)->getPointerOperand();
    }
    int getOffset(Value *getInst) {
        assert(isa<GetElementPtrInst>(getInst));
        GetElementPtrInst *getInstr = dyn_cast<GetElementPtrInst>(getInst);

        int numINdices = getInstr->getNumIndices();
        int count = 0;
    
        #if IS_DEBUG
        errs() << "=== Get Offset ===\n";
        getInstr->dump();
        errs() << "NumDices: " << numINdices << "\n";
        #endif

        for (Use *u = getInstr->idx_begin(); u != getInstr->idx_end(); ++u) {
            ++count;

            if ((count == numINdices) && isa<ConstantInt>(u->get())) {
                ConstantInt *c = dyn_cast<ConstantInt>(u->get());

                #if IS_DEBUG
                errs() << "Offset is : " << c->getLimitedValue() << "\n";
                #endif

                return c->getLimitedValue();
            }
        }

        return 0;
    }
    void insertOffsetMap(Value *des, Value *source) {
        if (this->isOwnerExist(source)) {
            map<int, set<Pointer *>> offsetMap = this->ownerMap[source];

            #if IS_DEBUG
            errs() << "@ insertOffsetMap @\n";
            errs() << "source map size: " << offsetMap.size() << "\n";
            errs() << "des:\n";
            des->dump();
            #endif

            if (this->isOwnerExist(des)) {
                map<int, set<Pointer *>> oldMap = this->ownerMap[source];
                map<int, set<Pointer *>>::iterator it;
                for (it = oldMap.begin(); it != oldMap.end(); ++it) {
                    int offset = it->first;
                    if (offsetMap.find(offset) != offsetMap.end()) {
                        set<Pointer *> oldSet = oldMap[offset];
                        set<Pointer *> newSet = offsetMap[offset];
                        newSet.insert(oldSet.begin(), oldSet.end());
                        offsetMap[offset] = newSet;
                    }
                }
                this->ownerMap[des] = offsetMap;
            }
            else {
                this->ownerMap.insert(pair<Value *, map<int, set<Pointer *>>>(des, offsetMap));
            }
        }
    }
    /*
    r_fptr[1] = q_fptr[0];
    %arrayidx12 = getelementptr inbounds [1 x i32 (i32, i32)*], [1 x i32 (i32, i32)*]* %q_fptr, i64 0, i64 0, !dbg !78
    %1 = load i32 (i32, i32)*, i32 (i32, i32)** %arrayidx12, align 8, !dbg !78
    %arrayidx13 = getelementptr inbounds [2 x i32 (i32, i32)*], [2 x i32 (i32, i32)*]* %r_fptr, i64 0, i64 1, !dbg !79
    store i32 (i32, i32)* %1, i32 (i32, i32)** %arrayidx13, align 8, !dbg !80
    */
    set<Pointer *> propertyPointerSet(Value *owner, int offset) {
        #if IS_DEBUG
        owner->dump();
        if (this->isOwnerExist(owner)) {
            errs() << "Set Exist\n";
        }
        else {
            errs() << "Set Not Exist\n";
        }
        #endif

        if (this->isOwnerExist(owner)) {
            return this->ownerMap[owner][offset];
        } 
        else {
            set<Pointer *> rSet;
            return rSet;
        }
    }
    void insertPropertyPointer(Value *getInst, Value *source, Value *stInst) {
        assert(isa<GetElementPtrInst>(getInst));
        assert(isa<StoreInst>(stInst));
        StoreInst *storeInst = dyn_cast<StoreInst>(stInst);

        #if IS_DEBUG
        errs() << "=== insertPropertyPointer === \n";
        errs() << "GetELementPtrInst:\n";
        getInst->dump();
        errs() << "*** propertyManager output ***\n";
        this->output();
        #endif

        // getelementptr
        Value *owner = this->getOwner(getInst);
        int offset = this->getOffset(getInst);

        Pointer *ownerPtr = pointerManager.getPointerFromValue(owner);
        set<Pointer *> ownerPtrSet = ownerPtr->getPointerSet();

        #if IS_DEBUG
        errs() << "__Owner:\n";
        owner->dump();
        ownerPtr->output();
        errs() << "Offset: " << offset << "\n";
        errs() << "Source:\n";
        source->dump();
        #endif

        // getelementptr ... <LoadInst> offset
        if (isa<LoadInst>(owner)) {
            set<Pointer *>::iterator it;
            for (it = ownerPtrSet.begin(); it != ownerPtrSet.end(); ++it) {
                Value *newOwner = (*it)->getValue();//struct fptr

                #if IS_DEBUG
                errs() << "&&&\n";
                storeInst->dump();
                owner->dump();
                errs() << "SubValue:\n";
                newOwner->dump();
                #endif

                this->insertOwnerPointer(newOwner, offset, source, storeInst);
            }
        }
        // getelementptr ... <struct> offset
        else {
            this->insertOwnerPointer(owner, offset, source, storeInst);
        }
    }
    void initProperty(Value *getInst) {
        assert(isa<GetElementPtrInst>(getInst));

        // get owner and offset
        Value *owner = this->getOwner(getInst);
        int offset = this->getOffset(getInst);

        if (!this->isOwnerExist(owner)) {
            // construct a property value set
            set<Pointer *> propertyValueSet;

            // construct a offset map
            map<int, set<Pointer *>> offsetMap;
            offsetMap.insert(pair<int, set<Pointer *>>(offset, propertyValueSet));

            // insert into ownerMap
            this->ownerMap.insert(pair<Value *, map<int, set<Pointer *>>>(owner, offsetMap));
        }
    }

    // !TODO delete
    void output() {
        errs() << "【Output begin】\n";
        map<Value *, map<int, set<Pointer *>>>::iterator it1;
        for (it1 = this->ownerMap.begin(); it1 != this->ownerMap.end(); ++it1) {
            errs() << "___Owner:\n";
            it1->first->dump();

            map<int, set<Pointer *>> m = it1->second;
            map<int, set<Pointer *>>::iterator it2;
            for (it2 = m.begin(); it2 != m.end(); ++it2) {
                errs() << "Offset: " << it2->first << "\n";

                set<Pointer *> s = it2->second;
                set<Pointer *>::iterator it3;
                errs() << "Value:\n";
                for (it3 = s.begin(); it3 != s.end(); ++it3) {
                    (*it3)->getValue()->dump();
                }
            }
            errs() << "\n";
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
            bool isDealFunction = true;
            Argument *arg = (&F)->arg_begin();  // Argument *
            while (arg != (&F)->arg_end()) {
                if (this->isPointer(arg)) {
                    isDealFunction = false;
                    break;
                }
                ++arg;
            }

            if (isDealFunction)
                this->dealInstructionsInFunction(F);
        }
        return false;
    }
    bool doFinalization(Module &M) override {
        this->lineFuncs.output();
        //this->propertyManager.output();

        return true;
    }

    // tools
    bool isLLVMCall(Instruction &I) {
        CallInst *callInst = dyn_cast<CallInst>(&I);
        StringRef calledName = callInst->getCalledValue()->getName();
        bool llvm_dbg = calledName.find("llvm.dbg") != std::string::npos;
        bool llvm_memset = calledName.find("llvm.memset") != std::string::npos;
        bool llvm_memcpy = calledName.find("llvm.memcpy") != std::string::npos;
        return llvm_dbg || llvm_memset || llvm_memcpy;
    }
    bool isPointer(Value *v) {
        return v->getType()->isPointerTy();
    }
    bool isArrayPointer(Value *v) {
        Type *ty = v->getType();
        if (ty->isPointerTy() && ty->getPointerElementType()->isArrayTy())
            return true;
        return false;
    }
    bool isNULL(Value *v) {
        return v->getName() == "";
    }
    bool isMalloc(Value *v) {
        return v->getName() == "malloc";
    }

    void dealInstructionsInFunction(Function &F) {
        #if IS_DEBUG
        errs() << "*** dealInstructionsInFunction ***\nFunction:\n";
        F.dump();
        #endif

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
    void dealGetElementPtrInst(Value *v) {
        #if IS_DEBUG
        errs() << "=== dealGetElementPtrInst ===\nGetInst:\n";
        v->dump();
        this->propertyManager.output();
        #endif

        assert(isa<GetElementPtrInst>(v));
        GetElementPtrInst *getInst = dyn_cast<GetElementPtrInst>(v);
        Pointer *getPtr = pointerManager.getPointerFromValue(v);

        Value *operandValue = getInst->getPointerOperand();
        int offset = this->propertyManager.getOffset(v);
        set<Pointer *> rSet;   

        // load or call(the return value of a call)
        if (isa<LoadInst>(operandValue) || isa<CallInst>(operandValue)) {
            #if IS_DEBUG
            errs() << "Sub LoadInst:\n";
            operandValue->dump();
            #endif

            Pointer *operandPtr = pointerManager.getPointerFromValue(operandValue);
            set<Pointer *> ptrSet = operandPtr->getPointerSet();

            // traverse the pointer set
            // get the sub pointer set with offset
            set<Pointer *>::iterator it;
            for (it = ptrSet.begin(); it != ptrSet.end(); ++it) {
                Value *owner = (*it)->getValue();

                #if IS_DEBUG
                errs() << "____owner:\n";
                owner->dump();
                errs() << "offset:" << offset << "\n";
                errs() << "insert:\n";
                #endif

                set<Pointer *> subSet = this->propertyManager.propertyPointerSet(owner, offset);
                rSet.insert(subSet.begin(), subSet.end());
            }
        }
        // struct: scope variable, argument
        else {
            if (!this->propertyManager.isOwnerExist(operandValue))
                this->propertyManager.initProperty(v);
            rSet = this->propertyManager.propertyPointerSet(operandValue, offset);
        }

        // update pointer set
        getPtr->pointToPointSet(rSet, v);

        #if IS_DEBUG
        errs() << "GetInst:\n";
        getInst->dump();
        getPtr->output();
        #endif
    }
    /*
    【1】getelementptr
    %arrayidx13 = getelementptr inbounds [2 x i32 (i32, i32)*], [2 x i32 (i32, i32)*]* %r_fptr, i64 0, i64 1, !dbg !79
    store i32 (i32, i32)* %1, i32 (i32, i32)** %arrayidx13, align 8, !dbg !80

    %sptr = getelementptr inbounds %struct.fsptr, %struct.fsptr* %j_fptr, i32 0, i32 0, !dbg !45
    %0 = load %struct.fptr*, %struct.fptr** %sptr, align 8, !dbg !45
    %sptr1 = getelementptr inbounds %struct.fsptr, %struct.fsptr* %i_fptr, i32 0, i32 0, !dbg !46
    store %struct.fptr* %0, %struct.fptr** %sptr1, align 8, !dbg !47

    【2】bitcast
    %call = call noalias i8* @malloc(i64 8) #3, !dbg !71
    %0 = bitcast i8* %call to i32 (i32, i32)**, !dbg !72
    store i32 (i32, i32)* @plus, i32 (i32, i32)** %0, align 8, !dbg !79

    【3】normal value
    %0 = load i32 (i32, i32)*, i32 (i32, i32)** %a_fptr, align 8, !dbg !51
    %1 = load i32 (i32, i32)*, i32 (i32, i32)** %b_fptr, align 8, !dbg !54
    store i32 (i32, i32)* %1, i32 (i32, i32)** %a_fptr, align 8, !dbg !55
    store i32 (i32, i32)* %0, i32 (i32, i32)** %b_fptr, align 8, !dbg !56
    */
    void dealStoreInst(Value *v) {
        assert(isa<StoreInst>(v));
        StoreInst *storeInst = dyn_cast<StoreInst>(v);

        Value *des = storeInst->getPointerOperand();
        Value *source = storeInst->getValueOperand();

        // 1
        if (isa<GetElementPtrInst>(des)) {
            #if IS_DEBUG
            Pointer *sourcePtr = pointerManager.getPointerFromValue(source);
            errs() << "Source:\n";
            source->dump();
            sourcePtr->output();
            #endif
            this->propertyManager.insertPropertyPointer(des, source, storeInst);
        }
        // 2 3
        else /*if (isa<BitCastInst>(des))*/ {
            Pointer *desPtr = pointerManager.getPointerFromValue(des);
            Pointer *sourcePtr = pointerManager.getPointerFromValue(source);
            desPtr->pointToPointer(sourcePtr, v);
        }

        #if IS_DEBUG
        errs() << "After StoreInst\n";
        storeInst->dump();
        errs() << "*** propertyManager output ***\n";
        this->propertyManager.output();
        #endif
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
        Pointer *desPtr = pointerManager.getPointerFromValue(des);
        Pointer *loadInstPtr = pointerManager.getPointerFromValue(v);
        loadInstPtr->copyPointToSet(desPtr, v);

        #if IS_DEBUG
        errs() << "@@@@@@@ Load Output:\n";
        v->dump();
        loadInstPtr->output();
        #endif
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

        #if IS_DEBUG
        calledValue->dump();
        calledPtr->output();
        #endif

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
        else if (this->isPointer(calledValue)) {
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
        assert(isa<Function>(func));

        #if IS_DEBUG
        errs() << "=== dealCallFunction ===\n";
        func->dump();
        errs() << "@@@@@@@@@@@@@@@@@@@@@@@@@ Call Function!\n";
        #endif

        // null or malloc, don't deal it
        if (this->isNULL(func) || this->isMalloc(func))
            return;

        // bind the parameters
        this->bindFunctionParams(call, func);

        // deal the instructions in called function
        Function *f = dyn_cast<Function>(func);
        this->dealInstructionsInFunction(*f);

        // if return pointer value
        if (f->getReturnType()->isPointerTy()) {
            Value *ret = this->returnManager.getReturnValueByFuncValue(func);
            Pointer *retPtr = pointerManager.getPointerFromValue(ret);

            // bind the callinst and the return value
            Pointer *callPtr = pointerManager.getPointerFromValue(call);
            callPtr->copyPointToSet(retPtr, call);

            #if IS_DEBUG
            errs() << "ret set out:\n";
            retPtr->output();
            errs() << "call set out:\n";
            callPtr->output();
            #endif
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
            if (this->isPointer(arg)) {
                #if IS_DEBUG
                errs() << "\n$$$ BIND PTR ARG! &&&\n";
                arg->dump();
                realV->dump();
                #endif

                this->bindFuncPtrParam(call, arg, realV);
            }
            
            ++op;
            ++arg;
        }
    }
    void bindFuncPtrParam(Value *call, Argument *arg, Value *realV) {
        Pointer *argPtr = pointerManager.getPointerFromValue(arg);
        Pointer *realVPtr = pointerManager.getPointerFromValue(realV);

        #if IS_DEBUG
        errs() << "\n### bindFuncPtrParam ###\n";
        errs() << "realV:\n";
        realV->dump();
        errs() << "*** propertyManager output ***\n";
        this->propertyManager.output();
        #endif

        // struct* type
        if (this->propertyManager.isOwnerExist(realV)) {
            #if IS_DEBUG
            errs() << "realV Exist in propertyManager!\n";
            realV->dump();
            realVPtr->output();
            this->propertyManager.output();
            #endif

            this->propertyManager.insertOffsetMap(arg, realV);
        }
        // int (*arr[x])
        else if (isa<GetElementPtrInst>(realV)) {
            GetElementPtrInst *getInst = dyn_cast<GetElementPtrInst>(realV);
            Value *operandValue = getInst->getPointerOperand();
            if (this->isArrayPointer(operandValue))
                this->propertyManager.insertOffsetMap(arg, operandValue);
        }
        // normal type: int, int *(int ...), struct(load instruction) 
        else {
            #if IS_DEBUG
            errs() << "realV Ptr set:\n";
            realVPtr->output();
            #endif

            argPtr->copyPointToSet(realVPtr, call);
        }
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