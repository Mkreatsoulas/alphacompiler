#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quads.h"

// arrays and counters for quads
quad* quad_array = NULL;
unsigned total_quads = 0;
unsigned curr_quad = 0;
#define EXPAND_SIZE 1024
#define CURR_SIZE (total_quads * sizeof(quad))
#define NEW_SIZE (EXPAND_SIZE * sizeof(quad) + CURR_SIZE)

// temporary variable counter
unsigned temp_counter = 0;

// scope space and offset management
unsigned program_var_offset = 0;
unsigned function_local_offset = 0;
unsigned formal_arg_offset = 0;
unsigned scope_space_counter = 1;

// array to print opcodes nicely
const char* opcode_strings[] = {
    "assign", "add", "sub", "mul", "div", "mod",  // Print "div" even though enum is op_div
    "uminus", "and", "or", "not", "if_eq", "if_noteq",
    "if_lesseq", "if_greatereq", "if_less", "if_greater",
    "call", "param", "ret", "getretval", "funcstart",
    "funcend", "tablecreate", "tablegetelem", "tablesetelem",
    "jump"
};

// emit a new quad
void emit_quad(enum iopcode op, expr* arg1, expr* arg2, expr* result, unsigned label, unsigned line) {
    if (curr_quad == total_quads) {
        quad* new_array = (quad*)malloc(NEW_SIZE);
        if (quad_array) {
            memcpy(new_array, quad_array, CURR_SIZE);
            free(quad_array);
        }
        quad_array = new_array;
        total_quads += EXPAND_SIZE;
    }

    quad* q = quad_array + curr_quad;
    q->op = op;
    q->arg1 = arg1;
    q->arg2 = arg2;
    q->result = result;
    q->label = label;
    q->line = line;
    curr_quad++;
}

unsigned next_quad_label() {
    return curr_quad;
}

// expression constructors
expr* make_expr(enum expr_t t) {
    expr* e = (expr*)malloc(sizeof(expr));
    memset(e, 0, sizeof(expr));
    e->type = t;
    return e;
}

expr* make_const_num_expr(double v) {
    expr* e = make_expr(constnum_e);
    e->numConst = v;
    return e;
}

expr* make_const_bool_expr(unsigned char b) {
    expr* e = make_expr(constbool_e);
    e->boolConst = b;
    return e;
}

expr* make_const_string_expr(char* s) {
    expr* e = make_expr(conststring_e);
    e->strConst = strdup(s);
    return e;
}

// temporary variables logic
char* create_temp_name() {
    char* buf = (char*)malloc(32);
    sprintf(buf, "_t%u", temp_counter++);
    return buf;
}

void reset_temp_counter() {
    temp_counter = 0;
}

symrec* create_temp_var(int scope, int line) {
    char* name = create_temp_name();
    symrec* sym = lookup_in_scope(name, scope);
    if (sym == NULL) {
        sym = add_symbol(name, SYM_LOCAL_VAR, scope, line);
        sym->space = get_curr_scopespace();
        sym->offset = get_curr_offset();
        inc_curr_offset();
    } else {
        free(name);
    }
    return sym;
}

// space and offset management
enum scopespace_t get_curr_scopespace() {
    if (scope_space_counter == 1) return programvar;
    if (scope_space_counter % 2 == 0) return formalarg;
    return functionlocal;
}

unsigned get_curr_offset() {
    switch (get_curr_scopespace()) {
        case programvar:    return program_var_offset;
        case functionlocal: return function_local_offset;
        case formalarg:     return formal_arg_offset;
        default:            return 0;
    }
}

void inc_curr_offset() {
    switch (get_curr_scopespace()) {
        case programvar:    program_var_offset++; break;
        case functionlocal: function_local_offset++; break;
        case formalarg:     formal_arg_offset++; break;
    }
}

void enter_scopespace() { scope_space_counter++; }
void exit_scopespace()  { scope_space_counter--; }

void reset_formal_args_offset() { formal_arg_offset = 0; }
void reset_function_locals_offset() { function_local_offset = 0; }

// backpatching lists logic
bp_list* make_bplist(unsigned label) {
    bp_list* node = (bp_list*)malloc(sizeof(bp_list));
    node->quad_label = label;
    node->next = NULL;
    return node;
}

bp_list* merge_bplists(bp_list* l1, bp_list* l2) {
    if (!l1) return l2;
    if (!l2) return l1;
    bp_list* temp = l1;
    while (temp->next != NULL) {
        temp = temp->next;
    }
    temp->next = l2;
    return l1;
}

void patch_list(bp_list* l, unsigned label) {
    while (l != NULL) {
        if (l->quad_label < curr_quad) {
            quad_array[l->quad_label].label = label;
        }
        bp_list* temp = l;
        l = l->next;
        free(temp);
    }
}

// print helper for expression names
void print_expr(FILE* f, expr* e) {
    if (!e) return;
    switch (e->type) {
        case var_e:
        case tableitem_e:
        case programfunc_e:
        case libraryfunc_e:
        case arithexpr_e:
        case boolexpr_e:
        case assignexpr_e:
        case newtable_e:
            if (e->sym) fprintf(f, "%s", e->sym->s_name);
            break;
        case constnum_e:
            if (e->numConst == (double)(long long)e->numConst)
                fprintf(f, "%lld", (long long)e->numConst);
            else
                fprintf(f, "%.3f", e->numConst);
            break;
        case constbool_e:   fprintf(f, "%s", e->boolConst ? "true" : "false"); break;
        case conststring_e: fprintf(f, "\"%s\"", e->strConst); break;
        case nil_e:         fprintf(f, "'nil'"); break;
        default:            break;
    }
}

// output to quads.txt
void print_quads() {
    FILE* f = fopen("quads.txt", "w");
    if (!f) return;

    fprintf(f, "quad#\topcode\t\tresult\t\targ1\t\targ2\t\tlabel\t\tline\n");
    fprintf(f, "---------------------------------------------------------------------------------\n");

    for (unsigned i = 0; i < curr_quad; i++) {
        quad q = quad_array[i];
        fprintf(f, "%u:\t%-15s\t", i+1, opcode_strings[q.op]);

        // result
        if (q.result) { print_expr(f, q.result); fprintf(f, "\t\t"); }
        else fprintf(f, "\t\t");

        // arg1
        if (q.arg1) { print_expr(f, q.arg1); fprintf(f, "\t\t"); }
        else fprintf(f, "\t\t");

        // arg2
        if (q.arg2) { print_expr(f, q.arg2); fprintf(f, "\t\t"); }
        else fprintf(f, "\t\t");

        // label
        if (q.op == jump || (q.op >= if_eq && q.op <= if_greater)) {
            fprintf(f, "%u\t\t", q.label + 1);
        } else {
            fprintf(f, "\t\t");
        }

        // line
        fprintf(f, "%u", q.line);

        fprintf(f, "\n");
    }
    fclose(f);
}
