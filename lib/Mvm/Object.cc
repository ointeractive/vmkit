//===--------- Object.cc - Common objects for vmlets ----------------------===//
//
//                     The Micro Virtual Machine
//
// This file is distributed under the University of Illinois Open Source 
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include <stdlib.h>

#include "mvm/CollectableArea.h"
#include "mvm/Method.h"
#include "mvm/Object.h"
#include "mvm/PrintBuffer.h"
#include "mvm/GC/GC.h"
#include "mvm/Threads/Cond.h"
#include "mvm/Threads/Key.h"
#include "mvm/Threads/Locks.h"
#include "mvm/VMLet.h"


#include "llvm/DerivedTypes.h"
#include "llvm/Module.h"
#include "llvm/Type.h"


using namespace mvm;

VirtualTable *Object::VT = 0;
VirtualTable *Method::VT = 0;
VirtualTable *Code::VT = 0;
VirtualTable *ExceptionTable::VT = 0;
VirtualTable *NativeString::VT = 0;
VirtualTable *PrintBuffer::VT = 0;
//VirtualTable *Lock::VT = 0;
//VirtualTable *LockNormal::VT = 0;
//VirtualTable *LockRecursive::VT = 0;
//VirtualTable *ThreadKey::VT = 0;
//VirtualTable *Cond::VT = 0;


Object **Object::rootTable= 0;
int	  Object::rootTableSize= 0;
int	  Object::rootTableLimit= 0;

void Object::growRootTable(void) {
  if (rootTableLimit != 0) {
    assert(rootTable != 0);
    rootTableLimit*= 2;
    rootTable= (Object **)
      ::realloc((void *)rootTable, rootTableLimit * sizeof(Object **));
    return;
  }
  rootTableLimit= 32;
  rootTable= (Object **)malloc(32 * sizeof(Object **));
}


void Object::markAndTraceRoots(void) {
  for (int i= 0; i < rootTableSize; ++i)
    rootTable[i]->markAndTrace();
}

size_t Object::objectSize(void) const {
  return gc::objectSize();
}

bool Object::isObject() const { 
  return gc::isObject(this); 
}

Object *Object::begOf() const { 
  return (Object *)gc::begOf(this);
}

Object *Object::gcmalloc(size_t sz, VirtualTable* VT) {
  return (Object *)operator new(sz, VT);
}

void Object::initialise(void *b_sp) {
# define INIT(X) { \
  X fake; \
  X::VT = ((void**)(void*)(&fake))[0]; }
  
  INIT(Object);
  INIT(Method);
  INIT(Code);
  INIT(NativeString);
  INIT(PrintBuffer);
  //INIT(Lock);
  //INIT(LockNormal);
  //INIT(LockRecursive);
  //INIT(ThreadKey);
  //INIT(Cond);
  INIT(ExceptionTable);
  
#undef INIT

  gc::initialise(Object::markAndTraceRoots, b_sp);
}

void Code::tracer(size_t sz) {
	((Code *)this)->method(sz)->markAndTrace();
}

void Method::tracer(size_t sz) {
	Method *const self= (Method *)this;
  self->definition()->markAndTrace();
  self->literals()->markAndTrace();
  self->name()->markAndTrace();
  self->code()->markAndTrace();
  self->exceptionTable()->markAndTrace();
}

void PrintBuffer::tracer(size_t sz) {
	((PrintBuffer *)this)->contents()->markAndTrace();
}

PrintBuffer *PrintBuffer::alloc(void) {
  return allocPrintBuffer();
}


PrintBuffer *PrintBuffer::writeObj(const Object *obj) {
	Object *beg = obj->begOf();
	
	if(beg) {
		if(beg == obj)
			obj->print((mvm::PrintBuffer*)this);
		else {
			write("<In Object [");
			beg->print(this);
			write("] -- offset ");
			writeS4((int)obj - (int)beg);
			write(">");
		}
	} else {
		write("<DirectValue: ");
		writeS4((int)obj);
		write(">");
	}
	return this;
}

extern "C" void write_ptr(PrintBuffer* buf, void* obj) {
  buf->writePtr(obj);
}

extern "C" void write_int(PrintBuffer* buf, int a) {
  buf->writeS4(a);
}

extern "C" void write_str(PrintBuffer* buf, char* a) {
  buf->write(a);
}

char *Object::printString(void) const {
  PrintBuffer *buf= PrintBuffer::alloc();
	buf->writeObj(this);
  return buf->contents()->cString();
}

char * Object::printStatic(const Object * obj) {
  return obj->printString();
}

void Object::print(PrintBuffer *buf) const {
	buf->write("<Object@");
	buf->writePtr((void*)this);
  buf->write(">");
}

void Code::print(PrintBuffer *buf) const {
	buf->write("Code<");
	buf->write(">");
}

void Method::print(PrintBuffer *buf) const {
  Method *const self= (Method *)this;
	buf->write("Method<");
  if (self->name()) {
	  self->name()->print(buf);
  } else {
    buf->write("lambda");
  }
	buf->write(">");
}

void NativeString::print(PrintBuffer *buf) const {
  NativeString *const self= (NativeString *)this;
  buf->write("\"");
  for (size_t i= 0; i < strlen(self->cString()); ++i) {
    int c= self->cString()[i];
    switch (c) {
	    case '\b': buf->write("\\b"); break;
	    case '\f': buf->write("\\f"); break;
	    case '\n': buf->write("\\n"); break;
	    case '\r': buf->write("\\r"); break;
	    case '\t': buf->write("\\t"); break;
	    case '"':  buf->write("\\\""); break;
	    default: {
	      char esc[32];
	      if (c < 32)
	        sprintf(esc, "\\x%02x", c);
	      else
	        sprintf(esc, "%c", c);
	      buf->write(esc);
	    }
	  }
  }
  buf->write("\"");
}

NativeString      *PrintBuffer::getContents() { return contents(); }
