#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "symtable.h"

sym_env_t my_env;

// custom hash function to minimize collisions
unsigned int hash(const char* key) {
    unsigned int hash_val = 0;
    int i = 0;

    while (key[i] != '\0') {
        // add the character multiplied by its position (i+1)
        // the (i+1) ensures that "ab" produces a different hash than "ba"
        hash_val = hash_val + (key[i] * (i + 1));
        i++;
    }

    return hash_val % 509; // return modulo array size
}

void init_environment() {
    int i;
    for (i = 0; i < 509; i++) my_env.hash_table[i] = NULL;
    for (i = 0; i < 1000; i++) my_env.scope_lists[i] = NULL;

    // hardcoded library functions
    const char* builtins[] = {
        "print", "input", "objectmemberkeys", "objecttotalmembers",
        "objectcopy", "totalarguments", "argument", "typeof",
        "strtonum", "sqrt", "cos", "sin"
    };

    for (i = 0; i < 12; i++) {
        add_symbol(builtins[i], SYM_LIB_FUNC, 0, 0);
    }
}

symrec* add_symbol(const char* name, enum symbol_t type, int scope, int line) {
    symrec* node = (symrec*)malloc(sizeof(symrec));
    node->s_name = strdup(name);
    node->s_type = type;
    node->s_scope = scope;
    node->s_line = line;
    node->is_active = 1;

    unsigned int idx = hash(name);

    // insert into hash table at the head of the list
    node->next_hash = my_env.hash_table[idx];
    my_env.hash_table[idx] = node;

    // insert into scope list at the head too
    node->next_scope = my_env.scope_lists[scope];
    my_env.scope_lists[scope] = node;

    return node;
}

symrec* lookup_in_scope(const char* name, int scope) {
    unsigned int idx = hash(name);
    symrec* curr = my_env.hash_table[idx];

    while (curr != NULL) {
        if (curr->is_active && curr->s_scope == scope && strcmp(curr->s_name, name) == 0) {
            return curr;
        }
        curr = curr->next_hash;
    }
    return NULL;
}

symrec* lookup_active_symbol(const char* name) {
    unsigned int idx = hash(name);
    symrec* curr = my_env.hash_table[idx];
    symrec* best = NULL;

    while (curr != NULL) {
        if (curr->is_active && strcmp(curr->s_name, name) == 0) {
            if (best == NULL || curr->s_scope > best->s_scope) {
                best = curr;
            }
        }
        curr = curr->next_hash;
    }
    return best;
}

void hide_scope(int scope) {
    if (scope < 0 || scope >= 1000) return;
    symrec* curr = my_env.scope_lists[scope];
    while (curr != NULL) {
        curr->is_active = 0; // just hide it, do not free the memory
        curr = curr->next_scope;
    }
}

// helper function to get the string format of the enum
const char* get_type_str(enum symbol_t t) {
    switch (t) {
        case SYM_GLOBAL_VAR: return "global variable";
        case SYM_LOCAL_VAR:  return "local variable";
        case SYM_FORMAL_ARG: return "formal argument";
        case SYM_USER_FUNC:  return "user function";
        case SYM_LIB_FUNC:   return "library function";
        default: return "unknown";
    }
}

// we use recursion to print them in the correct order
// because we insert at the head now
void print_list_recursive(symrec* node) {
    if (node == NULL) return;
    print_list_recursive(node->next_scope);
    printf("\"%s\" [%s] (line %d) (scope %d)\n",
        node->s_name, get_type_str(node->s_type), node->s_line, node->s_scope);
}

void print_environment() {
    printf("\n----------- SYMBOL TABLE -----------\n");
    for (int i = 0; i < 1000; i++) {
        if (my_env.scope_lists[i] != NULL) {
            printf("Scope #%d\n", i);
            print_list_recursive(my_env.scope_lists[i]);
            printf("\n");
        }
    }
    printf("------------------------------------\n");
}