#ifndef SYMTABLE_H
#define SYMTABLE_H

// define the different symbol types
enum symbol_t {
    SYM_GLOBAL_VAR,
    SYM_LOCAL_VAR,
    SYM_FORMAL_ARG,
    SYM_USER_FUNC,
    SYM_LIB_FUNC
};

// define scope spaces for offset tracking
enum scopespace_t {
    programvar,
    functionlocal,
    formalarg
};

// main symbol record structure
typedef struct symrec {
    int is_active;
    char* s_name;
    enum symbol_t s_type;
    int s_scope;
    int s_line;

    // fields for scope space and intermediate code
    enum scopespace_t space;
    unsigned offset;
    unsigned iaddress;

    unsigned total_locals;

    struct symrec *next_hash;
    struct symrec *next_scope;
} symrec;

// wrapper struct to hold our hash table and scope lists securely
typedef struct sym_env {
    symrec* hash_table[509];
    symrec* scope_lists[1000];
} sym_env_t;

// function prototypes for symbol table management
void init_environment();
symrec* add_symbol(const char* name, enum symbol_t type, int scope, int line);
symrec* lookup_in_scope(const char* name, int scope);
symrec* lookup_active_symbol(const char* name);
void hide_scope(int scope);
void print_environment();

#endif