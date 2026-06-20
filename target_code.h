#ifndef TARGET_CODE_H
#define TARGET_CODE_H

// csd5434 - phase 4 & 5 implementation

#include "quads.h"

#define MAGIC_NUMBER 340200501

// vm instruction opcodes
enum vmopcode {
    assign_v = 0,
    add_v, sub_v, mul_v, div_v, mod_v,
    jeq_v, jne_v, jle_v, jge_v, jlt_v, jgt_v,
    call_v, pusharg_v,
    ret_v, getretval_v,
    funcenter_v, funcexit_v,
    newtable_v, tablegetelem_v, tablesetelem_v,
    jump_v
};

// vm argument types
enum vmarg_t {
    label_a    = 0,
    global_a   = 1,
    local_a    = 2,
    formal_a   = 3,
    number_a   = 4,
    string_a   = 5,
    bool_a     = 6,
    nil_a      = 7,
    userfunc_a = 8,
    libfunc_a  = 9,
    retval_a   = 10
};

// a vm operand
typedef struct vmarg {
    enum vmarg_t type;
    unsigned     val;
} vmarg;

// a single vm instruction
typedef struct instruction {
    enum vmopcode opcode;
    vmarg         result;
    vmarg         arg1;
    vmarg         arg2;
    unsigned      srcLine;
} instruction;

// user-defined function entry in the constant table
typedef struct userfunc {
    unsigned address;
    unsigned localSize;
    char*    id;
    symrec*  sym;   
} userfunc;

// sizes for constant tables
#define MAX_INSTRUCTIONS  65536
#define MAX_CONST_STRINGS 8192
#define MAX_CONST_NUMBERS 4096
#define MAX_USER_FUNCS    1024
#define MAX_LIB_FUNCS     64

// the instruction array
extern instruction  instructions[];
extern unsigned     curr_instruction;

// constant tables
extern char*        const_strings[];
extern unsigned     total_strings;

extern double       const_numbers[];
extern unsigned     total_numbers;

extern userfunc     user_funcs[];
extern unsigned     total_user_funcs;

extern char*        lib_funcs_used[];
extern unsigned     total_lib_funcs;

// main entry points
void generate_all();
void write_binary(const char* filename);
void write_text(const char* filename);

// constant table helpers
unsigned consts_newstring(const char* s);
unsigned consts_newnumber(double n);
unsigned userfuncs_newfunc(symrec* sym);
unsigned libfuncs_newused(const char* name);
void     userfuncs_setaddress(symrec* sym, unsigned addr);
void     userfuncs_setlocals(symrec* sym, unsigned locals);

#endif
