#include <stdio.h>                                    
#include <string.h>                                   

#include "memory.h"                                   
#include "object.h"
#include "table.h"                                   
#include "value.h"                                    
#include "vm.h"

#define ALLOCATE_OBJ(type, objectType) \
    (type*)allocateObject(sizeof(type), objectType)

static Obj* allocateObject(size_t size, ObjType type) {
  // all kinds of obj are allocated on the heap
  // so they will stay active until freeObject is called    
  Obj* object = (Obj*)reallocate(NULL, 0, size);           
  object->type = type;
  object->isMarked = false;

  object->next = vm.objects;
  vm.objects = object;

#ifdef DEBUG_LOG_GC                                             
  printf("%p allocate %ld for %d\n", (void*)object, size, type);
#endif

  return object;                                           
}

ObjClass* newClass(ObjString* name) {                 
  ObjClass* klass = ALLOCATE_OBJ(ObjClass, OBJ_CLASS);
  klass->name = name; 
  return klass;                                       
}

ObjClosure* newClosure(ObjFunction* function) {
  ObjUpvalue** upvalues = ALLOCATE(ObjUpvalue*, function->upvalueCount);
  for (int i = 0; i < function->upvalueCount; i++) {                    
    upvalues[i] = NULL;                                                 
  }

  ObjClosure* closure = ALLOCATE_OBJ(ObjClosure, OBJ_CLOSURE);
  closure->function = function;
  closure->upvalues = upvalues;                  
  closure->upvalueCount = function->upvalueCount;                               
  return closure;                                             
}

ObjFunction* newFunction() {                                      
  ObjFunction* function = ALLOCATE_OBJ(ObjFunction, OBJ_FUNCTION);

  function->arity = 0;
  function->upvalueCount = 0;                                            
  function->name = NULL;                                          
  initChunk(&function->chunk);                                    
  return function;                                                
}

ObjInstance* newInstance(ObjClass* klass) {                       
  ObjInstance* instance = ALLOCATE_OBJ(ObjInstance, OBJ_INSTANCE);
  instance->klass = klass;                                        
  initTable(&instance->fields);                                   
  return instance;                                                
}

ObjNative* newNative(NativeFn function) {                 
  ObjNative* native = ALLOCATE_OBJ(ObjNative, OBJ_NATIVE);
  native->function = function;                            
  return native;                                          
}

static ObjString* allocateString(char* chars, int length, 
                                 uint32_t hash) { 
  ObjString* string = ALLOCATE_OBJ(ObjString, OBJ_STRING); 
  string->length = length;                                 
  string->chars = chars;
  string->hash = hash;

  // this can be called during compile time
  // so compiler will access vm.stack
  push(OBJ_VAL(string));
  tableSet(&vm.strings, string, NIL_VAL);
  pop();

  return string;                                           
}

static uint32_t hashString(const char* key, int length) {
  uint32_t hash = 2166136261u;

  for (int i = 0; i < length; i++) {                     
    hash ^= key[i];                                      
    hash *= 16777619;                                    
  }                                                      

  return hash;                                           
} 

ObjString* takeString(char* chars, int length) {
  // takeString is called in execution stage, to support concatenate
  // the deduplicated string is stored in vm.strings
  // but its referenece is stored on the stack, not in constants
  uint32_t hash = hashString(chars, length);
  ObjString* interned = tableFindString(&vm.strings, chars, length,
                                        hash);                     
  if (interned != NULL) {                                          
    FREE_ARRAY(char, chars, length + 1);                           
    return interned;                                               
  }

  return allocateString(chars, length, hash);         
}

ObjString* copyString(const char* chars, int length) {
  // copyString is called during compile stage
  // but it affects vm.strings which should appear later only at execution stage
  uint32_t hash = hashString(chars, length);
  ObjString* interned = tableFindString(&vm.strings, chars, length,
                                        hash);                     
  if (interned != NULL) return interned; 

  // ALLOCATE will call reallocate, therefore
  // memory of heapChar* also count in vm.bytesAllocated
  // it just does not print log "%p allocate %ld for %d"
  // also since it's not an obj, it will not be garbage collected directly
  // it will only be collected as part of objString
  char* heapChars = ALLOCATE(char, length + 1);       
  memcpy(heapChars, chars, length);                   
  heapChars[length] = '\0';                           

  return allocateString(heapChars, length, hash);         
}

ObjUpvalue* newUpvalue(Value* slot) {                         
  ObjUpvalue* upvalue = ALLOCATE_OBJ(ObjUpvalue, OBJ_UPVALUE);
  upvalue->closed = NIL_VAL;
  upvalue->location = slot;
  upvalue->next = NULL;
  return upvalue;                                             
}

static void printFunction(ObjFunction* function) {
  if (function->name == NULL) {                   
    printf("<script>");                           
    return;                                       
  }
  printf("<fn %s>", function->name->chars);       
}

void printObject(Value value) {       
  switch (OBJ_TYPE(value)) {
    case OBJ_CLASS:                              
      printf("%s", AS_CLASS(value)->name->chars);
      break;
    case OBJ_CLOSURE:                            
      printFunction(AS_CLOSURE(value)->function);
      break;
    case OBJ_FUNCTION:                  
      printFunction(AS_FUNCTION(value));
      break;
    case OBJ_INSTANCE:                                              
      printf("%s instance", AS_INSTANCE(value)->klass->name->chars);
      break;
    case OBJ_NATIVE:                    
      printf("<native fn>");            
      break;          
    case OBJ_STRING:                  
      printf("%s", AS_CSTRING(value));
      break;
    case OBJ_UPVALUE:                 
      printf("upvalue");              
      break;
  }                                   
} 