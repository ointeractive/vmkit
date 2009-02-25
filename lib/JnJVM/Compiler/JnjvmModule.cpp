//===--------- JnjvmModule.cpp - Definition of a Jnjvm module -------------===//
//
//                              JnJVM
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/BasicBlock.h"
#include "llvm/CallingConv.h"
#include "llvm/Constants.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/Instructions.h"
#include "llvm/Support/MutexGuard.h"
#include "llvm/Target/TargetData.h"


#include "mvm/JIT.h"

#include "JavaCache.h"
#include "JavaConstantPool.h"
#include "JavaJIT.h"
#include "JavaString.h"
#include "JavaThread.h"
#include "JavaTypes.h"
#include "JavaUpcalls.h"
#include "Jnjvm.h"
#include "Reader.h"

#include "jnjvm/JnjvmModule.h"
#include "jnjvm/JnjvmModuleProvider.h"

#include <cstdio>

using namespace jnjvm;
using namespace llvm;

llvm::Constant* JnjvmModule::PrimitiveArrayVT;
llvm::Constant* JnjvmModule::ReferenceArrayVT;
llvm::Function* JnjvmModule::StaticInitializer;
llvm::Function* JnjvmModule::ObjectPrinter;
llvm::Function* JnjvmModule::NativeLoader;

extern void* JavaArrayVT[];
extern void* ArrayObjectVT[];
extern void* JavaObjectVT[];

extern ClassArray ArrayOfBool;
extern ClassArray ArrayOfByte;
extern ClassArray ArrayOfChar;
extern ClassArray ArrayOfShort;
extern ClassArray ArrayOfInt;
extern ClassArray ArrayOfFloat;
extern ClassArray ArrayOfDouble;
extern ClassArray ArrayOfLong;

#ifdef WITH_TRACER
const llvm::FunctionType* JnjvmModule::MarkAndTraceType = 0;
#endif

const llvm::Type* JnjvmModule::JavaObjectType = 0;
const llvm::Type* JnjvmModule::JavaArrayType = 0;
const llvm::Type* JnjvmModule::JavaArrayUInt8Type = 0;
const llvm::Type* JnjvmModule::JavaArraySInt8Type = 0;
const llvm::Type* JnjvmModule::JavaArrayUInt16Type = 0;
const llvm::Type* JnjvmModule::JavaArraySInt16Type = 0;
const llvm::Type* JnjvmModule::JavaArrayUInt32Type = 0;
const llvm::Type* JnjvmModule::JavaArraySInt32Type = 0;
const llvm::Type* JnjvmModule::JavaArrayFloatType = 0;
const llvm::Type* JnjvmModule::JavaArrayDoubleType = 0;
const llvm::Type* JnjvmModule::JavaArrayLongType = 0;
const llvm::Type* JnjvmModule::JavaArrayObjectType = 0;
const llvm::Type* JnjvmModule::CacheNodeType = 0;
const llvm::Type* JnjvmModule::EnveloppeType = 0;
const llvm::Type* JnjvmModule::ConstantPoolType = 0;
const llvm::Type* JnjvmModule::UTF8Type = 0;
const llvm::Type* JnjvmModule::JavaFieldType = 0;
const llvm::Type* JnjvmModule::JavaMethodType = 0;
const llvm::Type* JnjvmModule::AttributType = 0;
const llvm::Type* JnjvmModule::JavaThreadType = 0;

#ifdef ISOLATE_SHARING
const llvm::Type* JnjvmModule::JnjvmType = 0;
#endif

llvm::Constant*     JnjvmModule::JavaObjectNullConstant;
llvm::Constant*     JnjvmModule::MaxArraySizeConstant;
llvm::Constant*     JnjvmModule::JavaArraySizeConstant;
llvm::ConstantInt*  JnjvmModule::OffsetObjectSizeInClassConstant;
llvm::ConstantInt*  JnjvmModule::OffsetVTInClassConstant;
llvm::ConstantInt*  JnjvmModule::OffsetDepthInClassConstant;
llvm::ConstantInt*  JnjvmModule::OffsetDisplayInClassConstant;
llvm::ConstantInt*  JnjvmModule::OffsetTaskClassMirrorInClassConstant;
llvm::ConstantInt*  JnjvmModule::OffsetStaticInstanceInTaskClassMirrorConstant;
llvm::ConstantInt*  JnjvmModule::OffsetStatusInTaskClassMirrorConstant;
llvm::ConstantInt*  JnjvmModule::OffsetInitializedInTaskClassMirrorConstant;
llvm::ConstantInt*  JnjvmModule::OffsetJavaExceptionInThreadConstant;
llvm::ConstantInt*  JnjvmModule::OffsetCXXExceptionInThreadConstant;
llvm::ConstantInt*  JnjvmModule::ClassReadyConstant;
const llvm::Type*   JnjvmModule::JavaClassType;
const llvm::Type*   JnjvmModule::JavaClassPrimitiveType;
const llvm::Type*   JnjvmModule::JavaClassArrayType;
const llvm::Type*   JnjvmModule::JavaCommonClassType;
const llvm::Type*   JnjvmModule::VTType;
llvm::ConstantInt*  JnjvmModule::JavaArrayElementsOffsetConstant;
llvm::ConstantInt*  JnjvmModule::JavaArraySizeOffsetConstant;
llvm::ConstantInt*  JnjvmModule::JavaObjectLockOffsetConstant;
llvm::ConstantInt*  JnjvmModule::JavaObjectClassOffsetConstant;


bool JnjvmModule::isCompiling(const CommonClass* cl) const {
  if (cl->isClass()) {
    // A class is being static compiled if owner class is not null.
    return (((Class*)cl)->getOwnerClass() != 0);
  } else if (cl->isArray()) {
    return isCompiling(((ClassArray*)cl)->baseClass());
  } else {
    return false;
  }
}

Constant* JnjvmModule::getNativeClass(CommonClass* classDef) {

  if (staticCompilation) {

    if (classDef->isClass() || 
        (classDef->isArray() && isCompiling(classDef)) ||
        (assumeCompiled && !(classDef->isArray() && 
          classDef->asArrayClass()->baseClass()->isPrimitive()))) {
      native_class_iterator End = nativeClasses.end();
      native_class_iterator I = nativeClasses.find(classDef);
      if (I == End) {
        const llvm::Type* Ty = 0;
        
        if (classDef->isArray()) {
          Ty = JavaClassArrayType->getContainedType(0); 
        } else {
          Ty = JavaClassType->getContainedType(0); 
        }
      
        GlobalVariable* varGV = 
          new GlobalVariable(Ty, false, GlobalValue::ExternalLinkage, 0,
                             classDef->printString(), this);
      
        nativeClasses.insert(std::make_pair(classDef, varGV));

        if (classDef->isClass() && isCompiling(classDef->asClass())) {
          Constant* C = CreateConstantFromClass((Class*)classDef);
          varGV->setInitializer(C);
        } else if (classDef->isArray()) {
          Constant* C = CreateConstantFromClassArray((ClassArray*)classDef);
          varGV->setInitializer(C);
        }

        return varGV;

      } else {
        return I->second;
      }
    } else if (classDef->isArray()) {
      array_class_iterator End = arrayClasses.end();
      array_class_iterator I = arrayClasses.find((ClassArray*)classDef);
      if (I == End) {
        const llvm::Type* Ty = JavaClassArrayType; 
      
        GlobalVariable* varGV = 
          new GlobalVariable(Ty, false, GlobalValue::InternalLinkage,
                             Constant::getNullValue(Ty),
                             classDef->printString(), this);
      
        arrayClasses.insert(std::make_pair((ClassArray*)classDef, varGV));
        return varGV;
      } else {
        return I->second;
      }
    } else if (classDef->isPrimitive()) {
      assert(0 && "implement me");
    }
    return 0;
  } else {
    const llvm::Type* Ty = classDef->isClass() ? JavaClassType : 
                                                 JavaCommonClassType;
    
    ConstantInt* CI = ConstantInt::get(Type::Int64Ty, uint64_t(classDef));
    return ConstantExpr::getIntToPtr(CI, Ty);
  }
}

Constant* JnjvmModule::getConstantPool(JavaConstantPool* ctp) {
  if (staticCompilation) {
    llvm::Constant* varGV = 0;
    constant_pool_iterator End = constantPools.end();
    constant_pool_iterator I = constantPools.find(ctp);
    if (I == End) {
      const Type* Ty = ConstantPoolType->getContainedType(0);
      varGV = new GlobalVariable(Ty, false,
                                 GlobalValue::InternalLinkage,
                                 Constant::getNullValue(Ty), "", this);
      constantPools.insert(std::make_pair(ctp, varGV));
      return varGV;
    } else {
      return I->second;
    }
    
  } else {
    void* ptr = ctp->ctpRes;
    assert(ptr && "No constant pool found");
    ConstantInt* CI = ConstantInt::get(Type::Int64Ty, uint64_t(ptr));
    return ConstantExpr::getIntToPtr(CI, ConstantPoolType);
  }
}

Constant* JnjvmModule::getMethodInClass(JavaMethod* meth) {
  if (staticCompilation) {
    Class* cl = meth->classDef;
    Constant* MOffset = 0;
    Constant* Array = 0;
    method_iterator SI = virtualMethods.find(cl);
    for (uint32 i = 0; i < cl->nbVirtualMethods + cl->nbStaticMethods; ++i) {
      if (&cl->virtualMethods[i] == meth) {
        MOffset = ConstantInt::get(Type::Int32Ty, i);
        break;
      }
    }
    Array = SI->second; 
    Constant* GEPs[2] = { constantZero, MOffset };
    return ConstantExpr::getGetElementPtr(Array, GEPs, 2);
    
  } else {
    ConstantInt* CI = ConstantInt::get(Type::Int64Ty, (int64_t)meth);
    return ConstantExpr::getIntToPtr(CI, JavaMethodType);
  }
}

Constant* JnjvmModule::getString(JavaString* str) {
  if (staticCompilation) {
    string_iterator SI = strings.find(str);
    if (SI != strings.end()) {
      return SI->second;
    } else {
      assert(str && "No string given");
      LLVMClassInfo* LCI = (LLVMClassInfo*)getClassInfo((Class*)str->getClass());
      const llvm::Type* Ty = LCI->getVirtualType();
      GlobalVariable* varGV = 
        new GlobalVariable(Ty->getContainedType(0), false,
                           GlobalValue::InternalLinkage,
                           0, "", this);
      Constant* res = ConstantExpr::getCast(Instruction::BitCast, varGV,
                                            JavaObjectType);
      strings.insert(std::make_pair(str, res));
      Constant* C = CreateConstantFromJavaString(str);
      varGV->setInitializer(C);
      return res;
    }
    
  } else {
    assert(str && "No string given");
    ConstantInt* CI = ConstantInt::get(Type::Int64Ty, uint64(str));
    return ConstantExpr::getIntToPtr(CI, JavaObjectType);
  }
}

Constant* JnjvmModule::getEnveloppe(Enveloppe* enveloppe) {
  if (staticCompilation) {
    enveloppe_iterator SI = enveloppes.find(enveloppe);
    if (SI != enveloppes.end()) {
      return SI->second;
    } else {
      GlobalVariable* varGV = 
        new GlobalVariable(EnveloppeType->getContainedType(0), false,
                           GlobalValue::InternalLinkage, 0, "", this);
      enveloppes.insert(std::make_pair(enveloppe, varGV));
      
      Constant* C = CreateConstantFromEnveloppe(enveloppe);
      varGV->setInitializer(C);
      return varGV;
    }
  
  } else {
    assert(enveloppe && "No enveloppe given");
    ConstantInt* CI = ConstantInt::get(Type::Int64Ty, uint64(enveloppe));
    return ConstantExpr::getIntToPtr(CI, EnveloppeType);
  }
}

Constant* JnjvmModule::getJavaClass(CommonClass* cl) {
  if (staticCompilation) {
    java_class_iterator End = javaClasses.end();
    java_class_iterator I = javaClasses.find(cl);
    if (I == End) {
      Class* javaClass = cl->classLoader->bootstrapLoader->upcalls->newClass;
      LLVMClassInfo* LCI = (LLVMClassInfo*)getClassInfo(javaClass);
      const llvm::Type* Ty = LCI->getVirtualType();
      
      GlobalVariable* varGV = 
        new GlobalVariable(Ty->getContainedType(0), false,
                           GlobalValue::InternalLinkage, 0, "", this);
      
      Constant* res = ConstantExpr::getCast(Instruction::BitCast, varGV,
                                            JavaObjectType);
    
      javaClasses.insert(std::make_pair(cl, res));
      varGV->setInitializer(CreateConstantFromJavaClass(cl));
      return res;
    } else {
      return I->second;
    }
  
  } else {
    JavaObject* obj = cl->getClassDelegatee(JavaThread::get()->getJVM());
    assert(obj && "Delegatee not created");
    Constant* CI = ConstantInt::get(Type::Int64Ty, uint64(obj));
    return ConstantExpr::getIntToPtr(CI, JavaObjectType);
  }
}

JavaObject* JnjvmModule::getFinalObject(llvm::Value* obj) {
  if (staticCompilation) {
    // TODO
  } else {
    if (ConstantExpr* CE = dyn_cast<ConstantExpr>(obj)) {
      if (ConstantInt* C = dyn_cast<ConstantInt>(CE->getOperand(0))) {
        return (JavaObject*)C->getZExtValue();
      }
    }
  }
  return 0;
}



Constant* JnjvmModule::getFinalObject(JavaObject* obj) {
  if (staticCompilation) {
    final_object_iterator End = finalObjects.end();
    final_object_iterator I = finalObjects.find(obj);
    if (I == End) {
      abort();
      return 0;
    } else {
      return I->second;
    }
  
  } else {
    Constant* CI = ConstantInt::get(Type::Int64Ty, uint64(obj));
    return ConstantExpr::getIntToPtr(CI, JavaObjectType);
  }
}

Constant* JnjvmModule::CreateConstantFromStaticInstance(Class* cl) {
  LLVMClassInfo* LCI = getClassInfo(cl);
  const Type* Ty = LCI->getStaticType();
  const StructType* STy = dyn_cast<StructType>(Ty->getContainedType(0));
  
  std::vector<Constant*> Elts;
  
  for (uint32 i = 0; i < cl->nbStaticFields; ++i) {
    JavaField& field = cl->staticFields[i];
    const Typedef* type = field.getSignature();
    LLVMAssessorInfo& LAI = getTypedefInfo(type);
    const Type* Ty = LAI.llvmType;

    Attribut* attribut = field.lookupAttribut(Attribut::constantAttribut);

    if (!attribut) {
      Elts.push_back(Constant::getNullValue(Ty));
    } else {
      Reader reader(attribut, cl->bytes);
      JavaConstantPool * ctpInfo = cl->ctpInfo;
      uint16 idx = reader.readU2();
      if (type->isPrimitive()) {
        if (Ty == Type::Int64Ty) {
          Elts.push_back(ConstantInt::get(Ty, (uint64)ctpInfo->LongAt(idx)));
        } else if (Ty == Type::DoubleTy) {
          Elts.push_back(ConstantFP::get(Ty, ctpInfo->DoubleAt(idx)));
        } else if (Ty == Type::FloatTy) {
          Elts.push_back(ConstantFP::get(Ty, ctpInfo->FloatAt(idx)));
        } else {
          Elts.push_back(ConstantInt::get(Ty, (uint64)ctpInfo->IntegerAt(idx)));
        }
      } else if (type->isReference()){
        const UTF8* utf8 = ctpInfo->UTF8At(ctpInfo->ctpDef[idx]);
        JavaString* obj = ctpInfo->resolveString(utf8, idx);
        Constant* C = getString(obj);
        C = ConstantExpr::getBitCast(C, JavaObjectType);
        Elts.push_back(C);
      } else {
        fprintf(stderr, "Implement me");
        abort();
      }
    }
  }
   
  return ConstantStruct::get(STy, Elts);
}

Constant* JnjvmModule::getStaticInstance(Class* classDef) {
#ifdef ISOLATE
  assert(0 && "Should not be here");
  abort();
#endif
  if (staticCompilation) {
    static_instance_iterator End = staticInstances.end();
    static_instance_iterator I = staticInstances.find(classDef);
    if (I == End) {
      
      LLVMClassInfo* LCI = getClassInfo(classDef);
      const Type* Ty = LCI->getStaticType();
      Ty = Ty->getContainedType(0);
      GlobalVariable* varGV = 
        new GlobalVariable(Ty, false, GlobalValue::ExternalLinkage,
                           0, classDef->printString("<static>"), this);

      Constant* res = ConstantExpr::getCast(Instruction::BitCast, varGV,
                                            ptrType);
      staticInstances.insert(std::make_pair(classDef, res));
      
      if (isCompiling(classDef)) { 
        Constant* C = CreateConstantFromStaticInstance(classDef);
        varGV->setInitializer(C);
      }

      return res;
    } else {
      return I->second;
    }

  } else {
    void* obj = ((Class*)classDef)->getStaticInstance();
    if (!obj) {
      Class* cl = (Class*)classDef;
      classDef->acquire();
      obj = cl->getStaticInstance();
      if (!obj) {
        // Allocate now so that compiled code can reference it.
        obj = cl->allocateStaticInstance(JavaThread::get()->getJVM());
      }
      classDef->release();
    }
    Constant* CI = ConstantInt::get(Type::Int64Ty, (uint64_t(obj)));
    return ConstantExpr::getIntToPtr(CI, ptrType);
  }
}

Constant* JnjvmModule::getVirtualTable(Class* classDef) {
  LLVMClassInfo* LCI = getClassInfo((Class*)classDef);
  LCI->getVirtualType();
  if (staticCompilation) {
    llvm::Constant* res = 0;
    virtual_table_iterator End = virtualTables.end();
    virtual_table_iterator I = virtualTables.find(classDef);
    if (I == End) {
      
      const ArrayType* ATy = dyn_cast<ArrayType>(VTType->getContainedType(0));
      const PointerType* PTy = dyn_cast<PointerType>(ATy->getContainedType(0));
      ATy = ArrayType::get(PTy, classDef->virtualTableSize);
      // Do not set a virtual table as a constant, because the runtime may
      // modify it.
      GlobalVariable* varGV = new GlobalVariable(ATy, false,
                                                 GlobalValue::ExternalLinkage,
                                                 0, 
                                                 classDef->printString("<VT>"),
                                                 this);
    
      res = ConstantExpr::getCast(Instruction::BitCast, varGV, VTType);
      virtualTables.insert(std::make_pair(classDef, res));
    
      if (isCompiling(classDef)) {
        Constant* C = CreateConstantFromVT(classDef);
        varGV->setInitializer(C);
      }
      
      return res;
    } else {
      return I->second;
    } 

  } else {
    assert(classDef->virtualVT && "Virtual VT not created");
    void* ptr = classDef->virtualVT;
    ConstantInt* CI = ConstantInt::get(Type::Int64Ty, uint64_t(ptr));
    return ConstantExpr::getIntToPtr(CI, VTType);
  }
}

Constant* JnjvmModule::getNativeFunction(JavaMethod* meth, void* ptr) {
  if (staticCompilation) {
    llvm::Constant* varGV = 0;
    native_function_iterator End = nativeFunctions.end();
    native_function_iterator I = nativeFunctions.find(meth);
    if (I == End) {
      
      LLVMSignatureInfo* LSI = getSignatureInfo(meth->getSignature());
      const llvm::Type* valPtrType = LSI->getNativePtrType();
      
      varGV = new GlobalVariable(valPtrType, true,
                                 GlobalValue::InternalLinkage,
                                 Constant::getNullValue(valPtrType), "", this);
    
      nativeFunctions.insert(std::make_pair(meth, varGV));
      return varGV;
    } else {
      return I->second;
    }
  
  } else {
    LLVMSignatureInfo* LSI = getSignatureInfo(meth->getSignature());
    const llvm::Type* valPtrType = LSI->getNativePtrType();
    
    assert(ptr && "No native function given");

    Constant* CI = ConstantInt::get(Type::Int64Ty, uint64_t(ptr));
    return ConstantExpr::getIntToPtr(CI, valPtrType);
  }
}

#ifndef WITHOUT_VTABLE
void JnjvmModule::allocateVT(Class* cl) {
  for (uint32 i = 0; i < cl->nbVirtualMethods; ++i) {
    JavaMethod& meth = cl->virtualMethods[i];
    if (meth.name->equals(cl->classLoader->bootstrapLoader->finalize)) {
      meth.offset = 0;
    } else {
      JavaMethod* parent = cl->super? 
        cl->super->lookupMethodDontThrow(meth.name, meth.type, false, true,
                                         0) :
        0;

      uint64_t offset = 0;
      if (!parent) {
        offset = cl->virtualTableSize++;
        meth.offset = offset;
      } else {
        offset = parent->offset;
        meth.offset = parent->offset;
      }
    }
  }

  VirtualTable* VT = 0;
  if (cl->super) {
    uint64 size = cl->virtualTableSize;
    mvm::BumpPtrAllocator& allocator = cl->classLoader->allocator;
    VT = (VirtualTable*)allocator.Allocate(size * sizeof(void*));
    Class* super = (Class*)cl->super;
    assert(cl->virtualTableSize >= cl->super->virtualTableSize &&
      "Super VT bigger than own VT");
    assert(super->virtualVT && "Super does not have a VT!");
    memcpy(VT, super->virtualVT, cl->super->virtualTableSize * sizeof(void*));
  } else {
    VT = JavaObjectVT;
  }

  cl->virtualVT = VT;
}
#endif


#ifdef WITH_TRACER
llvm::Function* JnjvmModule::makeTracer(Class* cl, bool stat) {
  
  if (isStaticCompiling() && !generateTracers) {
    return JavaObjectTracerFunction;
  }

  LLVMClassInfo* LCI = (LLVMClassInfo*)getClassInfo(cl);
  const Type* type = stat ? LCI->getStaticType() : LCI->getVirtualType();
  JavaField* fields = 0;
  uint32 nbFields = 0;
  if (stat) {
    fields = cl->getStaticFields();
    nbFields = cl->nbStaticFields;
  } else {
    fields = cl->getVirtualFields();
    nbFields = cl->nbVirtualFields;
  }
  
  Function* func = Function::Create(JnjvmModule::MarkAndTraceType,
                                    GlobalValue::InternalLinkage,
                                    "",
                                    this);

  Constant* zero = mvm::MvmModule::constantZero;
  Argument* arg = func->arg_begin();
  BasicBlock* block = BasicBlock::Create("", func);
  llvm::Value* realArg = new BitCastInst(arg, type, "", block);

  std::vector<Value*> Args;
  Args.push_back(arg);
#ifdef MULTIPLE_GC
  Value* GC = ++func->arg_begin();
  Args.push_back(GC);
#endif
  if (!stat) {
    if (cl->super == 0) {
      CallInst::Create(JavaObjectTracerFunction, Args.begin(), Args.end(),
                        "", block);

    } else {
      LLVMClassInfo* LCP = (LLVMClassInfo*)getClassInfo((Class*)(cl->super));
      Function* F = LCP->virtualTracerFunction;
      if (!F) {
        if (isStaticCompiling()) {
          F = makeTracer(cl->super, false);
        } else {
          F = LCP->getVirtualTracer();
        }
        assert(F && "Still no virtual tracer for super");
      }
      CallInst::Create(F, Args.begin(), Args.end(), "", block);
    }
  }
  
  for (uint32 i = 0; i < nbFields; ++i) {
    JavaField& cur = fields[i];
    if (cur.getSignature()->trace()) {
      LLVMFieldInfo* LFI = getFieldInfo(&cur);
      std::vector<Value*> args; //size = 2
      args.push_back(zero);
      args.push_back(LFI->getOffset());
      Value* ptr = GetElementPtrInst::Create(realArg, args.begin(), args.end(), 
                                             "",block);
      Value* val = new LoadInst(ptr, "", block);
      Value* valCast = new BitCastInst(val, JnjvmModule::JavaObjectType, "",
                                       block);
      std::vector<Value*> Args;
      Args.push_back(valCast);
#ifdef MULTIPLE_GC
      Args.push_back(GC);
#endif
      CallInst::Create(JnjvmModule::MarkAndTraceFunction, Args.begin(),
                       Args.end(), "", block);
    }
  }

  ReturnInst::Create(block);
  
  if (!stat) {
    LCI->virtualTracerFunction = func;
  } else {
    LCI->staticTracerFunction = func;
  }

  return func;
}
#endif

Constant* JnjvmModule::CreateConstantForBaseObject(CommonClass* cl) {
  const StructType* STy = 
    dyn_cast<StructType>(JavaObjectType->getContainedType(0));
  
  std::vector<Constant*> Elmts;

  // virtual table
  if (cl->isClass()) {
    Elmts.push_back(getVirtualTable(cl->asClass()));
  } else {
    ClassArray* clA = cl->asArrayClass();
    if (clA->baseClass()->isPrimitive()) {
      Elmts.push_back(PrimitiveArrayVT);
    } else {
      Elmts.push_back(ReferenceArrayVT);
    }
  }
  
  // classof
  Constant* Cl = getNativeClass(cl);
  Constant* ClGEPs[2] = { constantZero, constantZero };
  Cl = ConstantExpr::getGetElementPtr(Cl, ClGEPs, 2);
    
  Elmts.push_back(Cl);

  // lock
  Constant* L = ConstantInt::get(Type::Int64Ty,
                                 JavaThread::get()->getThreadID());
  Elmts.push_back(ConstantExpr::getIntToPtr(L, ptrType));

  return ConstantStruct::get(STy, Elmts);
}

Constant* JnjvmModule::CreateConstantFromJavaClass(CommonClass* cl) {
  Class* javaClass = cl->classLoader->bootstrapLoader->upcalls->newClass;
  LLVMClassInfo* LCI = (LLVMClassInfo*)getClassInfo(javaClass);
  const StructType* STy = 
    dyn_cast<StructType>(LCI->getVirtualType()->getContainedType(0));

  std::vector<Constant*> Elmts;

  // JavaObject
  Elmts.push_back(CreateConstantForBaseObject(javaClass));
  
  // signers
  Elmts.push_back(Constant::getNullValue(JavaObjectType));
  
  // pd
  Elmts.push_back(Constant::getNullValue(JavaObjectType));
  
  // vmdata
  Constant* Cl = getNativeClass(cl);
  Cl = ConstantExpr::getCast(Instruction::BitCast, Cl, JavaObjectType);
  Elmts.push_back(Cl);

  // constructor
  Elmts.push_back(Constant::getNullValue(JavaObjectType));

  return ConstantStruct::get(STy, Elmts);
}

Constant* JnjvmModule::CreateConstantFromJavaObject(JavaObject* obj) {
  CommonClass* cl = obj->getClass();

  if (cl->isArray()) {
    Classpath* upcalls = cl->classLoader->bootstrapLoader->upcalls;
    CommonClass* subClass = cl->asArrayClass()->baseClass();
    if (subClass->isPrimitive()) {
      if (subClass == upcalls->OfBool) {
        return CreateConstantFromArray<ArrayUInt8>((ArrayUInt8*)obj,
                                                   Type::Int8Ty);
      } else if (subClass == upcalls->OfByte) {
        return CreateConstantFromArray<ArraySInt8>((ArraySInt8*)obj,
                                                   Type::Int8Ty);
      } else if (subClass == upcalls->OfShort) {
        return CreateConstantFromArray<ArraySInt16>((ArraySInt16*)obj,
                                                    Type::Int16Ty);
      } else if (subClass == upcalls->OfChar) {
        return CreateConstantFromArray<ArrayUInt16>((ArrayUInt16*)obj,
                                                    Type::Int16Ty);
      } else if (subClass == upcalls->OfInt) {
        return CreateConstantFromArray<ArraySInt32>((ArraySInt32*)obj,
                                                    Type::Int32Ty);
      } else if (subClass == upcalls->OfFloat) {
        return CreateConstantFromArray<ArrayFloat>((ArrayFloat*)obj,
                                                   Type::FloatTy);
      } else if (subClass == upcalls->OfLong) {
        return CreateConstantFromArray<ArrayLong>((ArrayLong*)obj,
                                                  Type::Int64Ty);
      } else if (subClass == upcalls->OfDouble) {
        return CreateConstantFromArray<ArrayDouble>((ArrayDouble*)obj,
                                                    Type::DoubleTy);
      } else {
        abort();
      }
    } else {
      return CreateConstantFromArray<ArrayObject>((ArrayObject*)obj,
                                                  JavaObjectType);
    }
  } else {
    
    std::vector<Constant*> Elmts;
    
    // JavaObject
    Constant* CurConstant = CreateConstantForBaseObject(obj->getClass());

    for (uint32 j = 0; j <= cl->depth; ++j) {
      std::vector<Constant*> TempElts;
      Elmts.push_back(CurConstant);
      TempElts.push_back(CurConstant);
      Class* curCl = cl->display[j]->asClass();
      LLVMClassInfo* LCI = getClassInfo(curCl);
      const StructType* STy = 
        dyn_cast<StructType>(LCI->getVirtualType()->getContainedType(0));

      for (uint32 i = 0; i < curCl->nbVirtualFields; ++i) {
        JavaField& field = curCl->virtualFields[i];
        const Typedef* type = field.getSignature();
        if (type->isPrimitive()) {
          const PrimitiveTypedef* prim = (PrimitiveTypedef*)type;
          if (prim->isBool() || prim->isByte()) {
            ConstantInt* CI = ConstantInt::get(Type::Int8Ty,
                                               field.getInt8Field(obj));
            TempElts.push_back(CI);
          } else if (prim->isShort() || prim->isChar()) {
            ConstantInt* CI = ConstantInt::get(Type::Int16Ty,
                                               field.getInt16Field(obj));
            TempElts.push_back(CI);
          } else if (prim->isInt()) {
            ConstantInt* CI = ConstantInt::get(Type::Int32Ty,
                                               field.getInt32Field(obj));
            TempElts.push_back(CI);
          } else if (prim->isLong()) {
            ConstantInt* CI = ConstantInt::get(Type::Int64Ty,
                                               field.getLongField(obj));
            TempElts.push_back(CI);
          } else if (prim->isFloat()) {
            ConstantInt* CI = ConstantInt::get(Type::FloatTy,
                                               field.getFloatField(obj));
            TempElts.push_back(CI);
          } else if (prim->isDouble()) {
            ConstantInt* CI = ConstantInt::get(Type::DoubleTy,
                                               field.getDoubleField(obj));
            TempElts.push_back(CI);
          } else {
            abort();
          }
        } else {
          Constant* C = getFinalObject(field.getObjectField(obj));
          TempElts.push_back(C);
        }
      }
      CurConstant = ConstantStruct::get(STy, TempElts);
    }

    return CurConstant;
  }
}

Constant* JnjvmModule::CreateConstantFromJavaString(JavaString* str) {
  Class* cl = (Class*)str->getClass();
  LLVMClassInfo* LCI = (LLVMClassInfo*)getClassInfo(cl);
  const StructType* STy = 
    dyn_cast<StructType>(LCI->getVirtualType()->getContainedType(0));

  std::vector<Constant*> Elmts;

  Elmts.push_back(CreateConstantForBaseObject(cl));

  Constant* Array = getUTF8(str->value);
  Constant* ObjGEPs[2] = { constantZero, constantZero };
  Array = ConstantExpr::getGetElementPtr(Array, ObjGEPs, 2);
  Elmts.push_back(Array);
  
  Elmts.push_back(ConstantInt::get(Type::Int32Ty, str->count));
  Elmts.push_back(ConstantInt::get(Type::Int32Ty, str->cachedHashCode));
  Elmts.push_back(ConstantInt::get(Type::Int32Ty, str->offset));
  
  return ConstantStruct::get(STy, Elmts);
}


Constant* JnjvmModule::CreateConstantFromCacheNode(CacheNode* CN) {
  const StructType* STy = 
    dyn_cast<StructType>(CacheNodeType->getContainedType(0));

  std::vector<Constant*> Elmts;
  Elmts.push_back(Constant::getNullValue(STy->getContainedType(0)));
  Elmts.push_back(Constant::getNullValue(STy->getContainedType(1)));
  Elmts.push_back(Constant::getNullValue(STy->getContainedType(2)));
  Elmts.push_back(getEnveloppe(CN->enveloppe));
  
  return ConstantStruct::get(STy, Elmts);
}

Constant* JnjvmModule::CreateConstantFromEnveloppe(Enveloppe* val) {
  
  const StructType* STy = 
    dyn_cast<StructType>(EnveloppeType->getContainedType(0));
  const StructType* CNTy = 
    dyn_cast<StructType>(CacheNodeType->getContainedType(0));
  
  std::vector<Constant*> Elmts;
  
  Constant* firstCache = CreateConstantFromCacheNode(val->firstCache);
  Elmts.push_back(new GlobalVariable(CNTy, false,
                                     GlobalValue::InternalLinkage,
                                     firstCache, "", this));
  Elmts.push_back(getUTF8(val->methodName));
  Elmts.push_back(getUTF8(val->methodSign));

  Elmts.push_back(Constant::getNullValue(Type::Int8Ty));
  Elmts.push_back(getNativeClass(val->classDef));
  Elmts.push_back(firstCache);

  return ConstantStruct::get(STy, Elmts);
  
}

Constant* JnjvmModule::CreateConstantFromAttribut(Attribut& attribut) {
  const StructType* STy = 
    dyn_cast<StructType>(AttributType->getContainedType(0));


  std::vector<Constant*> Elmts;

  // name
  Elmts.push_back(getUTF8(attribut.name));

  // start
  Elmts.push_back(ConstantInt::get(Type::Int32Ty, attribut.start));

  // nbb
  Elmts.push_back(ConstantInt::get(Type::Int32Ty, attribut.nbb));
  
  return ConstantStruct::get(STy, Elmts);
}

Constant* JnjvmModule::CreateConstantFromCommonClass(CommonClass* cl) {
  const StructType* STy = 
    dyn_cast<StructType>(JavaCommonClassType->getContainedType(0));

  const ArrayType* ATy = ArrayType::get(JavaCommonClassType, cl->depth + 1);
  
  std::vector<Constant*> CommonClassElts;
  std::vector<Constant*> TempElmts;
  Constant* ClGEPs[2] = { constantZero, constantZero };

  // display
  for (uint32 i = 0; i <= cl->depth; ++i) {
    Constant* Cl = getNativeClass(cl->display[i]);
    if (Cl->getType() != JavaCommonClassType)
      Cl = ConstantExpr::getGetElementPtr(Cl, ClGEPs, 2);
    
    TempElmts.push_back(Cl);
  }

  Constant* display = ConstantArray::get(ATy, TempElmts);
  TempElmts.clear();
  display = new GlobalVariable(ATy, true, GlobalValue::InternalLinkage,
                               display, "", this);
  display = ConstantExpr::getCast(Instruction::BitCast, display,
                                  PointerType::getUnqual(JavaCommonClassType));
  CommonClassElts.push_back(display);

  // depth
  CommonClassElts.push_back(ConstantInt::get(Type::Int32Ty, cl->depth));
  
  // delegatee
  ATy = dyn_cast<ArrayType>(STy->getContainedType(2));
  assert(ATy && "Malformed type");

  Constant* TCM[1] = { getJavaClass(cl) };
  CommonClassElts.push_back(ConstantArray::get(ATy, TCM, 1));
  
  // access
  CommonClassElts.push_back(ConstantInt::get(Type::Int16Ty, cl->access));
 
  // interfaces
  if (cl->nbInterfaces) {
    for (uint32 i = 0; i < cl->nbInterfaces; ++i) {
      TempElmts.push_back(getNativeClass(cl->interfaces[i]));
    }

    ATy = ArrayType::get(JavaClassType, cl->nbInterfaces);
    Constant* interfaces = ConstantArray::get(ATy, TempElmts);
    interfaces = new GlobalVariable(ATy, true, GlobalValue::InternalLinkage,
                                    interfaces, "", this);
    interfaces = ConstantExpr::getCast(Instruction::BitCast, interfaces,
                                       PointerType::getUnqual(JavaClassType));

    CommonClassElts.push_back(interfaces);
  } else {
    const Type* Ty = PointerType::getUnqual(JavaClassType);
    CommonClassElts.push_back(Constant::getNullValue(Ty));
  }

  // nbInterfaces
  CommonClassElts.push_back(ConstantInt::get(Type::Int16Ty, cl->nbInterfaces));

  // name
  CommonClassElts.push_back(getUTF8(cl->name));

  // super
  if (cl->super) {
    CommonClassElts.push_back(getNativeClass(cl->super));
  } else {
    CommonClassElts.push_back(Constant::getNullValue(JavaClassType));
  }

  // classLoader: store the static initializer, it will be overriden once
  // the class is loaded.
  Constant* loader = ConstantExpr::getBitCast(StaticInitializer, ptrType);
  CommonClassElts.push_back(loader);
 
  return ConstantStruct::get(STy, CommonClassElts);
}

Constant* JnjvmModule::CreateConstantFromJavaField(JavaField& field) {
  const StructType* STy = 
    dyn_cast<StructType>(JavaFieldType->getContainedType(0));
  
  std::vector<Constant*> FieldElts;
  std::vector<Constant*> TempElts;
  
  // signature
  FieldElts.push_back(Constant::getNullValue(ptrType));
  
  // access
  FieldElts.push_back(ConstantInt::get(Type::Int16Ty, field.access));

  // name
  FieldElts.push_back(getUTF8(field.name));

  // type
  FieldElts.push_back(getUTF8(field.type));
  
  // attributs 
  if (field.nbAttributs) {
    const ArrayType* ATy = ArrayType::get(AttributType->getContainedType(0),
                                          field.nbAttributs);
    for (uint32 i = 0; i < field.nbAttributs; ++i) {
      TempElts.push_back(CreateConstantFromAttribut(field.attributs[i]));
    }

    Constant* attributs = ConstantArray::get(ATy, TempElts);
    TempElts.clear();
    attributs = new GlobalVariable(ATy, true, GlobalValue::InternalLinkage,
                                   attributs, "", this);
    attributs = ConstantExpr::getCast(Instruction::BitCast, attributs,
                                      AttributType);
  
    FieldElts.push_back(attributs);
  } else {
    FieldElts.push_back(Constant::getNullValue(AttributType));
  }
  
  // nbAttributs
  FieldElts.push_back(ConstantInt::get(Type::Int16Ty, field.nbAttributs));

  // classDef
  FieldElts.push_back(getNativeClass(field.classDef));

  // ptrOffset
  FieldElts.push_back(ConstantInt::get(Type::Int32Ty, field.ptrOffset));

  // num
  FieldElts.push_back(ConstantInt::get(Type::Int16Ty, field.num));

  //JInfo
  FieldElts.push_back(Constant::getNullValue(ptrType));
  
  return ConstantStruct::get(STy, FieldElts); 
}

Constant* JnjvmModule::CreateConstantFromJavaMethod(JavaMethod& method) {
  const StructType* STy = 
    dyn_cast<StructType>(JavaMethodType->getContainedType(0));
  
  std::vector<Constant*> MethodElts;
  std::vector<Constant*> TempElts;
  
  // signature
  MethodElts.push_back(Constant::getNullValue(ptrType));
  
  // access
  MethodElts.push_back(ConstantInt::get(Type::Int16Ty, method.access));
 
  // attributs
  if (method.nbAttributs) {
    const ArrayType* ATy = ArrayType::get(AttributType->getContainedType(0),
                                          method.nbAttributs);
    for (uint32 i = 0; i < method.nbAttributs; ++i) {
      TempElts.push_back(CreateConstantFromAttribut(method.attributs[i]));
    }

    Constant* attributs = ConstantArray::get(ATy, TempElts);
    TempElts.clear();
    attributs = new GlobalVariable(ATy, true, GlobalValue::InternalLinkage,
                                   attributs, "", this);
    attributs = ConstantExpr::getCast(Instruction::BitCast, attributs,
                                      AttributType);

    MethodElts.push_back(attributs);
  } else {
    MethodElts.push_back(Constant::getNullValue(AttributType));
  }
  
  // nbAttributs
  MethodElts.push_back(ConstantInt::get(Type::Int16Ty, method.nbAttributs));
  
  // enveloppes
  // already allocated by the JIT, don't reallocate them.
  MethodElts.push_back(Constant::getNullValue(EnveloppeType));
  
  // nbEnveloppes
  // 0 because we're not allocating here.
  MethodElts.push_back(ConstantInt::get(Type::Int16Ty, 0));
  
  // classDef
  MethodElts.push_back(getNativeClass(method.classDef));
  
  // name
  MethodElts.push_back(getUTF8(method.name));

  // type
  MethodElts.push_back(getUTF8(method.type));
  
  // canBeInlined
  MethodElts.push_back(ConstantInt::get(Type::Int8Ty, method.canBeInlined));

  // code
  if (isAbstract(method.access)) {
    MethodElts.push_back(Constant::getNullValue(ptrType));
  } else {
    LLVMMethodInfo* LMI = getMethodInfo(&method);
    Function* func = LMI->getMethod();
    MethodElts.push_back(ConstantExpr::getCast(Instruction::BitCast, func,
                                               ptrType));
  }

  // offset
  MethodElts.push_back(ConstantInt::get(Type::Int32Ty, method.offset));

  //JInfo
  MethodElts.push_back(Constant::getNullValue(ptrType));
  
  return ConstantStruct::get(STy, MethodElts); 
}

Constant* JnjvmModule::CreateConstantFromClassPrimitive(ClassPrimitive* cl) {
  const StructType* STy = 
    dyn_cast<StructType>(JavaClassPrimitiveType->getContainedType(0));
  
  std::vector<Constant*> ClassElts;
  
  // common class
  ClassElts.push_back(CreateConstantFromCommonClass(cl));

  // primSize
  ClassElts.push_back(ConstantInt::get(Type::Int32Ty, cl->primSize));

  return ConstantStruct::get(STy, ClassElts);
}

Constant* JnjvmModule::CreateConstantFromClassArray(ClassArray* cl) {
  const StructType* STy = 
    dyn_cast<StructType>(JavaClassArrayType->getContainedType(0));
  
  std::vector<Constant*> ClassElts;
  Constant* ClGEPs[2] = { constantZero, constantZero };
  
  // common class
  ClassElts.push_back(CreateConstantFromCommonClass(cl));

  // baseClass
  Constant* Cl = getNativeClass(cl->baseClass());
  if (Cl->getType() != JavaCommonClassType)
    Cl = ConstantExpr::getGetElementPtr(Cl, ClGEPs, 2);
    
  ClassElts.push_back(Cl);

  return ConstantStruct::get(STy, ClassElts);
}

Constant* JnjvmModule::CreateConstantFromClass(Class* cl) {
  const StructType* STy = 
    dyn_cast<StructType>(JavaClassType->getContainedType(0));
  
  std::vector<Constant*> ClassElts;
  std::vector<Constant*> TempElts;

  // common class
  ClassElts.push_back(CreateConstantFromCommonClass(cl));

  // virtualSize
  ClassElts.push_back(ConstantInt::get(Type::Int32Ty, cl->virtualSize));

  // virtualTable
  ClassElts.push_back(getVirtualTable(cl));

  // IsolateInfo
  const ArrayType* ATy = dyn_cast<ArrayType>(STy->getContainedType(3));
  assert(ATy && "Malformed type");
  
  const StructType* TCMTy = dyn_cast<StructType>(ATy->getContainedType(0));
  assert(TCMTy && "Malformed type");

  uint32 status = cl->needsInitialisationCheck() ? vmjc : ready;
  TempElts.push_back(ConstantInt::get(Type::Int8Ty, status));
  TempElts.push_back(ConstantInt::get(Type::Int1Ty, status == ready ? 1 : 0));
  TempElts.push_back(getStaticInstance(cl));
  Constant* CStr[1] = { ConstantStruct::get(TCMTy, TempElts) };
  TempElts.clear();
  ClassElts.push_back(ConstantArray::get(ATy, CStr, 1));

  // thinlock
  ClassElts.push_back(Constant::getNullValue(ptrType));

  if (cl->nbVirtualFields + cl->nbStaticFields) {
    ATy = ArrayType::get(JavaFieldType->getContainedType(0), 
                         cl->nbVirtualFields + cl->nbStaticFields);
  }

  // virtualFields
  if (cl->nbVirtualFields) {

    for (uint32 i = 0; i < cl->nbVirtualFields; ++i) {
      TempElts.push_back(CreateConstantFromJavaField(cl->virtualFields[i]));
    }

  } 
  
  // staticFields
  if (cl->nbStaticFields) {

    for (uint32 i = 0; i < cl->nbStaticFields; ++i) {
      TempElts.push_back(CreateConstantFromJavaField(cl->staticFields[i]));
    }

  }

  Constant* fields = 0;
  if (cl->nbStaticFields + cl->nbVirtualFields) {
  
    fields = ConstantArray::get(ATy, TempElts);
    TempElts.clear();
    fields = new GlobalVariable(ATy, false, GlobalValue::InternalLinkage,
                                fields, "", this);
    fields = ConstantExpr::getCast(Instruction::BitCast, fields, JavaFieldType);
  } else {
    fields = Constant::getNullValue(JavaFieldType);
  }

  // virtualFields
  ClassElts.push_back(fields);

  ConstantInt* nbVirtualFields = 
    ConstantInt::get(Type::Int16Ty, cl->nbVirtualFields);
  // nbVirtualFields
  ClassElts.push_back(nbVirtualFields);
  
  // staticFields
  // Output null, this will be set in  the initializer. Otherwise, the
  // assembly emitter of LLVM will try to align the data.
  ClassElts.push_back(Constant::getNullValue(JavaFieldType));

  // nbStaticFields
  ClassElts.push_back(ConstantInt::get(Type::Int16Ty, cl->nbStaticFields));
  
  // virtualMethods
  if (cl->nbVirtualMethods + cl->nbStaticMethods) {
    ATy = ArrayType::get(JavaMethodType->getContainedType(0),
                         cl->nbVirtualMethods + cl->nbStaticMethods);
  }

  if (cl->nbVirtualMethods) {
    for (uint32 i = 0; i < cl->nbVirtualMethods; ++i) {
      TempElts.push_back(CreateConstantFromJavaMethod(cl->virtualMethods[i]));
    }
  }
    
  if (cl->nbStaticMethods) {
    for (uint32 i = 0; i < cl->nbStaticMethods; ++i) {
      TempElts.push_back(CreateConstantFromJavaMethod(cl->staticMethods[i]));
    }
  }

  Constant* methods = 0;
  if (cl->nbVirtualMethods + cl->nbStaticMethods) {
    methods = ConstantArray::get(ATy, TempElts);
    TempElts.clear();
    GlobalVariable* GV = new GlobalVariable(ATy, false,
                                            GlobalValue::InternalLinkage,
                                            methods, "", this);
    virtualMethods.insert(std::make_pair(cl, GV));
    methods = ConstantExpr::getCast(Instruction::BitCast, GV,
                                    JavaMethodType);
  } else {
    methods = Constant::getNullValue(JavaMethodType);
  }

  // virtualMethods
  ClassElts.push_back(methods);

  ConstantInt* nbVirtualMethods = 
    ConstantInt::get(Type::Int16Ty, cl->nbVirtualMethods);
  // nbVirtualMethods
  ClassElts.push_back(nbVirtualMethods);
  
  // staticMethods
  // Output null, this will be set in  the initializer.
  ClassElts.push_back(Constant::getNullValue(JavaMethodType));

  // nbStaticMethods
  ClassElts.push_back(ConstantInt::get(Type::Int16Ty, cl->nbStaticMethods));

  // ownerClass
  ClassElts.push_back(Constant::getNullValue(ptrType));
  
  // bytes
  ClassElts.push_back(Constant::getNullValue(JavaArrayUInt8Type));

  // ctpInfo
  ClassElts.push_back(Constant::getNullValue(ptrType));

  // attributs
  if (cl->nbAttributs) {
    ATy = ArrayType::get(AttributType->getContainedType(0),
                         cl->nbAttributs);

    for (uint32 i = 0; i < cl->nbAttributs; ++i) {
      TempElts.push_back(CreateConstantFromAttribut(cl->attributs[i]));
    }

    Constant* attributs = ConstantArray::get(ATy, TempElts);
    TempElts.clear();
    attributs = new GlobalVariable(ATy, true, GlobalValue::InternalLinkage,
                                   attributs, "", this);
    attributs = ConstantExpr::getCast(Instruction::BitCast, attributs,
                                      AttributType);
    ClassElts.push_back(attributs);
  } else {
    ClassElts.push_back(Constant::getNullValue(AttributType));
  }
  
  // nbAttributs
  ClassElts.push_back(ConstantInt::get(Type::Int16Ty, cl->nbAttributs));
  
  // innerClasses
  if (cl->nbInnerClasses) {
    for (uint32 i = 0; i < cl->nbInnerClasses; ++i) {
      TempElts.push_back(getNativeClass(cl->innerClasses[i]));
    }

    ATy = ArrayType::get(JavaClassType, cl->nbInnerClasses);
    Constant* innerClasses = ConstantArray::get(ATy, TempElts);
    innerClasses = new GlobalVariable(ATy, true, GlobalValue::InternalLinkage,
                                      innerClasses, "", this);
    innerClasses = ConstantExpr::getCast(Instruction::BitCast, innerClasses,
                                         PointerType::getUnqual(JavaClassType));

    ClassElts.push_back(innerClasses);
  } else {
    const Type* Ty = PointerType::getUnqual(JavaClassType);
    ClassElts.push_back(Constant::getNullValue(Ty));
  }

  // nbInnerClasses
  ClassElts.push_back(ConstantInt::get(Type::Int16Ty, cl->nbInnerClasses));

  // outerClass
  if (cl->outerClass) {
    ClassElts.push_back(getNativeClass(cl->outerClass));
  } else {
    ClassElts.push_back(Constant::getNullValue(JavaClassType));
  }

  // innerAccess
  ClassElts.push_back(ConstantInt::get(Type::Int16Ty, cl->innerAccess));
  
  // innerOuterResolved
  ClassElts.push_back(ConstantInt::get(Type::Int8Ty, cl->innerOuterResolved));
  
  // virtualTableSize
  ClassElts.push_back(ConstantInt::get(Type::Int32Ty, cl->virtualTableSize));
  
  // staticSize
  ClassElts.push_back(ConstantInt::get(Type::Int32Ty, cl->staticSize));

  // JInfo
  ClassElts.push_back(Constant::getNullValue(ptrType));

  // staticTracer
  const Type* FTy = STy->getContainedType(STy->getNumContainedTypes() - 1);
#ifdef WITH_TRACER
  Function* F = makeTracer(cl, true);
  Constant* staticTracer = ConstantExpr::getCast(Instruction::BitCast, F, FTy);
#else
  Constant* staticTracer = ConstantExpr::getNullValue(FTy);
#endif
  ClassElts.push_back(staticTracer);


  return ConstantStruct::get(STy, ClassElts);
}

template<typename T>
Constant* JnjvmModule::CreateConstantFromArray(T* val, const llvm::Type* Ty) {
  std::vector<const Type*> Elemts;
  const ArrayType* ATy = ArrayType::get(Ty, val->size);
  Elemts.push_back(JavaObjectType->getContainedType(0));
  Elemts.push_back(pointerSizeType == Type::Int32Ty ? Type::Int32Ty : 
                                                      Type::Int64Ty);

  Elemts.push_back(ATy);

  const StructType* STy = StructType::get(Elemts);
  
  std::vector<Constant*> Cts;
  Cts.push_back(CreateConstantForBaseObject(val->getClass()));
  Cts.push_back(ConstantInt::get(pointerSizeType, val->size));
  
  std::vector<Constant*> Vals;
  for (sint32 i = 0; i < val->size; ++i) {
    if (Ty->isInteger()) {
      Vals.push_back(ConstantInt::get(Ty, (uint64)val->elements[i]));
    } else if (Ty->isFloatingPoint()) {
      Vals.push_back(ConstantFP::get(Ty, (double)(size_t)val->elements[i]));
    } else {
      Vals.push_back(getFinalObject((JavaObject*)(size_t)val->elements[i]));
    }
  }

  Cts.push_back(ConstantArray::get(ATy, Vals));
  
  return ConstantStruct::get(STy, Cts);
}

Constant* JnjvmModule::CreateConstantFromUTF8(const UTF8* val) {
  std::vector<const Type*> Elemts;
  const ArrayType* ATy = ArrayType::get(Type::Int16Ty, val->size);
  Elemts.push_back(JavaObjectType->getContainedType(0));
  Elemts.push_back(pointerSizeType == Type::Int32Ty ? Type::Int32Ty : 
                                                      Type::Int64Ty);

  Elemts.push_back(ATy);

  const StructType* STy = StructType::get(Elemts);
  
  std::vector<Constant*> Cts;
  Cts.push_back(CreateConstantForBaseObject(&ArrayOfChar));
  Cts.push_back(ConstantInt::get(pointerSizeType, val->size));
  
  std::vector<Constant*> Vals;
  for (sint32 i = 0; i < val->size; ++i) {
    Vals.push_back(ConstantInt::get(Type::Int16Ty, val->elements[i]));
  }

  Cts.push_back(ConstantArray::get(ATy, Vals));
  
  return ConstantStruct::get(STy, Cts);

}

Constant* JnjvmModule::getUTF8(const UTF8* val) {
  utf8_iterator End = utf8s.end();
  utf8_iterator I = utf8s.find(val);
  if (I == End) {
    Constant* C = CreateConstantFromUTF8(val);
    GlobalVariable* varGV = new GlobalVariable(C->getType(), true,
                                               GlobalValue::InternalLinkage,
                                               C, "", this);
    
    Constant* res = ConstantExpr::getCast(Instruction::BitCast, varGV,
                                          UTF8Type);
    utf8s.insert(std::make_pair(val, res));

    return res;
  } else {
    return I->second;
  }
}

Constant* JnjvmModule::CreateConstantFromVT(Class* classDef) {
  uint32 size = classDef->virtualTableSize;
  VirtualTable* VT = classDef->virtualVT;
  const ArrayType* ATy = dyn_cast<ArrayType>(VTType->getContainedType(0));
  const PointerType* PTy = dyn_cast<PointerType>(ATy->getContainedType(0));
  ATy = ArrayType::get(PTy, size);

  ConstantPointerNull* N = ConstantPointerNull::get(PTy);
  std::vector<Constant*> Elemts;
   
  // Destructor
  JavaMethod* meth = ((JavaMethod**)VT)[0];
  LLVMMethodInfo* LMI = getMethodInfo(meth);
  Function* Finalizer = LMI->getMethod();
  Elemts.push_back(Finalizer ? 
      ConstantExpr::getCast(Instruction::BitCast, Finalizer, PTy) : N);
  
  // Delete
  Elemts.push_back(N);
  
  // Tracer
#ifdef WITH_TRACER
  Function* Tracer = makeTracer(classDef, false);
  Elemts.push_back(Tracer ? 
      ConstantExpr::getCast(Instruction::BitCast, Tracer, PTy) : N);
#else
  Elemts.push_back(N);
#endif

  // Printer
  Elemts.push_back(ConstantExpr::getBitCast(ObjectPrinter, PTy));
  
  // Hashcode
  Elemts.push_back(N);  

  for (uint32 i = VT_NB_FUNCS; i < size; ++i) {
    JavaMethod* meth = ((JavaMethod**)VT)[i];
    LLVMMethodInfo* LMI = getMethodInfo(meth);
    Function* F = LMI->getMethod();
    if (isAbstract(meth->access)) {
      Elemts.push_back(Constant::getNullValue(PTy));
    } else {
      Elemts.push_back(ConstantExpr::getCast(Instruction::BitCast, F, PTy));
    }
  }

  Constant* Array = ConstantArray::get(ATy, Elemts);
  
  return Array;
}

void JnjvmModule::makeVT(Class* cl) {
  
  VirtualTable* VT = 0;
#ifdef WITHOUT_VTABLE
  mvm::BumpPtrAllocator& allocator = cl->classLoader->allocator;
  VT = (VirtualTable*)allocator.Allocate(VT_SIZE);
  memcpy(VT, JavaObjectVT, VT_SIZE);
  cl->virtualVT = VT;
#else
  if (cl->super) {
    if (isStaticCompiling() && !cl->super->virtualVT) {
      makeVT(cl->super);
    }

    cl->virtualTableSize = cl->super->virtualTableSize;
  } else {
    cl->virtualTableSize = VT_NB_FUNCS;
  }

  // Allocate the virtual table.
  allocateVT(cl);
  VT = cl->virtualVT;

  if (!staticCompilation) {
    // Fill the virtual table with function pointers.
    ExecutionEngine* EE = mvm::MvmModule::executionEngine;
    for (uint32 i = 0; i < cl->nbVirtualMethods; ++i) {
      JavaMethod& meth = cl->virtualMethods[i];
      LLVMMethodInfo* LMI = getMethodInfo(&meth);
      Function* func = LMI->getMethod();

      // Special handling for finalize method. Don't put a finalizer
      // if there is none, or if it is empty.
      if (meth.offset == 0) {
#if defined(ISOLATE_SHARING) || defined(USE_GC_BOEHM)
        ((void**)VT)[0] = 0;
#else
        JnjvmClassLoader* loader = cl->classLoader;
        Function* func = loader->getModuleProvider()->parseFunction(&meth);
        if (!cl->super) {
          meth.canBeInlined = true;
          ((void**)VT)[0] = 0;
        } else {
          Function::iterator BB = func->begin();
          BasicBlock::iterator I = BB->begin();
          if (isa<ReturnInst>(I)) {
            ((void**)VT)[0] = 0;
          } else {
            // LLVM does not allow recursive compilation. Create the code now.
            ((void**)VT)[0] = EE->getPointerToFunction(func);
          }
        }
#endif
      } else {
        ((void**)VT)[meth.offset] = EE->getPointerToFunctionOrStub(func);
      }
    }

#ifdef WITH_TRACER
    Function* func = makeTracer(cl, false);
  
    void* codePtr = mvm::MvmModule::executionEngine->getPointerToFunction(func);
    ((void**)VT)[VT_TRACER_OFFSET] = codePtr;
    func->deleteBody();
#endif
    
    // If there is no super, then it's the first VT that we allocate. Assign
    // this VT to native types.
    if (!(cl->super)) {
      ClassArray::initialiseVT(cl);
    }

  } else {
    for (uint32 i = 0; i < cl->nbVirtualMethods; ++i) {
      JavaMethod& meth = cl->virtualMethods[i];
      ((void**)VT)[meth.offset] = &meth;
    }
  }
 
#endif
  
}


const Type* LLVMClassInfo::getVirtualType() {
  if (!virtualType) {
    std::vector<const llvm::Type*> fields;
    
    if (classDef->super && classDef->super->super) {
      LLVMClassInfo* CLI = 
        JnjvmModule::getClassInfo((Class*)classDef->super);
      fields.push_back(CLI->getVirtualType()->getContainedType(0));
    } else {
      fields.push_back(JnjvmModule::JavaObjectType->getContainedType(0));
    }
    
    for (uint32 i = 0; i < classDef->nbVirtualFields; ++i) {
      JavaField& field = classDef->virtualFields[i];
      field.num = i + 1;
      Typedef* type = field.getSignature();
      LLVMAssessorInfo& LAI = JnjvmModule::getTypedefInfo(type);
      fields.push_back(LAI.llvmType);
    }
    
    
    JnjvmModule* Mod = classDef->classLoader->getModule();
    StructType* structType = StructType::get(fields, false);
    virtualType = PointerType::getUnqual(structType);
    const TargetData* targetData = Mod->TheTargetData;
    const StructLayout* sl = targetData->getStructLayout(structType);
    
    for (uint32 i = 0; i < classDef->nbVirtualFields; ++i) {
      JavaField& field = classDef->virtualFields[i];
      field.ptrOffset = sl->getElementOffset(i + 1);
    }
    
    uint64 size = Mod->getTypeSize(structType);
    classDef->virtualSize = (uint32)size;
    virtualSizeConstant = ConstantInt::get(Type::Int32Ty, size);
    
    if (!Mod->isStaticCompiling()) {
      if (!classDef->virtualVT) {
        Mod->makeVT((Class*)classDef);
      } else {
#ifdef WITH_TRACER
        // So the class is vmjc'ed. Create the virtual tracer.
        Function* func = Function::Create(JnjvmModule::MarkAndTraceType,
                                          GlobalValue::ExternalLinkage,
                                          "markAndTraceObject", Mod);
       
        void* ptr = ((void**)classDef->virtualVT)[VT_TRACER_OFFSET];
        Mod->executionEngine->addGlobalMapping(func, ptr);
        virtualTracerFunction = func;
#endif
      }
    } else {
      Mod->makeVT(classDef);
    }
  
  }

  return virtualType;
}

const Type* LLVMClassInfo::getStaticType() {
  
  if (!staticType) {
    Class* cl = (Class*)classDef;
    std::vector<const llvm::Type*> fields;

    for (uint32 i = 0; i < classDef->nbStaticFields; ++i) {
      JavaField& field = classDef->staticFields[i];
      field.num = i;
      Typedef* type = field.getSignature();
      LLVMAssessorInfo& LAI = JnjvmModule::getTypedefInfo(type);
      fields.push_back(LAI.llvmType);
    }
  
    JnjvmModule* Mod = cl->classLoader->getModule();
    StructType* structType = StructType::get(fields, false);
    staticType = PointerType::getUnqual(structType);
    const TargetData* targetData = Mod->TheTargetData;
    const StructLayout* sl = targetData->getStructLayout(structType);
    
    for (uint32 i = 0; i < classDef->nbStaticFields; ++i) {
      JavaField& field = classDef->staticFields[i];
      field.ptrOffset = sl->getElementOffset(i);
    }
    
    uint64 size = Mod->getTypeSize(structType);
    cl->staticSize = size;
#ifdef WITH_TRACER
    if (!Mod->isStaticCompiling()) {
      Function* F = Mod->makeTracer(cl, true);
      cl->staticTracer = (void (*)(void*)) (uintptr_t)
        Mod->executionEngine->getPointerToFunction(F);
      F->deleteBody();
    }
#endif
  }
  return staticType;
}


Value* LLVMClassInfo::getVirtualSize() {
  if (!virtualSizeConstant) {
    getVirtualType();
    assert(classDef->virtualSize && "Zero size for a class?");
    virtualSizeConstant = 
      ConstantInt::get(Type::Int32Ty, classDef->virtualSize);
  }
  return virtualSizeConstant;
}

Function* LLVMClassInfo::getStaticTracer() {
  if (!staticTracerFunction) {
    getStaticType();
  }
  return staticTracerFunction;
}

Function* LLVMClassInfo::getVirtualTracer() {
  if (!virtualTracerFunction) {
    getVirtualType();
  }
  return virtualTracerFunction;
}

Function* LLVMMethodInfo::getMethod() {
  if (!methodFunction) {
    JnjvmClassLoader* JCL = methodDef->classDef->classLoader;
    JnjvmModule* Mod = JCL->getModule();
    if (Mod->isStaticCompiling()) {

      const UTF8* jniConsClName = methodDef->classDef->name;
      const UTF8* jniConsName = methodDef->name;
      const UTF8* jniConsType = methodDef->type;
      sint32 clen = jniConsClName->size;
      sint32 mnlen = jniConsName->size;
      sint32 mtlen = jniConsType->size;

      char* buf = (char*)alloca(3 + JNI_NAME_PRE_LEN + 1 +
                                ((mnlen + clen + mtlen) << 1));
      
      bool jnjvm = false;
      if (isNative(methodDef->access)) {
        // Verify if it's defined by JnJVM
        JCL->nativeLookup(methodDef, jnjvm, buf);
      }

      if (!jnjvm) {
        methodDef->jniConsFromMethOverloaded(buf + 1);
        memcpy(buf, "JnJVM", 5);
      }

      methodFunction = Function::Create(getFunctionType(), 
                                        GlobalValue::GhostLinkage, buf, Mod);

    } else {

      methodFunction = Function::Create(getFunctionType(), 
                                        GlobalValue::GhostLinkage,
                                        "", Mod);

    }
    methodFunction->addAnnotation(this);
  }
  return methodFunction;
}

const FunctionType* LLVMMethodInfo::getFunctionType() {
  if (!functionType) {
    Signdef* sign = methodDef->getSignature();
    LLVMSignatureInfo* LSI = JnjvmModule::getSignatureInfo(sign);
    assert(LSI);
    if (isStatic(methodDef->access)) {
      functionType = LSI->getStaticType();
    } else {
      functionType = LSI->getVirtualType();
    }
  }
  return functionType;
}

ConstantInt* LLVMMethodInfo::getOffset() {
  if (!offsetConstant) {
    JnjvmModule::resolveVirtualClass(methodDef->classDef);
    offsetConstant = ConstantInt::get(Type::Int32Ty, methodDef->offset);
  }
  return offsetConstant;
}

ConstantInt* LLVMFieldInfo::getOffset() {
  if (!offsetConstant) {
    if (isStatic(fieldDef->access)) {
      JnjvmModule::resolveStaticClass(fieldDef->classDef); 
    } else {
      JnjvmModule::resolveVirtualClass(fieldDef->classDef); 
    }
    
    offsetConstant = ConstantInt::get(Type::Int32Ty, fieldDef->num);
  }
  return offsetConstant;
}

const llvm::FunctionType* LLVMSignatureInfo::getVirtualType() {
 if (!virtualType) {
    // Lock here because we are called by arbitrary code
    mvm::MvmModule::protectIR();
    std::vector<const llvm::Type*> llvmArgs;
    uint32 size = signature->nbArguments;
    Typedef* const* arguments = signature->getArgumentsType();

    llvmArgs.push_back(JnjvmModule::JavaObjectType);

    for (uint32 i = 0; i < size; ++i) {
      Typedef* type = arguments[i];
      LLVMAssessorInfo& LAI = JnjvmModule::getTypedefInfo(type);
      llvmArgs.push_back(LAI.llvmType);
    }

#if defined(ISOLATE_SHARING)
    llvmArgs.push_back(JnjvmModule::ConstantPoolType); // cached constant pool
#endif

    LLVMAssessorInfo& LAI = 
      JnjvmModule::getTypedefInfo(signature->getReturnType());
    virtualType = FunctionType::get(LAI.llvmType, llvmArgs, false);
    mvm::MvmModule::unprotectIR();
  }
  return virtualType;
}

const llvm::FunctionType* LLVMSignatureInfo::getStaticType() {
 if (!staticType) {
    // Lock here because we are called by arbitrary code
    mvm::MvmModule::protectIR();
    std::vector<const llvm::Type*> llvmArgs;
    uint32 size = signature->nbArguments;
    Typedef* const* arguments = signature->getArgumentsType();

    for (uint32 i = 0; i < size; ++i) {
      Typedef* type = arguments[i];
      LLVMAssessorInfo& LAI = JnjvmModule::getTypedefInfo(type);
      llvmArgs.push_back(LAI.llvmType);
    }

#if defined(ISOLATE_SHARING)
    llvmArgs.push_back(JnjvmModule::ConstantPoolType); // cached constant pool
#endif

    LLVMAssessorInfo& LAI = 
      JnjvmModule::getTypedefInfo(signature->getReturnType());
    staticType = FunctionType::get(LAI.llvmType, llvmArgs, false);
    mvm::MvmModule::unprotectIR();
  }
  return staticType;
}

const llvm::FunctionType* LLVMSignatureInfo::getNativeType() {
  if (!nativeType) {
    // Lock here because we are called by arbitrary code
    mvm::MvmModule::protectIR();
    std::vector<const llvm::Type*> llvmArgs;
    uint32 size = signature->nbArguments;
    Typedef* const* arguments = signature->getArgumentsType();
    
    llvmArgs.push_back(mvm::MvmModule::ptrType); // JNIEnv
    llvmArgs.push_back(JnjvmModule::JavaObjectType); // Class

    for (uint32 i = 0; i < size; ++i) {
      Typedef* type = arguments[i];
      LLVMAssessorInfo& LAI = JnjvmModule::getTypedefInfo(type);
      llvmArgs.push_back(LAI.llvmType);
    }

#if defined(ISOLATE_SHARING)
    llvmArgs.push_back(JnjvmModule::ConstantPoolType); // cached constant pool
#endif

    LLVMAssessorInfo& LAI = 
      JnjvmModule::getTypedefInfo(signature->getReturnType());
    nativeType = FunctionType::get(LAI.llvmType, llvmArgs, false);
    mvm::MvmModule::unprotectIR();
  }
  return nativeType;
}


Function* LLVMSignatureInfo::createFunctionCallBuf(bool virt) {
  
  std::vector<Value*> Args;

  JnjvmModule* Mod = signature->initialLoader->getModule();
  const char* name = 0;
  if (Mod->isStaticCompiling()) {
    name = virt ? signature->printString("virtual_buf") :
                  signature->printString("static_buf");
  } else {
    name = "";
  }

  Function* res = Function::Create(virt ? getVirtualBufType() : 
                                          getStaticBufType(),
                                   GlobalValue::InternalLinkage, name, Mod);
  
  BasicBlock* currentBlock = BasicBlock::Create("enter", res);
  Function::arg_iterator i = res->arg_begin();
  Value *obj, *ptr, *func;
#if defined(ISOLATE_SHARING)
  Value* ctp = i;
#endif
  ++i;
  func = i;
  ++i;
  if (virt) {
    obj = i;
    ++i;
    Args.push_back(obj);
  }
  ptr = i;
  
  Typedef* const* arguments = signature->getArgumentsType();
  for (uint32 i = 0; i < signature->nbArguments; ++i) {
  
    LLVMAssessorInfo& LAI = JnjvmModule::getTypedefInfo(arguments[i]);
    Value* val = new BitCastInst(ptr, LAI.llvmTypePtr, "", currentBlock);
    Value* arg = new LoadInst(val, "", currentBlock);
    Args.push_back(arg);
    ptr = GetElementPtrInst::Create(ptr, JnjvmModule::constantEight, "",
                                    currentBlock);
  }

#if defined(ISOLATE_SHARING)
  Args.push_back(ctp);
#endif

  Value* val = CallInst::Create(func, Args.begin(), Args.end(), "",
                                currentBlock);
  if (res->getFunctionType()->getReturnType() != Type::VoidTy)
    ReturnInst::Create(val, currentBlock);
  else
    ReturnInst::Create(currentBlock);
  
  return res;
}

Function* LLVMSignatureInfo::createFunctionCallAP(bool virt) {
  
  std::vector<Value*> Args;
  
  JnjvmModule* Mod = signature->initialLoader->getModule();
  const char* name = 0;
  if (Mod->isStaticCompiling()) {
    name = virt ? signature->printString("virtual_ap") :
                  signature->printString("static_ap");
  } else {
    name = "";
  }

  Function* res = Function::Create(virt ? getVirtualBufType() :
                                          getStaticBufType(),
                                   GlobalValue::InternalLinkage, name, Mod);
  
  BasicBlock* currentBlock = BasicBlock::Create("enter", res);
  Function::arg_iterator i = res->arg_begin();
  Value *obj, *ap, *func;
#if defined(ISOLATE_SHARING)
  Value* ctp = i;
#endif
  ++i;
  func = i;
  ++i;
  if (virt) {
    obj = i;
    Args.push_back(obj);
    ++i;
  }
  ap = i;

  Typedef* const* arguments = signature->getArgumentsType();
  for (uint32 i = 0; i < signature->nbArguments; ++i) {
    LLVMAssessorInfo& LAI = JnjvmModule::getTypedefInfo(arguments[i]);
    Args.push_back(new VAArgInst(ap, LAI.llvmType, "", currentBlock));
  }

#if defined(ISOLATE_SHARING)
  Args.push_back(ctp);
#endif

  Value* val = CallInst::Create(func, Args.begin(), Args.end(), "",
                                currentBlock);
  if (res->getFunctionType()->getReturnType() != Type::VoidTy)
    ReturnInst::Create(val, currentBlock);
  else
    ReturnInst::Create(currentBlock);
  
  return res;
}

const PointerType* LLVMSignatureInfo::getStaticPtrType() {
  if (!staticPtrType) {
    staticPtrType = PointerType::getUnqual(getStaticType());
  }
  return staticPtrType;
}

const PointerType* LLVMSignatureInfo::getVirtualPtrType() {
  if (!virtualPtrType) {
    virtualPtrType = PointerType::getUnqual(getVirtualType());
  }
  return virtualPtrType;
}

const PointerType* LLVMSignatureInfo::getNativePtrType() {
  if (!nativePtrType) {
    nativePtrType = PointerType::getUnqual(getNativeType());
  }
  return nativePtrType;
}


const FunctionType* LLVMSignatureInfo::getVirtualBufType() {
  if (!virtualBufType) {
    // Lock here because we are called by arbitrary code
    mvm::MvmModule::protectIR();
    std::vector<const llvm::Type*> Args2;
    Args2.push_back(JnjvmModule::ConstantPoolType); // ctp
    Args2.push_back(getVirtualPtrType());
    Args2.push_back(JnjvmModule::JavaObjectType);
    Args2.push_back(JnjvmModule::ptrType);
    LLVMAssessorInfo& LAI = 
      JnjvmModule::getTypedefInfo(signature->getReturnType());
    virtualBufType = FunctionType::get(LAI.llvmType, Args2, false);
    mvm::MvmModule::unprotectIR();
  }
  return virtualBufType;
}

const FunctionType* LLVMSignatureInfo::getStaticBufType() {
  if (!staticBufType) {
    // Lock here because we are called by arbitrary code
    mvm::MvmModule::protectIR();
    std::vector<const llvm::Type*> Args;
    Args.push_back(JnjvmModule::ConstantPoolType); // ctp
    Args.push_back(getStaticPtrType());
    Args.push_back(JnjvmModule::ptrType);
    LLVMAssessorInfo& LAI = 
      JnjvmModule::getTypedefInfo(signature->getReturnType());
    staticBufType = FunctionType::get(LAI.llvmType, Args, false);
    mvm::MvmModule::unprotectIR();
  }
  return staticBufType;
}

Function* LLVMSignatureInfo::getVirtualBuf() {
  if (!virtualBufFunction) {
    // Lock here because we are called by arbitrary code
    mvm::MvmModule::protectIR();
    virtualBufFunction = createFunctionCallBuf(true);
    if (!signature->initialLoader->getModule()->isStaticCompiling()) {
      signature->setVirtualCallBuf((intptr_t)
        mvm::MvmModule::executionEngine->getPointerToGlobal(virtualBufFunction));
      // Now that it's compiled, we don't need the IR anymore
      virtualBufFunction->deleteBody();
    }
    mvm::MvmModule::unprotectIR();
  }
  return virtualBufFunction;
}

Function* LLVMSignatureInfo::getVirtualAP() {
  if (!virtualAPFunction) {
    // Lock here because we are called by arbitrary code
    mvm::MvmModule::protectIR();
    virtualAPFunction = createFunctionCallAP(true);
    if (!signature->initialLoader->getModule()->isStaticCompiling()) {
      signature->setVirtualCallAP((intptr_t)
        mvm::MvmModule::executionEngine->getPointerToGlobal(virtualAPFunction));
      // Now that it's compiled, we don't need the IR anymore
      virtualAPFunction->deleteBody();
    }
    mvm::MvmModule::unprotectIR();
  }
  return virtualAPFunction;
}

Function* LLVMSignatureInfo::getStaticBuf() {
  if (!staticBufFunction) {
    // Lock here because we are called by arbitrary code
    mvm::MvmModule::protectIR();
    staticBufFunction = createFunctionCallBuf(false);
    if (!signature->initialLoader->getModule()->isStaticCompiling()) {
      signature->setStaticCallBuf((intptr_t)
        mvm::MvmModule::executionEngine->getPointerToGlobal(staticBufFunction));
      // Now that it's compiled, we don't need the IR anymore
      staticBufFunction->deleteBody();
    }
    mvm::MvmModule::unprotectIR();
  }
  return staticBufFunction;
}

Function* LLVMSignatureInfo::getStaticAP() {
  if (!staticAPFunction) {
    // Lock here because we are called by arbitrary code
    mvm::MvmModule::protectIR();
    staticAPFunction = createFunctionCallAP(false);
    if (!signature->initialLoader->getModule()->isStaticCompiling()) {
      signature->setStaticCallAP((intptr_t)
        mvm::MvmModule::executionEngine->getPointerToGlobal(staticAPFunction));
      // Now that it's compiled, we don't need the IR anymore
      staticAPFunction->deleteBody();
    }
    mvm::MvmModule::unprotectIR();
  }
  return staticAPFunction;
}

void JnjvmModule::resolveVirtualClass(Class* cl) {
  // Lock here because we may be called by a class resolver
  mvm::MvmModule::protectIR();
  LLVMClassInfo* LCI = (LLVMClassInfo*)getClassInfo(cl);
  LCI->getVirtualType();
  mvm::MvmModule::unprotectIR();
}

void JnjvmModule::resolveStaticClass(Class* cl) {
  // Lock here because we may be called by a class initializer
  mvm::MvmModule::protectIR();
  LLVMClassInfo* LCI = (LLVMClassInfo*)getClassInfo(cl);
  LCI->getStaticType();
  mvm::MvmModule::unprotectIR();
}


namespace jnjvm { 
  namespace llvm_runtime { 
    #include "LLVMRuntime.inc"
  }
}

Module* JnjvmModule::initialModule;

void JnjvmModule::initialise() {
  jnjvm::llvm_runtime::makeLLVMModuleContents(this);
  Module* module = this;
  initialModule = this;

  VTType = PointerType::getUnqual(module->getTypeByName("VT"));

#ifdef ISOLATE_SHARING
  JnjvmType = 
    PointerType::getUnqual(module->getTypeByName("Jnjvm"));
#endif
  ConstantPoolType = ptrPtrType;
  
  JavaObjectType = 
    PointerType::getUnqual(module->getTypeByName("JavaObject"));

  JavaArrayType =
    PointerType::getUnqual(module->getTypeByName("JavaArray"));
  
  JavaCommonClassType =
    PointerType::getUnqual(module->getTypeByName("JavaCommonClass"));
  JavaClassPrimitiveType =
    PointerType::getUnqual(module->getTypeByName("JavaClassPrimitive"));
  JavaClassArrayType =
    PointerType::getUnqual(module->getTypeByName("JavaClassArray"));
  JavaClassType =
    PointerType::getUnqual(module->getTypeByName("JavaClass"));
  
  JavaArrayUInt8Type =
    PointerType::getUnqual(module->getTypeByName("ArrayUInt8"));
  JavaArraySInt8Type =
    PointerType::getUnqual(module->getTypeByName("ArraySInt8"));
  JavaArrayUInt16Type =
    PointerType::getUnqual(module->getTypeByName("ArrayUInt16"));
  JavaArraySInt16Type =
    PointerType::getUnqual(module->getTypeByName("ArraySInt16"));
  JavaArrayUInt32Type =
    PointerType::getUnqual(module->getTypeByName("ArrayUInt32"));
  JavaArraySInt32Type =
    PointerType::getUnqual(module->getTypeByName("ArraySInt32"));
  JavaArrayLongType =
    PointerType::getUnqual(module->getTypeByName("ArrayLong"));
  JavaArrayFloatType =
    PointerType::getUnqual(module->getTypeByName("ArrayFloat"));
  JavaArrayDoubleType =
    PointerType::getUnqual(module->getTypeByName("ArrayDouble"));
  JavaArrayObjectType =
    PointerType::getUnqual(module->getTypeByName("ArrayObject"));

  CacheNodeType =
    PointerType::getUnqual(module->getTypeByName("CacheNode"));
  
  EnveloppeType =
    PointerType::getUnqual(module->getTypeByName("Enveloppe"));
  
  JavaFieldType =
    PointerType::getUnqual(module->getTypeByName("JavaField"));
  JavaMethodType =
    PointerType::getUnqual(module->getTypeByName("JavaMethod"));
  UTF8Type =
    PointerType::getUnqual(module->getTypeByName("UTF8"));
  AttributType =
    PointerType::getUnqual(module->getTypeByName("Attribut"));
  JavaThreadType =
    PointerType::getUnqual(module->getTypeByName("JavaThread"));

#ifdef WITH_TRACER
  MarkAndTraceType = module->getFunction("MarkAndTrace")->getFunctionType();
#endif
 
  JavaObjectNullConstant = Constant::getNullValue(JnjvmModule::JavaObjectType);
  MaxArraySizeConstant = ConstantInt::get(Type::Int32Ty,
                                          JavaArray::MaxArraySize);
  JavaArraySizeConstant = 
    ConstantInt::get(Type::Int32Ty, sizeof(JavaObject) + sizeof(ssize_t));
  
  
  JavaArrayElementsOffsetConstant = mvm::MvmModule::constantTwo;
  JavaArraySizeOffsetConstant = mvm::MvmModule::constantOne;
  JavaObjectLockOffsetConstant = mvm::MvmModule::constantTwo;
  JavaObjectClassOffsetConstant = mvm::MvmModule::constantOne; 
  
  OffsetDisplayInClassConstant = mvm::MvmModule::constantZero;
  OffsetDepthInClassConstant = mvm::MvmModule::constantOne;
  
  OffsetObjectSizeInClassConstant = mvm::MvmModule::constantOne;
  OffsetVTInClassConstant = mvm::MvmModule::constantTwo;
  OffsetTaskClassMirrorInClassConstant = mvm::MvmModule::constantThree;
  OffsetStaticInstanceInTaskClassMirrorConstant = mvm::MvmModule::constantTwo;
  OffsetStatusInTaskClassMirrorConstant = mvm::MvmModule::constantZero;
  OffsetInitializedInTaskClassMirrorConstant = mvm::MvmModule::constantOne;
  
  OffsetJavaExceptionInThreadConstant = ConstantInt::get(Type::Int32Ty, 9);
  OffsetCXXExceptionInThreadConstant = ConstantInt::get(Type::Int32Ty, 10);
  
  ClassReadyConstant = ConstantInt::get(Type::Int8Ty, ready);
 

  if (staticCompilation) {
    const Type* ATy = VTType->getContainedType(0);
    PrimitiveArrayVT = new GlobalVariable(ATy, true,
                                          GlobalValue::ExternalLinkage,
                                          0, "JavaArrayVT", this);
  
    ReferenceArrayVT = new GlobalVariable(ATy, true, 
                                          GlobalValue::ExternalLinkage,
                                          0, "ArrayObjectVT", this);



    ATy = JavaClassArrayType->getContainedType(0);
    GlobalVariable* varGV = 0;
    
#define PRIMITIVE_ARRAY(name) \
    varGV = new GlobalVariable(ATy, true, GlobalValue::ExternalLinkage, \
                               0, #name, this); \
    arrayClasses.insert(std::make_pair(&name, varGV));
    
    PRIMITIVE_ARRAY(ArrayOfBool)
    PRIMITIVE_ARRAY(ArrayOfByte)
    PRIMITIVE_ARRAY(ArrayOfChar)
    PRIMITIVE_ARRAY(ArrayOfShort)
    PRIMITIVE_ARRAY(ArrayOfInt)
    PRIMITIVE_ARRAY(ArrayOfFloat)
    PRIMITIVE_ARRAY(ArrayOfDouble)
    PRIMITIVE_ARRAY(ArrayOfLong)

#undef PRIMITIVE_ARRAY

    std::vector<const llvm::Type*> llvmArgs;
    llvmArgs.push_back(ptrType); // class loader.
    const FunctionType* FTy = FunctionType::get(Type::VoidTy, llvmArgs, false);
  
    StaticInitializer = Function::Create(FTy, GlobalValue::InternalLinkage,
                                         "Init", this);

    llvmArgs.clear();
    llvmArgs.push_back(JavaMethodType);
    
    FTy = FunctionType::get(ptrType, llvmArgs, false);
  
    NativeLoader = Function::Create(FTy, GlobalValue::ExternalLinkage,
                                    "vmjcNativeLoader", this);
    
    llvmArgs.clear();
    FTy = FunctionType::get(Type::VoidTy, llvmArgs, false);
    ObjectPrinter = Function::Create(FTy, GlobalValue::ExternalLinkage,
                                     "printJavaObject", this);

  } else {
    PrimitiveArrayVT = ConstantExpr::getIntToPtr(ConstantInt::get(Type::Int64Ty,
                                                         uint64(JavaArrayVT)),
                                                 VTType);
  
    ReferenceArrayVT = ConstantExpr::getIntToPtr(ConstantInt::get(Type::Int64Ty,
                                                       uint64(ArrayObjectVT)),
                                                 VTType);
  }

  LLVMAssessorInfo::initialise();
}

Constant* JnjvmModule::getReferenceArrayVT() {
  return ReferenceArrayVT;
}

Constant* JnjvmModule::getPrimitiveArrayVT() {
  return PrimitiveArrayVT;
}

void JnjvmModule::setMethod(JavaMethod* meth, void* ptr, const char* name) {
  Function* func = getMethodInfo(meth)->getMethod();
  func->setName(name);
  assert(ptr && "No value given");
  if (executionEngine) executionEngine->addGlobalMapping(func, ptr);
  func->setLinkage(GlobalValue::ExternalLinkage);
}

void JnjvmModule::printStats() {
  fprintf(stderr, "----------------- Info from the module -----------------\n");
  fprintf(stderr, "Number of native classes            : %llu\n", 
          (unsigned long long int) nativeClasses.size());
  fprintf(stderr, "Number of Java classes              : %llu\n",
          (unsigned long long int) javaClasses.size());
  fprintf(stderr, "Number of external array classes    : %llu\n",
          (unsigned long long int) arrayClasses.size());
  fprintf(stderr, "Number of virtual tables            : %llu\n", 
          (unsigned long long int) virtualTables.size());
  fprintf(stderr, "Number of static instances          : %llu\n", 
          (unsigned long long int) staticInstances.size());
  fprintf(stderr, "Number of constant pools            : %llu\n", 
          (unsigned long long int) constantPools.size());
  fprintf(stderr, "Number of strings                   : %llu\n", 
          (unsigned long long int) strings.size());
  fprintf(stderr, "Number of enveloppes                : %llu\n", 
          (unsigned long long int) enveloppes.size());
  fprintf(stderr, "Number of native functions          : %llu\n", 
          (unsigned long long int) nativeFunctions.size());
  fprintf(stderr, "----------------- Total size in .data ------------------\n");
  uint64 size = 0;
  for (Module::const_global_iterator i = global_begin(), e = global_end();
       i != e; ++i) {
    size += getTypeSize(i->getType());
  }
  fprintf(stderr, "%lluB\n", (unsigned long long int)size);
}


Function* JnjvmModule::getMethod(JavaMethod* meth) {
  return getMethodInfo(meth)->getMethod();
}

JnjvmModule::JnjvmModule(const std::string &ModuleID, bool sc) : 
  MvmModule(ModuleID) {
  
  staticCompilation = sc;
  generateTracers = true;
  generateStubs = true;
  enabledException = true;
  assumeCompiled = false;
  if (!VTType) initialise();

  Module* module = initialModule;
   
  InterfaceLookupFunction = module->getFunction("jnjvmVirtualLookup");
  MultiCallNewFunction = module->getFunction("multiCallNew");
  ForceLoadedCheckFunction = module->getFunction("forceLoadedCheck");
  InitialisationCheckFunction = module->getFunction("initialisationCheck");
  ForceInitialisationCheckFunction = 
    module->getFunction("forceInitialisationCheck");
  InitialiseClassFunction = module->getFunction("jnjvmRuntimeInitialiseClass");
  
  GetConstantPoolAtFunction = module->getFunction("getConstantPoolAt");
  ArrayLengthFunction = module->getFunction("arrayLength");
  GetVTFunction = module->getFunction("getVT");
  GetClassFunction = module->getFunction("getClass");
  ClassLookupFunction = module->getFunction("classLookup");
  GetVTFromClassFunction = module->getFunction("getVTFromClass");
  GetObjectSizeFromClassFunction = 
    module->getFunction("getObjectSizeFromClass");
 
  GetClassDelegateeFunction = module->getFunction("getClassDelegatee");
  RuntimeDelegateeFunction = module->getFunction("jnjvmRuntimeDelegatee");
  InstanceOfFunction = module->getFunction("instanceOf");
  IsAssignableFromFunction = module->getFunction("isAssignableFrom");
  ImplementsFunction = module->getFunction("implements");
  InstantiationOfArrayFunction = module->getFunction("instantiationOfArray");
  GetDepthFunction = module->getFunction("getDepth");
  GetStaticInstanceFunction = module->getFunction("getStaticInstance");
  GetDisplayFunction = module->getFunction("getDisplay");
  GetClassInDisplayFunction = module->getFunction("getClassInDisplay");
  AquireObjectFunction = module->getFunction("JavaObjectAquire");
  ReleaseObjectFunction = module->getFunction("JavaObjectRelease");
  OverflowThinLockFunction = module->getFunction("overflowThinLock");

  VirtualFieldLookupFunction = module->getFunction("virtualFieldLookup");
  StaticFieldLookupFunction = module->getFunction("staticFieldLookup");
  
  JniProceedPendingExceptionFunction = 
    module->getFunction("jniProceedPendingException");
  GetSJLJBufferFunction = module->getFunction("getSJLJBuffer");
  
  NullPointerExceptionFunction =
    module->getFunction("jnjvmNullPointerException");
  ClassCastExceptionFunction = module->getFunction("jnjvmClassCastException");
  IndexOutOfBoundsExceptionFunction = 
    module->getFunction("indexOutOfBoundsException");
  NegativeArraySizeExceptionFunction = 
    module->getFunction("negativeArraySizeException");
  OutOfMemoryErrorFunction = module->getFunction("outOfMemoryError");

  JavaObjectAllocateFunction = module->getFunction("gcmalloc");

  PrintExecutionFunction = module->getFunction("printExecution");
  PrintMethodStartFunction = module->getFunction("printMethodStart");
  PrintMethodEndFunction = module->getFunction("printMethodEnd");

  ThrowExceptionFunction = module->getFunction("JavaThreadThrowException");

  GetArrayClassFunction = module->getFunction("getArrayClass");
 
  GetFinalInt8FieldFunction = module->getFunction("getFinalInt8Field");
  GetFinalInt16FieldFunction = module->getFunction("getFinalInt16Field");
  GetFinalInt32FieldFunction = module->getFunction("getFinalInt32Field");
  GetFinalLongFieldFunction = module->getFunction("getFinalLongField");
  GetFinalFloatFieldFunction = module->getFunction("getFinalFloatField");
  GetFinalDoubleFieldFunction = module->getFunction("getFinalDoubleField");
  GetFinalObjectFieldFunction = module->getFunction("getFinalObjectField");

#ifdef ISOLATE
  StringLookupFunction = module->getFunction("stringLookup");
#ifdef ISOLATE_SHARING
  EnveloppeLookupFunction = module->getFunction("enveloppeLookup");
  GetCtpCacheNodeFunction = module->getFunction("getCtpCacheNode");
  GetCtpClassFunction = module->getFunction("getCtpClass");
  GetJnjvmExceptionClassFunction = 
    module->getFunction("getJnjvmExceptionClass");
  GetJnjvmArrayClassFunction = module->getFunction("getJnjvmArrayClass");
  StaticCtpLookupFunction = module->getFunction("staticCtpLookup");
  SpecialCtpLookupFunction = module->getFunction("specialCtpLookup");
#endif
#endif
 
#ifdef SERVICE
  ServiceCallStartFunction = module->getFunction("serviceCallStart");
  ServiceCallStopFunction = module->getFunction("serviceCallStop");
#endif

#ifdef WITH_TRACER
  MarkAndTraceFunction = module->getFunction("MarkAndTrace");
  JavaObjectTracerFunction = module->getFunction("JavaObjectTracer");
  JavaArrayTracerFunction = module->getFunction("JavaArrayTracer");
  ArrayObjectTracerFunction = module->getFunction("ArrayObjectTracer");
#endif

#ifndef WITHOUT_VTABLE
  VirtualLookupFunction = module->getFunction("vtableLookup");
#endif

  GetLockFunction = module->getFunction("getLock");
  
  addTypeName("JavaObject", JavaObjectType);
  addTypeName("JavaArray", JavaArrayType);
  addTypeName("JavaCommonClass", JavaCommonClassType);
  addTypeName("JavaClass", JavaClassType);
  addTypeName("JavaClassPrimitive", JavaClassPrimitiveType);
  addTypeName("JavaClassArray", JavaClassArrayType);
  addTypeName("ArrayUInt8", JavaArrayUInt8Type);
  addTypeName("ArraySInt8", JavaArraySInt8Type);
  addTypeName("ArrayUInt16", JavaArrayUInt16Type);
  addTypeName("ArraySInt16", JavaArraySInt16Type);
  addTypeName("ArraySInt32", JavaArraySInt32Type);
  addTypeName("ArrayLong", JavaArrayLongType);
  addTypeName("ArrayFloat", JavaArrayFloatType);
  addTypeName("ArrayDouble", JavaArrayDoubleType);
  addTypeName("ArrayObject", JavaArrayObjectType);
  addTypeName("CacheNode", CacheNodeType); 
  addTypeName("Enveloppe", EnveloppeType); 
}

void LLVMAssessorInfo::initialise() {
  AssessorInfo[I_VOID].llvmType = Type::VoidTy;
  AssessorInfo[I_VOID].llvmTypePtr = 0;
  AssessorInfo[I_VOID].llvmNullConstant = 0;
  AssessorInfo[I_VOID].sizeInBytesConstant = 0;
  
  AssessorInfo[I_BOOL].llvmType = Type::Int8Ty;
  AssessorInfo[I_BOOL].llvmTypePtr = PointerType::getUnqual(Type::Int8Ty);
  AssessorInfo[I_BOOL].llvmNullConstant = 
    Constant::getNullValue(Type::Int8Ty);
  AssessorInfo[I_BOOL].sizeInBytesConstant = mvm::MvmModule::constantOne;
  
  AssessorInfo[I_BYTE].llvmType = Type::Int8Ty;
  AssessorInfo[I_BYTE].llvmTypePtr = PointerType::getUnqual(Type::Int8Ty);
  AssessorInfo[I_BYTE].llvmNullConstant = 
    Constant::getNullValue(Type::Int8Ty);
  AssessorInfo[I_BYTE].sizeInBytesConstant = mvm::MvmModule::constantOne;
  
  AssessorInfo[I_SHORT].llvmType = Type::Int16Ty;
  AssessorInfo[I_SHORT].llvmTypePtr = PointerType::getUnqual(Type::Int16Ty);
  AssessorInfo[I_SHORT].llvmNullConstant = 
    Constant::getNullValue(Type::Int16Ty);
  AssessorInfo[I_SHORT].sizeInBytesConstant = mvm::MvmModule::constantTwo;
  
  AssessorInfo[I_CHAR].llvmType = Type::Int16Ty;
  AssessorInfo[I_CHAR].llvmTypePtr = PointerType::getUnqual(Type::Int16Ty);
  AssessorInfo[I_CHAR].llvmNullConstant = 
    Constant::getNullValue(Type::Int16Ty);
  AssessorInfo[I_CHAR].sizeInBytesConstant = mvm::MvmModule::constantTwo;
  
  AssessorInfo[I_INT].llvmType = Type::Int32Ty;
  AssessorInfo[I_INT].llvmTypePtr = PointerType::getUnqual(Type::Int32Ty);
  AssessorInfo[I_INT].llvmNullConstant = 
    Constant::getNullValue(Type::Int32Ty);
  AssessorInfo[I_INT].sizeInBytesConstant = mvm::MvmModule::constantFour;
  
  AssessorInfo[I_FLOAT].llvmType = Type::FloatTy;
  AssessorInfo[I_FLOAT].llvmTypePtr = PointerType::getUnqual(Type::FloatTy);
  AssessorInfo[I_FLOAT].llvmNullConstant = 
    Constant::getNullValue(Type::FloatTy);
  AssessorInfo[I_FLOAT].sizeInBytesConstant = mvm::MvmModule::constantFour;
  
  AssessorInfo[I_LONG].llvmType = Type::Int64Ty;
  AssessorInfo[I_LONG].llvmTypePtr = PointerType::getUnqual(Type::Int64Ty);
  AssessorInfo[I_LONG].llvmNullConstant = 
    Constant::getNullValue(Type::Int64Ty);
  AssessorInfo[I_LONG].sizeInBytesConstant = mvm::MvmModule::constantEight;
  
  AssessorInfo[I_DOUBLE].llvmType = Type::DoubleTy;
  AssessorInfo[I_DOUBLE].llvmTypePtr = PointerType::getUnqual(Type::DoubleTy);
  AssessorInfo[I_DOUBLE].llvmNullConstant = 
    Constant::getNullValue(Type::DoubleTy);
  AssessorInfo[I_DOUBLE].sizeInBytesConstant = mvm::MvmModule::constantEight;
  
  AssessorInfo[I_TAB].llvmType = JnjvmModule::JavaObjectType;
  AssessorInfo[I_TAB].llvmTypePtr =
    PointerType::getUnqual(JnjvmModule::JavaObjectType);
  AssessorInfo[I_TAB].llvmNullConstant =
    JnjvmModule::JavaObjectNullConstant;
  AssessorInfo[I_TAB].sizeInBytesConstant = mvm::MvmModule::constantPtrSize;
  
  AssessorInfo[I_REF].llvmType = JnjvmModule::JavaObjectType;
  AssessorInfo[I_REF].llvmTypePtr =
    PointerType::getUnqual(JnjvmModule::JavaObjectType);
  AssessorInfo[I_REF].llvmNullConstant =
    JnjvmModule::JavaObjectNullConstant;
  AssessorInfo[I_REF].sizeInBytesConstant = mvm::MvmModule::constantPtrSize;
}

std::map<const char, LLVMAssessorInfo> LLVMAssessorInfo::AssessorInfo;

LLVMAssessorInfo& JnjvmModule::getTypedefInfo(const Typedef* type) {
  return LLVMAssessorInfo::AssessorInfo[type->getKey()->elements[0]];
}

static AnnotationID JavaMethod_ID(
  AnnotationManager::getID("Java::JavaMethod"));


LLVMMethodInfo::LLVMMethodInfo(JavaMethod* M) : 
  llvm::Annotation(JavaMethod_ID), methodDef(M), methodFunction(0),
  offsetConstant(0), functionType(0) {}

JavaMethod* LLVMMethodInfo::get(const llvm::Function* F) {
  LLVMMethodInfo *MI = (LLVMMethodInfo*)F->getAnnotation(JavaMethod_ID);
  if (MI) return MI->methodDef;
  return 0;
}

#ifdef SERVICE
Value* JnjvmModule::getIsolate(Jnjvm* isolate, Value* Where) {
  if (staticCompilation) {
    llvm::Constant* varGV = 0;
    isolate_iterator End = isolates.end();
    isolate_iterator I = isolates.find(isolate);
    if (I == End) {
    
      
      Constant* cons = 
        ConstantExpr::getIntToPtr(ConstantInt::get(Type::Int64Ty,
                                                   uint64_t(isolate)),
                                  ptrType);

      varGV = new GlobalVariable(ptrType, !staticCompilation,
                                 GlobalValue::ExternalLinkage,
                                 cons, "", this);
    
      isolates.insert(std::make_pair(isolate, varGV));
    } else {
      varGV = I->second;
    }
    if (BasicBlock* BB = dyn_cast<BasicBlock>(Where)) {
      return new LoadInst(varGV, "", BB);
    } else {
      assert(dyn_cast<Instruction>(Where) && "Wrong use of module");
      return new LoadInst(varGV, "", dyn_cast<Instruction>(Where));
    }
  } else {
    ConstantInt* CI = ConstantInt::get(Type::Int64Ty, uint64_t(isolate));
    return ConstantExpr::getIntToPtr(CI, ptrType);
  }
}
#endif

void JnjvmModule::CreateStaticInitializer() {

  std::vector<const llvm::Type*> llvmArgs;
  llvmArgs.push_back(ptrType); // class loader
  llvmArgs.push_back(JavaCommonClassType); // cl
  const FunctionType* FTy = FunctionType::get(Type::VoidTy, llvmArgs, false);

  Function* AddClass = Function::Create(FTy, GlobalValue::ExternalLinkage,
                                        "vmjcAddPreCompiledClass", this);
 
  llvmArgs.clear();
  llvmArgs.push_back(ptrType); // class loader
  llvmArgs.push_back(PointerType::getUnqual(JavaClassArrayType)); // array ptr
  llvmArgs.push_back(UTF8Type); // name
  FTy = FunctionType::get(Type::VoidTy, llvmArgs, false);
  
  Function* GetClassArray = Function::Create(FTy, GlobalValue::ExternalLinkage,
                                             "vmjcGetClassArray", this);
  
  if (!StaticInitializer) {
    std::vector<const llvm::Type*> llvmArgs;
    llvmArgs.push_back(ptrType); // class loader.
    const FunctionType* FTy = FunctionType::get(Type::VoidTy, llvmArgs, false);
    
    StaticInitializer = Function::Create(FTy, GlobalValue::InternalLinkage,
                                         "Init", this);
  }

  BasicBlock* currentBlock = BasicBlock::Create("enter", StaticInitializer);
  Function::arg_iterator loader = StaticInitializer->arg_begin();
  
  Value* Args[3];
  // If we have defined some strings.
  if (strings.begin() != strings.end()) {
    llvmArgs.clear();
    llvmArgs.push_back(ptrType); // class loader
    llvmArgs.push_back(strings.begin()->second->getType()); // val
    FTy = FunctionType::get(Type::VoidTy, llvmArgs, false);
  
    Function* AddString = Function::Create(FTy, GlobalValue::ExternalLinkage,
                                           "vmjcAddString", this);
  

  
    for (string_iterator i = strings.begin(), e = strings.end(); i != e; ++i) {
      Args[0] = loader;
      Args[1] = i->second;
      CallInst::Create(AddString, Args, Args + 2, "", currentBlock);
    }
  }
  
  for (native_class_iterator i = nativeClasses.begin(), 
       e = nativeClasses.end(); i != e; ++i) {
    if (isCompiling(i->first)) {
      Args[0] = loader;
      Args[1] = ConstantExpr::getBitCast(i->second, JavaCommonClassType);
      CallInst::Create(AddClass, Args, Args + 2, "", currentBlock);
    }
  }
  
  for (array_class_iterator i = arrayClasses.begin(), 
       e = arrayClasses.end(); i != e; ++i) {
    if (!(i->first->baseClass()->isPrimitive())) {
      Args[0] = loader;
      Args[1] = i->second;
      Args[2] = getUTF8(i->first->name);
      CallInst::Create(GetClassArray, Args, Args + 3, "", currentBlock);
    }
  }
  

  ReturnInst::Create(currentBlock);
}

void JnjvmModule::setNoInline(Class* cl) {
  for (uint32 i = 0; i < cl->nbVirtualMethods; ++i) {
    JavaMethod& meth = cl->virtualMethods[i];
    if (!isAbstract(meth.access)) {
      LLVMMethodInfo* LMI = getMethodInfo(&meth);
      Function* func = LMI->getMethod();
      func->addFnAttr(Attribute::NoInline);
    }
  }
  
  for (uint32 i = 0; i < cl->nbStaticMethods; ++i) {
    JavaMethod& meth = cl->staticMethods[i];
    if (!isAbstract(meth.access)) {
      LLVMMethodInfo* LMI = getMethodInfo(&meth);
      Function* func = LMI->getMethod();
      func->addFnAttr(Attribute::NoInline);
    }
  }
}

LLVMSignatureInfo* JnjvmModule::getSignatureInfo(Signdef* sign) {
  return sign->getInfo<LLVMSignatureInfo>();
}
  
LLVMClassInfo* JnjvmModule::getClassInfo(Class* cl) {
  return cl->getInfo<LLVMClassInfo>();
}

LLVMFieldInfo* JnjvmModule::getFieldInfo(JavaField* field) {
  return field->getInfo<LLVMFieldInfo>();
}
  
LLVMMethodInfo* JnjvmModule::getMethodInfo(JavaMethod* method) {
  return method->getInfo<LLVMMethodInfo>();
}