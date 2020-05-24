#include <stdio.h> 
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "chunk.h"   
#include "debug.h"
#include "vm.h"  

static void repl() {                        
  char line[1024];                          
  for (;;) {                                
    printf("> ");

    if (!fgets(line, sizeof(line), stdin)) {
      printf("\n");                         
      break;                                
    }                                       

    interpret(line);                        
  }                                         
}

static char* readFile(const char* path) {                        
  FILE* file = fopen(path, "rb");
  if (file == NULL) {                                      
    fprintf(stderr, "Could not open file \"%s\".\n", path);
    exit(74);                                              
  }  

  fseek(file, 0L, SEEK_END);                                     
  size_t fileSize = ftell(file);                                 
  rewind(file);                                                  

  char* buffer = (char*)malloc(fileSize + 1); 
  if (buffer == NULL) {                                          
    fprintf(stderr, "Not enough memory to read \"%s\".\n", path);
    exit(74);                                                    
  } 

  size_t bytesRead = fread(buffer, sizeof(char), fileSize, file);
  if (bytesRead < fileSize) {                                    
    fprintf(stderr, "Could not read file \"%s\".\n", path);      
    exit(74);                                                    
  } 

  buffer[bytesRead] = '\0';                                      

  fclose(file);                                                  
  return buffer;                                                 
}   

static void runFile(const char* path) {           
  char* source = readFile(path);                  
  InterpretResult result = interpret(source);     
  free(source); 

  if (result == INTERPRET_COMPILE_ERROR) exit(65);
  if (result == INTERPRET_RUNTIME_ERROR) exit(70);
}

// static void printSizes() {
//   printf("== <size start> ==\n");
//   printf("ObjType: %lu\n", sizeof(ObjType));
//   printf("bool: %lu\n", sizeof(bool));
//   printf("Obj*: %lu\n", sizeof(Obj*));
//   printf("Obj: %lu\n", sizeof(Obj));
//   printf("int: %lu\n", sizeof(int));
//   printf("uint32_t: %lu\n", sizeof(uint32_t));
//   printf("char*: %lu\n", sizeof(char*));
//   printf("ObjString: %lu\n", sizeof(ObjString));
//   printf("ValueArray: %lu\n", sizeof(ValueArray));
//   printf("Chunk: %lu\n", sizeof(Chunk));
//   printf("ObjFunction: %lu\n", sizeof(ObjFunction));
//   printf("== <size end> ==\n");
// }

int main(int argc, const char* argv[]) {
  // printSizes();
  initVM();

  if (argc == 1) {                          
    repl();                                 
  } else if (argc == 2) {                   
    runFile(argv[1]);                       
  } else {                                  
    fprintf(stderr, "Usage: clox [path]\n");
    exit(64);                               
  }

  freeVM(); 
  return 0;                             
}  