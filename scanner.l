%{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.h"

int token_counter = 1;
int comment_nesting = 0;
int comment_start_line = 0;

char* string_buf = NULL;
size_t string_buf_size = 0;
size_t string_buf_len = 0;

void append_char(char c) {
    if (string_buf_len + 2 > string_buf_size) {
        string_buf_size = (string_buf_size == 0) ? 256 : string_buf_size * 2;
        string_buf = (char*)realloc(string_buf, string_buf_size);
    }
    string_buf[string_buf_len++] = c;
    string_buf[string_buf_len] = '\0';
}

void append_str(char* s) {
    while (*s) {
        append_char(*s++);
    }
}
%}

%option noyywrap
%option yylineno

%x COMMENT_BLOCK
%x STRING_STATE

%%

"if"        { return IF; }
"for"       { return FOR; }
"while"     { return WHILE; }
"else"      { return ELSE; }
"function"  { return FUNCTION; }
"continue"  { return CONTINUE; }
"break"     { return BREAK; }
"return"    { return RETURN; }
"and"       { return AND; }
"or"        { return OR; }
"not"       { return NOT; }
"local"     { return LOCAL; }
"true"      { return TRUE; }
"false"     { return FALSE; }
"nil"       { return NIL; }

"=="        { return EQUAL; }
"="         { return ASSIGN; }
"+"         { return PLUS; }
"-"         { return MINUS; }
"*"         { return MULTIPLY; }
"/"         { return DIVIDE; }
"%"         { return MODULO; }
"!="        { return NOT_EQUAL; }
"++"        { return PLUS_PLUS; }
"--"        { return MINUS_MINUS; }
">="        { return GREATER_EQUAL; }
"<="        { return LESS_EQUAL; }
">"         { return GREATER; }
"<"         { return LESS; }

"{"         { return LEFT_BRACE; }
"}"         { return RIGHT_BRACE; }
"["         { return LEFT_BRACKET; }
"]"         { return RIGHT_BRACKET; }
"("         { return LEFT_PAREN; }
")"         { return RIGHT_PAREN; }
";"         { return SEMICOLON; }
","         { return COMMA; }
"::"        { return DOUBLE_COLON; }
":"         { return COLON; }
".."        { return DOUBLE_DOT; }
"."         { return DOT; }

[0-9]+ {
    yylval.intVal = atoi(yytext);
    return INTCONST;
}

[0-9]+\.[0-9]* {
    yylval.realVal = atof(yytext);
    return REALCONST;
}

[a-zA-Z][a-zA-Z_0-9]* {
    yylval.strVal = strdup(yytext);
    return IDENT;
}

[ \t\r\n]+ { }

"//".* { }

"/*" {
    BEGIN(COMMENT_BLOCK);
    comment_nesting = 1;
    comment_start_line = yylineno;
}

<COMMENT_BLOCK>"/*" { comment_nesting++; }

<COMMENT_BLOCK>"*/" {
    comment_nesting--;
    if(comment_nesting == 0) {
        BEGIN(INITIAL);
    }
}

<COMMENT_BLOCK>.|\n {}

<COMMENT_BLOCK><<EOF>> {
    fprintf(stderr, "Error: Unclosed block comment starting at line %d\n", comment_start_line);
    exit(1);
}

\" {
    BEGIN(STRING_STATE);
    string_buf_len = 0;
    if (string_buf) string_buf[0] = '\0';
}

<STRING_STATE>\" {
    BEGIN(INITIAL);
    yylval.strVal = strdup(string_buf ? string_buf : "");
    return STRING;
}

<STRING_STATE>\\n  { append_char('\n'); }
<STRING_STATE>\\t  { append_char('\t'); }
<STRING_STATE>\\\\ { append_char('\\'); }
<STRING_STATE>\\\" { append_char('\"'); }

<STRING_STATE>\\.  {
    fprintf(stderr, "Warning: Illegal escape char(\\%c) at line %d\n", yytext[1], yylineno);
    append_char(yytext[1]);
}

<STRING_STATE>\n { append_char('\n'); }

<STRING_STATE>[^\\\n\"]+ {
    append_str(yytext);
}

<STRING_STATE><<EOF>> {
    fprintf(stderr, "Error: EOF inside string\n");
    exit(1);
}

. {
    fprintf(stderr, "Error: Undefined input %s in line %d\n", yytext, yylineno);
}

%%