#ifndef QUADS_H
#define QUADS_H

#include "symtable.h"

// opcodes for intermediate code
enum iopcode {
    assign, add, sub, mul, op_div, mod,
    uminus, and, or, not, if_eq, if_noteq,
    if_lesseq, if_greatereq, if_less, if_greater,
    call, param, ret, getretval, funcstart,
    funcend, tablecreate, tablegetelem, tablesetelem,
    jump
};

// expression types
enum expr_t {
    var_e,
    tableitem_e,
    programfunc_e,
    libraryfunc_e,
    arithexpr_e,
    boolexpr_e,
    assignexpr_e,
    newtable_e,
    constnum_e,
    constbool_e,
    conststring_e,
    nil_e
};



// linked list node for backpatching
typedef struct bp_list {
    unsigned quad_label;
    struct bp_list* next;
} bp_list;

// expression structure
typedef struct expr {
    enum expr_t type;
    symrec* sym;
    struct expr* index;
    double numConst;
    char* strConst;
    unsigned char boolConst;

    // fields for short-circuit evaluation
    bp_list* truelist;
    bp_list* falselist;

    struct expr* next;
} expr;

// quad structure
typedef struct quad {
    enum iopcode op;
    expr* result;
    expr* arg1;
    expr* arg2;
    unsigned label;
    unsigned line;
    unsigned taddress;
} quad;

// function prototypes for intermediate code generation
void emit_quad(enum iopcode op, expr* arg1, expr* arg2, expr* result, unsigned label, unsigned line);
expr* make_expr(enum expr_t t);
expr* make_const_num_expr(double v);
expr* make_const_bool_expr(unsigned char b);
expr* make_const_string_expr(char* s);

// temporary variables
char* create_temp_name();
symrec* create_temp_var(int scope, int line);
void reset_temp_counter();

// offset and scope space management
void enter_scopespace();
void exit_scopespace();
void reset_formal_args_offset();
void reset_function_locals_offset();
unsigned get_curr_offset();
void inc_curr_offset();
enum scopespace_t get_curr_scopespace();

// backpatching lists
bp_list* make_bplist(unsigned label);
bp_list* merge_bplists(bp_list* l1, bp_list* l2);
void patch_list(bp_list* l, unsigned label);

// quad tracking
unsigned next_quad_label();
void print_quads();

// exposed for target code generation
extern struct quad* quad_array;
extern unsigned curr_quad;
extern unsigned program_var_offset;
extern unsigned function_local_offset;

#endif