#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h> 

#include "common.h"
#include "compiler.h"
#include "debug.h"
#include "object.h"
#include "memory.h"
#include "vm.h"    

VM vm;

static Value clockNative(int argCount, Value* args) {
  // doesn't check arity 
  return NUMBER_VAL((double)clock() / CLOCKS_PER_SEC);
}

static void resetStack() {
  vm.stackTop = vm.stack;
  vm.frameCount = 0;
  vm.openUpvalues = NULL;
}

static void runtimeError(const char* format, ...) {
  va_list args;                                    
  va_start(args, format);                          
  vfprintf(stderr, format, args);                  
  va_end(args);                                    
  fputs("\n", stderr);

  for (int i = vm.frameCount - 1; i >= 0; i--) {                 
    CallFrame* frame = &vm.frames[i];                            
    ObjFunction* function = frame->closure->function;                     
    // -1 because the IP is sitting on the next instruction to be
    // executed.                                                 
    size_t instruction = frame->ip - function->chunk.code - 1;   
    fprintf(stderr, "[line %d] in ",                             
            function->chunk.lines[instruction]);                 
    if (function->name == NULL) {                                
      fprintf(stderr, "script\n");                               
    } else {                                                     
      fprintf(stderr, "%s()\n", function->name->chars);          
    }                                                            
  }

  resetStack();                                    
}

static void defineNative(const char* name, NativeFn function) {
  // push then pop for gc
  // the c-string name itself is a string literal, which should always exists, and as it's not an obj, it will not go through gc
  // but after using copyString, an objstring will be created, which wraps a copy of the c-string literal
  // this objstring is not referenced by anything, only later will it be referenced by the function
  // so it can be removed by gc when immediately newNative and tableGet try to allocate memory
  // to ensure it not get removed, we push it onto the stack, so that it get marked via stack
  // the same trick apply to the newly created ObjNative via newNative, which can be removed via tableGet, if not pushed onto the stack
  push(OBJ_VAL(copyString(name, (int)strlen(name))));
  push(OBJ_VAL(newNative(function)));                          
  tableSet(&vm.globals, AS_STRING(vm.stack[0]), vm.stack[1]);  
  pop();                                                       
  pop();

  // for lox function, we need to
  // define fnName as constant, define fn as constant
  // push fn to stack, then run [OP_DEFINE_GLOBAL, fnName] for tableSet and pop fn
  // for native function, it's defined before compile, so just tableSet                                                 
}

void initVM() { 
  resetStack();
  vm.objects = NULL;

  vm.bytesAllocated = 0;  
  vm.nextGC = 1024 * 1024;

  vm.grayCount = 0;      
  vm.grayCapacity = 0;   
  vm.grayStack = NULL;

  initTable(&vm.globals);
  initTable(&vm.strings);

  vm.initString = NULL;
  vm.initString = copyString("init", 4);

  defineNative("clock", clockNative);  
}                  

void freeVM() {
  freeTable(&vm.globals);
  freeTable(&vm.strings);
  vm.initString = NULL;
  freeObjects();    
}

void push(Value value) {
  *vm.stackTop = value; 
  vm.stackTop++;        
}

Value pop() {         
  vm.stackTop--;      
  return *vm.stackTop;
}

static Value peek(int distance) {   
  return vm.stackTop[-1 - distance];
}

static bool call(ObjClosure* closure, int argCount) {
  if (argCount != closure->function->arity) {                   
    runtimeError("Expected %d arguments but got %d.",  
        closure->function->arity, argCount);                    
    return false;                                      
  }

  if (vm.frameCount == FRAMES_MAX) {             
    runtimeError("Stack overflow.");             
    return false;                                
  }

  CallFrame* frame = &vm.frames[vm.frameCount++];      
  frame->closure = closure;                          
  frame->ip = closure->function->chunk.code;

  frame->slots = vm.stackTop - argCount - 1;           
  return true;                                         
}

static bool callValue(Value callee, int argCount) {    
  if (IS_OBJ(callee)) {                                
    switch (OBJ_TYPE(callee)) {
      case OBJ_BOUND_METHOD: {                          
        ObjBoundMethod* bound = AS_BOUND_METHOD(callee);
        // reuse closure slot to store this instance
        vm.stackTop[-argCount - 1] = bound->receiver;
        return call(bound->method, argCount);           
      }
      case OBJ_CLASS: {
        ObjClass* klass = AS_CLASS(callee);
        // change klass to instance
        vm.stackTop[-argCount - 1] = OBJ_VAL(newInstance(klass));
        Value initializer;                                           
        if (tableGet(&klass->methods, vm.initString, &initializer)) {
          return call(AS_CLOSURE(initializer), argCount);            
        } else if (argCount != 0) {                                  
          runtimeError("Expected 0 arguments but got %d.", argCount);
          return false;                                              
        }
        return true;                                             
      }
      case OBJ_CLOSURE:                           
        return call(AS_CLOSURE(callee), argCount);                        
      // case OBJ_FUNCTION: 
      //   return call(AS_FUNCTION(callee), argCount);
      case OBJ_NATIVE: {                                        
        NativeFn native = AS_NATIVE(callee);                    
        Value result = native(argCount, vm.stackTop - argCount);
        vm.stackTop -= argCount + 1;                            
        push(result);                                           
        return true;                                            
      }
      default:                                         
        // Non-callable object type.                   
        break;                                         
    }                                                  
  }                                                    

  runtimeError("Can only call functions and classes.");
  return false;                                        
}

static bool invokeFromClass(ObjClass* klass, ObjString* name,
                            int argCount) {                  
  Value method;                                              
  if (!tableGet(&klass->methods, name, &method)) {           
    runtimeError("Undefined property '%s'.", name->chars);   
    return false;                                            
  }

  // this = receiver is already on the start of the substack
  return call(AS_CLOSURE(method), argCount);                 
}

static bool invoke(ObjString* name, int argCount) {       
  Value receiver = peek(argCount);
  if (!IS_INSTANCE(receiver)) {                  
    runtimeError("Only instances have methods.");
    return false;                                
  }
  ObjInstance* instance = AS_INSTANCE(receiver);
  
  Value value;                                            
  if (tableGet(&instance->fields, name, &value)) {        
    vm.stackTop[-argCount - 1] = value; // do not think this is necessary, but it aligns with OP_GET_PROPERTY                  
    return callValue(value, argCount);                    
  }

  return invokeFromClass(instance->klass, name, argCount);
}

static bool bindMethod(ObjClass* klass, ObjString* name) {            
  Value method;                                                       
  if (!tableGet(&klass->methods, name, &method)) {                    
    runtimeError("Undefined property '%s'.", name->chars);            
    return false;                                                     
  }

  ObjBoundMethod* bound = newBoundMethod(peek(0), AS_CLOSURE(method));
  pop();                                                              
  push(OBJ_VAL(bound));                                               
  return true;                                                        
}

static ObjUpvalue* captureUpvalue(Value* local) {
  ObjUpvalue* prevUpvalue = NULL;                                   
  ObjUpvalue* upvalue = vm.openUpvalues;

  while (upvalue != NULL && upvalue->location > local) {            
    prevUpvalue = upvalue;                                          
    upvalue = upvalue->next;                                        
  }

  // reuse existing upvalue
  if (upvalue != NULL && upvalue->location == local) return upvalue;

  ObjUpvalue* createdUpvalue = newUpvalue(local);
  createdUpvalue->next = upvalue;

  // captureUpvalue is called when inner function closure get defined
  // at this time inner function is not executed, it's the outer function executing
  // captured upvalues will be open and appended to vm.openUpvalues
  // then when outer function returned, they will be closed via closeUpValues
  if (prevUpvalue == NULL) {                     
    vm.openUpvalues = createdUpvalue;            
  } else {                                       
    prevUpvalue->next = createdUpvalue;          
  }

  return createdUpvalue;                         
}

static void closeUpvalues(Value* last) {
  // this moves upvalues from stack to heap  
  while (vm.openUpvalues != NULL &&          
         vm.openUpvalues->location >= last) {
    ObjUpvalue* upvalue = vm.openUpvalues;   
    upvalue->closed = *upvalue->location;    
    upvalue->location = &upvalue->closed;    
    vm.openUpvalues = upvalue->next;         
  }                                          
}

static void defineMethod(ObjString* name) {
  Value method = peek(0);                  
  ObjClass* klass = AS_CLASS(peek(1));     
  tableSet(&klass->methods, name, method); 
  pop();                                   
}

static bool isFalsey(Value value) {                           
  return IS_NIL(value) || (IS_BOOL(value) && !AS_BOOL(value));
}

static void concatenate() {                      
  ObjString* b = AS_STRING(peek(0)); 
  ObjString* a = AS_STRING(peek(1));

  int length = a->length + b->length;
  char* chars = ALLOCATE(char, length + 1);
  memcpy(chars, a->chars, a->length);            
  memcpy(chars + a->length, b->chars, b->length);
  chars[length] = '\0';                          

  ObjString* result = takeString(chars, length);
  pop();                                        
  pop(); 
  push(OBJ_VAL(result));                         
}

static InterpretResult run() {
#ifdef DEBUG_TRACE_EXECUTION
  printf("== <execute script> ==\n");
#endif 

  CallFrame* frame = &vm.frames[vm.frameCount - 1];

#define READ_BYTE() (*frame->ip++)
#define READ_SHORT() \
    (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define READ_CONSTANT() \
    (frame->closure->function->chunk.constants.values[READ_BYTE()])
#define READ_STRING() AS_STRING(READ_CONSTANT())
#define BINARY_OP(valueType, op) \
    do { \
      if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) { \
        runtimeError("Operands must be numbers."); \
        return INTERPRET_RUNTIME_ERROR; \
      } \
      \
      double b = AS_NUMBER(pop()); \
      double a = AS_NUMBER(pop()); \
      push(valueType(a op b)); \
    } while (false)           

  for (;;) {
#ifdef DEBUG_TRACE_EXECUTION
    printf("          ");                                           
    for (Value* slot = vm.stack; slot < vm.stackTop; slot++) {      
      printf("[ ");                                                 
      printValue(*slot);                                            
      printf(" ]");                                                 
    }                                                               
    printf("\n");                                      
    disassembleInstruction(&frame->closure->function->chunk,     
        (int)(frame->ip - frame->closure->function->chunk.code));
#endif

    uint8_t instruction;                
    switch (instruction = READ_BYTE()) {
      case OP_CONSTANT: {                
        Value constant = READ_CONSTANT();
        push(constant);                  
        break;                           
      }
      case OP_NIL: push(NIL_VAL); break;                
      case OP_TRUE: push(BOOL_VAL(true)); break;        
      case OP_FALSE: push(BOOL_VAL(false)); break;
      case OP_POP: pop(); break;
      case OP_GET_LOCAL: {             
        uint8_t slot = READ_BYTE();    
        push(frame->slots[slot]);
        break;                         
      }
      case OP_SET_LOCAL: {         
        uint8_t slot = READ_BYTE();
        frame->slots[slot] = peek(0);  
        break;                     
      }
      case OP_GET_GLOBAL: {                                     
        ObjString* name = READ_STRING();                        
        Value value;                                            
        if (!tableGet(&vm.globals, name, &value)) {             
          runtimeError("Undefined variable '%s'.", name->chars);
          return INTERPRET_RUNTIME_ERROR;                       
        }                                                       
        push(value);                                            
        break;                                                  
      }
      case OP_DEFINE_GLOBAL: {               
        ObjString* name = READ_STRING();     
        tableSet(&vm.globals, name, peek(0));
        pop();                               
        break;                               
      }
      case OP_SET_GLOBAL: {                                     
        ObjString* name = READ_STRING();                        
        if (tableSet(&vm.globals, name, peek(0))) {             
          tableDelete(&vm.globals, name); 
          runtimeError("Undefined variable '%s'.", name->chars);
          return INTERPRET_RUNTIME_ERROR;                       
        }                                                       
        break;                                                  
      } 
      case OP_GET_UPVALUE: {                            
        uint8_t slot = READ_BYTE();                     
        push(*frame->closure->upvalues[slot]->location);
        break;                                          
      }
      case OP_SET_UPVALUE: {                                
        uint8_t slot = READ_BYTE();                         
        *frame->closure->upvalues[slot]->location = peek(0);
        break;                                              
      }
      case OP_GET_PROPERTY: {
        if (!IS_INSTANCE(peek(0))) {                      
          runtimeError("Only instances have properties.");
          return INTERPRET_RUNTIME_ERROR;                 
        } 

        ObjInstance* instance = AS_INSTANCE(peek(0)); // read from stack
        ObjString* name = READ_STRING(); // read from bytecode

        Value value;                                        
        if (tableGet(&instance->fields, name, &value)) {    
          pop(); // Instance.                               
          push(value);                                      
          break;                                            
        }

        if (!bindMethod(instance->klass, name)) {
          return INTERPRET_RUNTIME_ERROR;        
        }                                        
        break;                                               
      }
      case OP_SET_PROPERTY: {
        if (!IS_INSTANCE(peek(1))) {                  
          runtimeError("Only instances have fields.");
          return INTERPRET_RUNTIME_ERROR;             
        }                                 
        
        ObjInstance* instance = AS_INSTANCE(peek(1));         
        tableSet(&instance->fields, READ_STRING(), peek(0));

        Value value = pop();                                  
        pop();                                                
        push(value);                                          
        break;                                                
      }
      case OP_GET_SUPER: {                     
        ObjString* name = READ_STRING();       
        ObjClass* superclass = AS_CLASS(pop());
        if (!bindMethod(superclass, name)) {   
          return INTERPRET_RUNTIME_ERROR;      
        }                                      
        break;                                 
      }
      case OP_EQUAL: {                                  
        Value b = pop();                                
        Value a = pop();                                
        push(BOOL_VAL(valuesEqual(a, b)));              
        break;                                          
      }
      case OP_GREATER:  BINARY_OP(BOOL_VAL, >); break;  
      case OP_LESS:     BINARY_OP(BOOL_VAL, <); break;
      case OP_ADD: {                                                   
        if (IS_STRING(peek(0)) && IS_STRING(peek(1))) {                
          concatenate();                                               
        } else if (IS_NUMBER(peek(0)) && IS_NUMBER(peek(1))) {         
          double b = AS_NUMBER(pop());                                 
          double a = AS_NUMBER(pop());                                 
          push(NUMBER_VAL(a + b));                                     
        } else {                                                       
          runtimeError("Operands must be two numbers or two strings.");
          return INTERPRET_RUNTIME_ERROR;                              
        }                                                              
        break;                                                         
      }
      case OP_SUBTRACT: BINARY_OP(NUMBER_VAL, -); break;
      case OP_MULTIPLY: BINARY_OP(NUMBER_VAL, *); break;
      case OP_DIVIDE:   BINARY_OP(NUMBER_VAL, /); break;
      case OP_NOT:                                      
        push(BOOL_VAL(isFalsey(pop())));                
        break;
      case OP_NEGATE:                               
        if (!IS_NUMBER(peek(0))) {                  
          runtimeError("Operand must be a number.");
          return INTERPRET_RUNTIME_ERROR;           
        }

        push(NUMBER_VAL(-AS_NUMBER(pop())));        
        break;
      case OP_PRINT: {    
        printValue(pop());
        printf("\n");     
        break;            
      }
      case OP_JUMP: {                  
        uint16_t offset = READ_SHORT();
        frame->ip += offset;               
        break;                         
      }
      case OP_JUMP_IF_FALSE: {                 
        uint16_t offset = READ_SHORT();        
        if (isFalsey(peek(0))) frame->ip += offset;
        break;                                 
      }
      case OP_LOOP: {                  
        uint16_t offset = READ_SHORT();
        frame->ip -= offset;               
        break;                         
      } 
      case OP_CALL: {                              
        int argCount = READ_BYTE();                
        if (!callValue(peek(argCount), argCount)) {
          // the first slot of the substack is peek(argCount) = callee
          // and it is only used to correctly invoking callValue
          // so after that it can be changed to this if callee is a method
          return INTERPRET_RUNTIME_ERROR;          
        }
        frame = &vm.frames[vm.frameCount - 1];                                          
        break;                                     
      }
      case OP_INVOKE: {                       
        ObjString* method = READ_STRING();    
        int argCount = READ_BYTE();           
        if (!invoke(method, argCount)) {      
          return INTERPRET_RUNTIME_ERROR;     
        }                                     
        frame = &vm.frames[vm.frameCount - 1];
        break;                                
      }
      case OP_SUPER_INVOKE: {                                
        ObjString* method = READ_STRING();                   
        int argCount = READ_BYTE();                          
        ObjClass* superclass = AS_CLASS(pop());              
        if (!invokeFromClass(superclass, method, argCount)) {
          return INTERPRET_RUNTIME_ERROR;                    
        }                                                    
        frame = &vm.frames[vm.frameCount - 1];               
        break;                                               
      }
      case OP_CLOSURE: {                                     
        ObjFunction* function = AS_FUNCTION(READ_CONSTANT());
        ObjClosure* closure = newClosure(function);          
        push(OBJ_VAL(closure));
        // upvalues get captured when function closure is declared
        for (int i = 0; i < closure->upvalueCount; i++) {               
          uint8_t isLocal = READ_BYTE();                                
          uint8_t index = READ_BYTE();                                  
          if (isLocal) {                                                
            closure->upvalues[i] = captureUpvalue(frame->slots + index);
          } else {     
            // if the upvalue is a local variable of grand parent function
            // it should already be captured when parent function get declared
            // and it will be closed when grand parent function get returned
            // so it's safe to just =        
            closure->upvalues[i] = frame->closure->upvalues[index];     
          }                                                             
        }                              
        break;                                               
      }
      case OP_CLOSE_UPVALUE: {
        // emitted from endScope, will be used for upvalues for loop and block
        // it's doing similar things as the start of OP_RETURN
        // except that we don't have the start of locals for this scope
        // so we cannot just reassign vm.stackTop = frame->slots;
        // we need to do it one step at a time, only at location vm.stackTop-1
        closeUpvalues(vm.stackTop - 1);
        pop();                         
        break;
      }
      case OP_RETURN: {
        Value result = pop();
        // when inner function closure get declared
        // related upvalues should already be appended to vm.openUpvalues
        // and they should all be local variables of outer function, so we use frame->slots as last
        closeUpvalues(frame->slots);
        vm.frameCount--;                      
        if (vm.frameCount == 0) {             
          pop();                              
          return INTERPRET_OK;                
        }                                     

        vm.stackTop = frame->slots;           
        push(result);                         

        frame = &vm.frames[vm.frameCount - 1];
        break;
      }

      case OP_CLASS:                           
        push(OBJ_VAL(newClass(READ_STRING())));
        break;
      case OP_INHERIT: {                                                
        Value superclass = peek(1);
        if (!IS_CLASS(superclass)) {                                    
          runtimeError("Superclass must be a class.");                  
          return INTERPRET_RUNTIME_ERROR;                               
        }                                     
        ObjClass* subclass = AS_CLASS(peek(0));                         
        tableAddAll(&AS_CLASS(superclass)->methods, &subclass->methods);
        pop(); // only pop subclass, so superclass is still on stackTop, to behave as a local variable in outer scope
        break;                                                          
      }
      case OP_METHOD:               
        defineMethod(READ_STRING());
        break;                                
    }                                   
  }                                     

#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
#undef READ_STRING
#undef BINARY_OP                          
}

InterpretResult interpret(const char* source) {
  ObjFunction* function = compile(source);             
  if (function == NULL) return INTERPRET_COMPILE_ERROR;

  // current now point to NULL not function
  // so nothing to track it, have to push to the stack
  push(OBJ_VAL(function));
  ObjClosure* closure = newClosure(function);          
  pop();                                               
  push(OBJ_VAL(closure));                              
  callValue(OBJ_VAL(closure), 0);                

  return run();                     
}