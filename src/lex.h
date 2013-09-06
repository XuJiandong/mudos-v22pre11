#ifndef _LEX_H_
#define _LEX_H_

#include "std.h"

#define DEFMAX 10000
#define MAXLINE 1024
#define MLEN 4096
#define NSIZE 256
#define MAX_INSTRS 512
#define EXPANDMAX 25000
#define NARGS 25
#define MARKS '@'

#define SKIPWHITE while (isspace(*p) && (*p != '\n')) p++

#define DEFAULT_NONE           0xff
#define DEFAULT_THIS_OBJECT    0xfe

#define PRAGMA_STRICT_TYPES    1
#define PRAGMA_WARNINGS        2
#define PRAGMA_SAVE_TYPES      4
#define PRAGMA_SAVE_BINARY     8
#define PRAGMA_OPTIMIZE       16
#define PRAGMA_ERROR_CONTEXT  32
#define PRAGMA_OPTIMIZE_HIGH  64
/* for find_or_add_ident */
#define FOA_GLOBAL_SCOPE       0x1
#define FOA_NEEDS_MALLOC       0x2

typedef struct {
  SIGNED short local_num, global_num, efun_num;
  SIGNED short function_num, simul_num, class_num;
} defined_name_t;

typedef struct ifstate_s {
    struct ifstate_s *next;
    int state;
} ifstate_t;

typedef struct defn_s {
    struct defn_s *next;
    char *name;
    char *exps;
    int flags;
    int nargs;
} defn_t;

/* must be a power of 4 */
#define DEFHASH 64
#define defhash(s) (whashstr((s), 10) & (DEFHASH - 1))

#define DEF_IS_UNDEFINED 1
#define DEF_IS_PREDEF    2
/* used only in edit_source */
#define DEF_IS_NOT_LOCAL 4

/* to speed up cleaning the hash table, and identify the union */
#define IHE_RESWORD    0x8000
#define IHE_EFUN       0x4000
#define IHE_SIMUL      0x2000
#define IHE_PERMANENT  (IHE_RESWORD | IHE_EFUN | IHE_SIMUL)
#define TOKEN_MASK     0x0fff

#define INDENT_HASH_SIZE 1024 /* must be a power of 2 */

typedef struct ident_hash_elem_s {
    char *name;
    short token; /* only flags */
    short sem_value; /* for these, a count of the ambiguity */
    struct ident_hash_elem_s *next;
/* the fields above must correspond to struct keyword_t */
    struct ident_hash_elem_s *next_dirty;
    defined_name_t dn;
} ident_hash_elem_t;

typedef struct {
    char *word;
    unsigned short token;       /* flags here too */
    short sem_value;            /* semantic value for predefined tokens */
    ident_hash_elem_t *next;
/* the fields above must correspond to struct ident_hash_elem */
    short min_args;		/* Minimum number of arguments. */
    short max_args;		/* Maximum number of arguments. */
    short ret_type;		/* The return type used by the compiler. */
    unsigned short arg_type1;	/* Type of argument 1 */
    unsigned short arg_type2;	/* Type of argument 2 */
    unsigned short arg_type3;	/* Type of argument 1 */
    unsigned short arg_type4;	/* Type of argument 2 */
    short arg_index;		/* Index pointing to where to find arg type */
    short Default;		/* an efun to use as default for last
				 * argument */
} keyword_t;

typedef struct lpc_predef_s {
    char *flag;
    struct lpc_predef_s *next;
} lpc_predef_t;

#define EXPECT_ELSE 1
#define EXPECT_ENDIF 2

extern lpc_predef_t *lpc_predefs;

#define isalunum(c) (isalnum(c) || (c) == '_')

/*
 * Information about all instructions. This is not really needed as the
 * automatically generated efun_arg_types[] should be used.
 */

/* indicates that the instruction is only used at compile time */
#define F_ALIAS_FLAG 1024

typedef struct {
    short max_arg, min_arg;  /* Can't use char to represent -1 */
    short type[4];	     /* need a short to hold the biggest type flag */
    short Default;
    short ret_type;
    char *name;
#ifdef LPC_TO_C
    char *routine;
#endif
    int arg_index;
} instr_t;

/*
 * lex.c
 */
extern instr_t instrs[512];
extern int current_line;
extern int current_line_base;
extern int current_line_saved;
extern int total_lines;
extern char *current_file;
extern int current_file_id;
extern int pragmas;
extern int num_parse_error;
extern lpc_predef_t *lpc_predefs;
extern int efun_arg_types[];
extern char yytext[1024];
extern instr_t instrs[];
extern keyword_t predefs[];

INLINE void push_function_context PROT((void));
void pop_function_context PROT((void));
int yylex PROT((void));
void init_num_args PROT((void));
char *query_instr_name PROT((int));
char *get_f_name PROT((int));
void set_inc_list PROT((char *));
void start_new_file PROT((int));
void end_new_file PROT((void));
int lookup_predef PROT((char *));
void add_predefines PROT((void));
char *main_file_name PROT((void));
char *get_defined_name PROT((defined_name_t *));
ident_hash_elem_t *find_or_add_ident PROT((char *, int));
ident_hash_elem_t *find_or_add_perm_ident PROT((char *));
ident_hash_elem_t *lookup_ident PROT((char *));
void free_unused_identifiers PROT((void));
void init_identifiers PROT((void));
char *show_error_context PROT((void));
#ifdef DEBUGMALLOC_EXTENSIONS
void mark_all_defines PROT((void));
#endif

#endif
