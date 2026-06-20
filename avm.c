#include <stdarg.h>
#include "avm.h"

//vm state
avm_memcell  stack[AVM_STACKSIZE];
unsigned     top             = AVM_STACKSIZE - 1;
unsigned     topsp           = AVM_STACKSIZE - 1;
unsigned     pc              = 0;
int          executionFinished = 0;
avm_memcell  retval_reg;
unsigned     totalActuals    = 0;

//  constant table
char**       avm_strings     = NULL;
unsigned     avm_total_strings = 0;
double*      avm_numbers     = NULL;
unsigned     avm_total_numbers = 0;
avm_userfunc* avm_user_funcs = NULL;
unsigned     avm_total_user_funcs = 0;
char**       avm_lib_funcs   = NULL;
unsigned     avm_total_lib_funcs  = 0;
unsigned     avm_total_globals    = 0;

instruction* code     = NULL;
unsigned     codeSize = 0;

//  helper for errors
void avm_error(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[runtime error] (instr %u, line %u): ", pc,
            (pc < codeSize) ? code[pc].srcLine : 0);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    executionFinished = 1;
}

// memcell helper

void avm_memcell_clear(avm_memcell* m) {
    if (m->type == string_m && m->data.strVal) {
        free(m->data.strVal);
        m->data.strVal = NULL;
    } else if (m->type == table_m && m->data.tableVal) {
        avm_table_decref(m->data.tableVal);
        m->data.tableVal = NULL;
    }
    m->type = undef_m;
}

void avm_assign(avm_memcell* lv, avm_memcell* rv) {
    if (lv == rv) return;
    if (lv->type == table_m && lv->data.tableVal == rv->data.tableVal &&
        rv->type == table_m) return;
    avm_memcell_clear(lv);
    *lv = *rv;
    if (lv->type == string_m && lv->data.strVal)
        lv->data.strVal = strdup(lv->data.strVal);
    else if (lv->type == table_m && lv->data.tableVal)
        avm_table_incref(lv->data.tableVal);
}

// forward declaration for recursive table printing
static char* table_tostring(avm_table* t, int depth);

// dynamic string buffer helpers
typedef struct { char* s; unsigned len; unsigned cap; } strbuf;

static void sb_init(strbuf* b) {
    b->cap = 64; b->len = 0;
    b->s = (char*)malloc(b->cap);
    b->s[0] = '\0';
}

static void sb_append(strbuf* b, const char* t) {
    unsigned tlen = (unsigned)strlen(t);
    if (b->len + tlen + 1 > b->cap) {
        while (b->len + tlen + 1 > b->cap) b->cap *= 2;
        b->s = (char*)realloc(b->s, b->cap);
    }
    memcpy(b->s + b->len, t, tlen + 1);
    b->len += tlen;
}

// key-value entry for sorting
typedef struct { double numKey; char* strKey; avm_memcell* val; } kv_pair;

static int cmp_num_kv(const void* a, const void* b) {
    double ka = ((const kv_pair*)a)->numKey;
    double kb = ((const kv_pair*)b)->numKey;
    return (ka > kb) - (ka < kb);
}
static int cmp_str_kv(const void* a, const void* b) {
    return strcmp(((const kv_pair*)a)->strKey, ((const kv_pair*)b)->strKey);
}

static char* cell_tostring(avm_memcell* m, int depth) {
    char buf[256];
    switch (m->type) {
        case number_m:   sprintf(buf, "%.3f", m->data.numVal); return strdup(buf);
        case string_m:   return strdup(m->data.strVal);
        case bool_m:     return strdup(m->data.boolVal ? "true" : "false");
        case nil_m:      return strdup("nil");
        case undef_m:    return strdup("undef");
        case userfunc_m:
            sprintf(buf, "%s()", avm_user_funcs[m->data.funcVal].id);
            return strdup(buf);
        case libfunc_m:
            sprintf(buf, "lib::%s", m->data.libfuncVal);
            return strdup(buf);
        case table_m:    return table_tostring(m->data.tableVal, depth);
        default:         return strdup("?");
    }
}

static char* table_tostring(avm_table* t, int depth) {
    if (!t) return strdup("[ ]");
    if (depth > 4) return strdup("[...]");

    // count non-nil numeric and string entries
    unsigned n_num = 0, n_str = 0;
    for (int i = 0; i < AVM_TABLE_HASHSIZE; i++) {
        for (avm_table_bucket* b = t->numIndexed[i]; b; b = b->next)
            if (b->value.type != nil_m) n_num++;
        for (avm_table_bucket* b = t->strIndexed[i]; b; b = b->next)
            if (b->value.type != nil_m) n_str++;
    }
    if (n_num == 0 && n_str == 0) return strdup("[ ]");

    // collect and sort
    kv_pair* nums = n_num ? (kv_pair*)malloc(n_num * sizeof(kv_pair)) : NULL;
    kv_pair* strs = n_str ? (kv_pair*)malloc(n_str * sizeof(kv_pair)) : NULL;
    unsigned ni = 0, si = 0;
    for (int i = 0; i < AVM_TABLE_HASHSIZE; i++) {
        for (avm_table_bucket* b = t->numIndexed[i]; b; b = b->next)
            if (b->value.type != nil_m)
                nums[ni++] = (kv_pair){b->key.data.numVal, NULL, &b->value};
        for (avm_table_bucket* b = t->strIndexed[i]; b; b = b->next)
            if (b->value.type != nil_m)
                strs[si++] = (kv_pair){0, b->key.data.strVal, &b->value};
    }
    if (n_num) qsort(nums, n_num, sizeof(kv_pair), cmp_num_kv);
    if (n_str) qsort(strs, n_str, sizeof(kv_pair), cmp_str_kv);

    // check if pure sequential 0-indexed numeric (array format)
    int array_fmt = (n_str == 0 && n_num > 0);
    if (array_fmt) {
        for (unsigned i = 0; i < n_num; i++)
            if (nums[i].numKey != (double)i) { array_fmt = 0; break; }
    }

    strbuf sb; sb_init(&sb);
    sb_append(&sb, "[ ");
    int first = 1;

    if (array_fmt) {
        for (unsigned i = 0; i < n_num; i++) {
            if (!first) sb_append(&sb, ", ");
            first = 0;
            char* vs = cell_tostring(nums[i].val, depth + 1);
            sb_append(&sb, vs); free(vs);
        }
    } else {
        // string keys first alphabetical
        for (unsigned i = 0; i < n_str; i++) {
            if (!first) sb_append(&sb, ", ");
            first = 0;
            sb_append(&sb, "{ ");
            sb_append(&sb, strs[i].strKey);
            sb_append(&sb, " : ");
            char* vs = cell_tostring(strs[i].val, depth + 1);
            sb_append(&sb, vs); free(vs);
            sb_append(&sb, " }");
        }
        // then numeric keys
        for (unsigned i = 0; i < n_num; i++) {
            if (!first) sb_append(&sb, ", ");
            first = 0;
            sb_append(&sb, "{ ");
            char kbuf[32];
            double k = nums[i].numKey;
            if (k == (double)(long long)k) sprintf(kbuf, "%lld", (long long)k);
            else                           sprintf(kbuf, "%.3f", k);
            sb_append(&sb, kbuf);
            sb_append(&sb, " : ");
            char* vs = cell_tostring(nums[i].val, depth + 1);
            sb_append(&sb, vs); free(vs);
            sb_append(&sb, " }");
        }
    }
    sb_append(&sb, " ]");
    if (nums) free(nums);
    if (strs) free(strs);
    return sb.s;
}

char* avm_tostring(avm_memcell* m) {
    return cell_tostring(m, 0);
}

unsigned char avm_tobool(avm_memcell* m) {
    switch (m->type) {
        case number_m:   return m->data.numVal != 0;
        case string_m:   return m->data.strVal && m->data.strVal[0] != '\0';
        case bool_m:     return m->data.boolVal;
        case nil_m:      return 0;
        case undef_m:    return 0;
        case table_m:    return 1;
        case userfunc_m: return 1;
        case libfunc_m:  return 1;
        default:         return 0;
    }
}

// table implementation

avm_table* avm_table_new() {
    avm_table* t = (avm_table*)malloc(sizeof(avm_table));
    t->refCount    = 1;
    t->total       = 0;
    t->otherIndexed = NULL;
    memset(t->numIndexed, 0, sizeof(t->numIndexed));
    memset(t->strIndexed, 0, sizeof(t->strIndexed));
    return t;
}

// equality check for non-string/on-number keys
static int other_key_eq(avm_memcell* a, avm_memcell* b) {
    if (a->type != b->type) return 0;
    switch (a->type) {
        case bool_m:     return a->data.boolVal == b->data.boolVal;
        case userfunc_m: return a->data.funcVal == b->data.funcVal;
        case libfunc_m:  return strcmp(a->data.libfuncVal, b->data.libfuncVal) == 0;
        case table_m:    return a->data.tableVal == b->data.tableVal;
        case nil_m:      return 1;
        default:         return 0;
    }
}

// copy key value into a bucket key for storage
static void bucket_key_copy(avm_memcell* dst, avm_memcell* src) {
    *dst = *src;
    if (src->type == libfunc_m && src->data.libfuncVal)
        dst->data.libfuncVal = strdup(src->data.libfuncVal);
}

void avm_table_incref(avm_table* t) { if (t) t->refCount++; }

static void avm_table_bucket_destroy(avm_table_bucket* b) {
    while (b) {
        avm_table_bucket* next = b->next;
        avm_memcell_clear(&b->key);
        avm_memcell_clear(&b->value);
        free(b);
        b = next;
    }
}

void avm_table_decref(avm_table* t) {
    if (!t) return;
    if (--t->refCount == 0) {
        for (int i = 0; i < AVM_TABLE_HASHSIZE; i++) {
            avm_table_bucket_destroy(t->numIndexed[i]);
            avm_table_bucket_destroy(t->strIndexed[i]);
        }
        avm_table_bucket_destroy(t->otherIndexed);
        free(t);
    }
}

static unsigned hash_str(const char* s) {
    unsigned h = 0;
    while (*s) h = h * 31 + (unsigned char)*s++;
    return h % AVM_TABLE_HASHSIZE;
}

static unsigned hash_num(double n) {
    unsigned long long v;
    memcpy(&v, &n, sizeof(v));
    return (unsigned)(v ^ (v >> 32)) % AVM_TABLE_HASHSIZE;
}

avm_memcell* avm_table_getelem(avm_table* t, avm_memcell* key) {
    if (!t || !key) return NULL;
    if (key->type == number_m) {
        unsigned h = hash_num(key->data.numVal);
        avm_table_bucket* b = t->numIndexed[h];
        while (b) {
            if (b->key.type == number_m && b->key.data.numVal == key->data.numVal)
                return &b->value;
            b = b->next;
        }
    } else if (key->type == string_m) {
        unsigned h = hash_str(key->data.strVal);
        avm_table_bucket* b = t->strIndexed[h];
        while (b) {
            if (b->key.type == string_m && strcmp(b->key.data.strVal, key->data.strVal) == 0)
                return &b->value;
            b = b->next;
        }
    } else {
        avm_table_bucket* b = t->otherIndexed;
        while (b) {
            if (other_key_eq(&b->key, key)) return &b->value;
            b = b->next;
        }
    }
    return NULL;
}

void avm_table_setelem(avm_table* t, avm_memcell* key, avm_memcell* val) {
    if (!t || !key) return;
    if (val && val->type == nil_m) {
        // setting to nil deletes the key
        // for simplicity mark the value nil and decrement total if present
        avm_memcell* existing = avm_table_getelem(t, key);
        if (existing && existing->type != nil_m) {
            avm_memcell_clear(existing);
            existing->type = nil_m;
            t->total--;
        }
        return;
    }
    if (key->type == number_m) {
        unsigned h = hash_num(key->data.numVal);
        avm_table_bucket* b = t->numIndexed[h];
        while (b) {
            if (b->key.type == number_m && b->key.data.numVal == key->data.numVal) {
                avm_assign(&b->value, val);
                return;
            }
            b = b->next;
        }
        avm_table_bucket* nb = (avm_table_bucket*)malloc(sizeof(avm_table_bucket));
        memset(nb, 0, sizeof(*nb));
        nb->key.type = number_m; nb->key.data.numVal = key->data.numVal;
        avm_assign(&nb->value, val);
        nb->next = t->numIndexed[h];
        t->numIndexed[h] = nb;
        t->total++;
    } else if (key->type == string_m) {
        unsigned h = hash_str(key->data.strVal);
        avm_table_bucket* b = t->strIndexed[h];
        while (b) {
            if (b->key.type == string_m && strcmp(b->key.data.strVal, key->data.strVal) == 0) {
                avm_assign(&b->value, val);
                return;
            }
            b = b->next;
        }
        avm_table_bucket* nb = (avm_table_bucket*)malloc(sizeof(avm_table_bucket));
        memset(nb, 0, sizeof(*nb));
        nb->key.type = string_m; nb->key.data.strVal = strdup(key->data.strVal);
        avm_assign(&nb->value, val);
        nb->next = t->strIndexed[h];
        t->strIndexed[h] = nb;
        t->total++;
    } else if (key->type != undef_m && key->type != nil_m) {
        // other key types store in the otherindexed list
        avm_table_bucket* b = t->otherIndexed;
        while (b) {
            if (other_key_eq(&b->key, key)) {
                avm_assign(&b->value, val);
                return;
            }
            b = b->next;
        }
        avm_table_bucket* nb = (avm_table_bucket*)malloc(sizeof(avm_table_bucket));
        memset(nb, 0, sizeof(*nb));
        bucket_key_copy(&nb->key, key);
        avm_assign(&nb->value, val);
        nb->next = t->otherIndexed;
        t->otherIndexed = nb;
        t->total++;
    }
}

// translate operand

avm_memcell* avm_translate_operand(vmarg* a, avm_memcell* reg) {
    switch (a->type) {
        case global_a:
            return &stack[AVM_STACKSIZE - 1 - a->val];
        case local_a:
            return &stack[topsp - a->val];
        case formal_a: {
            avm_memcell* nc = &stack[topsp + AVM_NUMACTUALS_OFFSET];
            unsigned numAct = (nc->type == number_m) ? (unsigned)nc->data.numVal : 0;
            if (a->val >= numAct) {
                fprintf(stderr, "error: out of stack - ask argument which is not pushed in the arguments list\n");
                executionFinished = 1;
                reg->type = undef_m;
                return reg;
            }
            return &stack[topsp + AVM_STACKENV_SIZE + a->val];
        }
        case retval_a:
            return &retval_reg;
        case number_a:
            reg->type = number_m;
            reg->data.numVal = avm_numbers[a->val];
            return reg;
        case string_a:
            reg->type = string_m;
            reg->data.strVal = avm_strings[a->val];
            return reg;
        case bool_a:
            reg->type = bool_m;
            reg->data.boolVal = (unsigned char)a->val;
            return reg;
        case nil_a:
            reg->type = nil_m;
            return reg;
        case userfunc_a:
            reg->type = userfunc_m;
            reg->data.funcVal = a->val;
            return reg;
        case libfunc_a:
            reg->type = libfunc_m;
            reg->data.libfuncVal = avm_lib_funcs[a->val];
            return reg;
        default:
            assert(0);
            return NULL;
    }
}

//  execute functions

static avm_memcell tmp1, tmp2, tmp3;

static void execute_assign(instruction* instr) {
    avm_memcell* lv = avm_translate_operand(&instr->result, &tmp1);
    avm_memcell* rv = avm_translate_operand(&instr->arg1,   &tmp2);
    avm_assign(lv, rv);
    pc++;
}

static void execute_arith(instruction* instr) {
    avm_memcell* lv  = avm_translate_operand(&instr->result, &tmp1);
    avm_memcell* rv1 = avm_translate_operand(&instr->arg1,   &tmp2);
    avm_memcell* rv2 = avm_translate_operand(&instr->arg2,   &tmp3);

    if (rv1->type != number_m || rv2->type != number_m) {
        avm_error("arithmetic on non-number");
        return;
    }
    double a = rv1->data.numVal, b = rv2->data.numVal, res = 0;
    switch (instr->opcode) {
        case add_v: res = a + b; break;
        case sub_v: res = a - b; break;
        case mul_v: res = a * b; break;
        case div_v:
            if (b == 0) { avm_error("division by zero"); return; }
            res = a / b; break;
        case mod_v:
            if ((long long)b == 0) { avm_error("modulo by zero"); return; }
            res = (double)((long long)a % (long long)b); break;
        default: break;
    }
    avm_memcell_clear(lv);
    lv->type = number_m;
    lv->data.numVal = res;
    pc++;
}

// comparison helpers
static unsigned char compare_eq(avm_memcell* a, avm_memcell* b) {
    if (a->type != b->type) {
        // nil == nil, nil != anything else
        if (a->type == nil_m && b->type == nil_m) return 1;
        return 0;
    }
    switch (a->type) {
        case number_m: {
            double diff = a->data.numVal - b->data.numVal;
            return diff >= -1e-9 && diff <= 1e-9;
        }
        case string_m:   return strcmp(a->data.strVal, b->data.strVal) == 0;
        case bool_m:     return a->data.boolVal == b->data.boolVal;
        case nil_m:      return 1;
        case userfunc_m: return a->data.funcVal == b->data.funcVal;
        case libfunc_m:  return strcmp(a->data.libfuncVal, b->data.libfuncVal) == 0;
        case table_m:    return a->data.tableVal == b->data.tableVal;
        default:         return 0;
    }
}

static void execute_jeq(instruction* instr) {
    avm_memcell* a = avm_translate_operand(&instr->arg1, &tmp1);
    avm_memcell* b = avm_translate_operand(&instr->arg2, &tmp2);
    int cond;
    // if (x) compiles to jeq x bool[1] use tobool for truthiness
    if (b->type == bool_m)      cond = (avm_tobool(a) == b->data.boolVal);
    else if (a->type == bool_m) cond = (a->data.boolVal == avm_tobool(b));
    else                        cond = compare_eq(a, b);
    if (cond) pc = instr->result.val;
    else pc++;
}
static void execute_jne(instruction* instr) {
    avm_memcell* a = avm_translate_operand(&instr->arg1, &tmp1);
    avm_memcell* b = avm_translate_operand(&instr->arg2, &tmp2);
    int cond;
    if (b->type == bool_m)      cond = (avm_tobool(a) != b->data.boolVal);
    else if (a->type == bool_m) cond = (a->data.boolVal != avm_tobool(b));
    else                        cond = !compare_eq(a, b);
    if (cond) pc = instr->result.val;
    else pc++;
}

static double numcmp_tonum(avm_memcell* m) {
    if (m->type == number_m) return m->data.numVal;
    if (m->type == bool_m)   return m->data.boolVal ? 1.0 : 0.0;
    return 0.0;
}

static void execute_numcmp(instruction* instr) {
    avm_memcell* a = avm_translate_operand(&instr->arg1, &tmp1);
    avm_memcell* b = avm_translate_operand(&instr->arg2, &tmp2);
    if ((a->type != number_m && a->type != bool_m) ||
        (b->type != number_m && b->type != bool_m)) {
        avm_error("relational operator on non-number");
        return;
    }
    double va = numcmp_tonum(a), vb = numcmp_tonum(b);
    int cond = 0;
    switch (instr->opcode) {
        case jle_v: cond = va <= vb; break;
        case jge_v: cond = va >= vb; break;
        case jlt_v: cond = va <  vb; break;
        case jgt_v: cond = va >  vb; break;
        default: break;
    }
    if (cond) pc = instr->result.val;
    else pc++;
}

static void execute_jump(instruction* instr) {
    pc = instr->result.val;
}

// call,push,enter,exit

static void execute_pusharg(instruction* instr) {
    avm_memcell* arg = avm_translate_operand(&instr->result, &tmp1);
    avm_assign(&stack[top], arg);
    top--;
    totalActuals++;
    pc++;
}

static void execute_call(instruction* instr);  // forward

static void execute_funcenter(instruction* instr) {
    // instr->result is userfunc_a with the function index
    unsigned ufidx    = instr->result.val;
    unsigned localSz  = avm_user_funcs[ufidx].localSize;
    // make space for locals
    if (top < localSz) {
        fprintf(stderr, "run time error: Stack overflow!\n");
        executionFinished = 1;
        return;
    }
    top -= localSz;
    // initialize locals to undef
    for (unsigned i = 0; i < localSz; i++) {
        stack[topsp - i].type = undef_m;
    }
    pc++;
}

static void execute_funcexit(instruction* instr) {
    unsigned old_top   = (unsigned)stack[topsp + AVM_SAVEDTOP_OFFSET].data.numVal;
    unsigned old_topsp = (unsigned)stack[topsp + AVM_SAVEDTOPSP_OFFSET].data.numVal;
    unsigned ret_pc    = (unsigned)stack[topsp + AVM_SAVEDPC_OFFSET].data.numVal;
    top   = old_top;
    topsp = old_topsp;
    pc    = ret_pc;
}

static void execute_call(instruction* instr) {
    avm_memcell* func = avm_translate_operand(&instr->result, &tmp1);

    //  table with () key is callable
    if (func->type == table_m) {
        avm_memcell fnkey;
        fnkey.type = string_m;
        fnkey.data.strVal = "()";
        avm_memcell* fn = avm_table_getelem(func->data.tableVal, &fnkey);
        if (!fn || (fn->type != userfunc_m && fn->type != libfunc_m)) {
            avm_error("call on table without '()' method");
            return;
        }
        // push the table itself as first argument formal[0]
        avm_assign(&stack[top], func);
        top--;
        totalActuals++;
        // replace func pointer to point to the () function
        if (fn->type == userfunc_m) {
            tmp1.type = userfunc_m;
            tmp1.data.funcVal = fn->data.funcVal;
        } else {
            tmp1.type = libfunc_m;
            tmp1.data.libfuncVal = fn->data.libfuncVal;
        }
        func = &tmp1;
    }

    if (func->type == userfunc_m) {
        unsigned ufidx   = func->data.funcVal;
        unsigned savedTop = top + totalActuals;  // before pushargs

        // stack overflow check need 4 env slots
        if (top < 4) {
            fprintf(stderr, "run time error: Stack overflow!\n");
            executionFinished = 1;
            return;
        }

        // push saved pc
        stack[top].type = number_m;
        stack[top].data.numVal = (double)(pc + 1);
        top--;

        // push saved top
        stack[top].type = number_m;
        stack[top].data.numVal = (double)savedTop;
        top--;

        // push saved topsp
        stack[top].type = number_m;
        stack[top].data.numVal = (double)topsp;
        top--;

        // push numactuals
        stack[top].type = number_m;
        stack[top].data.numVal = (double)totalActuals;
        top--;

        topsp        = top;
        totalActuals = 0;
        pc           = avm_user_funcs[ufidx].address;  // points to funcenter
    } else if (func->type == libfunc_m) {
        avm_calllibfunc(func->data.libfuncVal);
        pc++;
    } else if (func->type == string_m) {
        // calling a string look up as library function name
        avm_calllibfunc(func->data.strVal);
        pc++;
    } else {
        char* vs = avm_tostring(func);
        unsigned ln = (pc < codeSize) ? code[pc].srcLine : 0;
        fprintf(stderr, "error: line %u: call: cannot bind '%s' to function!\n", ln, vs);
        free(vs);
        executionFinished = 1;
    }
}

static void execute_getretval(instruction* instr) {
    avm_memcell* lv = avm_translate_operand(&instr->result, &tmp1);
    avm_assign(lv, &retval_reg);
    pc++;
}

static void execute_newtable(instruction* instr) {
    avm_memcell* lv = avm_translate_operand(&instr->result, &tmp1);
    avm_memcell_clear(lv);
    lv->type = table_m;
    lv->data.tableVal = avm_table_new();
    pc++;
}

static void execute_tablegetelem(instruction* instr) {
    avm_memcell* lv  = avm_translate_operand(&instr->result, &tmp1);
    avm_memcell* tbl = avm_translate_operand(&instr->arg1,   &tmp2);
    avm_memcell* key = avm_translate_operand(&instr->arg2,   &tmp3);

    if (tbl->type != table_m) { avm_error("tablegetelem on non-table"); return; }
    avm_memcell* val = avm_table_getelem(tbl->data.tableVal, key);
    avm_memcell_clear(lv);
    if (val) avm_assign(lv, val);
    else     lv->type = nil_m;
    pc++;
}

static void execute_tablesetelem(instruction* instr) {
    avm_memcell* tbl = avm_translate_operand(&instr->result, &tmp1);
    avm_memcell* key = avm_translate_operand(&instr->arg1,   &tmp2);
    avm_memcell* val = avm_translate_operand(&instr->arg2,   &tmp3);

    if (tbl->type != table_m) { avm_error("tablesetelem on non-table"); return; }
    // warn when deleting an existing string key set to nil
    if (val->type == nil_m && key->type == string_m) {
        avm_memcell* existing = avm_table_getelem(tbl->data.tableVal, key);
        if (existing && existing->type != nil_m) {
            printf("WARNING: table[%s] not found! at line %u\n", key->data.strVal, code[pc].srcLine);
        }
    }
    avm_table_setelem(tbl->data.tableVal, key, val);
    pc++;
}

static void execute_nop(instruction* instr) { pc++; }

//  execute dispatch table

typedef void (*execute_func_t)(instruction*);

static execute_func_t executeFuncs[AVM_MAX_INSTRUCTIONS] = {
    [assign_v]       = execute_assign,
    [add_v]          = execute_arith,
    [sub_v]          = execute_arith,
    [mul_v]          = execute_arith,
    [div_v]          = execute_arith,
    [mod_v]          = execute_arith,
    [jeq_v]          = execute_jeq,
    [jne_v]          = execute_jne,
    [jle_v]          = execute_numcmp,
    [jge_v]          = execute_numcmp,
    [jlt_v]          = execute_numcmp,
    [jgt_v]          = execute_numcmp,
    [call_v]         = execute_call,
    [pusharg_v]      = execute_pusharg,
    [ret_v]          = execute_nop,
    [getretval_v]    = execute_getretval,
    [funcenter_v]    = execute_funcenter,
    [funcexit_v]     = execute_funcexit,
    [newtable_v]     = execute_newtable,
    [tablegetelem_v] = execute_tablegetelem,
    [tablesetelem_v] = execute_tablesetelem,
    [jump_v]         = execute_jump
};

void execute_cycle() {
    if (pc >= codeSize) { executionFinished = 1; return; }
    instruction* instr = &code[pc];
    assert(instr->opcode < AVM_MAX_INSTRUCTIONS);
    execute_func_t fn = executeFuncs[instr->opcode];
    assert(fn);
    fn(instr);
}

// library functions

static void libfunc_print() {
    for (unsigned i = 1; i <= totalActuals; i++) {
        avm_memcell* arg = &stack[top + i];
        char* s = avm_tostring(arg);
        printf("%s", s);
        free(s);
    }
    fflush(stdout);
    totalActuals = 0;
}

static void libfunc_input() {
    char buf[1024];
    if (!fgets(buf, sizeof(buf), stdin)) {
        retval_reg.type = nil_m;
    } else {
        // strip newline
        buf[strcspn(buf, "\n")] = '\0';
        // try to parse as number
        char* end;
        double d = strtod(buf, &end);
        if (*end == '\0' && end != buf) {
            retval_reg.type = number_m;
            retval_reg.data.numVal = d;
        } else if (strcmp(buf, "true") == 0) {
            retval_reg.type = bool_m;
            retval_reg.data.boolVal = 1;
        } else if (strcmp(buf, "false") == 0) {
            retval_reg.type = bool_m;
            retval_reg.data.boolVal = 0;
        } else {
            avm_memcell_clear(&retval_reg);
            retval_reg.type = string_m;
            retval_reg.data.strVal = strdup(buf);
        }
    }
    totalActuals = 0;
}

static void libfunc_typeof() {
    if (totalActuals != 1) { avm_error("typeof expects 1 argument"); return; }
    avm_memcell* arg = &stack[top + 1];
    const char* typestr = "undef";
    switch (arg->type) {
        case number_m:   typestr = "number";        break;
        case string_m:   typestr = "string";        break;
        case bool_m:     typestr = "bool";           break;
        case table_m:    typestr = "table";          break;
        case userfunc_m: typestr = "userfunc";       break;
        case libfunc_m:  typestr = "libraryfunction"; break;
        case nil_m:      typestr = "nil";            break;
        default:         typestr = "undef";          break;
    }
    avm_memcell_clear(&retval_reg);
    retval_reg.type = string_m;
    retval_reg.data.strVal = strdup(typestr);
    totalActuals = 0;
}

static void libfunc_totalarguments() {
    avm_memcell* c = &stack[topsp + AVM_NUMACTUALS_OFFSET];
    avm_memcell_clear(&retval_reg);
    // not inside a user function: stack slot was never set up
    if (c->type != number_m) { retval_reg.type = nil_m; totalActuals = 0; return; }
    retval_reg.type = number_m;
    retval_reg.data.numVal = c->data.numVal;
    totalActuals = 0;
}

static void libfunc_argument() {
    if (totalActuals != 1) { avm_error("argument expects 1 argument"); return; }
    avm_memcell* idxcell = &stack[top + 1];
    if (idxcell->type != number_m) { avm_error("argument index must be a number"); return; }
    unsigned idx = (unsigned)idxcell->data.numVal;
    avm_memcell* nc = &stack[topsp + AVM_NUMACTUALS_OFFSET];
    avm_memcell_clear(&retval_reg);
    if (nc->type != number_m) { retval_reg.type = nil_m; totalActuals = 0; return; }
    unsigned numAct = (unsigned)nc->data.numVal;
    if (idx >= numAct) {
        retval_reg.type = nil_m;
    } else {
        avm_assign(&retval_reg, &stack[topsp + AVM_STACKENV_SIZE + idx]);
    }
    totalActuals = 0;
}

static void libfunc_strtonum() {
    if (totalActuals != 1) { avm_error("strtonum expects 1 argument"); return; }
    avm_memcell* arg = &stack[top + 1];
    if (arg->type != string_m) { retval_reg.type = nil_m; totalActuals = 0; return; }
    char* end;
    double d = strtod(arg->data.strVal, &end);
    avm_memcell_clear(&retval_reg);
    if (*end == '\0' && end != arg->data.strVal) {
        retval_reg.type = number_m;
        retval_reg.data.numVal = d;
    } else {
        printf("Failed to convert parameter to number, in strtonum\n");
        retval_reg.type = nil_m;
    }
    totalActuals = 0;
}

static void libfunc_sqrt() {
    if (totalActuals != 1) { avm_error("sqrt expects 1 argument"); return; }
    avm_memcell* arg = &stack[top + 1];
    if (arg->type != number_m) { avm_error("sqrt expects number"); return; }
    avm_memcell_clear(&retval_reg);
    if (arg->data.numVal < 0) { retval_reg.type = nil_m; }
    else { retval_reg.type = number_m; retval_reg.data.numVal = sqrt(arg->data.numVal); }
    totalActuals = 0;
}

static void libfunc_cos() {
    if (totalActuals != 1) { avm_error("cos expects 1 argument"); return; }
    avm_memcell* arg = &stack[top + 1];
    if (arg->type != number_m) { avm_error("cos expects number"); return; }
    avm_memcell_clear(&retval_reg);
    retval_reg.type = number_m;
    // argument is in degrees
    retval_reg.data.numVal = cos(arg->data.numVal * 3.14159265358979323846 / 180.0);
    totalActuals = 0;
}

static void libfunc_sin() {
    if (totalActuals != 1) { avm_error("sin expects 1 argument"); return; }
    avm_memcell* arg = &stack[top + 1];
    if (arg->type != number_m) { avm_error("sin expects number"); return; }
    avm_memcell_clear(&retval_reg);
    retval_reg.type = number_m;
    // argument is in degrees
    retval_reg.data.numVal = sin(arg->data.numVal * 3.14159265358979323846 / 180.0);
    totalActuals = 0;
}

static void libfunc_objectmemberkeys() {
    if (totalActuals != 1) { avm_error("objectmemberkeys expects 1 argument"); return; }
    avm_memcell* arg = &stack[top + 1];
    if (arg->type != table_m) { avm_error("objectmemberkeys expects table"); return; }
    avm_table* t = arg->data.tableVal;
    avm_table* keys = avm_table_new();
    unsigned idx = 0;
    for (int i = 0; i < AVM_TABLE_HASHSIZE; i++) {
        avm_table_bucket* b = t->strIndexed[i];
        while (b) {
            if (b->value.type != nil_m) {
                avm_memcell kidx; kidx.type = number_m; kidx.data.numVal = idx++;
                avm_table_setelem(keys, &kidx, &b->key);
            }
            b = b->next;
        }
    }
    for (int i = 0; i < AVM_TABLE_HASHSIZE; i++) {
        avm_table_bucket* b = t->numIndexed[i];
        while (b) {
            if (b->value.type != nil_m) {
                avm_memcell kidx; kidx.type = number_m; kidx.data.numVal = idx++;
                avm_table_setelem(keys, &kidx, &b->key);
            }
            b = b->next;
        }
    }
    avm_memcell_clear(&retval_reg);
    retval_reg.type = table_m;
    retval_reg.data.tableVal = keys;
    totalActuals = 0;
}

static void libfunc_objecttotalmembers() {
    if (totalActuals != 1) { avm_error("objecttotalmembers expects 1 argument"); return; }
    avm_memcell* arg = &stack[top + 1];
    if (arg->type != table_m) { avm_error("objecttotalmembers expects table"); return; }
    avm_memcell_clear(&retval_reg);
    retval_reg.type = number_m;
    retval_reg.data.numVal = (double)arg->data.tableVal->total;
    totalActuals = 0;
}

static void libfunc_objectcopy() {
    if (totalActuals != 1) { avm_error("objectcopy expects 1 argument"); return; }
    avm_memcell* arg = &stack[top + 1];
    if (arg->type != table_m) { avm_error("objectcopy expects table"); return; }
    avm_table* src = arg->data.tableVal;
    avm_table* dst = avm_table_new();
    for (int i = 0; i < AVM_TABLE_HASHSIZE; i++) {
        avm_table_bucket* b = src->strIndexed[i];
        while (b) { avm_table_setelem(dst, &b->key, &b->value); b = b->next; }
    }
    for (int i = 0; i < AVM_TABLE_HASHSIZE; i++) {
        avm_table_bucket* b = src->numIndexed[i];
        while (b) { avm_table_setelem(dst, &b->key, &b->value); b = b->next; }
    }
    avm_memcell_clear(&retval_reg);
    retval_reg.type = table_m;
    retval_reg.data.tableVal = dst;
    totalActuals = 0;
}

//  library function dispatch

typedef void (*libfunc_handler_t)();

typedef struct { const char* name; libfunc_handler_t fn; } libfunc_entry;

static libfunc_entry lib_dispatch[] = {
    { "print",               libfunc_print },
    { "input",               libfunc_input },
    { "typeof",              libfunc_typeof },
    { "totalarguments",      libfunc_totalarguments },
    { "argument",            libfunc_argument },
    { "strtonum",            libfunc_strtonum },
    { "sqrt",                libfunc_sqrt },
    { "cos",                 libfunc_cos },
    { "sin",                 libfunc_sin },
    { "objectmemberkeys",    libfunc_objectmemberkeys },
    { "objecttotalmembers",  libfunc_objecttotalmembers },
    { "objectcopy",          libfunc_objectcopy },
    { NULL, NULL }
};

void avm_calllibfunc(const char* name) {
    for (int i = 0; lib_dispatch[i].name; i++) {
        if (strcmp(lib_dispatch[i].name, name) == 0) {
            lib_dispatch[i].fn();
            return;
        }
    }
    avm_error("undefined library function: %s", name);
}

// binary loader 

static unsigned read_u32(FILE* f) {
    unsigned v = 0;
    fread(&v, sizeof(unsigned), 1, f);
    return v;
}
static unsigned char read_u8(FILE* f) {
    unsigned char v = 0;
    fread(&v, 1, 1, f);
    return v;
}
static double read_dbl(FILE* f) {
    double d = 0;
    fread(&d, sizeof(double), 1, f);
    return d;
}
static char* read_str(FILE* f) {
    char buf[4096];
    int i = 0;
    int c;
    while ((c = fgetc(f)) != EOF && c != '\0' && i < (int)sizeof(buf)-1)
        buf[i++] = (char)c;
    buf[i] = '\0';
    return strdup(buf);
}

int load_binary(const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", filename); return 0; }

    unsigned magic = read_u32(f);
    if (magic != MAGIC_NUMBER) {
        fprintf(stderr, "bad magic: %u\n", magic);
        fclose(f);
        return 0;
    }

    avm_total_globals = read_u32(f);

    // strings
    avm_total_strings = read_u32(f);
    avm_strings = (char**)malloc(avm_total_strings * sizeof(char*));
    for (unsigned i = 0; i < avm_total_strings; i++) avm_strings[i] = read_str(f);

    // numbers
    avm_total_numbers = read_u32(f);
    avm_numbers = (double*)malloc(avm_total_numbers * sizeof(double));
    for (unsigned i = 0; i < avm_total_numbers; i++) avm_numbers[i] = read_dbl(f);

    // user funcs
    avm_total_user_funcs = read_u32(f);
    avm_user_funcs = (avm_userfunc*)malloc(avm_total_user_funcs * sizeof(avm_userfunc));
    for (unsigned i = 0; i < avm_total_user_funcs; i++) {
        avm_user_funcs[i].address   = read_u32(f);
        avm_user_funcs[i].localSize = read_u32(f);
        avm_user_funcs[i].id        = read_str(f);
    }

    // lib funcs
    avm_total_lib_funcs = read_u32(f);
    avm_lib_funcs = (char**)malloc(avm_total_lib_funcs * sizeof(char*));
    for (unsigned i = 0; i < avm_total_lib_funcs; i++) avm_lib_funcs[i] = read_str(f);

    // instructions
    codeSize = read_u32(f);
    code = (instruction*)malloc(codeSize * sizeof(instruction));
    for (unsigned i = 0; i < codeSize; i++) {
        code[i].opcode          = (vmopcode)read_u8(f);
        code[i].result.type     = (vmarg_t)read_u8(f);
        code[i].result.val      = read_u32(f);
        code[i].arg1.type       = (vmarg_t)read_u8(f);
        code[i].arg1.val        = read_u32(f);
        code[i].arg2.type       = (vmarg_t)read_u8(f);
        code[i].arg2.val        = read_u32(f);
        code[i].srcLine         = read_u32(f);
    }

    fclose(f);
    return 1;
}

//  vm main

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: avm <file.abc>\n");
        return 1;
    }

    // init stack to undef
    for (int i = 0; i < AVM_STACKSIZE; i++) stack[i].type = undef_m;
    retval_reg.type = undef_m;

    if (!load_binary(argv[1])) return 1;

    // globals live at top of stack high indices
    // top starts just below the global area
    top   = AVM_STACKSIZE - 1 - avm_total_globals;
    topsp = top;
    pc    = 0;
    executionFinished = (codeSize == 0);

    while (!executionFinished) {
        execute_cycle();
    }

    return 0;
}
