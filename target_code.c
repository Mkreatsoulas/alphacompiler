#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "target_code.h"

// instruction array
instruction instructions[MAX_INSTRUCTIONS];
unsigned    curr_instruction = 0;

// constant tables
char*    const_strings[MAX_CONST_STRINGS];
unsigned total_strings = 0;

double   const_numbers[MAX_CONST_NUMBERS];
unsigned total_numbers = 0;

userfunc user_funcs[MAX_USER_FUNCS];
unsigned total_user_funcs = 0;

char*    lib_funcs_used[MAX_LIB_FUNCS];
unsigned total_lib_funcs = 0;

// incomplete jump tracking
typedef struct incomplete_jump {
    unsigned instrNo;
    unsigned iaddress;
    struct incomplete_jump* next;
} incomplete_jump;

static incomplete_jump* jump_list = NULL;

// per-function stack for patching funcstart jumps and returns
#define FUNCSTACK_SIZE 256
#define RETLIST_SIZE   2048

typedef struct func_frame {
    unsigned jump_instr;
    unsigned ret_list[RETLIST_SIZE];
    unsigned ret_count;
    unsigned ufidx;
} func_frame;

static func_frame func_stack[FUNCSTACK_SIZE];
static int        func_stack_top = -1;

//  constant table helpers functions

unsigned consts_newstring(const char* s) {
    for (unsigned i = 0; i < total_strings; i++) {
        if (strcmp(const_strings[i], s) == 0) return i;
    }
    const_strings[total_strings] = strdup(s);
    return total_strings++;
}

unsigned consts_newnumber(double n) {
    for (unsigned i = 0; i < total_numbers; i++) {
        if (const_numbers[i] == n) return i;
    }
    const_numbers[total_numbers] = n;
    return total_numbers++;
}

unsigned userfuncs_newfunc(symrec* sym) {
    for (unsigned i = 0; i < total_user_funcs; i++) {
        if (user_funcs[i].sym == sym) return i;
    }
    user_funcs[total_user_funcs].address   = 0;
    user_funcs[total_user_funcs].localSize = 0;
    user_funcs[total_user_funcs].id        = strdup(sym->s_name);
    user_funcs[total_user_funcs].sym       = sym;
    return total_user_funcs++;
}

void userfuncs_setaddress(symrec* sym, unsigned addr) {
    for (unsigned i = 0; i < total_user_funcs; i++) {
        if (user_funcs[i].sym == sym) { user_funcs[i].address = addr; return; }
    }
    // create new entry if not found
    unsigned idx = userfuncs_newfunc(sym);
    user_funcs[idx].address = addr;
}

void userfuncs_setlocals(symrec* sym, unsigned locals) {
    for (unsigned i = 0; i < total_user_funcs; i++) {
        if (user_funcs[i].sym == sym) { user_funcs[i].localSize = locals; return; }
    }
}

unsigned libfuncs_newused(const char* name) {
    for (unsigned i = 0; i < total_lib_funcs; i++) {
        if (strcmp(lib_funcs_used[i], name) == 0) return i;
    }
    lib_funcs_used[total_lib_funcs] = strdup(name);
    return total_lib_funcs++;
}

// instruction emission

static void emit_instr(instruction i) {
    assert(curr_instruction < MAX_INSTRUCTIONS);
    instructions[curr_instruction++] = i;
}

static void add_incomplete_jump(unsigned instrNo, unsigned iaddress) {
    incomplete_jump* ij = (incomplete_jump*)malloc(sizeof(incomplete_jump));
    ij->instrNo  = instrNo;
    ij->iaddress = iaddress;
    ij->next     = jump_list;
    jump_list    = ij;
}

static void patch_incomplete_jumps() {
    incomplete_jump* ij = jump_list;
    while (ij) {
        unsigned target = 0;
        if (ij->iaddress < curr_quad)
            target = quad_array[ij->iaddress].taddress;
        else
            target = curr_instruction;
        instructions[ij->instrNo].result.val = target;
        incomplete_jump* tmp = ij;
        ij = ij->next;
        free(tmp);
    }
    jump_list = NULL;
}

static void push_func_frame(unsigned jump_instr, unsigned ufidx) {
    assert(func_stack_top + 1 < FUNCSTACK_SIZE);
    func_stack_top++;
    func_stack[func_stack_top].jump_instr = jump_instr;
    func_stack[func_stack_top].ret_count  = 0;
    func_stack[func_stack_top].ufidx      = ufidx;
}

static void add_ret_to_frame(unsigned jump_instr) {
    func_frame* f = &func_stack[func_stack_top];
    assert(f->ret_count < RETLIST_SIZE);
    f->ret_list[f->ret_count++] = jump_instr;
}

static void pop_func_frame(unsigned jump_target, unsigned ret_target) {
    func_frame* f = &func_stack[func_stack_top];
    // funcstart jump skips past funcexit
    instructions[f->jump_instr].result.val = jump_target;
    // ret jumps go TO funcexit so execute_funcexit runs
    for (unsigned i = 0; i < f->ret_count; i++)
        instructions[f->ret_list[i]].result.val = ret_target;
    func_stack_top--;
}

// make_operand convert expr* to vmarg

static void make_operand(expr* e, vmarg* arg) {
    if (!e) { arg->type = nil_a; arg->val = 0; return; }
    switch (e->type) {
        case var_e:
        case tableitem_e:
        case arithexpr_e:
        case boolexpr_e:
        case assignexpr_e:
        case newtable_e:
            assert(e->sym);
            // library and user functions need special arg types
            if (e->sym->s_type == SYM_LIB_FUNC) {
                arg->type = libfunc_a;
                arg->val  = libfuncs_newused(e->sym->s_name);
            } else if (e->sym->s_type == SYM_USER_FUNC) {
                arg->type = userfunc_a;
                arg->val  = userfuncs_newfunc(e->sym);
            } else {
                switch (e->sym->space) {
                    case programvar:    arg->type = global_a; break;
                    case functionlocal: arg->type = local_a;  break;
                    case formalarg:     arg->type = formal_a; break;
                    default:            arg->type = global_a; break;
                }
                arg->val = e->sym->offset;
            }
            break;
        case constnum_e:
            arg->type = number_a;
            arg->val  = consts_newnumber(e->numConst);
            break;
        case constbool_e:
            arg->type = bool_a;
            arg->val  = (unsigned)e->boolConst;
            break;
        case conststring_e:
            arg->type = string_a;
            arg->val  = consts_newstring(e->strConst);
            break;
        case nil_e:
            arg->type = nil_a;
            arg->val  = 0;
            break;
        case programfunc_e:
            arg->type = userfunc_a;
            arg->val  = userfuncs_newfunc(e->sym);
            break;
        case libraryfunc_e:
            arg->type = libfunc_a;
            arg->val  = libfuncs_newused(e->sym->s_name);
            break;
        default:
            arg->type = nil_a;
            arg->val  = 0;
            break;
    }
}

static vmarg retval_vmarg = { retval_a, 0 };

// generate_* functions

static void generate_assign(quad* q) {
    q->taddress = curr_instruction;
    instruction i;
    i.opcode   = assign_v;
    make_operand(q->result, &i.result);
    make_operand(q->arg1,   &i.arg1);
    i.arg2.type = nil_a; i.arg2.val = 0;
    i.srcLine  = q->line;
    emit_instr(i);
}

static void generate_arith(quad* q, enum vmopcode op) {
    q->taddress = curr_instruction;
    instruction i;
    i.opcode  = op;
    make_operand(q->result, &i.result);
    make_operand(q->arg1,   &i.arg1);
    make_operand(q->arg2,   &i.arg2);
    i.srcLine = q->line;
    emit_instr(i);
}

static void generate_uminus(quad* q) {
    q->taddress = curr_instruction;
    instruction i;
    i.opcode  = mul_v;
    make_operand(q->result, &i.result);
    make_operand(q->arg1,   &i.arg1);
    // multiply by -1
    i.arg2.type = number_a;
    i.arg2.val  = consts_newnumber(-1);
    i.srcLine   = q->line;
    emit_instr(i);
}

static void generate_relop(quad* q, enum vmopcode op) {
    q->taddress = curr_instruction;
    instruction i;
    i.opcode = op;
    // label = jump target
    i.result.type = label_a;
    i.result.val  = 0;
    make_operand(q->arg1, &i.arg1);
    make_operand(q->arg2, &i.arg2);
    i.srcLine = q->line;
    emit_instr(i);
    add_incomplete_jump(curr_instruction - 1, q->label);
}

static void generate_jump(quad* q) {
    q->taddress = curr_instruction;
    instruction i;
    i.opcode      = jump_v;
    i.result.type = label_a;
    i.result.val  = 0;
    i.arg1.type   = nil_a; i.arg1.val = 0;
    i.arg2.type   = nil_a; i.arg2.val = 0;
    i.srcLine     = q->line;
    emit_instr(i);
    add_incomplete_jump(curr_instruction - 1, q->label);
}

static void generate_call(quad* q) {
    q->taddress = curr_instruction;
    instruction i;
    i.opcode  = call_v;
    make_operand(q->arg1, &i.result);  // function being called
    i.arg1.type = nil_a; i.arg1.val = 0;
    i.arg2.type = nil_a; i.arg2.val = 0;
    i.srcLine   = q->line;
    emit_instr(i);
}

static void generate_param(quad* q) {
    q->taddress = curr_instruction;
    instruction i;
    i.opcode = pusharg_v;
    make_operand(q->arg1, &i.result);
    i.arg1.type = nil_a; i.arg1.val = 0;
    i.arg2.type = nil_a; i.arg2.val = 0;
    i.srcLine   = q->line;
    emit_instr(i);
}

static void generate_ret(quad* q) {
    q->taddress = curr_instruction;
    // if there is a return value, assign it to retval
    if (q->result) {
        instruction ai;
        ai.opcode     = assign_v;
        ai.result     = retval_vmarg;
        make_operand(q->result, &ai.arg1);
        ai.arg2.type  = nil_a; ai.arg2.val = 0;
        ai.srcLine    = q->line;
        emit_instr(ai);
    }
    // jump to funcend patched by generate_funcend
    instruction ji;
    ji.opcode      = jump_v;
    ji.result.type = label_a;
    ji.result.val  = 0;
    ji.arg1.type   = nil_a; ji.arg1.val = 0;
    ji.arg2.type   = nil_a; ji.arg2.val = 0;
    ji.srcLine     = q->line;
    emit_instr(ji);
    add_ret_to_frame(curr_instruction - 1);
}

static void generate_getretval(quad* q) {
    q->taddress = curr_instruction;
    instruction i;
    i.opcode    = getretval_v;
    make_operand(q->result, &i.result);
    i.arg1.type = nil_a; i.arg1.val = 0;
    i.arg2.type = nil_a; i.arg2.val = 0;
    i.srcLine   = q->line;
    emit_instr(i);
}

static void generate_funcstart(quad* q) {
    q->taddress = curr_instruction;

    // get or create the userfunc entry
    unsigned ufidx = userfuncs_newfunc(q->result->sym);

    // emit jump over function body label patched by generate_funcend
    instruction ji;
    ji.opcode      = jump_v;
    ji.result.type = label_a;
    ji.result.val  = 0;
    ji.arg1.type   = nil_a; ji.arg1.val = 0;
    ji.arg2.type   = nil_a; ji.arg2.val = 0;
    ji.srcLine     = q->line;
    emit_instr(ji);
    unsigned jump_idx = curr_instruction - 1;

    // record the funcenter instruction address
    user_funcs[ufidx].address = curr_instruction;

    // emit funcenter
    instruction fi;
    fi.opcode      = funcenter_v;
    fi.result.type = userfunc_a;
    fi.result.val  = ufidx;
    fi.arg1.type   = nil_a; fi.arg1.val = 0;
    fi.arg2.type   = nil_a; fi.arg2.val = 0;
    fi.srcLine     = q->line;
    emit_instr(fi);

    push_func_frame(jump_idx, ufidx);
}

static void generate_funcend(quad* q) {
    q->taddress = curr_instruction;

    // set localSize for this function
    unsigned ufidx = func_stack[func_stack_top].ufidx;
    user_funcs[ufidx].localSize = q->result->sym->total_locals;

    // emit funcexit
    instruction fi;
    fi.opcode      = funcexit_v;
    fi.result.type = userfunc_a;
    fi.result.val  = ufidx;
    fi.arg1.type   = nil_a; fi.arg1.val = 0;
    fi.arg2.type   = nil_a; fi.arg2.val = 0;
    fi.srcLine     = q->line;
    emit_instr(fi);

    unsigned funcexit_idx   = curr_instruction - 1;
    unsigned after_funcexit = curr_instruction;
    // funcstart jump after funcexit, ret jumps to funcexit
    pop_func_frame(after_funcexit, funcexit_idx);
}

static void generate_tablecreate(quad* q) {
    q->taddress = curr_instruction;
    instruction i;
    i.opcode  = newtable_v;
    make_operand(q->result, &i.result);
    i.arg1.type = nil_a; i.arg1.val = 0;
    i.arg2.type = nil_a; i.arg2.val = 0;
    i.srcLine   = q->line;
    emit_instr(i);
}

static void generate_tablegetelem(quad* q) {
    q->taddress = curr_instruction;
    instruction i;
    i.opcode  = tablegetelem_v;
    make_operand(q->result, &i.result);
    make_operand(q->arg1,   &i.arg1);   // table
    make_operand(q->arg2,   &i.arg2);   // key
    i.srcLine = q->line;
    emit_instr(i);
}

static void generate_tablesetelem(quad* q) {
    q->taddress = curr_instruction;
    instruction i;
    i.opcode  = tablesetelem_v;
    make_operand(q->result, &i.result);  // table
    make_operand(q->arg1,   &i.arg1);    // key
    make_operand(q->arg2,   &i.arg2);    // value
    i.srcLine = q->line;
    emit_instr(i);
}

// dispatcher

typedef void (*generate_func_t)(quad*);

static generate_func_t generate_funcs[] = {
    generate_assign,                        // assign
    [add]       = (void(*)(quad*))NULL,     // handled below
    [sub]       = (void(*)(quad*))NULL,
    [mul]       = (void(*)(quad*))NULL,
    [op_div]    = (void(*)(quad*))NULL,
    [mod]       = (void(*)(quad*))NULL,
    [uminus]    = generate_uminus,
    [and]       = NULL,
    [or]        = NULL,
    [not]       = NULL,
    [if_eq]     = NULL,
    [if_noteq]  = NULL,
    [if_lesseq] = NULL,
    [if_greatereq] = NULL,
    [if_less]   = NULL,
    [if_greater] = NULL,
    [call]      = generate_call,
    [param]     = generate_param,
    [ret]       = generate_ret,
    [getretval] = generate_getretval,
    [funcstart] = generate_funcstart,
    [funcend]   = generate_funcend,
    [tablecreate]   = generate_tablecreate,
    [tablegetelem]  = generate_tablegetelem,
    [tablesetelem]  = generate_tablesetelem,
    [jump]      = generate_jump
};

//  main generate_all

void generate_all() {
    for (unsigned i = 0; i < curr_quad; i++) {
        quad* q = &quad_array[i];
        switch (q->op) {
            case assign:        generate_assign(q);                       break;
            case add:           generate_arith(q, add_v);                 break;
            case sub:           generate_arith(q, sub_v);                 break;
            case mul:           generate_arith(q, mul_v);                 break;
            case op_div:        generate_arith(q, div_v);                 break;
            case mod:           generate_arith(q, mod_v);                 break;
            case uminus:        generate_uminus(q);                       break;
            case and:           q->taddress = curr_instruction;           break;
            case or:            q->taddress = curr_instruction;           break;
            case not:           q->taddress = curr_instruction;           break;
            case if_eq:         generate_relop(q, jeq_v);                 break;
            case if_noteq:      generate_relop(q, jne_v);                 break;
            case if_lesseq:     generate_relop(q, jle_v);                 break;
            case if_greatereq:  generate_relop(q, jge_v);                 break;
            case if_less:       generate_relop(q, jlt_v);                 break;
            case if_greater:    generate_relop(q, jgt_v);                 break;
            case call:          generate_call(q);                         break;
            case param:         generate_param(q);                        break;
            case ret:           generate_ret(q);                          break;
            case getretval:     generate_getretval(q);                    break;
            case funcstart:     generate_funcstart(q);                    break;
            case funcend:       generate_funcend(q);                      break;
            case tablecreate:   generate_tablecreate(q);                  break;
            case tablegetelem:  generate_tablegetelem(q);                 break;
            case tablesetelem:  generate_tablesetelem(q);                 break;
            case jump:          generate_jump(q);                         break;
            default: break;
        }
    }
    patch_incomplete_jumps();
}

// binary output 

static void write_u32(FILE* f, unsigned v) { fwrite(&v, sizeof(unsigned), 1, f); }
static void write_u8(FILE* f, unsigned char v) { fwrite(&v, 1, 1, f); }
static void write_str(FILE* f, const char* s) {
    unsigned len = (unsigned)strlen(s) + 1;
    fwrite(s, 1, len, f);
}
static void write_dbl(FILE* f, double d) { fwrite(&d, sizeof(double), 1, f); }

void write_binary(const char* filename) {
    FILE* f = fopen(filename, "wb");
    if (!f) { perror("write_binary"); return; }

    write_u32(f, MAGIC_NUMBER);
    write_u32(f, program_var_offset);  // total globals

    // strings
    write_u32(f, total_strings);
    for (unsigned i = 0; i < total_strings; i++) write_str(f, const_strings[i]);

    // numbers
    write_u32(f, total_numbers);
    for (unsigned i = 0; i < total_numbers; i++) write_dbl(f, const_numbers[i]);

    // user functions
    write_u32(f, total_user_funcs);
    for (unsigned i = 0; i < total_user_funcs; i++) {
        write_u32(f, user_funcs[i].address);
        write_u32(f, user_funcs[i].localSize);
        write_str(f, user_funcs[i].id);
    }

    // library functions used
    write_u32(f, total_lib_funcs);
    for (unsigned i = 0; i < total_lib_funcs; i++) write_str(f, lib_funcs_used[i]);

    // instructions
    write_u32(f, curr_instruction);
    for (unsigned i = 0; i < curr_instruction; i++) {
        instruction* instr = &instructions[i];
        write_u8(f, (unsigned char)instr->opcode);
        write_u8(f, (unsigned char)instr->result.type);
        write_u32(f, instr->result.val);
        write_u8(f, (unsigned char)instr->arg1.type);
        write_u32(f, instr->arg1.val);
        write_u8(f, (unsigned char)instr->arg2.type);
        write_u32(f, instr->arg2.val);
        write_u32(f, instr->srcLine);
    }

    fclose(f);
}

// text output

static const char* opcode_names[] = {
    "assign", "add", "sub", "mul", "div", "mod",
    "jeq", "jne", "jle", "jge", "jlt", "jgt",
    "call", "pusharg",
    "ret", "getretval",
    "funcenter", "funcexit",
    "newtable", "tablegetelem", "tablesetelem",
    "jump"
};

static const char* argtype_names[] = {
    "label", "global", "local", "formal",
    "number", "string", "bool", "nil",
    "userfunc", "libfunc", "retval"
};

static void print_vmarg(FILE* f, vmarg* a) {
    if (a->type == nil_a)    { fprintf(f, "-"); return; }
    if (a->type == retval_a) { fprintf(f, "retval"); return; }
    if (a->type == bool_a)   { fprintf(f, "%s", a->val ? "true" : "false"); return; }
    if (a->type == label_a)  { fprintf(f, "L%u", a->val); return; }
    if (a->type == number_a) {
        double v = const_numbers[a->val];
        if (v == (double)(long long)v) fprintf(f, "%lld", (long long)v);
        else fprintf(f, "%.3f", v);
        return;
    }
    if (a->type == string_a)   { fprintf(f, "\"%s\"", const_strings[a->val]); return; }
    if (a->type == userfunc_a) { fprintf(f, "func:%s", user_funcs[a->val].id); return; }
    if (a->type == libfunc_a)  { fprintf(f, "lib:%s", lib_funcs_used[a->val]); return; }
    fprintf(f, "%s[%u]", argtype_names[a->type], a->val);
}

void write_text(const char* filename) {
    FILE* f = fopen(filename, "w");
    if (!f) { perror("write_text"); return; }

    fprintf(f, " target code instructions\n");
    fprintf(f, "%-6s %-14s %-20s %-20s %-20s %s\n",
            "no", "opcode", "result", "arg1", "arg2", "line");
    fprintf(f, "-------------------------------------------------------------\n");

    for (unsigned i = 0; i < curr_instruction; i++) {
        instruction* instr = &instructions[i];
        fprintf(f, "%-6u %-14s ", i, opcode_names[instr->opcode]);
        char buf[64];
        FILE* tmp;
        // print result
        if (instr->result.type == nil_a) fprintf(f, "%-20s", "-");
        else { char r[64]; sprintf(r, "%s[%u]", argtype_names[instr->result.type], instr->result.val); fprintf(f, "%-20s", r); }
        if (instr->arg1.type == nil_a) fprintf(f, "%-20s", "-");
        else { char r[64]; sprintf(r, "%s[%u]", argtype_names[instr->arg1.type], instr->arg1.val); fprintf(f, "%-20s", r); }
        if (instr->arg2.type == nil_a) fprintf(f, "%-20s", "-");
        else { char r[64]; sprintf(r, "%s[%u]", argtype_names[instr->arg2.type], instr->arg2.val); fprintf(f, "%-20s", r); }
        fprintf(f, "%u\n", instr->srcLine);
        (void)buf; (void)tmp;
    }

    fprintf(f, "\nconstant tables\n");
    fprintf(f, "globals: %u\n", program_var_offset);
    fprintf(f, "strings (%u):\n", total_strings);
    for (unsigned i = 0; i < total_strings; i++) fprintf(f, "  [%u] \"%s\"\n", i, const_strings[i]);
    fprintf(f, "numbers (%u):\n", total_numbers);
    for (unsigned i = 0; i < total_numbers; i++) {
        if (const_numbers[i] == (double)(long long)const_numbers[i])
            fprintf(f, "  [%u] %lld\n", i, (long long)const_numbers[i]);
        else
            fprintf(f, "  [%u] %.6f\n", i, const_numbers[i]);
    }
    fprintf(f, "userfuncs (%u):\n", total_user_funcs);
    for (unsigned i = 0; i < total_user_funcs; i++)
        fprintf(f, "  [%u] id=%s addr=%u locals=%u\n", i,
                user_funcs[i].id, user_funcs[i].address, user_funcs[i].localSize);
    fprintf(f, "libfuncs (%u):\n", total_lib_funcs);
    for (unsigned i = 0; i < total_lib_funcs; i++)
        fprintf(f, "  [%u] %s\n", i, lib_funcs_used[i]);

    fclose(f);
}
