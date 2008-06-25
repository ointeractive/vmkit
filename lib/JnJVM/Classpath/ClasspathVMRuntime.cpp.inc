//===------ ClasspathVMRuntime.cpp - GNU classpath java/lang/VMRuntime ----===//
//
//                              JnJVM
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include <dlfcn.h>
#include <string.h>

#include "MvmGC.h"

#include "types.h"

#include "JavaArray.h"
#include "JavaClass.h"
#include "JavaObject.h"
#include "JavaString.h"
#include "JavaTypes.h"
#include "JavaThread.h"
#include "Jnjvm.h"
#include "NativeUtil.h"


using namespace jnjvm;

extern "C" {


JNIEXPORT jobject JNICALL Java_java_lang_VMRuntime_mapLibraryName(
#ifdef NATIVE_JNI
JNIEnv *env,
                                                                  jclass clazz,
#endif
                                                                  jobject _strLib) {
  JavaString* strLib = (JavaString*)_strLib;
  Jnjvm* vm = JavaThread::get()->isolate;

  const UTF8* utf8Lib = strLib->value;
  uint32 stLib = strLib->offset;
  sint32 lgLib = strLib->count;
  sint32 lgPre = vm->prelib->size;
  sint32 lgPost = vm->postlib->size;
  
  uint32 size = (uint32)(lgPre + lgLib + lgPost);
  uint16* elements = (uint16*)alloca(size * sizeof(uint16));

  memmove(elements, vm->prelib->elements, lgPre * sizeof(uint16));
  memmove(&(elements[lgPre]), &(utf8Lib->elements[stLib]), lgLib * sizeof(uint16));
  memmove(&(elements[lgPre + lgLib]), vm->postlib->elements, lgPost * sizeof(uint16));
  
  const UTF8* res = vm->readerConstructUTF8(elements, size);

  return (jobject)(vm->UTF8ToStr(res));
  
}

typedef int (*onLoad_t)(const void**, void*);

JNIEXPORT jint JNICALL Java_java_lang_VMRuntime_nativeLoad(
#ifdef NATIVE_JNI
JNIEnv *env,
jclass clazz,
#endif
                                                           jobject _str,
                                                           jobject _loader) {
  JavaString* str = (JavaString*)_str;
#ifndef SERVICE_VM
  Jnjvm* vm = JavaThread::get()->isolate;
#else
  Jnjvm* vm = Jnjvm::bootstrapVM;
#endif
  
  char* buf = str->strToAsciiz();
  
  void* res = dlopen(buf, RTLD_LAZY | RTLD_LOCAL);
  if (res != 0) {
    vm->nativeLibs.push_back(res);
    onLoad_t onLoad = (onLoad_t)(intptr_t)dlsym(res, "JNI_OnLoad");
    if (onLoad) onLoad(&vm->javavmEnv, 0);
    return 1;
  } else {
    return 0;
  }
}


JNIEXPORT void JNICALL Java_java_lang_VMRuntime_gc(
#ifdef NATIVE_JNI
JNIEnv *env,
jclass clazz,
#endif
) {
#ifdef MULTIPLE_GC
    mvm::Thread::get()->GC->collect();
#else
  Collector::collect();
#endif
}

JNIEXPORT void JNICALL Java_java_lang_VMRuntime_runFinalization(
#ifdef NATIVE_JNI
JNIEnv *env,
jclass clazz,
#endif
) {
  return;
}

JNIEXPORT void JNICALL Java_java_lang_VMRuntime_runFinalizationForExit(
#ifdef NATIVE_JNI
JNIEnv *env,
jclass clazz,
#endif
) {
  return;
}

JNIEXPORT void JNICALL Java_java_lang_VMRuntime_exit(
#ifdef NATIVE_JNI
JNIEnv *env,
jclass clazz,
#endif
jint par1) {
#ifndef MULTIPLE_VM
  exit(par1);
#else
  // TODO: do a longjmp
  exit(par1);
#endif
}

JNIEXPORT jlong Java_java_lang_VMRuntime_freeMemory(
#ifdef NATIVE_JNI
JNIEnv *env,
jclass clazz,
#endif
) {
#ifdef MULTIPLE_GC
  return (jlong)mvm::Thread::get()->GC->getFreeMemory();
#else
  return (jlong)Collector::getFreeMemory();
#endif
}

JNIEXPORT jlong Java_java_lang_VMRuntime_totalMemory(
#ifdef NATIVE_JNI
JNIEnv *env,
jclass clazz,
#endif
) {
#ifdef MULTIPLE_GC
  return (jlong)mvm::Thread::get()->GC->getTotalMemory();
#else
  return (jlong)Collector::getTotalMemory();
#endif
}

JNIEXPORT jlong Java_java_lang_VMRuntime_maxMemory(
#ifdef NATIVE_JNI
JNIEnv *env,
jclass clazz,
#endif
) {
#ifdef MULTIPLE_GC
  return (jlong)mvm::Thread::get()->GC->getMaxMemory();
#else
  return (jlong)Collector::getMaxMemory();
#endif
}

}