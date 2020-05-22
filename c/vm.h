#ifndef clox_vm_h 
#define clox_vm_h 

#include "object.h"
#include "table.h"
#include "value.h"

#define FRAMES_MAX 64                       
#define STACK_MAX (FRAMES_MAX * UINT8_COUNT)

typedef struct {
  // contains the function and its execution environment      
  ObjClosure* closure;
  // cpu: ip starts from function->chunk->code
  uint8_t* ip;
  // memory: slots is the part of the vm.stack relavant to the function
  // it begins with the function itself OBJ_VAL(function)  
  Value* slots;
} CallFrame;

typedef struct {  
  CallFrame frames[FRAMES_MAX];
  int frameCount;

  Value stack[STACK_MAX];
  Value* stackTop;
  Table globals;
  Table strings;
  ObjUpvalue* openUpvalues;

  Obj* objects;   
} VM;

typedef enum {            
  INTERPRET_OK,           
  INTERPRET_COMPILE_ERROR,
  INTERPRET_RUNTIME_ERROR 
} InterpretResult; 

extern VM vm;

void initVM();    
void freeVM();
InterpretResult interpret(const char* source);
void push(Value value);                 
Value pop();    

#endif  