#ifndef AVM_H
#define AVM_H



#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>

#define AVM_STACKSIZE     4096
#define AVM_TABLE_HASHSIZE 211

// stack environment block offsets relative to topsp
#define AVM_NUMACTUALS_OFFSET  1
#define AVM_SAVEDTOPSP_OFFSET  2
#define AVM_SAVEDTOP_OFFSET    3
#define AVM_SAVEDPC_OFFSET     4
#define AVM_STACKENV_SIZE      5

#define MAGIC_NUMBER 340200501

// vm opcodes (must match target_code file order)
typedef enum {
    assign_v = 0,
    add_v, sub_v, mul_v, div_v, mod_v,
    jeq_v, jne_v, jle_v, jge_v, jlt_v, jgt_v,
    call_v, pusharg_v,
    ret_v, getretval_v,
    funcenter_v, funcexit_v,
    newtable_v, tablegetelem_v, tablesetelem_v,
    jump_v,
    AVM_MAX_INSTRUCTIONS
} vmopcode;

// vm arg types
typedef enum {
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
} vmarg_t;

typedef struct {
    vmarg_t  type;
    unsigned val;
} vmarg;

typedef struct {
    vmopcode opcode;
    vmarg    result;
    vmarg    arg1;
    vmarg    arg2;
    unsigned srcLine;
} instruction;

// memory cell types
typedef enum {
    number_m = 0,
    string_m,
    bool_m,
    table_m,
    userfunc_m,
    libfunc_m,
    nil_m,
    undef_m
} avm_memcell_t;

struct avm_table;

typedef struct avm_memcell {
    avm_memcell_t type;
    union {
        double  numVal;
        char*   strVal;
        unsigned char boolVal;
        struct avm_table* tableVal;
        unsigned  funcVal;
        char*     libfuncVal;
    } data;
} avm_memcell;

// hash bucket for table entries
typedef struct avm_table_bucket {
    avm_memcell key;
    avm_memcell value;
    struct avm_table_bucket* next;
} avm_table_bucket;

typedef struct avm_table {
    unsigned refCount;
    avm_table_bucket* numIndexed[AVM_TABLE_HASHSIZE];
    avm_table_bucket* strIndexed[AVM_TABLE_HASHSIZE];
    avm_table_bucket* otherIndexed;  // linked list for bool/userfunc/libfunc/table keys
    unsigned total;
} avm_table;

// userfunc entry loaded from binary
typedef struct {
    unsigned address;
    unsigned localSize;
    char*    id;
} avm_userfunc;

// vm state
extern avm_memcell  stack[];
extern unsigned     top;
extern unsigned     topsp;
extern unsigned     pc;
extern int          executionFinished;
extern avm_memcell  retval_reg;
extern unsigned     totalActuals;

// constant tables
extern char**       avm_strings;
extern unsigned     avm_total_strings;
extern double*      avm_numbers;
extern unsigned     avm_total_numbers;
extern avm_userfunc* avm_user_funcs;
extern unsigned     avm_total_user_funcs;
extern char**       avm_lib_funcs;
extern unsigned     avm_total_lib_funcs;
extern unsigned     avm_total_globals;

// instructions
extern instruction* code;
extern unsigned     codeSize;

// prototypes
void execute_cycle();
avm_memcell* avm_translate_operand(vmarg* a, avm_memcell* reg);
void  avm_assign(avm_memcell* lv, avm_memcell* rv);
void  avm_calllibfunc(const char* name);
char* avm_tostring(avm_memcell* m);
unsigned char avm_tobool(avm_memcell* m);
avm_table* avm_table_new();
void avm_table_incref(avm_table* t);
void avm_table_decref(avm_table* t);
avm_memcell* avm_table_getelem(avm_table* t, avm_memcell* key);
void avm_table_setelem(avm_table* t, avm_memcell* key, avm_memcell* val);
void avm_memcell_clear(avm_memcell* m);
void avm_error(const char* fmt, ...);
int  load_binary(const char* filename);

#endif
