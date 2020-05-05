package com.craftinginterpreters.lox;

import java.util.HashMap;            
import java.util.Map;                

class LoxInstance {                  
  private LoxClass klass;
  private final Map<String, Object> fields = new HashMap<>();            

  LoxInstance(LoxClass klass) {      
    this.klass = klass;              
  }
  
  Object get(Token name) {    
    // fields assigned from other classes' methods will not bind                       
    if (fields.containsKey(name.lexeme)) {           
      return fields.get(name.lexeme);                
    }

    // only methods defined within the class or its superclass will be bound
    LoxFunction method = klass.findMethod(name.lexeme);
    if (method != null) return method.bind(this); 

    throw new RuntimeError(name, 
        "Undefined property '" + name.lexeme + "'.");
  }
  
  void set(Token name, Object value) {
    fields.put(name.lexeme, value);   
  }

  @Override                          
  public String toString() {         
    return klass.name + " instance"; 
  }                                  
}