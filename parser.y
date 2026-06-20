%{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "symtable.h"
#include "quads.h"
#include "target_code.h"

extern int yylex(void);
extern int yylineno;
extern char* yytext;
extern FILE* yyin;
// error handling function declaration
void yyerror(const char* msg);

// variables to track scope depth and function scopes
int current_scope_depth = 0;
int hidden_func_id = 0;
int active_func_scopes[100];
int active_func_top = 0;
extern unsigned temp_counter;
unsigned func_temp_counter_save[100];
int func_temp_save_top = 0;
// simple stack to handle break and continue lists cleanly
typedef struct loop_tracker {
struct bp_list* breaklist;
// list for continue statements
struct bp_list* contlist;
struct loop_tracker* next;
} loop_tracker;
loop_tracker* func_loop_stack_save[100];
int func_loop_save_top = 0;

loop_tracker* loop_stack = NULL;
// push a new loop context onto the stack
void push_loop() {
loop_tracker* l = (loop_tracker*)malloc(sizeof(loop_tracker));
l->breaklist = NULL;
l->contlist = NULL;
l->next = loop_stack;
// update the head of the loop stack
loop_stack = l;
}

// pop the current loop context from the stack
void pop_loop() {
if (loop_stack) {
loop_tracker* tmp = loop_stack;
// move head to the next element and free memory
loop_stack = loop_stack->next;
free(tmp);
}
}

// generate a unique name for anonymous functions
char* generate_hidden_name() {
char* buff = (char*)malloc(32);
sprintf(buff, "_f%d", hidden_func_id++);
return buff;
// end of name generation
}

// helper to make an expr from a symbol
expr* make_lvalue_expr(symrec* sym) {
expr* e = make_expr(var_e);
// assign the symbol to the expression
e->sym = sym;
return e;
}

// emit getelem if it is a table item
expr* emit_iftableitem(expr* e, int line) {
if (e && e->type == tableitem_e) {
expr* res = make_expr(var_e);
// create a temporary variable for the table element
res->sym = create_temp_var(current_scope_depth, line);
emit_quad(tablegetelem, e, e->index, res, 0, line);
return res;
}
return e;
// return the modified or original expression
}

// force expression to be a boolean for short-circuiting
expr* emit_ifboolexpr(expr* e, int line) {
if (e && e->type != boolexpr_e) {
expr* res = make_expr(boolexpr_e);
// setup true and false lists for boolean evaluation
res->sym = e->sym;
res->index = e->index;
res->truelist = make_bplist(next_quad_label());
res->falselist = make_bplist(next_quad_label() + 1);
emit_quad(if_eq, e, make_const_bool_expr(1), NULL, 0, line);
// jump to false list if not equal
emit_quad(jump, NULL, NULL, NULL, 0, line);
return res;
}
return e;
// return boolean expression
}

// convert short-circuit lists back to a value for assignments
expr* convert_to_value(expr* e, int line) {
if (e && e->type == boolexpr_e) {
expr* res = make_expr(var_e);
// assign a temporary variable for the result
res->sym = create_temp_var(current_scope_depth, line);

patch_list(e->truelist, next_quad_label());
emit_quad(assign, make_const_bool_expr(1), NULL, res, 0, line);
bp_list* jump_node = make_bplist(next_quad_label());
// jump over the false assignment
emit_quad(jump, NULL, NULL, NULL, 0, line);

patch_list(e->falselist, next_quad_label());
emit_quad(assign, make_const_bool_expr(0), NULL, res, 0, line);
patch_list(jump_node, next_quad_label());
return res;
// return the final value expression
}
return e;
}

// standard pipeline for an r-value
expr* finalize_expr(expr* e, int line) {
if (!e) return NULL;
// check if it is a table item and convert
e = emit_iftableitem(e, line);
e = convert_to_value(e, line);
return e;
// return finalized expression
}

// handle function calls
void emit_call(expr* func, expr* args, expr** res, int line) {
expr* curr = args;
// reverse args because they are pushed right-to-left
expr* rev = NULL;

// reverse args because they are pushed right-to-left
while(curr) {
expr* nxt = curr->next;
// update next pointers for reversal
curr->next = rev;
rev = curr;
curr = nxt;
}

curr = rev;
// emit param quads for each argument
while(curr) {
emit_quad(param, curr, NULL, NULL, 0, line);
curr = curr->next;
// end of arguments loop
}

emit_quad(call, func, NULL, NULL, 0, line);
*res = make_expr(var_e);
(*res)->sym = create_temp_var(current_scope_depth, line);
emit_quad(getretval, NULL, NULL, *res, 0, line);
}
%}

%define parse.error verbose
%expect 1

%union {
int intVal;
double realVal;
char* strVal;
// expression node pointer
struct expr* exprNode;
unsigned quadLabel;
struct symrec* symNode;
struct {
int is_method;
// function name and arguments
char* name;
struct expr* args;
} callInfo;
struct {
struct symrec* sym;
// label for jumping over function body
unsigned jump_label;
} funcInfo;
}

%token IF FOR WHILE ELSE FUNCTION CONTINUE BREAK RETURN
%token AND OR NOT LOCAL TRUE FALSE NIL
%token EQUAL ASSIGN PLUS MINUS MULTIPLY DIVIDE MODULO NOT_EQUAL
%token PLUS_PLUS MINUS_MINUS GREATER_EQUAL LESS_EQUAL GREATER LESS
%token LEFT_BRACE RIGHT_BRACE LEFT_BRACKET RIGHT_BRACKET LEFT_PAREN RIGHT_PAREN
%token SEMICOLON COMMA DOUBLE_COLON COLON DOUBLE_DOT DOT

%token <intVal> INTCONST
%token <realVal> REALCONST
%token <strVal> IDENT STRING

%type <exprNode> expr term assignexpr primary lvalue member call elist elist_opt objectdef indexed indexedelem indexed_opt const ifprefix
%type <callInfo> callsuffix normcall methodcall
%type <funcInfo> funcprefix
%type <quadLabel> M N
%type <symNode> funcdef

%right ASSIGN
%left OR
%left AND
%nonassoc EQUAL NOT_EQUAL
%nonassoc GREATER GREATER_EQUAL LESS LESS_EQUAL
%left PLUS MINUS
%left MULTIPLY DIVIDE MODULO
%right NOT PLUS_PLUS MINUS_MINUS UMINUS
%left DOT DOUBLE_DOT
// associative rules for brackets and parens
LEFT_BRACKET RIGHT_BRACKET LEFT_PAREN RIGHT_PAREN

%%

program:
stmts
;
// list of statements
stmts:
stmts stmt
| /* empty */
;
// single statement rule
stmt:
expr SEMICOLON {
if ($1 && $1->type == boolexpr_e) {
convert_to_value($1, yylineno);
}
}
| ifstmt
|
// while loop statement
whilestmt
| forstmt
| returnstmt
|
// handle break statement inside loops
BREAK SEMICOLON {
if (!loop_stack) fprintf(stderr, "Error: 'break' outside of loop at line %d\n", yylineno);
// add break label to the loop's breaklist
else {
loop_stack->breaklist = merge_bplists(loop_stack->breaklist, make_bplist(next_quad_label()));
// jump to the end of the loop
emit_quad(jump, NULL, NULL, NULL, 0, yylineno);
}
}
|
// handle continue statement inside loops
CONTINUE SEMICOLON {
if (!loop_stack) fprintf(stderr, "Error: 'continue' outside of loop at line %d\n", yylineno);
// add continue label to the loop's contlist
else {
loop_stack->contlist = merge_bplists(loop_stack->contlist, make_bplist(next_quad_label()));
// jump to the next iteration of the loop
emit_quad(jump, NULL, NULL, NULL, 0, yylineno);
}
}
| block
|
// function definition or empty statement
funcdef
| SEMICOLON
;
// marker rules for backpatching
M: /* empty */ { $$ = next_quad_label(); } ;
// next quad label for jumping
N: /* empty */ { $$ = next_quad_label(); emit_quad(jump, NULL, NULL, NULL, 0, yylineno); } ;
// expressions parsing
expr:
assignexpr { $$ = $1; }
|
// addition operation
expr PLUS expr {
expr* left = finalize_expr($1, yylineno);
expr* right = finalize_expr($3, yylineno);
if (left->type == constnum_e && right->type == constnum_e) {
$$ = make_const_num_expr(left->numConst + right->numConst);
} else {
$$ = make_expr(arithexpr_e);
$$->sym = create_temp_var(current_scope_depth, yylineno);
emit_quad(add, left, right, $$, 0, yylineno);
}
}
|
// subtraction operation
expr MINUS expr {
expr* left = finalize_expr($1, yylineno);
expr* right = finalize_expr($3, yylineno);
if (left->type == constnum_e && right->type == constnum_e) {
$$ = make_const_num_expr(left->numConst - right->numConst);
} else {
$$ = make_expr(arithexpr_e);
$$->sym = create_temp_var(current_scope_depth, yylineno);
emit_quad(sub, left, right, $$, 0, yylineno);
}
}
|
// multiplication operation
expr MULTIPLY expr {
expr* left = finalize_expr($1, yylineno);
expr* right = finalize_expr($3, yylineno);
if (left->type == constnum_e && right->type == constnum_e) {
$$ = make_const_num_expr(left->numConst * right->numConst);
} else {
$$ = make_expr(arithexpr_e);
$$->sym = create_temp_var(current_scope_depth, yylineno);
emit_quad(mul, left, right, $$, 0, yylineno);
}
}
|
// division operation
expr DIVIDE expr {
expr* left = finalize_expr($1, yylineno);
expr* right = finalize_expr($3, yylineno);
if (right->type == constnum_e && right->numConst == 0) {
if (left->type == constnum_e) {
fprintf(stderr, "[COMPILER WARNING] - division with 0 (when evaluating division with constants)\n");
$$ = make_const_num_expr(0);
} else {
fprintf(stderr, "[COMPILER WARNING] - division with 0\n");
$$ = make_expr(arithexpr_e);
$$->sym = create_temp_var(current_scope_depth, yylineno);
emit_quad(op_div, left, right, $$, 0, yylineno);
}
} else if (left->type == constnum_e && right->type == constnum_e) {
$$ = make_const_num_expr(left->numConst / right->numConst);
} else {
$$ = make_expr(arithexpr_e);
$$->sym = create_temp_var(current_scope_depth, yylineno);
emit_quad(op_div, left, right, $$, 0, yylineno);
}
}
|
// modulo operation
expr MODULO expr {
expr* left = finalize_expr($1, yylineno);
expr* right = finalize_expr($3, yylineno);
if (left->type == constnum_e && right->type == constnum_e) {
$$ = (right->numConst != 0) ? make_const_num_expr((long long)left->numConst % (long long)right->numConst) : make_const_num_expr(0);
} else {
$$ = make_expr(arithexpr_e);
$$->sym = create_temp_var(current_scope_depth, yylineno);
emit_quad(mod, left, right, $$, 0, yylineno);
}
}
|
// greater than comparison
expr GREATER expr {
$$ = make_expr(boolexpr_e);
expr* left = finalize_expr($1, yylineno);
// finalize right side for comparison
expr* right = finalize_expr($3, yylineno);
$$->truelist = make_bplist(next_quad_label());
$$->falselist = make_bplist(next_quad_label() + 1);
emit_quad(if_greater, left, right, NULL, 0, yylineno);
// jump if condition is false
emit_quad(jump, NULL, NULL, NULL, 0, yylineno);
}
|
// greater or equal comparison
expr GREATER_EQUAL expr {
$$ = make_expr(boolexpr_e);
expr* left = finalize_expr($1, yylineno);
// finalize right side
expr* right = finalize_expr($3, yylineno);
$$->truelist = make_bplist(next_quad_label());
$$->falselist = make_bplist(next_quad_label() + 1);
emit_quad(if_greatereq, left, right, NULL, 0, yylineno);
// false jump quad
emit_quad(jump, NULL, NULL, NULL, 0, yylineno);
}
|
// less than comparison
expr LESS expr {
$$ = make_expr(boolexpr_e);
expr* left = finalize_expr($1, yylineno);
// less than right side
expr* right = finalize_expr($3, yylineno);
$$->truelist = make_bplist(next_quad_label());
$$->falselist = make_bplist(next_quad_label() + 1);
emit_quad(if_less, left, right, NULL, 0, yylineno);
// jump if false
emit_quad(jump, NULL, NULL, NULL, 0, yylineno);
}
|
// less or equal comparison
expr LESS_EQUAL expr {
$$ = make_expr(boolexpr_e);
expr* left = finalize_expr($1, yylineno);
// less eq right side
expr* right = finalize_expr($3, yylineno);
$$->truelist = make_bplist(next_quad_label());
$$->falselist = make_bplist(next_quad_label() + 1);
emit_quad(if_lesseq, left, right, NULL, 0, yylineno);
// jump if false
emit_quad(jump, NULL, NULL, NULL, 0, yylineno);
}
|
// equality check
expr EQUAL expr {
$$ = make_expr(boolexpr_e);
expr* left = finalize_expr($1, yylineno);
// evaluate right side
expr* right = finalize_expr($3, yylineno);
$$->truelist = make_bplist(next_quad_label());
$$->falselist = make_bplist(next_quad_label() + 1);
emit_quad(if_eq, left, right, NULL, 0, yylineno);
// jump if not equal
emit_quad(jump, NULL, NULL, NULL, 0, yylineno);
}
|
// not equal check
expr NOT_EQUAL expr {
$$ = make_expr(boolexpr_e);
expr* left = finalize_expr($1, yylineno);
// evaluate right side
expr* right = finalize_expr($3, yylineno);
$$->truelist = make_bplist(next_quad_label());
$$->falselist = make_bplist(next_quad_label() + 1);
emit_quad(if_noteq, left, right, NULL, 0, yylineno);
// jump if equal
emit_quad(jump, NULL, NULL, NULL, 0, yylineno);
}
| expr AND { $<exprNode>$ = emit_ifboolexpr($1, yylineno);
// logical AND short-circuiting
} M expr {
$$ = make_expr(boolexpr_e);
expr* left = $<exprNode>3;
patch_list(left->truelist, $4);
// emit boolean for right side
expr* right = emit_ifboolexpr($5, yylineno);
$$->truelist = right->truelist;
$$->falselist = merge_bplists(left->falselist, right->falselist);
}
|
// logical OR short-circuiting
expr OR { $<exprNode>$ = emit_ifboolexpr($1, yylineno); } M expr {
$$ = make_expr(boolexpr_e);
// patch falselist for OR
expr* left = $<exprNode>3;
patch_list(left->falselist, $4);
expr* right = emit_ifboolexpr($5, yylineno);
$$->truelist = merge_bplists(left->truelist, right->truelist);
$$->falselist = right->falselist;
// close OR block
}
| term { $$ = $1; }
;
// term handling
term:
LEFT_PAREN expr RIGHT_PAREN { $$ = $2; }
|
// unary minus
MINUS expr %prec UMINUS {
$$ = make_expr(arithexpr_e);
$$->sym = create_temp_var(current_scope_depth, yylineno);
// emit uminus quad
emit_quad(uminus, finalize_expr($2, yylineno), NULL, $$, 0, yylineno);
}
|
// logical NOT operator
NOT expr {
$$ = make_expr(boolexpr_e);
expr* e = emit_ifboolexpr($2, yylineno);
// swap true and false lists
$$->truelist = e->falselist;
$$->falselist = e->truelist;
}
|
// pre-increment operator
PLUS_PLUS lvalue {
expr* one = make_const_num_expr(1);
// handle table item increment
if ($2->type == tableitem_e) {
expr* val = emit_iftableitem($2, yylineno);
// add and set back to table
emit_quad(add, val, one, val, 0, yylineno);
emit_quad(tablesetelem, $2->index, val, make_lvalue_expr($2->sym), 0, yylineno);
$$ = val;
} else {
emit_quad(add, $2, one, $2, 0, yylineno);
// assign incremented value
$$ = make_expr(arithexpr_e);
$$->sym = create_temp_var(current_scope_depth, yylineno);
emit_quad(assign, $2, NULL, $$, 0, yylineno);
// end if
}
}
| lvalue PLUS_PLUS {
expr* one = make_const_num_expr(1);
// post-increment temporary var
$$ = make_expr(arithexpr_e);
$$->sym = create_temp_var(current_scope_depth, yylineno);
if ($1->type == tableitem_e) {
expr* val = emit_iftableitem($1, yylineno);
// assign before incrementing
emit_quad(assign, val, NULL, $$, 0, yylineno);
emit_quad(add, val, one, val, 0, yylineno);
emit_quad(tablesetelem, $1->index, val, make_lvalue_expr($1->sym), 0, yylineno);
// normal variable post-increment
} else {
emit_quad(assign, $1, NULL, $$, 0, yylineno);
// add one to variable
emit_quad(add, $1, one, $1, 0, yylineno);
}
}
|
// pre-decrement operator
MINUS_MINUS lvalue {
expr* one = make_const_num_expr(1);
// handle table item decrement
if ($2->type == tableitem_e) {
expr* val = emit_iftableitem($2, yylineno);
// subtract one
emit_quad(sub, val, one, val, 0, yylineno);
emit_quad(tablesetelem, $2->index, val, make_lvalue_expr($2->sym), 0, yylineno);
$$ = val;
} else {
emit_quad(sub, $2, one, $2, 0, yylineno);
// normal var decrement
$$ = make_expr(arithexpr_e);
$$->sym = create_temp_var(current_scope_depth, yylineno);
emit_quad(assign, $2, NULL, $$, 0, yylineno);
// end if
}
}
| lvalue MINUS_MINUS {
expr* one = make_const_num_expr(1);
// post-decrement temporary var
$$ = make_expr(arithexpr_e);
$$->sym = create_temp_var(current_scope_depth, yylineno);
if ($1->type == tableitem_e) {
expr* val = emit_iftableitem($1, yylineno);
// assign before decrementing
emit_quad(assign, val, NULL, $$, 0, yylineno);
emit_quad(sub, val, one, val, 0, yylineno);
emit_quad(tablesetelem, $1->index, val, make_lvalue_expr($1->sym), 0, yylineno);
// normal variable post-decrement
} else {
emit_quad(assign, $1, NULL, $$, 0, yylineno);
// subtract one from variable
emit_quad(sub, $1, one, $1, 0, yylineno);
}
}
| primary { $$ = $1;
// return primary expr
}
;

assignexpr:
lvalue ASSIGN expr {
expr* rval = finalize_expr($3, yylineno);
// assign to table item
if ($1->type == tableitem_e) {
emit_quad(tablesetelem, $1->index, rval, make_lvalue_expr($1->sym), 0, yylineno);
$$ = make_expr(assignexpr_e);
$$->sym = create_temp_var(current_scope_depth, yylineno);
emit_quad(tablegetelem, make_lvalue_expr($1->sym), $1->index, $$, 0, yylineno);
// assign to normal variable
} else {
emit_quad(assign, rval, NULL, $1, 0, yylineno);
// create assign expression
$$ = make_expr(assignexpr_e);
$$->sym = create_temp_var(current_scope_depth, yylineno);
emit_quad(assign, $1, NULL, $$, 0, yylineno);
// end if
}
}
;

primary:
lvalue { $$ = emit_iftableitem($1, yylineno);
// handle lvalue evaluation
}
| call { $$ = $1; }
| objectdef { $$ = $1;
// handle object definition
}
| LEFT_PAREN funcdef RIGHT_PAREN { $$ = make_lvalue_expr($2); }
|
// handle constant values
const { $$ = $1; }
;
// left values logic
lvalue:
IDENT {
symrec* r = lookup_active_symbol($1);
// insert new variable if not found
if (r == NULL) {
r = add_symbol($1, current_scope_depth == 0 ? SYM_GLOBAL_VAR : SYM_LOCAL_VAR, current_scope_depth, yylineno);
// setup space and offset
r->space = get_curr_scopespace();
r->offset = get_curr_offset();
inc_curr_offset();
}
$$ = make_lvalue_expr(r);
// close IDENT rule
}
| LOCAL IDENT {
symrec* builtin = lookup_in_scope($2, 0);
// local lookup
symrec* curr_node = lookup_in_scope($2, current_scope_depth);
symrec* r = curr_node;

if (current_scope_depth > 0 && builtin && builtin->s_type == SYM_LIB_FUNC) {
fprintf(stderr, "Error: local '%s' shadows library function at line %d\n", $2, yylineno);
// use builtin function
r = builtin;
} else if (curr_node == NULL) {
r = add_symbol($2, current_scope_depth == 0 ? SYM_GLOBAL_VAR : SYM_LOCAL_VAR, current_scope_depth, yylineno);
// setup scope variables
r->space = get_curr_scopespace();
r->offset = get_curr_offset();
inc_curr_offset();
}
$$ = make_lvalue_expr(r);
// close LOCAL IDENT
}
| DOUBLE_COLON IDENT {
symrec* r = lookup_in_scope($2, 0);
// global lookup logic
$$ = r ? make_lvalue_expr(r) : NULL;
}
| member { $$ = $1;
// close member rule
}
;

member:
lvalue DOT IDENT {
expr* lval = emit_iftableitem($1, yylineno);
if (lval && lval->sym && (lval->sym->s_type == SYM_USER_FUNC || lval->sym->s_type == SYM_LIB_FUNC)) {
fprintf(stderr, "[COMPILER WARNING] - Attempted to get key '%s' on function '%s' instead of an object at line %d\n", $3, lval->sym->s_name, yylineno);
}
$$ = make_expr(tableitem_e);
$$->sym = lval->sym;
$$->index = make_const_string_expr($3);
}
|
lvalue LEFT_BRACKET expr RIGHT_BRACKET {
expr* lval = emit_iftableitem($1, yylineno);
if (lval && lval->sym && (lval->sym->s_type == SYM_USER_FUNC || lval->sym->s_type == SYM_LIB_FUNC)) {
fprintf(stderr, "[COMPILER WARNING] - Attempted to get member value on function '%s' instead of an object at line %d\n", lval->sym->s_name, yylineno);
}
$$ = make_expr(tableitem_e);
$$->sym = lval->sym;
$$->index = finalize_expr($3, yylineno);
}
|
call DOT IDENT {
$$ = make_expr(tableitem_e);
$$->sym = $1->sym;
$$->index = make_const_string_expr($3);
}
| call LEFT_BRACKET expr RIGHT_BRACKET {
$$ = make_expr(tableitem_e);
$$->sym = $1->sym;
$$->index = finalize_expr($3, yylineno);
}
;
// function call parsing
call:
call LEFT_PAREN elist RIGHT_PAREN {
emit_call($1, $3, &$$, yylineno);
// simple call evaluation
}
| lvalue callsuffix {
expr* lval = emit_iftableitem($1, yylineno);
// check if method call
if ($2.is_method) {
if (lval && lval->sym && (lval->sym->s_type == SYM_USER_FUNC || lval->sym->s_type == SYM_LIB_FUNC)) {
fprintf(stderr, "[COMPILER WARNING] - Attempted to call method '%s' on function '%s' instead of an object at line %d\n", $2.name, lval->sym->s_name, yylineno);
}
expr* t = make_expr(tableitem_e);
// prepare self argument
t->sym = lval->sym;
t->index = make_const_string_expr($2.name);
expr* func = emit_iftableitem(t, yylineno);

lval->next = $2.args;
$2.args = lval;
// call the method
emit_call(func, $2.args, &$$, yylineno);
} else {
emit_call(lval, $2.args, &$$, yylineno);
// close else
}
}
| LEFT_PAREN funcdef RIGHT_PAREN LEFT_PAREN elist RIGHT_PAREN {
emit_call(make_lvalue_expr($2), $5, &$$, yylineno);
// anonymous function call
}
;

callsuffix:
normcall { $$ = $1; }
|
// pass method suffix
methodcall { $$ = $1; }
;
// normal call arguments
normcall:
LEFT_PAREN elist RIGHT_PAREN {
$$.is_method = 0;
// reset method flag
$$.name = NULL;
$$.args = $2;
}
;
// method call arguments
methodcall:
DOUBLE_DOT IDENT LEFT_PAREN elist RIGHT_PAREN {
$$.is_method = 1;
// store method name
$$.name = $2;
$$.args = $4;
}
;
// argument list parsing
elist:
expr elist_opt {
expr* e = finalize_expr($1, yylineno);
// link expressions
e->next = $2;
$$ = e;
}
| /* empty */ { $$ = NULL;
// empty argument list
}
;

elist_opt:
COMMA expr elist_opt {
expr* e = finalize_expr($2, yylineno);
// link optional arguments
e->next = $3;
$$ = e;
}
| /* empty */ { $$ = NULL;
// empty optional argument
}
;

objectdef:
LEFT_BRACKET elist RIGHT_BRACKET {
expr* t = make_expr(newtable_e);
t->sym = create_temp_var(current_scope_depth, yylineno);
emit_quad(tablecreate, NULL, NULL, t, 0, yylineno);

int elist_n = 0;
expr* ecurr = $2;
while(ecurr) { elist_n++; ecurr = ecurr->next; }
if (elist_n > 0) {
int j;
expr** elems = (expr**)malloc(elist_n * sizeof(expr*));
ecurr = $2;
for (j = 0; j < elist_n; j++) { elems[j] = ecurr; ecurr = ecurr->next; }
for (j = elist_n - 1; j >= 0; j--) {
emit_quad(tablesetelem, make_const_num_expr(j), elems[j], t, 0, yylineno);
}
free(elems);
}
$$ = t;
}
|
// object definition with indexed items
LEFT_BRACKET indexed RIGHT_BRACKET {
expr* t = make_expr(newtable_e);
t->sym = create_temp_var(current_scope_depth, yylineno);
emit_quad(tablecreate, NULL, NULL, t, 0, yylineno);

int idx_n = 0;
expr* icurr = $2;
while(icurr) { idx_n++; icurr = icurr->next->next; }
if (idx_n > 0) {
int j;
expr** ikeys = (expr**)malloc(idx_n * sizeof(expr*));
expr** ivals = (expr**)malloc(idx_n * sizeof(expr*));
icurr = $2;
for (j = 0; j < idx_n; j++) { ikeys[j] = icurr; ivals[j] = icurr->next; icurr = icurr->next->next; }
for (j = idx_n - 1; j >= 0; j--) {
emit_quad(tablesetelem, ikeys[j], ivals[j], t, 0, yylineno);
}
free(ikeys); free(ivals);
}
$$ = t;
}
;
// indexed array elements
indexed:
indexedelem indexed_opt {
$1->next->next = $2;
$$ = $1;
// link indexed element pairs
}
;

indexed_opt:
COMMA indexedelem indexed_opt {
$2->next->next = $3;
// link optional elements
$$ = $2;
}
| /* empty */ { $$ = NULL; }
;
// key value pair parsing
indexedelem:
LEFT_BRACE expr COLON expr RIGHT_BRACE {
expr* k = finalize_expr($2, yylineno);
// link key to value
expr* v = finalize_expr($4, yylineno);
k->next = v;
$$ = k;
}
;
// starting a new block
block_start:
LEFT_BRACE { current_scope_depth++; }
;

block_end:
RIGHT_BRACE { hide_scope(current_scope_depth); current_scope_depth--;
// decrease scope
}
;

block:
block_start stmts block_end
;
// function declaration prefix
funcprefix:
FUNCTION IDENT {
symrec* f = lookup_in_scope($2, current_scope_depth);
if(!f) {
f = add_symbol($2, SYM_USER_FUNC, current_scope_depth, yylineno);
f->space = get_curr_scopespace();
f->offset = get_curr_offset();
inc_curr_offset();
}
f->iaddress = next_quad_label();
emit_quad(funcstart, NULL, NULL, make_lvalue_expr(f), 0, yylineno);
$$.sym = f;
$$.jump_label = 0;
}
| FUNCTION {
symrec* f = add_symbol(generate_hidden_name(), SYM_USER_FUNC, current_scope_depth, yylineno);
f->space = get_curr_scopespace();
f->offset = get_curr_offset();
inc_curr_offset();
f->iaddress = next_quad_label();
emit_quad(funcstart, NULL, NULL, make_lvalue_expr(f), 0, yylineno);
$$.sym = f;
$$.jump_label = 0;
}
;
// parse function arguments
funcargs:
LEFT_PAREN {
enter_scopespace();
reset_formal_args_offset();
current_scope_depth++;
active_func_scopes[++active_func_top] = current_scope_depth;
// read arguments
} idlist RIGHT_PAREN
;
funcbody:
LEFT_BRACE {
enter_scopespace();
reset_function_locals_offset();
func_loop_stack_save[func_loop_save_top++] = loop_stack;
loop_stack = NULL;
func_temp_counter_save[func_temp_save_top++] = temp_counter;
} stmts RIGHT_BRACE {
hide_scope(current_scope_depth);
current_scope_depth--;
active_func_top--;
exit_scopespace();
loop_stack = func_loop_stack_save[--func_loop_save_top];
temp_counter = func_temp_counter_save[--func_temp_save_top];
}
;
// complete function definition
funcdef:
funcprefix funcargs funcbody {
exit_scopespace();
$1.sym->total_locals = function_local_offset;
emit_quad(funcend, NULL, NULL, make_lvalue_expr($1.sym), 0, yylineno);
$$ = $1.sym;
}
;
// numeric and boolean constants
const:
INTCONST { $$ = make_const_num_expr($1); }
| REALCONST { $$ = make_const_num_expr($1);
// string constant
}
| STRING { $$ = make_const_string_expr($1); }
| NIL { $$ = make_expr(nil_e);
// true constant
}
| TRUE { $$ = make_const_bool_expr(1); }
| FALSE { $$ = make_const_bool_expr(0);
// false constant
}
;

formal_ident:
IDENT {
symrec* f = add_symbol($1, SYM_FORMAL_ARG, current_scope_depth, yylineno);
// init formal argument offset
f->space = get_curr_scopespace();
f->offset = get_curr_offset();
inc_curr_offset();
}
;
// identifier list
idlist:
formal_ident idlist_opt
| /* empty */
;
// optional identifier list
idlist_opt:
COMMA formal_ident idlist_opt
| /* empty */
;
// evaluate if condition
ifprefix:
IF LEFT_PAREN expr {
expr* val = convert_to_value($3, yylineno);
$$ = emit_ifboolexpr(val, yylineno);
}
;

ifstmt:
ifprefix RIGHT_PAREN M stmt {
patch_list($1->truelist, $3);
// backpatch false jump
patch_list($1->falselist, next_quad_label());
}
| ifprefix RIGHT_PAREN M stmt ELSE N M stmt {
patch_list($1->truelist, $3);
// if else logic
patch_list($1->falselist, $7);
patch_list(make_bplist($6), next_quad_label());
}
;

whilestmt:
WHILE { push_loop();
// while loop body logic
} M LEFT_PAREN expr { $<exprNode>$ = emit_ifboolexpr(convert_to_value($5, yylineno), yylineno); } RIGHT_PAREN M stmt {
expr* cond = $<exprNode>6;
// patch lists for break and continue
patch_list(cond->truelist, $8);
emit_quad(jump, NULL, NULL, NULL, $3, yylineno);
patch_list(cond->falselist, next_quad_label());

patch_list(loop_stack->breaklist, next_quad_label());
patch_list(loop_stack->contlist, $3);
pop_loop();
}
;
// for loop parsing
forstmt:
FOR { push_loop(); } LEFT_PAREN elist SEMICOLON M expr { $<exprNode>$ = emit_ifboolexpr(convert_to_value($7, yylineno), yylineno);
// parse loop body
} SEMICOLON M elist N RIGHT_PAREN M stmt N {
expr* cond = $<exprNode>8;
// handle loop exit and continues
patch_list(cond->truelist, $14);
patch_list(cond->falselist, next_quad_label());

patch_list(make_bplist($12), $6);
patch_list(make_bplist($16), $10);

patch_list(loop_stack->breaklist, next_quad_label());
patch_list(loop_stack->contlist, $10);
pop_loop();
}
;
// return a value from function
returnstmt:
RETURN expr SEMICOLON {
if (active_func_top == 0) {
fprintf(stderr, "Error: 'return' outside of function at line %d\n", yylineno);
// valid return inside func
} else {
emit_quad(ret, NULL, NULL, finalize_expr($2, yylineno), 0, yylineno);
}
}
| RETURN SEMICOLON {
if (active_func_top == 0) {
fprintf(stderr, "Error: 'return' outside of function at line %d\n", yylineno);
// empty return quad
} else {
emit_quad(ret, NULL, NULL, NULL, 0, yylineno);
}
}
;

%%

void yyerror(const char* msg) {
fprintf(stderr, "Syntax Error at line %d: %s before '%s'\n", yylineno, msg, yytext);
// close error handler
}

int main(int argc, char** argv) {
if (argc < 2) {
fprintf(stderr, "usage: compiler <source.asc> [output.abc]\n");
return 1;
}
if (!(yyin = fopen(argv[1], "r"))) {
fprintf(stderr, "Cannot read file: %s\n", argv[1]);
return 1;
}

init_environment();
if (yyparse() != 0) return 1;

// for phase 3: print intermediate quads
print_quads();

//for  phase 4: generate target code
generate_all();

// determine output filename
char outbin[512], outtxt[512];
if (argc >= 3) {
snprintf(outbin, sizeof(outbin), "%s", argv[2]);
} else {
// replace .asc with .abc, or append .abc
const char* dot = strrchr(argv[1], '.');
if (dot) {
size_t base = (size_t)(dot - argv[1]);
snprintf(outbin, sizeof(outbin), "%.*s.abc", (int)base, argv[1]);
} else {
snprintf(outbin, sizeof(outbin), "%s.abc", argv[1]);
}
}
snprintf(outtxt, sizeof(outtxt), "%s.txt", outbin);

write_binary(outbin);
write_text(outtxt);
fprintf(stdout, "compiled: %s  (text: %s)\n", outbin, outtxt);

return 0;
}
