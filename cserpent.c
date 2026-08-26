/*
	Harris M. Snyder, 2023
	This is free and unencumbered software released into the public domain.

	This is the main file for CSerpent. You only need this and stb_c_lexer.h
	if you are building CSerpent as a standalone program.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <limits.h>


#define STB_C_LEXER_IMPLEMENTATION
#include "stb_c_lexer.h"

#define MAX_FN_ARGS 40
#define MAX_DIRS 40
#define MAX_FILES 40

enum type_category {
	T_UNINITIALIZED = 0,
	T_UNKNOWN,
	T_CHAR,
	T_SHORT,
	T_INT,
	T_LONG,
	T_LLONG,
	T_FLOAT,
	T_FLOAT16,
	T_DOUBLE,
	T_LDOUBLE,
	T_STRUCT,
	T_UNION,
	T_VOID,
	T_BOOL,
};

const char *type_category_strings[] = {
	[T_UNINITIALIZED] = "<uninitialized>",
	[T_UNKNOWN] = "<unknown>",
	[T_VOID] = "void",
	[T_BOOL] = "_Bool",
	[T_CHAR] = "char",
	[T_SHORT] = "short",
	[T_INT] = "int",
	[T_LONG] = "long",
	[T_LLONG] = "long long",
	[T_FLOAT] = "float",
	[T_FLOAT16] = "_Float16",
	[T_DOUBLE] = "double",
	[T_LDOUBLE] = "long double",
	[T_STRUCT] = "struct",
	[T_UNION] = "union",
};

typedef struct type {
	enum type_category category;
	unsigned short explicit_signed : 1;
	unsigned short is_unsigned  : 1;
	unsigned short is_complex   : 1;
	unsigned short is_imaginary : 1;
	unsigned short is_const     : 1;
	unsigned short is_restrict  : 1;
	unsigned short is_volatile  : 1;
	unsigned short is_pointer          : 1;
	unsigned short is_pointer_const    : 1;
	unsigned short is_pointer_restrict : 1;
	unsigned short is_pointer_volatile : 1;
} Type;


typedef struct {
	char *name;
	Type type;
} Symbol;

typedef struct {
	Type type;
	char suffix; 
	char found;
} VariantSuffix;

typedef struct { // lexer token

	int toktype; // this will be one of the enum values in stb_c_lexer
	int string_len;
	union {
		double real_number;
		long long int_number;
		char * string;
	};
} Token;

#include <setjmp.h>

/*
	An annotation's source location, recovered from cpp line markers.
*/
typedef struct {
	const char *file;
	int line;
} Loc;

/*
	Per-item options, from the annotation that requested the wrap.
	Tri-state fields use -1 for "unset, inherit from CSERPENT_CONFIG".
*/
typedef struct {
	const char *py_name;
	const char *doc;
	const char *errcheck;
	int          errarg;
	signed char  errstr;
	signed char  addresses;
	signed char  bytes;
	signed char  strip_underscore;
} WrapOpts;

enum wrap_kind {
	WK_FN = 1,
	WK_GENERIC,
	WK_MANUAL,
	WK_CONST,
	WK_OPAQUE,
};

typedef struct {
	int       kind;
	const char *name;
	WrapOpts  opts;
	int       input;   /* index into the input file list */
	Loc       loc;
} WrapItem;

/*
	Output-global settings from CSERPENT_CONFIG. -1 means unset; loc records
	where a value was set, so a conflict can name both sites.
*/
typedef struct {
	signed char value;
	Loc         loc;
} ConfigVal;

typedef struct
{
	int verbose;
	int warnings;
	const char * modulename;
	const char * filename;
	const char * preprocessor;

	ConfigVal cfg_addresses;
	ConfigVal cfg_bytes;
	ConfigVal cfg_declarations;
	ConfigVal cfg_float16;

	/* the item currently being emitted, if any */
	const WrapOpts *opts;

	FILE *ostream;
	FILE *estream;
	FILE *istream;

	FILE *volatile * open_file;
	jmp_buf *jmp;

} CSerpentArgs;

/* resolve a per-item tri-state against the global default */
static int
opt_or_cfg(signed char item, ConfigVal cfg, int fallback)
{
	if (item >= 0) return item;
	if (cfg.value >= 0) return cfg.value;
	return fallback;
}

enum {
	MAX_STRINGS_EXP=17,
	MAX_STRING_HEAP=(1<<24),
	MAX_SYMBOLS=10000,
	MAX_ANNLOCS=8000,
	MAX_ITEMS=2000,
	MAX_ENUMCONSTS=40000,
	MAX_EXPORTS=4000,
	MAX_INPUTS=200,
};

/* one enum constant, with the tag of the enum it belongs to ("" if anonymous) */
typedef struct {
	const char *tag;
	const char *name;
} EnumConst;

/* one entry in the generated module's method table */
typedef struct {
	const char *py_name;
	const char *c_name;
	const char *doc;
} Export;

/* location of a CSERPENT_-prefixed identifier, keyed by its token index */
typedef struct {
	int  tok_index;
	Loc  loc;
} AnnLoc;

typedef struct {

	// String table
	int num_strings, heap_size;
	char heap[MAX_STRING_HEAP];
	char *table[1<<MAX_STRINGS_EXP];

	// Symbol table
	int nsym;
	Symbol symbols[MAX_SYMBOLS];

	// Annotation locations, recorded during lexing
	int nannlocs;
	AnnLoc annlocs[MAX_ANNLOCS];

	// Every enum constant seen across all inputs, for WRAPCONST resolution
	int nenumconsts;
	EnumConst enumconsts[MAX_ENUMCONSTS];

	// Work list, built in pass 1
	int nitems;
	WrapItem items[MAX_ITEMS];

	// Module method table, built in pass 2
	int nexports;
	Export exports[MAX_EXPORTS];

	// Preprocessed text of each input, kept between passes. stdin can only be
	// read once, and re-running cpp per pass would be wasteful anyway.
	char *cached[MAX_INPUTS];
} StorageBuffers;

typedef struct {
	Token     *tokens;   
	Token     *tokens_end;
	Token     *tokens_first;   
	StorageBuffers *storage;
	CSerpentArgs args;
} ParseCtx;


/*
	==========================================================
		Helper functions for parsing and handling errors
	==========================================================
*/

#define ssizeof(x) ((int64_t)sizeof(x))
#define COUNT_ARRAY(x) ((int64_t)(sizeof(x)/sizeof(x[0])))

#define RESTORE(p)  (*p = p_saved);
#define SAVE(p) ParseCtx p_saved = *p; 
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))

#ifdef CSERPENT_DISABLE_ASSERT
#  define assert(c)
#else
#  if defined(_MSC_VER)
#    define assert(c) if(!(c)){__debugbreak();}
#  else
#    if defined(__GNUC__) || defined(__clang__)
#      define assert(c) if(!(c)){__builtin_trap();}
#    else 
#      define assert(c) if(!(c)){*(volatile int*)0=0;}
#    endif 
#  endif
#endif


static int 
repr_type(int bufsz, char buf[], Type type) 
{

	char *is_signed    = type.explicit_signed ? "signed " : "";
        char *is_unsigned  = type.is_unsigned     ? "unsigned " : "";
	char *is_complex   = type.is_complex      ? "_Complex " : "";
	char *is_imaginary = type.is_imaginary    ? "_Imaginary " : "";
	char *is_const     = type.is_const        ? "const " : "";
	char *is_restrict  = type.is_restrict     ? "restrict " : "";
	char *is_volatile  = type.is_volatile     ? "volatile " : "";
	char *is_pointer   = type.is_pointer      ? "*" : "";

	char *is_pointer_const    = type.is_pointer_const    ? "const " : "";
	char *is_pointer_restrict = type.is_pointer_restrict ? "restrict " : "";
	char *is_pointer_volatile = type.is_pointer_volatile ? "volatile " : "";

	return snprintf(buf, bufsz, "%s%s%s %s%s%s%s%s%s%s%s%s", 
		is_signed, is_unsigned, type_category_strings[type.category], is_complex, is_imaginary,
		is_const, is_restrict, is_volatile, is_pointer,
		is_pointer_const, is_pointer_restrict, is_pointer_volatile);
}


static int 
repr_symbol(int bufsz, char buf[], Symbol s) 
{

	long x = snprintf(buf, bufsz, "%s :=  ", s.name);
	bufsz -= x;
	buf += x;
	// TODO bug: make sure we haven't already overflown the buffer.

	return x + repr_type(bufsz, buf, s.type);
}

static void 
repr_token(int bufsz, char buf[], Token t)
{
	switch (t.toktype) {
		case CLEX_id        : snprintf(buf, bufsz,"%s", t.string); break;
		case CLEX_eq        : snprintf(buf, bufsz,"=="); break;
		case CLEX_noteq     : snprintf(buf, bufsz,"!="); break;
		case CLEX_lesseq    : snprintf(buf, bufsz,"<="); break;
		case CLEX_greatereq : snprintf(buf, bufsz,">="); break;
		case CLEX_andand    : snprintf(buf, bufsz,"&&"); break;
		case CLEX_oror      : snprintf(buf, bufsz,"||"); break;
		case CLEX_shl       : snprintf(buf, bufsz,"<<"); break;
		case CLEX_shr       : snprintf(buf, bufsz,">>"); break;
		case CLEX_plusplus  : snprintf(buf, bufsz,"++"); break;
		case CLEX_minusminus: snprintf(buf, bufsz,"--"); break;
		case CLEX_arrow     : snprintf(buf, bufsz,"->"); break;
		case CLEX_andeq     : snprintf(buf, bufsz,"&="); break;
		case CLEX_oreq      : snprintf(buf, bufsz,"|="); break;
		case CLEX_xoreq     : snprintf(buf, bufsz,"^="); break;
		case CLEX_pluseq    : snprintf(buf, bufsz,"+="); break;
		case CLEX_minuseq   : snprintf(buf, bufsz,"-="); break;
		case CLEX_muleq     : snprintf(buf, bufsz,"*="); break;
		case CLEX_diveq     : snprintf(buf, bufsz,"/="); break;
		case CLEX_modeq     : snprintf(buf, bufsz,"%%="); break;
		case CLEX_shleq     : snprintf(buf, bufsz,"<<="); break;
		case CLEX_shreq     : snprintf(buf, bufsz,">>="); break;
		case CLEX_eqarrow   : snprintf(buf, bufsz,"=>"); break;
		case CLEX_dqstring  : snprintf(buf, bufsz,"\"%s\"", t.string); break;
		case CLEX_sqstring  : snprintf(buf, bufsz,"'\"%s\"'", t.string); break;
		case CLEX_charlit   : snprintf(buf, bufsz,"'%s'", t.string); break;
		case CLEX_intlit    : snprintf(buf, bufsz,"#%lli", t.int_number); break;
		case CLEX_floatlit  : snprintf(buf, bufsz,"%g", t.real_number); break;
		default:
				      if (t.toktype >= 0 && t.toktype < 256)
					      snprintf(buf, bufsz,"%c", (int) t.toktype);
				      else {
					      snprintf(buf, bufsz,"<<<UNKNOWN TOKEN %d >>>\n", t.toktype);
				      }
				      break;
	}
}

static void 
dump_context(FILE *f, ParseCtx *p)
{
	long long before = MIN(p->tokens - p->tokens_first, 20); 
	long long after  = MIN(p->tokens_end - p->tokens, 20);

	for (int i = -before; i < after; i++)
	{
		char buf[1000] = {0};
		repr_token(sizeof(buf), buf, p->tokens[i]);
		if( i == 0 )
			fprintf(f, ">>HERE<< %s ", buf);
		else 
			fprintf(f, "%s ", buf);
	}
	fprintf(f,"\n");
}

static _Noreturn void
terminate(CSerpentArgs *args) {
	(void) args;
	longjmp(*args->jmp, 1);
}

static _Noreturn void
die (ParseCtx *p, const char * fmt, ...)
{
	FILE *where = p ? p->args.estream : stderr;
	va_list va;
	va_start(va, fmt);
	vfprintf(where, fmt, va);
	va_end(va);
	fprintf(where, "\n");
	if(p) dump_context(where, p);
	terminate(&p->args);
}

static _Noreturn void
die2 (CSerpentArgs args, const char * fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	vfprintf(args.estream, fmt, va);
	va_end(va);
	fprintf(args.estream, "\n");
	terminate(&args);
}

static uint64_t 
hash (char *s, int32_t len)
{
	uint64_t h = 0x100;
	for (int32_t i = 0; i < len; i++) {
		h ^= s[i] & 255;
		h *= 1111111111111111111;
	}
	return h ^ h>>32;
}

static int32_t 
ht_lookup(uint64_t hash, int exp, int32_t idx)
{
	uint32_t mask = ((uint32_t)1 << exp) - 1;
	uint32_t step = (hash >> (64 - exp)) | 1;
	return (idx + step) & mask;
}

static char *
intern_string(CSerpentArgs args, StorageBuffers *st, char *key, int keylen)
{
	if (keylen == 0) keylen = strlen(key);
	uint64_t h = hash(key, keylen+1);
	for (int32_t i = h;;) {
		i = ht_lookup(h, MAX_STRINGS_EXP, i);
		if (!st->table[i]) {
			// empty, insert here
			if (st->num_strings+1 == COUNT_ARRAY(st->table)/2)
				die2(args, "intern: string table full");
			if (st->heap_size + keylen + 1 >= MAX_STRING_HEAP)
				die2(args, "intern: string heap full");
			st->num_strings++;
			st->table[i] = st->heap+st->heap_size;
			memcpy(st->table[i], key, keylen);
			st->heap_size += keylen;
			st->heap[st->heap_size++] = 0;
			return st->table[i];
		} else if (!strcmp(st->table[i], key)) {
			// found, return canonical instance
			return st->table[i];
		}
	}
}

static int 
xatoi (CSerpentArgs args, const char *x, int *nchars_read)
{
	const char * save = x;

	int sign = 1;
	int n = 0;
	int v = 0;

	if(x[0] == '-') {sign = -1; x++;}
	if(x[0] == '+') {sign =  1; x++;}

	while (x[0]  &&  x[0] >= 48  &&  x[0] <= 48+9)
	{
		int digit = x[0] - 48;

		if (INT_MAX / 10 < v) goto overflow;
		v *= 10;

		if (INT_MAX - digit < v) goto overflow;
		v += digit;

		n++;
		x++;
	}

	if (!n) die2(args, "couldn't parse '%6s' as an integer", save);

	if (nchars_read) *nchars_read = x-save;
	return v * sign;

overflow:
	die2(args, "integer overflow when trying to convert '%14s'", save);
}


static Symbol *
add_symbol(CSerpentArgs args, StorageBuffers *storage, Symbol s)
{
	if(storage->nsym == COUNT_ARRAY(storage->symbols)) die2(args, "symbol table full");
	s.name = intern_string(args, storage, s.name, 0);
	storage->symbols[storage->nsym] = s;
	return &storage->symbols[storage->nsym++];
}

static Symbol *
get_symbol(StorageBuffers *storage, char *name)
{
	for(int i = 0; i < storage->nsym; i++)
		if(!strcmp(name,storage->symbols[i].name)) return &storage->symbols[i];
	return 0;
}

static Symbol *
get_symbol_or_die(CSerpentArgs args, StorageBuffers *storage, char *name)
{
	Symbol *s = get_symbol(storage, name);
	if(!s) die2(args, "Unknown type: %s", name);
	return s;
}

static void 
clear_symbols(StorageBuffers *storage)
{
	storage->nsym = 0;
}

static int 
modify_type_pointer(ParseCtx *p, Type *type)
{
	assert(type);
	if (type->is_pointer) 
		return 0;
	type->is_pointer = 1;
	return 1;
}

static void 
modify_type_const(ParseCtx *p, Type *type)
{
	(void) p;
	assert(type);
	if(type->is_pointer) type->is_pointer_const = 1;
	else type->is_const = 1;
}

static void 
modify_type_restrict(ParseCtx *p, Type *type)
{
	(void) p;
	assert(type);
	if(type->is_pointer) type->is_pointer_restrict = 1;
	else type->is_restrict = 1;
}

static void 
modify_type_volatile(ParseCtx *p, Type *type)
{
	(void) p;
	assert(type);
	if(type->is_pointer) type->is_pointer_volatile = 1;
	else type->is_volatile = 1;
}

static void 
modify_type_struct(ParseCtx *p, Type *type)
{
	assert(type);
	if(type->category == T_UNKNOWN) return;
	if(type->category == T_UNINITIALIZED) type->category = T_STRUCT;
	else die(p, "'struct' does not make sense with '%s'", type_category_strings[type->category]);
}

static void 
modify_type_union(ParseCtx *p, Type *type)
{
	assert(type);
	if(type->category == T_UNKNOWN) return;
	if(type->category == T_UNINITIALIZED) type->category = T_UNION;
	else die(p, "'union' does not make sense with '%s'", type_category_strings[type->category]);
}

static void 
modify_type_void(ParseCtx *p, Type *type)
{
	assert(type);
	if(type->category == T_UNKNOWN) return;

	if(type->is_unsigned)     die(p, "'void' does not make sense with 'unsigned'");
	if(type->explicit_signed) die(p, "'void' does not make sense with 'signed'");
	if(type->is_imaginary)     die(p, "'void' does not make sense with '_Imaginary'");
	if(type->is_complex)       die(p, "'void' does not make sense with '_Complex'");

	if(type->category == T_UNINITIALIZED) type->category = T_VOID;
	else die(p, "'void' does not make sense with '%s'", type_category_strings[type->category]);
}

static void 
modify_type_char(ParseCtx *p, Type *type)
{
	assert(type);
	if(type->category == T_UNKNOWN) return;
	if(type->is_imaginary)     die(p, "'char' does not make sense with '_Imaginary'");
	if(type->is_complex)       die(p, "'char' does not make sense with '_Complex'");
	if(type->category == T_UNINITIALIZED) type->category = T_CHAR;
	else die(p, "'char' does not make sense with '%s'", type_category_strings[type->category]);
}

static void 
modify_type_short(ParseCtx *p, Type *type)
{
	assert(type);
	if(type->category == T_UNKNOWN) return;
	if(type->is_imaginary)     die(p, "'short' does not make sense with '_Imaginary'");
	if(type->is_complex)       die(p, "'short' does not make sense with '_Complex'");
	if(type->category == T_UNINITIALIZED || type->category == T_INT) type->category = T_SHORT;
	else die(p, "'short' does not make sense with '%s'", type_category_strings[type->category]);
}

static void 
modify_type_int(ParseCtx *p, Type *type)
{
	assert(type);
	if(type->category == T_UNKNOWN) return;
	if(type->is_imaginary)     die(p, "'int' does not make sense with '_Imaginary'");
	if(type->is_complex)       die(p, "'int' does not make sense with '_Complex'");
	if(type->category == T_SHORT 
		|| type->category == T_LONG 
		|| type->category == T_LLONG ) return;

	if(type->category == T_UNINITIALIZED) type->category = T_INT;
	else die(p, "'int' does not make sense with '%s'", type_category_strings[type->category]);
}

static void 
modify_type_long(ParseCtx *p, Type *type) 
{
	assert(type);
	if(type->category == T_UNKNOWN) return;

	if(type->category == T_UNINITIALIZED) type->category = T_LONG;
	else if (type->category == T_INT) type->category = T_LONG;
	else if (type->category == T_LONG) type->category = T_LLONG;
	else if (type->category == T_DOUBLE) type->category = T_LDOUBLE;
	else die(p, "'long' does not make sense with '%s'", type_category_strings[type->category]);
}

static void 
modify_type_float16(ParseCtx *p, Type *type)
{
	assert(type);
	if(type->category == T_UNKNOWN) return;
	if(type->is_unsigned)     die(p, "'_Float16' does not make sense with 'unsigned'");
	if(type->explicit_signed) die(p, "'_Float16' does not make sense with 'signed'");

	if(type->category == T_UNINITIALIZED) type->category = T_FLOAT16;
	else die(p, "'_Float16' does not make sense with '%s'", type_category_strings[type->category]);
}

static void 
modify_type_float(ParseCtx *p, Type *type)
{
	assert(type);
	if(type->category == T_UNKNOWN) return;
	if(type->is_unsigned)     die(p, "'float' does not make sense with 'unsigned'");
	if(type->explicit_signed) die(p, "'float' does not make sense with 'signed'");

	if(type->category == T_UNINITIALIZED) type->category = T_FLOAT;
	else die(p, "'float' does not make sense with '%s'", type_category_strings[type->category]);
}

static void 
modify_type_double(ParseCtx *p, Type *type)
{
	assert(type);
	if(type->category == T_UNKNOWN) return;
	if(type->is_unsigned)     die(p, "'double' does not make sense with 'unsigned'");
	if(type->explicit_signed) die(p, "'double' does not make sense with 'signed'");

	if(type->category == T_UNINITIALIZED) type->category = T_DOUBLE;
	else if(type->category == T_LONG) type->category = T_LDOUBLE;
	else die(p, "'double' does not make sense with '%s'", type_category_strings[type->category]);
}

static void 
modify_type_signed(ParseCtx *p, Type *type)
{
	assert(type);
	if(type->category >= T_FLOAT) die(p, "'signed' doesn't make sense with non-integer types");
	if(type->is_unsigned) die(p, "'signed' doesn't make sense with 'unsigned'");
	type->explicit_signed = 1;
}

static void 
modify_type_unsigned(ParseCtx *p, Type *type)
{
	assert(type);
	if(type->category >= T_FLOAT) die(p, "'unsigned' doesn't make sense with non-integer types");
	if(type->is_unsigned) die(p, "'unsigned' doesn't make sense with 'signed'");
	type->is_unsigned = 1;
}

static void 
modify_type_bool(ParseCtx *p, Type *type)
{
	assert(type);
	if(type->category == T_UNKNOWN) return;
	if(type->is_unsigned)     die(p, "'_Bool' does not make sense with 'unsigned'");
	if(type->explicit_signed) die(p, "'_Bool' does not make sense with 'signed'");

	if(type->category == T_UNINITIALIZED) type->category = T_BOOL;
	else die(p, "'_Bool' does not make sense with '%s'", type_category_strings[type->category]);
}

static void 
modify_type_complex(ParseCtx *p, Type *type)
{
	assert(type);
	if(type->category >= T_CHAR && type->category < T_FLOAT) die(p, "'_Complex' doesn't make sense with integer types");
	if(type->is_imaginary) die(p, "'_Complex' doesn't make sense with '_Imaginary'");
	type->is_complex = 1;
	// TODO check against struct/union?
}

static void 
modify_type_imaginary(ParseCtx *p, Type *type)
{
	assert(type);
	if(type->category >= T_CHAR && type->category < T_FLOAT) die(p, "'_Imaginary' doesn't make sense with integer types");
	if(type->is_complex) die(p, "'_Imaginary' doesn't make sense with '_Complex'");
	type->is_imaginary = 1;
	// TODO check against struct/union?
}

static int 
compare_types_equal(Type a, Type b, int compare_pointer, int compare_const, int compare_volatile, int compare_restrict) 
{
	if(a.category != b.category) return 0;

	if(a.category == T_FLOAT || a.category == T_FLOAT16 || a.category == T_DOUBLE || a.category == T_LDOUBLE) {

		if  (a.is_complex   != b.is_complex)     return 0;
		if  (a.is_imaginary != b.is_imaginary)  return 0;

	} else if (a.category >= T_CHAR && a.category <= T_LLONG) {

		if (a.is_unsigned != b.is_unsigned) return 0;

	}

	if (compare_const) if (a.is_const != b.is_const) return 0;
	if (compare_volatile) if (a.is_volatile != b.is_volatile) return 0;
	if (compare_restrict) if (a.is_restrict != b.is_restrict) return 0;

	if (compare_pointer) {
		if (a.is_pointer != b.is_pointer) return 0;

		if (compare_const) if (a.is_pointer_const != b.is_pointer_const) return 0;
		if (compare_volatile) if (a.is_pointer_volatile != b.is_pointer_volatile) return 0;
		if (compare_restrict) if (a.is_pointer_restrict != b.is_pointer_restrict) return 0;
	}

	return 1;
}




/*
	==========================================================
		Templates
	==========================================================
*/



/*
	==========================================================
		Wrapper generation
	==========================================================
*/

static void
emit_module(
	CSerpentArgs args,
	int n_exports,
	Export exports[],
	int num_enum_consts,
	char *enum_consts[])
{
	if(!args.modulename) return;

	fprintf(args.ostream, "static PyMethodDef module_functions[] = { \n");

	for (int i = 0; i < n_exports; i++) {
		fprintf(args.ostream, "{\"%s\", (PyCFunction) wrap_%s, METH_VARARGS|METH_KEYWORDS, \"%s\"},\n",
			exports[i].py_name, exports[i].c_name,
			exports[i].doc ? exports[i].doc : "");
	}

	fprintf(args.ostream,
	"	{ NULL, NULL, 0, NULL } \n"
	"}; \n"
	"\n\n"
	"static const char module_name[] = \"%s\"; \n"
	"\n"
	"static struct PyModuleDef module_def = { \n"
	"	PyModuleDef_HEAD_INIT, \n"
	"	module_name,      /* m_name */ \n"
	"	NULL,             /* m_doc */ \n"
	"	-1,               /* m_size */ \n"
	"	module_functions, /* m_methods */ \n"
	"	NULL,             /* m_reload */ \n"
	"	NULL,             /* m_traverse */ \n"
	"	NULL,             /* m_clear  */ \n"
	"	NULL,             /* m_free */ \n"
	"}; \n"
	" \n"
	"static PyObject *module_init(void) \n"
	"{ \n"
	"	PyObject *m; \n"
	" \n"
	"	// Import numpy arrays \n"
	"	import_array1(NULL); \n"
	" \n"
	"	// Register the module \n"
	"	if (!(m = PyModule_Create(&module_def))) \n"
	"		return NULL; \n"
	" \n", args.modulename);

	for(int e = 0; e < num_enum_consts; e++) {
		fprintf(args.ostream, "	PyModule_AddIntConstant(m, \"%s\", %s);\n", enum_consts[e], enum_consts[e]);
	}

	fprintf(args.ostream,
	" \n"
	"	return m; \n"
	"} \n"
	" \n"
	"PyMODINIT_FUNC PyInit_%s (void) \n"
	"{ \n"
	"	return module_init(); \n"
	"} \n", args.modulename);
}

static void 
emit_preamble(CSerpentArgs args)
{
	(void) args;
	int f16 = args.cfg_float16.value > 0;
	char * f16_cplusplus =
		f16 ?
			"template<> struct CSERPENT_C2NPY_struct<_Float16> { static constexpr int value = NPY_FLOAT16; };" :
			"";
	char * f16_c =
		f16 ?
			"_Float16: NPY_FLOAT16," :
			"";
	fprintf(args.ostream, 
		"#define NPY_NO_DEPRECATED_API NPY_1_8_API_VERSION \n"
		"#define PY_ARRAY_UNIQUE_SYMBOL SHARED_ARRAY_ARRAY_API \n"
		"#include <Python.h> \n"
		"#include <stddef.h> \n"
		"#include <numpy/arrayobject.h> \n"
		"#ifdef __cplusplus \n"
		"template<typename T> struct CSERPENT_C2NPY_struct; \n"
		"template<> struct CSERPENT_C2NPY_struct<signed char> { static constexpr int value = NPY_BYTE; }; \n"
		"template<> struct CSERPENT_C2NPY_struct<short> { static constexpr int value = NPY_SHORT; }; \n"
		"template<> struct CSERPENT_C2NPY_struct<int> { static constexpr int value = NPY_INT; }; \n"
		"template<> struct CSERPENT_C2NPY_struct<long> { static constexpr int value = NPY_LONG; }; \n"
		"template<> struct CSERPENT_C2NPY_struct<long long> { static constexpr int value = NPY_LONGLONG; }; \n"
		"template<> struct CSERPENT_C2NPY_struct<unsigned char> { static constexpr int value = NPY_UBYTE; }; \n"
		"template<> struct CSERPENT_C2NPY_struct<unsigned short> { static constexpr int value = NPY_USHORT; }; \n"
		"template<> struct CSERPENT_C2NPY_struct<unsigned int> { static constexpr int value = NPY_UINT; }; \n"
		"template<> struct CSERPENT_C2NPY_struct<unsigned long> { static constexpr int value = NPY_ULONG; }; \n"
		"template<> struct CSERPENT_C2NPY_struct<unsigned long long> { static constexpr int value = NPY_ULONGLONG; }; \n"
		"template<> struct CSERPENT_C2NPY_struct<float> { static constexpr int value = NPY_FLOAT; }; \n"
		"%s\n"
		"template<> struct CSERPENT_C2NPY_struct<double> { static constexpr int value = NPY_DOUBLE; }; \n"
		"#define C2NPY(type) CSERPENT_C2NPY_struct<type>::value \n"
		"#else \n"
		"#define C2NPY(type) _Generic((type){0},    \\\n"
		"	signed char:        NPY_BYTE,      \\\n"
		"	short:              NPY_SHORT,     \\\n"
		"	int:                NPY_INT,       \\\n"
		"	long:               NPY_LONG,      \\\n"
		"	long long:          NPY_LONGLONG,  \\\n"
		"	unsigned char:      NPY_UBYTE,     \\\n"
		"	unsigned short:     NPY_USHORT,    \\\n"
		"	unsigned int:       NPY_UINT,      \\\n"
		"	unsigned long:      NPY_ULONG,     \\\n"
		"	unsigned long long: NPY_ULONGLONG, \\\n"
		"	float:              NPY_FLOAT,     \\\n"
		"       %s\\\n"
		"	double:             NPY_DOUBLE,    \\\n"
		"	_Complex float:     NPY_CFLOAT,    \\\n"
		"	_Complex double:    NPY_CDOUBLE    \\\n"
		"	)\n"
		"#endif \n\n",

		f16_cplusplus, 
		f16_c);	
}

static int 
is_string(Type t)
{
	return t.category == T_CHAR
		&& !t.explicit_signed
		&& !t.is_unsigned
		&& t.is_pointer;
}

static int 
is_voidptr(Type t)
{
	return t.category == T_VOID
		&& t.is_pointer;
}

static int 
is_plainvoid(Type t)
{
	Type zero = {0};
	if(t.category == T_VOID){
		t.category = 0;
		return !memcmp(&t,&zero,sizeof(t));
	}
	return 0;
}

static int 
is_array(Type t)
{
	return !is_string(t) 
		&& !is_voidptr(t)
		&& t.is_pointer;
}

static Type 
basetype(Type t)
{
	t.is_pointer = 0;
	t.is_const = 0;
	t.is_volatile = 0;
	t.is_restrict = 0;
	t.is_pointer_const = 0;
	t.is_pointer_volatile = 0;
	t.is_pointer_restrict = 0;
	return t;
}

static void 
emit_exceptionhandling(const char *fn, CSerpentArgs args, int n_fnargs, Symbol fnargs[])
{
	const WrapOpts *o = args.opts;
	if(!o) return;

	if(o->errcheck) {

		if(o->errarg < 0 || o->errarg > n_fnargs)
			die2(args, "Error wrapping function '%s' in file '%s': "
			    "errcheck = %s, errarg = %i was specified, but the function "
			    "only has %i arguments",
			    fn, args.filename, o->errcheck, o->errarg, n_fnargs);

		char *exnarg = o->errarg == 0
			? "rtn"
			: fnargs[o->errarg-1].name;

		fprintf(args.ostream, "    const char *_exn = %s(%s);  \n", o->errcheck, exnarg);
		fprintf(args.ostream, "    if(_exn) {  \n");
		fprintf(args.ostream, "        PyErr_SetString(PyExc_RuntimeError, _exn);  \n");
		fprintf(args.ostream, "        return 0;  \n");
		fprintf(args.ostream, "    }  \n");

	} else if(o->errstr > 0) {

		fprintf(args.ostream, "    if(rtn) {  \n");
		fprintf(args.ostream, "        PyErr_SetString(PyExc_RuntimeError, rtn);  \n");
		fprintf(args.ostream, "        return 0;  \n");
		fprintf(args.ostream, "    }  \n");
	}
}

static void 
emit_call(const char *fn, CSerpentArgs args, int n_fnargs, Symbol fnargs[])
{
	fprintf(args.ostream, "%s (", fn);

	for(int i = 0; i < n_fnargs; i++)
	{
		char *sep  =  i ? ", " : "";
		if (is_array(fnargs[i].type)) {
			char buffer[200] = {0};
			if(200 <= repr_type(200, buffer, fnargs[i].type)) 
				die2(args, "bug: buffer overflow / type name too long");
			fprintf(args.ostream, "%s(%s)%s_data", sep, buffer, fnargs[i].name);
		} else {
			fprintf(args.ostream, "%s%s", sep, fnargs[i].name);
		}
	}

	fprintf(args.ostream, ");\n");
}

static int 
emit_py_buildvalue_fmt_char(CSerpentArgs args, Type t) 
{
	if      (t.category == T_CHAR) fprintf(args.ostream, "b");
	else if (t.category == T_DOUBLE && !t.is_complex && !t.is_imaginary) fprintf(args.ostream, "d");
	else if (t.category == T_FLOAT && !t.is_complex && !t.is_imaginary) fprintf(args.ostream, "f");
	else if (t.category == T_SHORT && !t.is_unsigned) fprintf(args.ostream, "h");
	else if (t.category == T_INT && !t.is_unsigned) fprintf(args.ostream, "i");
	else if (t.category == T_LONG && !t.is_unsigned) fprintf(args.ostream, "l");
	else if (t.category == T_LLONG && !t.is_unsigned) fprintf(args.ostream, "L");
	else if (t.category == T_SHORT) fprintf(args.ostream, "H");
	else if (t.category == T_INT) fprintf(args.ostream, "I");
	else if (t.category == T_LONG) fprintf(args.ostream, "k");
	else if (t.category == T_LLONG) fprintf(args.ostream, "K");
	else if (t.category == T_BOOL) fprintf(args.ostream, "p");
	else return 0;
	return 1;
}

static void 
emit_wrapper (const char *fn, CSerpentArgs args, int n_fnargs, Symbol fnargs[], Type rtntype)
{
	assert(n_fnargs >= 0);

	signed char o_addresses = args.opts ? args.opts->addresses : -1;
	signed char o_bytes     = args.opts ? args.opts->bytes     : -1;
	int use_addresses = opt_or_cfg(o_addresses, args.cfg_addresses, 0);
	int use_bytes     = opt_or_cfg(o_bytes,     args.cfg_bytes,     0);
	int emit_decls    = args.cfg_declarations.value < 0 ? 1 : args.cfg_declarations.value;

	// declaration for function to be wrapped
	if(emit_decls) {
		char buf[200] = {0};
		assert(ssizeof(buf) > repr_type(sizeof(buf), buf, rtntype));

		fprintf(args.ostream, "%s %s (", buf, fn);

		if (n_fnargs == 0) 
			fprintf(args.ostream, "void");

		else for(int i = 0; i < n_fnargs; i++)
		{
			memset(buf,0,sizeof(buf));
			assert(ssizeof(buf) > repr_type(sizeof(buf), buf, fnargs[i].type));

			char * sep  =  i ? ", " : "";
			fprintf(args.ostream, "%s%s %s", sep, buf, fnargs[i].name);
		}


		fprintf(args.ostream, ");\n");
	}

	// start of wrapper definition
	fprintf(args.ostream, "PyObject * wrap_%s (PyObject *self, PyObject *args, PyObject *kwds)\n{\n",fn);
	fprintf(args.ostream, "    (void) self;\n");

	if(n_fnargs) {

		// keyword name list
		fprintf(args.ostream, "    static char *kwlist[] = {");
	        for(int i = 0; i < n_fnargs; i++)
			fprintf(args.ostream, "\n        (char*)\"%s\",", fnargs[i].name);
		fprintf(args.ostream, "0};\n");

		// declare a C variable for each argument
		for(int i = 0; i < n_fnargs; i++) {
			Symbol arg = fnargs[i];

			if (is_string(arg.type)) {
				fprintf(args.ostream, "    char * %s = 0;\n", arg.name);
			}

			else if (is_voidptr(arg.type)) {
				fprintf(args.ostream, "    unsigned long long %s_ull = 0;\n", arg.name);
				fprintf(args.ostream, "    void * %s = 0;\n", arg.name);
			}

			else if (is_array(arg.type)) {
				fprintf(args.ostream, "    PyObject *%s_obj = NULL;\n", arg.name);
				fprintf(args.ostream, "    void *%s_data    = NULL;\n", arg.name);
			}

			else if (arg.type.category == T_BOOL) {
				fprintf(args.ostream, "    int %s = 0;\n", arg.name);
			}

			else {
				char buf[200] = {0};
				assert(ssizeof(buf) > repr_type(sizeof(buf), buf, arg.type));
				fprintf(args.ostream, "    %s %s = {0};\n", buf, arg.name);
			}
		}

		// parse python arguments into the above declared C variables
		fprintf(args.ostream, "\n    if(!PyArg_ParseTupleAndKeywords(args, kwds, \"");
		for (int i = 0; i < n_fnargs; i++) {
			// building the format string for ParseTupleAndKeywords
			Symbol arg = fnargs[i];

			if      (is_string(arg.type))  fprintf(args.ostream, "z");
			else if (is_voidptr(arg.type)) fprintf(args.ostream, "K");
			else if (is_array(arg.type))   fprintf(args.ostream, "O");
			else {
				Type t = arg.type;
				if (!emit_py_buildvalue_fmt_char(args, t)) {

					char buf[200] = {0};
					assert(ssizeof(buf) > repr_type(sizeof(buf), buf, t));
					die2(args, "Error wrapping function '%s' in file '%s': "
					    "argument %i has type '%s', "
					    "which c-serpent doesn't know how to convert "
					    "from python",
					    fn, args.filename, i, buf);
				}
			}
		}
		fprintf(args.ostream, "\", kwlist");
		for (int i = 0; i < n_fnargs; i++) {
			// emit addresses for the arguments we actually want
			fprintf(args.ostream, ",\n        ");
			Symbol arg = fnargs[i];

			if      (is_string(arg.type))  fprintf(args.ostream, "&%s", arg.name);
			else if (is_voidptr(arg.type)) fprintf(args.ostream, "&%s_ull", arg.name);
			else if (is_array(arg.type))   fprintf(args.ostream, "&%s_obj", arg.name);
			else  fprintf(args.ostream, "&%s", arg.name);

		}
		fprintf(args.ostream, ")) return 0;\n\n");

		// type checking for any numpy arrays, conversions for any void pointers
		for (int i = 0; i < n_fnargs; i++)
		{
			Symbol arg = fnargs[i];

			if (is_voidptr(arg.type)) 
				fprintf(args.ostream, "    memcpy(&%s, &%s_ull, sizeof(%s));\n", arg.name, arg.name, arg.name);

			else if (is_array(arg.type)) {
				char buf[200] = {0};
				assert(ssizeof(buf) > repr_type(sizeof(buf), buf, basetype(arg.type)));

				// emit array type check
				fprintf(args.ostream, "    if (%s_obj != Py_None) { \n", arg.name);
				if(use_addresses) {
					fprintf(args.ostream, "        if (PyLong_Check(%s_obj)) { \n", arg.name);
					fprintf(args.ostream, "            intptr_t value = PyLong_AsLongLong(%s_obj);\n", arg.name);
					fprintf(args.ostream, "            if(value==-1 && PyErr_Occurred()) return 0;\n");
					fprintf(args.ostream, "            %s_data = (void*)value;\n", arg.name);
					fprintf(args.ostream, "        } else \n");
				}
				if(use_bytes) {
					fprintf(args.ostream, "        if (PyBytes_Check(%s_obj)) { \n", arg.name);
					fprintf(args.ostream, "            char *value = PyBytes_AsString(%s_obj);\n", arg.name);
					fprintf(args.ostream, "            if(value==0) return 0;\n");
					fprintf(args.ostream, "            %s_data = (void*)value;\n", arg.name);
					fprintf(args.ostream, "        } else \n");
				}
				fprintf(args.ostream, "        if (!PyArray_Check(%s_obj)) { \n", arg.name);
				fprintf(args.ostream, "            PyErr_SetString(PyExc_ValueError, \"Argument '%s' must be a numpy array, or None\"); \n", arg.name);
				fprintf(args.ostream, "            return 0; \n");
				fprintf(args.ostream, "        } \n");
				fprintf(args.ostream, "        else if (PyArray_TYPE((PyArrayObject*)%s_obj) != C2NPY(%s)) {\n", arg.name, buf);
				fprintf(args.ostream, "            PyErr_SetString(PyExc_ValueError, \"Invalid array data type for argument '%s' (expected %s)\");\n", arg.name, buf);
			        fprintf(args.ostream, "            return 0; \n");
				fprintf(args.ostream, "        } \n");

				// emit array contiguity check
				fprintf(args.ostream, "        else if(!PyArray_ISCARRAY((PyArrayObject*)%s_obj)) {\n", arg.name);
				fprintf(args.ostream, "            PyErr_SetString(PyExc_ValueError, \"Argument '%s' is not C-contiguous\");\n", arg.name);
			        fprintf(args.ostream, "            return 0;\n");
				fprintf(args.ostream, "        }\n");
				fprintf(args.ostream, "        else %s_data = PyArray_DATA((PyArrayObject*)%s_obj); \n", arg.name, arg.name);
				fprintf(args.ostream, "    }\n");
			}
		}


	}	
	else {
		fprintf(args.ostream, "    (void) args;\n    (void) kwds;\n");
	}
	fprintf(args.ostream, "\n");

	// now, emit the actual call

	if (is_plainvoid(rtntype)) {

		fprintf(args.ostream, "    Py_BEGIN_ALLOW_THREADS;\n");
		fprintf(args.ostream, "    ");
		emit_call(fn, args, n_fnargs, fnargs);
		fprintf(args.ostream, "    Py_END_ALLOW_THREADS;\n");
		emit_exceptionhandling(fn, args, n_fnargs, fnargs);
		fprintf(args.ostream, "    Py_RETURN_NONE;\n");

	} else if (is_string(rtntype)) {

		char buf[200] = {0};
		assert(ssizeof(buf) > repr_type(sizeof(buf), buf, rtntype));

		fprintf(args.ostream, "    %s rtn = 0;\n", buf);
		fprintf(args.ostream, "    Py_BEGIN_ALLOW_THREADS;\n");
		fprintf(args.ostream, "    rtn = ");
		emit_call(fn, args, n_fnargs, fnargs);
		fprintf(args.ostream, "    Py_END_ALLOW_THREADS;\n");
		emit_exceptionhandling(fn, args, n_fnargs, fnargs);
		fprintf(args.ostream, "    return Py_BuildValue(\"s\", rtn);\n");

	} else if (is_voidptr(rtntype)) {

		char buf[200] = {0};
		assert(ssizeof(buf) > repr_type(sizeof(buf), buf, rtntype));

		fprintf(args.ostream, "    %s rtn = 0;\n", buf);
		fprintf(args.ostream, "    Py_BEGIN_ALLOW_THREADS;\n");
		fprintf(args.ostream, "    rtn = ");
		emit_call(fn, args, n_fnargs, fnargs);
		fprintf(args.ostream, "    Py_END_ALLOW_THREADS;\n");
		emit_exceptionhandling(fn, args, n_fnargs, fnargs);
		fprintf(args.ostream, "    return PyLong_FromVoidPtr(rtn);\n");

	} else if (is_array(rtntype)) {

		char buf[200] = {0};
		assert(ssizeof(buf) > repr_type(sizeof(buf), buf, rtntype));

		die2(args, "Error wrapping function '%s' in file '%s': "
		       "return type '%s' is not supported by c-serpent",
		       fn, args.filename, buf);

	} else {
		char buf[200] = {0};
		assert(ssizeof(buf) > repr_type(sizeof(buf), buf, rtntype));

		// python requires us to handle bools as ints
		if (rtntype.category == T_BOOL) 
			fprintf(args.ostream, "    int rtn = 0;\n");
		else
			fprintf(args.ostream, "    %s rtn = 0;\n", buf);

		fprintf(args.ostream, "    Py_BEGIN_ALLOW_THREADS;\n");
		fprintf(args.ostream, "    rtn = ");
		emit_call(fn, args, n_fnargs, fnargs);
		fprintf(args.ostream, "    Py_END_ALLOW_THREADS;\n");
		emit_exceptionhandling(fn, args, n_fnargs, fnargs);
		fprintf(args.ostream, "    return Py_BuildValue(\"");
		if(!emit_py_buildvalue_fmt_char(args, rtntype)) {
			die2(args, "Error wrapping function '%s' in file '%s': "
			       "return type '%s' is not supported by c-serpent",
			       fn, args.filename, buf);
		}
		fprintf(args.ostream, "\", rtn);\n");
	}

	fprintf(args.ostream, "}\n\n");

}

static void 
emit_dispatch_wrapper (
	ParseCtx p,
	const char *fn, 
	int n_variants_implemented,
	short arg_match_count[static MAX_FN_ARGS],
	int n_supported_suffixes,
	VariantSuffix suffixes[],
	Symbol fnargs[static MAX_FN_ARGS] ) 
{
	// emit a very general wrapper that just dispatches based on the type of the first argument that matches the suffix in all implementations 
	
	CSerpentArgs args = p.args;

	int idx_first_covariant_arg = -1;
	int n_args = 0;

	// try to find a covariant array argument first, only use scalar as a backup
	for (int i = MAX_FN_ARGS-1; i >= 0; i--) {
		if (arg_match_count[i] == n_variants_implemented && fnargs[i].type.is_pointer) 
			idx_first_covariant_arg = i;
		if (fnargs[i].type.category != T_UNINITIALIZED && fnargs[i].type.category != T_UNKNOWN)
			n_args++;
	}

	if (idx_first_covariant_arg < 0) 
		for (int i = MAX_FN_ARGS-1; i >= 0; i--) 
			if (arg_match_count[i] == n_variants_implemented) 
				idx_first_covariant_arg = i;

	// but there does need to be at least ONE covariant arg... 
	if (idx_first_covariant_arg < 0) 
		die(&p, "couldn't generate generic wrapper for '%s', no argument correctly matches suffix in all implemented variants.", fn);

	Type key_arg = fnargs[idx_first_covariant_arg].type;

	fprintf(args.ostream, "PyObject * wrap_%s (PyObject *self, PyObject *args, PyObject *kwds)\n{\n",fn);

	fprintf(args.ostream, "    PyObject *arglist[%i] = {0};\n", n_args);

	// keyword name list
	fprintf(args.ostream, "    static char *kwlist[] = {");
	for(int i = 0; i < n_args; i++)
		fprintf(args.ostream, "\n        (char*)\"%s\",", fnargs[i].name);
	fprintf(args.ostream, "0};\n");

	// extract args
	fprintf(args.ostream, "    if(!PyArg_ParseTupleAndKeywords(args, kwds, \"" );
	for (int i = 0; i < n_args; i++) fprintf(args.ostream, "O");
	fprintf(args.ostream, "\", kwlist");
	for (int i = 0; i < n_args; i++) fprintf(args.ostream, ",&arglist[%i]", i);
	fprintf(args.ostream, ")) return 0;\n");

	// emit dispatch if statements
	for (int i = 0; i < n_supported_suffixes; i++) {

		if(suffixes[i].found) {

			if(key_arg.is_pointer) {

				char buf[200] = {0};
				assert(ssizeof(buf) > repr_type(sizeof(buf), buf, basetype(suffixes[i].type)));
				// array 
				fprintf(args.ostream, "    if (PyArray_Check(arglist[%i]) && PyArray_TYPE((PyArrayObject*)arglist[%i]) == C2NPY(%s)) {\n", idx_first_covariant_arg, idx_first_covariant_arg, buf);

				fprintf(args.ostream, "        return wrap_%s%c(self, args, kwds);\n", fn, suffixes[i].suffix);

				fprintf(args.ostream, "    } else ");

			} else {

				// non-array

				// PyLong
				if (suffixes[i].type.category >= T_CHAR && suffixes[i].type.category <= T_LLONG) {
					fprintf(args.ostream, "    if (PyLong_Check(arglist[%i])) {\n", idx_first_covariant_arg);
					fprintf(args.ostream, "        return wrap_%s%c(self, args, kwds);\n", fn, suffixes[i].suffix);
					fprintf(args.ostream, "    } else ");
				}

				// PyFloat / PyComplex
				else if (suffixes[i].type.category >= T_FLOAT && suffixes[i].type.category <= T_LDOUBLE) {

					if (suffixes[i].type.is_complex) {
						fprintf(args.ostream, "    if (PyComplex_Check(arglist[%i])) {\n", idx_first_covariant_arg);
						fprintf(args.ostream, "        return wrap_%s%c(self, args, kwds);\n", fn, suffixes[i].suffix);
						fprintf(args.ostream, "    } else ");
					} else {
						fprintf(args.ostream, "    if (PyFloat_Check(arglist[%i])) {\n", idx_first_covariant_arg);
						fprintf(args.ostream, "        return wrap_%s%c(self, args, kwds);\n", fn, suffixes[i].suffix);
						fprintf(args.ostream, "    } else ");
					}
				}

				// ..wat?
				else {
					char buf[200] = {0};
					assert(ssizeof(buf) > repr_type(sizeof(buf), buf, key_arg));
					die(&p, "error generating dispatcher for '%s' unsupported scalar argument type '%s'", fn, buf);
				}
				
			}
		}
	}

	fprintf(args.ostream, "{\n        PyErr_SetString(PyExc_ValueError, \"No instance of generic function '%s' matches supplied argument types\");\n        return 0;\n    }\n", fn);

	fprintf(args.ostream, "}\n");
}

/*
	==========================================================
		Parsing
	==========================================================
*/


static int 
eat_identifier(ParseCtx *p, const char *id)
{
	if(p->tokens == p->tokens_end) return 0;
	if(p->tokens[0].toktype != CLEX_id) return 0;
	if((long long)strlen(id) != p->tokens[0].string_len) return 0;
	if(!memcmp(id, p->tokens[0].string, p->tokens[0].string_len)) {
		p->tokens++;
		return 1;
	}
	return 0;
}

static int 
eat_token(ParseCtx *p, int toktype)
{
	if(p->tokens == p->tokens_end) return 0;
	if(p->tokens[0].toktype == toktype) {
		p->tokens++;
		return 1;
	}
	return 0;
}

static int
check_token_is_identifier(Token *t, const char *id, long id_len)
{
	if (id_len == 0) 
		id_len = strlen(id);

	if (t->toktype == CLEX_id
		&& t->string_len == id_len
		&& !memcmp(t->string, id, id_len)) {
		return 1;
	}

	return 0;
}

static int 
identifier(ParseCtx *p, char** out_id)
{
	if(p->tokens == p->tokens_end) return 0;
	if(p->tokens[0].toktype == CLEX_id) {
		if(out_id) 
			*out_id = p->tokens[0].string;
		p->tokens++;
		return 1;
	}
	return 0;
}

static int 
typedef_name(ParseCtx *p, Type *t)
{
	if(p->tokens == p->tokens_end) return 0;
	if(p->tokens[0].toktype == CLEX_id) {

		Symbol *s = 0;
		if ((s = get_symbol(p->storage, p->tokens[0].string))) {
			if(t) *t = s->type;
			p->tokens++;
			return 1;
		}
	}
	return 0;
}

static int 
supported_type(ParseCtx *p, Type *t)
{
	if (p->tokens == p->tokens_end) return 0;

	else if (typedef_name(p, t)) return 1;

	else if (eat_token(p, '*')) { return modify_type_pointer(p,t); }

	else if (p->tokens[0].toktype == CLEX_id) {

		if (eat_identifier(p, "void"))     { modify_type_void(p,t); return 1; }
		if (eat_identifier(p, "char"))     { modify_type_char(p,t); return 1; } 
		if (eat_identifier(p, "short"))    { modify_type_short(p,t); return 1; }
		if (eat_identifier(p, "int"))      { modify_type_int(p,t); return 1; }
		if (eat_identifier(p, "long"))     { modify_type_long(p,t); return 1; }
		if (eat_identifier(p, "float"))    { modify_type_float(p,t); return 1; }
		if (eat_identifier(p, "_Float16")) { modify_type_float16(p,t); return 1; }
		if (eat_identifier(p, "double"))   { modify_type_double(p,t); return 1; }
		if (eat_identifier(p, "signed"))   { modify_type_signed(p,t); return 1; }
		if (eat_identifier(p, "unsigned")) { modify_type_unsigned(p,t); return 1; }
		if (eat_identifier(p, "_Bool"))      { modify_type_bool(p,t); return 1; }
		if (eat_identifier(p, "_Complex"))    { modify_type_complex(p,t); return 1; }
		if (eat_identifier(p, "_Imaginary"))  { modify_type_imaginary(p,t); return 1; }

		if (eat_identifier(p, "const"))  { modify_type_const(p,t); return 1; }
		if (eat_identifier(p, "restrict"))  { modify_type_restrict(p,t); return 1; }
		if (eat_identifier(p, "volatile"))  { modify_type_volatile(p,t); return 1; }

		return 0;
	}
	return 0;	
}

static int 
supported_type_list(ParseCtx *p, Type *t)
{
	if(!supported_type(p,t)) return 0;
	while(supported_type(p,t)) {}
	return 1;
}

static int 
supported_typedef(ParseCtx *p, Symbol *s)
{
	SAVE(p);
	Symbol tmp = *s;

	if(eat_identifier(p, "typedef")
		&& supported_type_list(p, &tmp.type)
		&& identifier(p, &tmp.name)
		&& eat_token(p, ';')) 
	{ 
		// can these actually happen in valid code? don't think so.. 
		if(!strcmp(tmp.name, "struct")) { RESTORE(p); return 0; }
		if(!strcmp(tmp.name, "union"))  { RESTORE(p); return 0; }
		if(!strcmp(tmp.name, "enum"))   { RESTORE(p); return 0; }
		*s = tmp;
		return 1; 
	}

	RESTORE(p);
	return 0;
}

static void 
populate_symbols(StorageBuffers *storage, ParseCtx p)
{
	while(p.tokens != p.tokens_end) {

		while (!check_token_is_identifier(p.tokens, "typedef", 7)) 
		{ 
			p.tokens++;
			if (p.tokens == p.tokens_end) return;
		}

		Symbol s = {0};
		if (supported_typedef(&p, &s)) {
			add_symbol(p.args, storage, s);

			if(p.args.verbose) {
				char buf[200] = {0};
				repr_symbol(sizeof(buf), buf, s);
				fprintf(p.args.estream, "registered type %s\n", buf); 
			}

		} else {
			p.tokens++;
		}
	}
}


static int 
arg(ParseCtx *p, const char *fn, Symbol *fnarg, int fatal)
{
	SAVE(p);

	Symbol tmp = {0};

	if(!supported_type_list(p, &tmp.type)) {
		if(fatal) die(p, "error wrapping function '%s' in '%s': unsupported type", fn, p->args.filename);
		RESTORE(p);
		return 0;
	}

	if(!identifier(p, &tmp.name)) {
		if(fatal) die(p, "error wrapping function '%s' in '%s': expected identifier (i.e. argument name)", fn, p->args.filename);
		RESTORE(p);
		return 0;
	}

	if(eat_token(p,'[')) {
		if(!modify_type_pointer(p, &tmp.type)) {
			if(fatal) die(p, "error wrapping function '%s' in '%s': unsupported type", fn, p->args.filename);
			RESTORE(p);
			return 0;
		}
		while(1) {
			if (eat_identifier(p, "static")) {}
			else if (eat_identifier(p, "const")) {modify_type_const(p, &tmp.type);}
			else if (eat_identifier(p, "restrict")) {modify_type_restrict(p, &tmp.type);}
			else break;
		}

		// skip over everything else until we close the square bracket
		// this isn't robust, just a hack that will probably work on common valid C code
		int depth = 1;
		while(depth > 0)
		{
			if(eat_token(p, '[')) depth++;
			else if(eat_token(p, ']')) depth--;
			else {
				p->tokens++;
				if(p->tokens == p->tokens_end) 
					die(p, "parse error: unexpected end of file");
			}
		}
	}

	*fnarg = tmp;
	return 1;
}

static int 
arglist(ParseCtx *p, const char *fn, int max_args, int *num_args, Symbol fnargs[], int fatal)
{
	SAVE(p);
	*num_args = 0;

	if (eat_token(p, '(') 
		&& eat_identifier(p, "void") 
		&& eat_token(p, ')')) {

		return 1;
	}

	RESTORE(p);

	if (eat_token(p, '('))
	{
		while(arg(p, fn, fnargs+(*num_args), fatal)) 
		{
			*num_args = *num_args + 1;
			if(eat_token(p, ',') && (*num_args == max_args))
				die(p, "error wrapping function '%s' in '%s': functions with more than %i arguments are not supported", fn, p->args.filename, max_args);
		}

		if(!eat_token(p, ')')) {
			RESTORE(p);
			return 0;
		}

		return 1;
	}

	RESTORE(p);
	return 0;
}

static void 
attributes(ParseCtx *p, const char *fn)
{
	while(1) {
		if (eat_identifier(p, "__attribute__")) {
			if (!eat_token(p, '(')) {
				die(p, "error wrapping function '%s' in '%s': parse error (malformed attribute)", fn, p->args.filename);
			}

			// skip over content of the brackets
			// not super robust
			int depth = 1;
			while(depth > 0) {
				if(eat_token(p, '(')) depth++;
				else if(eat_token(p, ')')) depth--;
				else {
					p->tokens++;
					if(p->tokens == p->tokens_end) 
						die(p, "parse error: unexpected end of file");
				}
			}
		}
		else break;
	}
}



static void 
process_function(ParseCtx p, Symbol argsyms[static MAX_FN_ARGS])
{
	// on entry, p.tokens is set right on the function name.	
	const char * fn = p.tokens[0].string;

	// rewind to last semicolon / closing brace
	while(p.tokens[0].toktype != ';' && p.tokens[0].toktype != '}') 
		p.tokens--;
	p.tokens++; // then move past the semicolon / closing brace we're on

	Type rtn_t = {0};
	memset(argsyms, 0, MAX_FN_ARGS*sizeof(argsyms[0]));

	if(!supported_type_list(&p, &rtn_t))
		die(&p, "error wrapping function '%s' in '%s': unsupported return type", fn, p.args.filename);
	
	char *sanity_check = 0;
	if(!identifier(&p, &sanity_check)) 
		die(&p, "error wrapping function '%s' in '%s': unsupported specifiers or qualifiers", fn, p.args.filename);
	if(sanity_check != fn)
		die(&p, "error wrapping function '%s' in '%s': parse error (encountered unrecognized garbage)", fn, p.args.filename);

	int num_args = 0;
	if(!arglist(&p, fn, MAX_FN_ARGS, &num_args, argsyms, 0)) {
		arglist(&p, fn, MAX_FN_ARGS, &num_args, argsyms, 1);
		die(&p, "error wrapping function '%s' in '%s': parse error (couldn't parse argument list)", fn, p.args.filename);
	}

	attributes(&p, fn);

	if(!(eat_token(&p, ';') || eat_token(&p, '{')))
		die(&p, "error wrapping function '%s' in '%s': parse error (encountered unrecognized garbage)", fn, p.args.filename);

	// Parse successful, emit the wrapper!
	emit_wrapper (fn, p.args, num_args, argsyms, rtn_t);
}

static void 
advance_and_skip_braced_blocks(ParseCtx *p)
{
	p->tokens++; 
	int depth = 0;
	while(1) {
		if(p->tokens == p->tokens_end) return;
		if(p->tokens[0].toktype == '{') depth++;
		if(p->tokens[0].toktype == '}') depth--;
		if(depth == 0) return;
		assert(depth >= 0);
		p->tokens++;
	}
}

static int 
parse_file_look_for_function(ParseCtx p, const char *function_name, Symbol argsyms[static MAX_FN_ARGS])
{
	long len = strlen(function_name);

	while (p.tokens != p.tokens_end) {
		if (check_token_is_identifier(p.tokens, function_name, len) )
		{
			process_function(p, argsyms);
			return 1;
		}
		advance_and_skip_braced_blocks(&p);
	}
	return 0;
}


static int
process_enum(ParseCtx p, StorageBuffers *st)
{
	// current token is set to the 'enum' keyword
	p.tokens++;
	if(p.tokens == p.tokens_end) die(&p, "parse error: unexpected end of file");

	// optional tag
	char *enum_name = "";
	identifier(&p, &enum_name);
	char *tag = intern_string(p.args, st, enum_name, 0);

	// expect brace
	if(!eat_token(&p, '{')) die(&p, "parse error in enum %s: expected '{'", enum_name);

	// parse enum constants
	int nconsts = 0;
	while(p.tokens != p.tokens_end) {
		if(p.tokens[0].toktype == '}') break;
		if(p.tokens[0].toktype == ',') { p.tokens++; continue; }

		char *const_name = 0;
		if(!identifier(&p, &const_name)) die(&p, "parse error in enum %s: expected identifier", enum_name);

		if(st->nenumconsts == MAX_ENUMCONSTS)
			die(&p, "too many enum constants (max %i)", (int)MAX_ENUMCONSTS);
		st->enumconsts[st->nenumconsts++] = (EnumConst){
			.tag  = tag,
			.name = intern_string(p.args, st, const_name, strlen(const_name)),
		};
		nconsts++;

		if(eat_token(&p, '=')) {
			// skip everything until comma or close brace
			// but that's tricky because the expression could have a comma in it, 
			// if it's enclosed in parens or square brackets (though such statements are uncommon).
			// let's not differentiate between the two, and just track depth in either delimiter.
			// technically that's a bug but it'll work correctly on the vast majority of sane code.
			int depth = 0;
			while (1) {
				if(p.tokens == p.tokens_end) die(&p, "parse error in enum %s: unexpected end of file", enum_name);
				if(p.tokens[0].toktype == ',' && depth == 0) break;
				if(p.tokens[0].toktype == '}' && depth == 0) break;
				if(p.tokens[0].toktype == '(' || p.tokens[0].toktype == '[') depth++;
				if(p.tokens[0].toktype == ')' || p.tokens[0].toktype == ']') depth--;
				p.tokens++;
			}
			// we're now left ON the comma or brace
		}

		if(p.tokens == p.tokens_end) die(&p, "parse error in enum %s: unexpected end of file", enum_name);
		if(p.tokens[0].toktype == '}') break;
	}

	return nconsts;
}


static int
collect_enums(ParseCtx p, StorageBuffers *st)
{
	int howmany = 0;
	while (p.tokens != p.tokens_end) {
		if (check_token_is_identifier(p.tokens, "enum", 4) )
			howmany += process_enum(p, st);
		advance_and_skip_braced_blocks(&p);
	}
	return howmany;
}


/*
	==========================================================
		Annotation scanning
	==========================================================

	Annotations are ordinary tokens by the time we see them: an undefined
	function-like macro passes through the preprocessor untouched. We scan for
	CSERPENT_-prefixed identifiers, parse the call, and then blank the tokens
	so the later definition search never mistakes an annotation's mention of a
	function for the function itself.
*/

enum { MAX_ANN_ARGS = 64 };

typedef struct {
	const char *key;   /* NULL for a positional argument */
	Token       val;
} AnnArg;

static Loc
annloc_for(StorageBuffers *st, int tok_index)
{
	for (int i = 0; i < st->nannlocs; i++)
		if (st->annlocs[i].tok_index == tok_index) return st->annlocs[i].loc;
	return (Loc){ .file = "<unknown>", .line = 0 };
}

static _Noreturn void
die_at (CSerpentArgs args, Loc loc, const char * fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	fprintf(args.estream, "%s:%i: ", loc.file, loc.line);
	vfprintf(args.estream, fmt, va);
	va_end(va);
	fprintf(args.estream, "\n");
	terminate(&args);
}

static void
warn_at (CSerpentArgs args, Loc loc, const char * fmt, ...)
{
	if(!args.warnings) return;
	va_list va;
	va_start(va, fmt);
	if (loc.line > 0) fprintf(args.estream, "%s:%i: warning: ", loc.file, loc.line);
	else              fprintf(args.estream, "%s: warning: ", loc.file);
	vfprintf(args.estream, fmt, va);
	va_end(va);
	fprintf(args.estream, "\n");
}

/* parse '( arg, arg, ... )' with tokens[i] on the '('; returns index past ')' */
static int
parse_ann_args(CSerpentArgs args, Loc loc, const char *macro,
               Token *tokens, int ntok, int i,
               int *out_nargs, AnnArg out[])
{
	if (i >= ntok || tokens[i].toktype != '(')
		die_at(args, loc, "%s must be followed by '('", macro);
	i++;

	int n = 0;

	if (i < ntok && tokens[i].toktype == ')') { *out_nargs = 0; return i+1; }

	while (1) {
		if (i >= ntok) die_at(args, loc, "%s: unterminated argument list", macro);
		if (n == MAX_ANN_ARGS) die_at(args, loc, "%s: too many arguments", macro);

		if (tokens[i].toktype == CLEX_id && i+1 < ntok && tokens[i+1].toktype == '=') {
			if (i+2 >= ntok) die_at(args, loc, "%s: expected a value after '='", macro);
			out[n].key = tokens[i].string;
			out[n].val = tokens[i+2];
			i += 3;
		} else {
			out[n].key = 0;
			out[n].val = tokens[i];
			i += 1;
		}
		n++;

		if (i < ntok && tokens[i].toktype == ',') { i++; continue; }
		if (i < ntok && tokens[i].toktype == ')') { i++; break; }
		die_at(args, loc, "%s: expected ',' or ')'", macro);
	}

	*out_nargs = n;
	return i;
}

static const char *
ann_ident(CSerpentArgs args, Loc loc, const char *macro, const char *what, Token t)
{
	if (t.toktype != CLEX_id)
		die_at(args, loc, "%s: %s must be an identifier", macro, what);
	return t.string;
}

static const char *
ann_string(CSerpentArgs args, Loc loc, const char *macro, const char *what, Token t)
{
	if (t.toktype != CLEX_dqstring)
		die_at(args, loc, "%s: %s must be a quoted string", macro, what);
	return t.string;
}

static int
ann_int(CSerpentArgs args, Loc loc, const char *macro, const char *what, Token t)
{
	if (t.toktype != CLEX_intlit)
		die_at(args, loc, "%s: %s must be an integer", macro, what);
	return (int) t.int_number;
}

static int
ann_bool(CSerpentArgs args, Loc loc, const char *macro, const char *what, Token t)
{
	int v = ann_int(args, loc, macro, what, t);
	if (v != 0 && v != 1)
		die_at(args, loc, "%s: %s must be 0 or 1", macro, what);
	return v;
}

static void
set_config(CSerpentArgs *args, ConfigVal *cv, const char *key, int v, Loc loc, int or_together)
{
	if (or_together) {
		cv->value = (cv->value > 0) || v;
		cv->loc = loc;
		return;
	}
	if (cv->value >= 0 && cv->value != v)
		die_at(*args, loc,
			"CSERPENT_CONFIG: '%s' set to %i here, but to %i at %s:%i",
			key, v, cv->value, cv->loc.file, cv->loc.line);
	cv->value = v;
	cv->loc = loc;
}

static void
handle_annotation(StorageBuffers *st, CSerpentArgs *args, const char *macro, Loc loc,
                  int na, AnnArg a[], int input_index, Loc *module_loc)
{
	int is_fn      = !strcmp(macro, "CSERPENT_WRAPFN");
	int is_generic = !strcmp(macro, "CSERPENT_WRAPFN_GENERIC");
	int is_manual  = !strcmp(macro, "CSERPENT_WRAPFN_MANUAL");
	int is_const   = !strcmp(macro, "CSERPENT_WRAPCONST");
	int is_module  = !strcmp(macro, "CSERPENT_MODULE");
	int is_config  = !strcmp(macro, "CSERPENT_CONFIG");
	int is_opaque  = !strcmp(macro, "CSERPENT_OPAQUE");

	if (!(is_fn || is_generic || is_manual || is_const || is_module || is_config || is_opaque))
		die_at(*args, loc, "unknown annotation '%s'", macro);

	if (is_module) {
		if (na != 1 || a[0].key)
			die_at(*args, loc, "CSERPENT_MODULE takes exactly one module name");
		const char *nm = ann_ident(*args, loc, macro, "the module name", a[0].val);
		if (args->modulename)
			die_at(*args, loc, "CSERPENT_MODULE already given at %s:%i "
			       "(at most once across all inputs)",
			       module_loc->file, module_loc->line);
		args->modulename = nm;
		*module_loc = loc;
		return;
	}

	if (is_config) {
		for (int k = 0; k < na; k++) {
			if (!a[k].key)
				die_at(*args, loc, "CSERPENT_CONFIG takes only 'key = value' options");
			const char *key = a[k].key;
			int v = ann_bool(*args, loc, macro, key, a[k].val);
			if      (!strcmp(key, "addresses"))    set_config(args, &args->cfg_addresses,    key, v, loc, 0);
			else if (!strcmp(key, "bytes"))        set_config(args, &args->cfg_bytes,        key, v, loc, 0);
			else if (!strcmp(key, "declarations")) set_config(args, &args->cfg_declarations, key, v, loc, 0);
			else if (!strcmp(key, "float16"))      set_config(args, &args->cfg_float16,      key, v, loc, 1);
			else die_at(*args, loc, "CSERPENT_CONFIG: unknown key '%s'", key);
		}
		return;
	}

	/* the remaining forms all take names, and all but WRAPCONST take options */

	WrapOpts o = {
		.errarg = 0, .errstr = -1,
		.addresses = -1, .bytes = -1, .strip_underscore = -1,
	};
	const char *names[MAX_ANN_ARGS];
	int nnames = 0;
	int saw_errarg = 0;

	for (int k = 0; k < na; k++) {
		if (!a[k].key) {
			/*
				A macro used as a name has already been expanded by the time
				we see it, so it arrives as a literal rather than an
				identifier. Say so; "expected an identifier" is baffling when
				you wrote what looks like one.
			*/
			if (a[k].val.toktype != CLEX_id)
				die_at(*args, loc, "%s: argument %i is a literal, not a name. "
				       "If you named a #define, note that it is expanded away "
				       "before c-serpent sees it, and cannot be wrapped.",
				       macro, k+1);
			names[nnames++] = a[k].val.string;
			continue;
		}
		const char *key = a[k].key;

		if (is_const || is_opaque)
			die_at(*args, loc, "%s takes names only, not options", macro);

		if      (!strcmp(key, "name"))     o.py_name   = ann_string(*args, loc, macro, key, a[k].val);
		else if (!strcmp(key, "doc"))      o.doc       = ann_string(*args, loc, macro, key, a[k].val);
		else if (!strcmp(key, "errcheck")) o.errcheck  = ann_ident (*args, loc, macro, key, a[k].val);
		else if (!strcmp(key, "errstr"))   o.errstr    = ann_bool  (*args, loc, macro, key, a[k].val);
		else if (!strcmp(key, "addresses"))o.addresses = ann_bool  (*args, loc, macro, key, a[k].val);
		else if (!strcmp(key, "bytes"))    o.bytes     = ann_bool  (*args, loc, macro, key, a[k].val);
		else if (!strcmp(key, "errarg")) {
			o.errarg = ann_int(*args, loc, macro, key, a[k].val);
			saw_errarg = 1;
			if (o.errarg < 0) die_at(*args, loc, "%s: errarg must not be negative", macro);
		}
		else if (!strcmp(key, "strip_underscore")) {
			if (!is_generic)
				die_at(*args, loc, "%s: 'strip_underscore' only applies to CSERPENT_WRAPFN_GENERIC", macro);
			o.strip_underscore = ann_bool(*args, loc, macro, key, a[k].val);
		}
		else die_at(*args, loc, "%s: unknown option '%s'", macro, key);
	}

	if (nnames == 0)
		die_at(*args, loc, "%s: expected at least one name", macro);
	if (is_generic && nnames != 1)
		die_at(*args, loc, "CSERPENT_WRAPFN_GENERIC takes exactly one prefix");
	if (o.py_name && nnames > 1)
		die_at(*args, loc, "%s: 'name' cannot be used with more than one name", macro);
	if (saw_errarg && !o.errcheck)
		warn_at(*args, loc, "%s: 'errarg' has no effect without 'errcheck'", macro);

	int kind = is_fn ? WK_FN : is_generic ? WK_GENERIC : is_manual ? WK_MANUAL
	         : is_const ? WK_CONST : WK_OPAQUE;

	for (int k = 0; k < nnames; k++) {
		if (st->nitems == MAX_ITEMS)
			die_at(*args, loc, "too many annotations (max %i)", (int)MAX_ITEMS);
		st->items[st->nitems++] = (WrapItem){
			.kind = kind, .name = names[k], .opts = o,
			.input = input_index, .loc = loc,
		};
	}
}

/*
	Walk the token stream, handling and then blanking every annotation.
	collect=0 blanks only, which is what pass 2 needs (the work list is
	already built, but the tokens still have to be neutralised).
*/
static void
scan_annotations(StorageBuffers *st, CSerpentArgs *args, int ntok, Token *tokens,
                 int input_index, int collect, Loc *module_loc)
{
	for (int i = 0; i < ntok; i++) {
		if (tokens[i].toktype != CLEX_id) continue;
		if (strncmp(tokens[i].string, "CSERPENT_", 9)) continue;

		const char *macro = tokens[i].string;
		Loc loc = annloc_for(st, i);

		AnnArg a[MAX_ANN_ARGS];
		int na = 0;
		int end = parse_ann_args(*args, loc, macro, tokens, ntok, i+1, &na, a);

		if (collect)
			handle_annotation(st, args, macro, loc, na, a, input_index, module_loc);

		for (int k = i; k < end; k++) tokens[k].toktype = ';';
		i = end - 1;
	}
}

/*
	==========================================================
		Lexing and File IO
	==========================================================
*/

#ifdef _WIN32
#define popen(x,y) _popen(x,y)
#define pclose(x) _pclose(x)
#endif


static void 
print_context(CSerpentArgs args, char *start, char *loc)
{
	char *first = loc;
	while(loc-first < 60 && first >= start) 
		first--;
	while(*first != '\n' && first >= start) 
		first--;

	char *last = loc;
	while(last-loc < 60 && *last != 0) 
		last++;
	while(*last != '\n' && *last != 0) 
		last++;

	while(first != last) {
		if (first == loc) fprintf(args.estream, " HERE>>>");
		putc(*(first++),args.estream);
	}
}

static void
ingest_file(StorageBuffers *st, CSerpentArgs args, long long text_bufsz, char *text)
{
	// if we're reading from stdin, we need to treat a lot of things differently
	if(!strcmp(args.filename, "-")) {

		// we ignore the preprocessor flag in this case, stdin MUST be the preprocessor output
		long long len = fread(text, 1, text_bufsz, args.istream);
		if(len == text_bufsz) die2(args, "input file too long");

	} else {

		// read via popen to preprocessor command. -DCSERPENT is what makes
		// the annotation blocks visible; nothing else defines it.
		char cmd[4096] = {0};
		if(ssizeof(cmd) <= snprintf(cmd, sizeof(cmd),
				"%s -DCSERPENT -DCSERPENT_VERSION=2 %s",
				args.preprocessor, args.filename))
			die2(args, "static buffer overflow");
		
		FILE *f = popen(cmd, "r");
		if(!f) die2(args, "couldn't popen '%s'", cmd);
		*args.open_file = f;
		long long len = fread(text, 1, text_bufsz, f);
		if(len == text_bufsz) die2(args, "input file too long");
		int exit_status = pclose(f);
		*args.open_file = 0;
		switch (exit_status) {
			case  0: break;
			case -1: die2(args, "wait4 on '%s' failed, or other static error occurred", cmd);
			default: die2(args, "'%s' failed with code %i", cmd, exit_status);
		}
	}
}


/*
	Parse a cpp line marker ('# 42 "file.h" 1 3') at the start of a line.
	The lexer itself discards everything after a '#', so this reads the raw
	text rather than tokens.
*/
static int
parse_line_marker(StorageBuffers *st, CSerpentArgs args, char *p, char *end,
                  int *out_line, const char **out_file)
{
	while (p < end && (*p == ' ' || *p == '\t')) p++;
	if (p >= end || *p != '#') return 0;
	p++;
	while (p < end && (*p == ' ' || *p == '\t')) p++;
	if (p >= end || !(*p >= '0' && *p <= '9')) return 0;

	int v = 0;
	while (p < end && *p >= '0' && *p <= '9') {
		if (v > 100000000) return 0;
		v = v*10 + (*p - '0');
		p++;
	}

	while (p < end && (*p == ' ' || *p == '\t')) p++;
	if (p >= end || *p != '"') return 0;
	p++;

	char *start = p;
	while (p < end && *p != '"' && *p != '\n') p++;
	if (p >= end || *p != '"') return 0;

	char buf[1024];
	int n = (int)(p - start);
	if (n >= ssizeof(buf)) n = sizeof(buf)-1;
	memcpy(buf, start, n);
	buf[n] = 0;

	*out_line = v;
	*out_file = intern_string(args, st, buf, 0);
	return 1;
}

static int
lex_file(StorageBuffers *st,
		CSerpentArgs args, 
		long long tokens_maxnum, 
		Token *tokens, 
		char *text, 
		long long string_store_bufsz, 
		char *string_store)
{
	int ntok = 0;
	char *text_start = text;

	/*
		Location tracking. cpp emits '# 42 "file.h"' markers at every file
		transition; between markers we count newlines ourselves. Only
		CSERPENT_-prefixed identifiers get their location recorded, so the
		side table stays small.
	*/
	const char *cur_file = args.filename;
	int  cur_line   = 1;
	int  pending_line = -1;     /* set by a marker; takes effect on the next line */
	char *cursor   = text_start;
	char *text_end = text_start + strlen(text_start);

	st->nannlocs = 0;

	{
		int ml; const char *mf;
		if (parse_line_marker(st, args, text_start, text_end, &ml, &mf)) {
			cur_file = mf;
			pending_line = ml;
		}
	}

	tokens[ntok++] = (Token){.toktype=';'}; // parser expects token stream to start with a semicolon

	stb_lexer lex = {0};
	stb_c_lexer_init(&lex, text_start, text+strlen(text_start), (char *) string_store, string_store_bufsz);
	while(stb_c_lexer_get_token(&lex)) {
		if(tokens_maxnum == ntok) die2(args, "static buffer overflow");

		while (cursor < lex.where_firstchar) {
			if (*cursor == '\n') {
				if (pending_line >= 0) { cur_line = pending_line; pending_line = -1; }
				else cur_line++;

				int ml; const char *mf;
				if (parse_line_marker(st, args, cursor+1, text_end, &ml, &mf)) {
					cur_file = mf;
					pending_line = ml;
				}
			}
			cursor++;
		}

		Token t = {.toktype = lex.token};
		switch(lex.token)
		{
			case CLEX_eof:
				t.toktype = ';';
				break;
			case CLEX_id: 
			case CLEX_dqstring:
			case CLEX_sqstring: 		
				t.string_len = strlen(lex.string);	
				t.string = intern_string(args, st, lex.string, t.string_len);
				break;
			case CLEX_charlit:
				t.int_number = lex.int_number;
				break;
			case CLEX_intlit:
				t.int_number = lex.int_number;
				break;
			case CLEX_floatlit:
				t.real_number = lex.real_number;
				break;
			case CLEX_eq:
			case CLEX_noteq:
			case CLEX_lesseq:
			case CLEX_greatereq:
			case CLEX_andand:
			case CLEX_oror:
			case CLEX_shl:
			case CLEX_shr:
			case CLEX_plusplus:
			case CLEX_minusminus:
			case CLEX_arrow:
			case CLEX_andeq:
			case CLEX_oreq:
			case CLEX_xoreq:
			case CLEX_pluseq:
			case CLEX_minuseq:
			case CLEX_muleq:
			case CLEX_diveq:
			case CLEX_modeq:
			case CLEX_shleq:
			case CLEX_shreq:
			case CLEX_eqarrow:
				break;
			default:
				if (!(lex.token >= 0 && lex.token < 256)) {
					stb_lex_location loc = {0};
					stb_c_lexer_get_location(&lex, lex.where_firstchar, &loc);
					die2(args,"Lex error at line %i, character %i: unknown token %ld", loc.line_number, loc.line_offset, lex.token);
				}
				break;
		}

		if (t.toktype == CLEX_id && !strncmp(t.string, "CSERPENT_", 9)) {
			if (st->nannlocs == MAX_ANNLOCS)
				die2(args, "too many CSERPENT_ annotations in one file (max %i)", (int)MAX_ANNLOCS);
			st->annlocs[st->nannlocs++] = (AnnLoc){
				.tok_index = ntok,
				.loc = { .file = cur_file, .line = cur_line },
			};
		}

		tokens[ntok++] = t;
	}

	return ntok;
}


static void
read_input(StorageBuffers *st, CSerpentArgs args, long long text_bufsz, char *text)
{
	// ensure always null terminated
	memset(text, 0, text_bufsz);
	ingest_file(st, args, text_bufsz-1, text);
}


/*
	==========================================================
		Main
	==========================================================
*/


static void
usage(void)
{
	const char *message =

	"c-serpent v2 \n"
	"============ \n"
	"                                                                             \n"
	"Usage:  c-serpent [-p CMD] [-v] [-W] [--explain] input.c [input2.c ...] \n"
	"                                                                             \n"
	"c-serpent generates CPython extension-module wrappers for C functions. What  \n"
	"to wrap is declared in the source itself, inside '#ifdef CSERPENT' blocks     \n"
	"that are invisible to every other build. Typical usage is a manifest file    \n"
	"that includes the headers it needs and otherwise contains only instructions: \n"
	"                                                                             \n"
	"    #include \"mylib.h\" \n"
	"    #ifdef CSERPENT \n"
	"    CSERPENT_MODULE(mymodule) \n"
	"    CSERPENT_WRAPFN(mean_i32) \n"
	"    #endif \n"
	"                                                                             \n"
	"    $ c-serpent mymodule.cs.c > wrappers.c \n"
	"    $ cc -fPIC -shared -I/path/to/python/headers \\\n"
	"          wrappers.c mylib.c -lpython -o mymodule.so \n"
	"                                                                             \n"
	"Annotations may also be placed next to the definitions they describe. They   \n"
	"work in headers too, so a manifest can wrap code you are unable to edit.     \n"
	"                                                                             \n"
	"Flags: \n"
	"                                                                             \n"
	"-h          print this message and exit \n"
	"                                                                             \n"
	"-p CMD      preprocessor command, default 'cc -E'. Also settable with the    \n"
	"            CSERPENT_PP environment variable; the flag takes precedence.     \n"
	"            Include directories go here, e.g. -p \"cc -E -Ivendor/include\".   \n"
	"                                                                             \n"
	"-v          verbose: list the typedefs that were parsed \n"
	"                                                                             \n"
	"-W          enable warnings \n"
	"                                                                             \n"
	"--explain   list every wrap c-serpent found, with the file and line it came  \n"
	"            from, then exit without generating anything \n"
	"                                                                             \n"
	"Input files are positional. '-' means already-preprocessed source on stdin,  \n"
	"which c-serpent will not preprocess again; whoever preprocessed it must have \n"
	"passed -DCSERPENT or the annotations will have vanished. \n"
	"                                                                             \n"
	"Annotations: \n"
	"                                                                             \n"
	"CSERPENT_MODULE(name) \n"
	"    Name the generated module; must match the .so you build. At most once    \n"
	"    across all inputs. Omit it to emit wrappers with no module definition.   \n"
	"                                                                             \n"
	"CSERPENT_WRAPFN(names..., options...) \n"
	"    Wrap one or more functions. Options apply to all names given: \n"
	"      name = \"str\"     name seen from Python, if different (one name only)  \n"
	"      doc = \"str\"      docstring \n"
	"      errstr = 1       a non-NULL 'const char *' return is an error message \n"
	"      errcheck = fn    call fn afterwards; non-NULL 'const char *' -> raise  \n"
	"      errarg = N       argument handed to errcheck; 0 means the return value \n"
	"      addresses = 0|1  accept python ints as raw addresses where arrays go   \n"
	"      bytes = 0|1      accept bytes objects where arrays go \n"
	"                                                                             \n"
	"CSERPENT_WRAPFN_GENERIC(prefix, options...) \n"
	"    Generate a type-dispatching wrapper over the suffixed variants of prefix,\n"
	"    plus a wrapper for each variant found. Takes all CSERPENT_WRAPFN options,\n"
	"    plus strip_underscore = 0|1 (default 1: sum_ dispatches as 'sum'). \n"
	"                                                                             \n"
	"    Variant suffixes: b s i l = int8/16/32/64, B S I L = uint8/16/32/64, \n"
	"    f = float, h = _Float16, d = double, and F H D for their complex forms. \n"
	"    Supply whichever variants you have; c-serpent uses the ones it finds. \n"
	"                                                                             \n"
	"CSERPENT_WRAPFN_MANUAL(names..., options...) \n"
	"    Register a hand-written wrapper. c-serpent adds 'name' to the module and \n"
	"    expects you to supply a function called 'wrap_name'. Accepts doc/name.   \n"
	"                                                                             \n"
	"CSERPENT_WRAPCONST(names...) \n"
	"    Add integer constants to the module. A name that is an enum tag adds all \n"
	"    of that enum's constants; a name that is an enum constant adds just it.  \n"
	"    Anything else is an error: #defines do not survive preprocessing and so  \n"
	"    cannot be checked. \n"
	"                                                                             \n"
	"CSERPENT_OPAQUE(TypeName) \n"
	"    Treat TypeName as equivalent to void, so pointers to it convert to and   \n"
	"    from python integers. Useful for structs c-serpent cannot wrap. \n"
	"                                                                             \n"
	"CSERPENT_CONFIG(options...) \n"
	"    Output-wide settings. Setting one key to two different values is an      \n"
	"    error, except float16, which is OR'd together. \n"
	"      addresses = 0|1     default for the per-function option \n"
	"      bytes = 0|1         default for the per-function option \n"
	"      declarations = 0|1  emit declarations for wrapped functions (default 1)\n"
	"      float16 = 0|1       enable _Float16 support (needs compiler support)   \n"
	"                                                                             \n"
	"Environment variables: \n"
	"                                                                             \n"
	"CSERPENT_PP    acts like -p, but the flag overrides it \n"
	;

	fprintf(stderr, "%s", message);
}

/*
	v1 flags, so that an old invocation gets told where its setting went rather
	than a bare "unrecognized flag".
*/
static const char *
retired_flag_replacement(const char *flag)
{
	if (!strcmp(flag, "-f"))  return "input files are now positional";
	if (!strcmp(flag, "-m"))  return "use CSERPENT_MODULE(name)";
	if (!strcmp(flag, "-D"))  return "use CSERPENT_CONFIG(declarations = 0)";
	if (!strcmp(flag, "-f16"))return "use CSERPENT_CONFIG(float16 = 1)";
	if (!strcmp(flag, "-x"))  return "use CSERPENT_WRAPFN_MANUAL(name)";
	if (!strcmp(flag, "-g"))  return "use CSERPENT_WRAPFN_GENERIC(prefix)";
	if (!strcmp(flag, "-G"))  return "use CSERPENT_WRAPFN_GENERIC(prefix, strip_underscore = 0)";
	if (!strcmp(flag, "-E"))  return "use CSERPENT_WRAPCONST(names...), naming the constants you want";
	if (!strcmp(flag, "-t"))  return "use CSERPENT_OPAQUE(TypeName)";
	if (!strcmp(flag, "-a"))  return "use CSERPENT_CONFIG(addresses = 1), or addresses = 1 on a wrap";
	if (!strcmp(flag, "-b"))  return "use CSERPENT_CONFIG(bytes = 1), or bytes = 1 on a wrap";
	if (!strcmp(flag, "-P"))  return "removed: preprocessing is now mandatory";
	if (!strcmp(flag, "-i"))  return "removed: use a real #include and let the preprocessor resolve it";
	if (!strcmp(flag, "-I"))  return "removed: put include directories in -p, e.g. -p \"cc -E -Ivendor\"";
	if (!strncmp(flag, "-e", 2)) return "use errstr = 1, or errcheck = fn and errarg = N, on a wrap";
	return 0;
}

static int
wrapopts_equal(WrapOpts a, WrapOpts b)
{
	#define SAMESTR(x) ((a.x == b.x) || (a.x && b.x && !strcmp(a.x, b.x)))
	return SAMESTR(py_name) && SAMESTR(doc) && SAMESTR(errcheck)
		&& a.errarg == b.errarg
		&& a.errstr == b.errstr
		&& a.addresses == b.addresses
		&& a.bytes == b.bytes
		&& a.strip_underscore == b.strip_underscore;
	#undef SAMESTR
}

static const char *
wrap_kind_name(int kind)
{
	switch(kind) {
		case WK_FN:      return "CSERPENT_WRAPFN";
		case WK_GENERIC: return "CSERPENT_WRAPFN_GENERIC";
		case WK_MANUAL:  return "CSERPENT_WRAPFN_MANUAL";
		case WK_CONST:   return "CSERPENT_WRAPCONST";
		case WK_OPAQUE:  return "CSERPENT_OPAQUE";
	}
	return "?";
}

static void
add_export(StorageBuffers *st, CSerpentArgs args, const char *py_name, const char *c_name, const char *doc)
{
	if (st->nexports == MAX_EXPORTS)
		die2(args, "too many exported functions (max %i)", (int)MAX_EXPORTS);
	st->exports[st->nexports++] = (Export){ .py_name=py_name, .c_name=c_name, .doc=doc };
}

int
cserpent_main (char *argv[], FILE *in_stream, FILE *out_stream, FILE *err_stream)
{
	// allocate all the memory we need up front

	char  *text         = calloc(1, 1<<27);
	char  *string_store = calloc(1, 0x10000);
	Token *tokens       = calloc(1, (1<<27) * sizeof(*tokens)); // same size as text buffer -> running out is impossible
	StorageBuffers *storage = calloc(1, sizeof(*storage));

	// to facilitate returning from errors deep in the call stack we will use setjmp/longjmp
	// so we need preserve the pointers on the stack that we need to free later on
	jmp_buf jmp;
	volatile struct {
		FILE *open_file;
		int success;
		void *ptrs[4];
	} _resources = {
		.success = 1,
		.ptrs = {text, string_store, tokens, storage},
	};

	if(setjmp(jmp)) {
		// we land here if longjmp is called
		_resources.success = 0;
		goto cleanup;
	}

	// function begins in earnest

	CSerpentArgs args = {
			.preprocessor = "cc -E",
			.istream=in_stream,
			.ostream=out_stream,
			.estream=err_stream,
			.jmp = &jmp,
			.open_file = &_resources.open_file,
			.cfg_addresses    = {.value = -1},
			.cfg_bytes        = {.value = -1},
			.cfg_declarations = {.value = -1},
			.cfg_float16      = {.value = -1},
		};

	if(!(text && string_store && tokens && storage))
		die2(args,"out of mem");

	if (getenv("CSERPENT_PP"))
		args.preprocessor = getenv("CSERPENT_PP");

	const char *inputs[MAX_INPUTS];
	int ninputs = 0;
	int explain = 0;

	/*
		Arguments
	*/

	while (*argv) {

		if (!strcmp(*argv, "-h")) { usage(); goto cleanup; }

		if (!strcmp(*argv, "-v")) { args.verbose  = 1; argv++; continue; }
		if (!strcmp(*argv, "-W")) { args.warnings = 1; argv++; continue; }
		if (!strcmp(*argv, "--explain")) { explain = 1; argv++; continue; }

		if (!strcmp(*argv, "-p")) {
			argv++;
			if(!*argv) die2(args, "-p must be followed by a preprocessor command");
			args.preprocessor = *argv;
			argv++;
			continue;
		}

		if ((*argv)[0] == '-' && (*argv)[1]) {
			const char *repl = retired_flag_replacement(*argv);
			if (repl) die2(args, "'%s' was removed in c-serpent v2: %s", *argv, repl);
			fprintf(args.estream, "unrecognized flag: '%s'\n\n", *argv);
			usage();
			_resources.success = 0;
			goto cleanup;
		}

		if (ninputs == MAX_INPUTS) die2(args, "too many input files (max %i)", (int)MAX_INPUTS);
		inputs[ninputs++] = *argv;
		argv++;
	}

	if (!ninputs) { usage(); _resources.success = 0; goto cleanup; }

	/*
		Pass 1: collate.

		Preprocess, lex and scan each input for annotations, building the work
		list and the enum tables. Nothing is emitted until this has all
		succeeded, so a failure never leaves half a wrapper file on stdout.
	*/

	Loc module_loc = {0};

	for (int i = 0; i < ninputs; i++) {

		args.filename = inputs[i];
		read_input(storage, args, 1<<27, text);

		long long len = strlen(text);
		char *cached = malloc(len+1);
		if(!cached) die2(args, "out of mem");
		memcpy(cached, text, len+1);
		storage->cached[i] = cached;

		int ntok = lex_file(storage, args, 1<<27, tokens, cached, 0x10000, string_store);

		scan_annotations(storage, &args, ntok, tokens, i, 1, &module_loc);

		clear_symbols(storage);
		collect_enums((ParseCtx){
				.tokens_first = tokens,
				.tokens       = tokens,
				.tokens_end   = tokens+ntok,
				.storage      = storage,
				.args         = args, },
			storage);
	}

	/*
		Validate the work list.
	*/

	if (!args.modulename)
		warn_at(args, (Loc){.file=inputs[0], .line=0},
			"no CSERPENT_MODULE found; emitting wrappers only, with no module definition");

	// duplicate wraps: identical is fine and collapses, conflicting is not
	for (int i = 0; i < storage->nitems; i++) {
		if (storage->items[i].kind == WK_OPAQUE) continue;
		for (int j = 0; j < i; j++) {
			if (storage->items[j].kind != storage->items[i].kind) continue;
			if (strcmp(storage->items[j].name, storage->items[i].name)) continue;
			if (!wrapopts_equal(storage->items[j].opts, storage->items[i].opts))
				die_at(args, storage->items[i].loc,
					"'%s' is wrapped here with different options than at %s:%i",
					storage->items[i].name,
					storage->items[j].loc.file, storage->items[j].loc.line);
			storage->items[i].kind = 0;   // identical duplicate: drop it
			break;
		}
	}

	// resolve CSERPENT_WRAPCONST names against every enum we saw
	int num_enum_consts = 0;
	char *enum_consts[MAX_EXPORTS];

	for (int i = 0; i < storage->nitems; i++) {
		if (storage->items[i].kind != WK_CONST) continue;
		const char *want = storage->items[i].name;
		Loc loc = storage->items[i].loc;

		int as_tag = 0, as_member = 0;
		for (int e = 0; e < storage->nenumconsts; e++) {
			if (storage->enumconsts[e].tag[0] && !strcmp(storage->enumconsts[e].tag, want)) as_tag = 1;
			if (!strcmp(storage->enumconsts[e].name, want)) as_member = 1;
		}

		if (as_tag && as_member)
			die_at(args, loc, "'%s' is both an enum tag and an enum constant; "
			       "c-serpent cannot tell which you meant", want);
		if (!as_tag && !as_member)
			die_at(args, loc, "CSERPENT_WRAPCONST: '%s' is not an enum tag or an enum constant "
			       "(note that #defines cannot be wrapped)", want);

		for (int e = 0; e < storage->nenumconsts; e++) {
			const char *add = 0;
			if (as_tag && storage->enumconsts[e].tag[0] && !strcmp(storage->enumconsts[e].tag, want))
				add = storage->enumconsts[e].name;
			else if (as_member && !strcmp(storage->enumconsts[e].name, want))
				add = storage->enumconsts[e].name;
			if (!add) continue;

			int already = 0;
			for (int k = 0; k < num_enum_consts; k++)
				if (!strcmp(enum_consts[k], add)) { already = 1; break; }
			if (already) continue;

			if (num_enum_consts == COUNT_ARRAY(enum_consts))
				die_at(args, loc, "too many module constants (max %i)", (int)COUNT_ARRAY(enum_consts));
			enum_consts[num_enum_consts++] = (char*) add;
		}
	}

	if (explain) {
		fprintf(args.estream, "module: %s\n", args.modulename ? args.modulename : "(none: wrappers only)");
		for (int i = 0; i < storage->nitems; i++) {
			WrapItem *it = &storage->items[i];
			if (!it->kind) continue;
			fprintf(args.estream, "%s(%s)  [%s:%i]\n",
				wrap_kind_name(it->kind), it->name, it->loc.file, it->loc.line);
		}
		fprintf(args.estream, "%i module constants\n", num_enum_consts);
		goto cleanup;
	}

	/*
		Pass 2: emit.
	*/

	emit_preamble(args);

	for (int i = 0; i < ninputs; i++) {

		args.filename = inputs[i];
		args.opts = 0;

		int ntok = lex_file(storage, args, 1<<27, tokens, storage->cached[i], 0x10000, string_store);
		scan_annotations(storage, &args, ntok, tokens, i, 0, &module_loc);

		// typedefs are per input; generic dispatch resolves int64_t and
		// friends through this table, so it has to be rebuilt for each file.
		clear_symbols(storage);
		populate_symbols(storage, (ParseCtx){
				.tokens_first = tokens,
				.tokens       = tokens,
				.tokens_end   = tokens+ntok,
				.storage      = storage,
				.args         = args, });

		// CSERPENT_OPAQUE is output-global, so apply every one of them to
		// every input's symbol table.
		for (int k = 0; k < storage->nitems; k++) {
			if (storage->items[k].kind != WK_OPAQUE) continue;
			add_symbol(args, storage, (Symbol){
				.name = (char*) storage->items[k].name,
				.type = {.category = T_VOID},
			});
		}

		for (int k = 0; k < storage->nitems; k++) {

			WrapItem *it = &storage->items[k];
			if (it->input != i) continue;

			args.opts = &it->opts;

			ParseCtx p = {
				.tokens_first = tokens,
				.tokens       = tokens,
				.tokens_end   = tokens+ntok,
				.storage      = storage,
				.args         = args,
			};

			Symbol argsyms[MAX_FN_ARGS] = {0};

			if (it->kind == WK_FN) {

				if(!parse_file_look_for_function(p, it->name, argsyms))
					die_at(args, it->loc, "no function called '%s' in '%s'", it->name, inputs[i]);
				add_export(storage, args,
					it->opts.py_name ? it->opts.py_name : it->name,
					it->name, it->opts.doc);

			} else if (it->kind == WK_MANUAL) {

				add_export(storage, args,
					it->opts.py_name ? it->opts.py_name : it->name,
					it->name, it->opts.doc);

			} else if (it->kind == WK_GENERIC) {

				int n_variants_found = 0;
				VariantSuffix variant_suffixes[] = {

					/*
						The order is important here.
						For scalar arguments, the generated dispatcher will call
						the first version that appears.
					*/

					{get_symbol_or_die(args, storage, "int64_t")->type, 'l'},
					{get_symbol_or_die(args, storage, "int32_t")->type, 'i'},
					{get_symbol_or_die(args, storage, "int16_t")->type, 's'},
					{get_symbol_or_die(args, storage, "int8_t")->type,  'b'},

					{get_symbol_or_die(args, storage, "uint64_t")->type, 'L'},
					{get_symbol_or_die(args, storage, "uint32_t")->type, 'I'},
					{get_symbol_or_die(args, storage, "uint16_t")->type, 'S'},
					{get_symbol_or_die(args, storage, "uint8_t")->type,  'B'},

					{(Type){.category=T_DOUBLE},  'd'},
					{(Type){.category=T_FLOAT16}, 'h'},
					{(Type){.category=T_FLOAT},   'f'},

					{(Type){.category=T_DOUBLE, .is_complex=1},  'D'},
					{(Type){.category=T_FLOAT16, .is_complex=1}, 'H'},
					{(Type){.category=T_FLOAT, .is_complex=1},   'F'},
				};

				short arg_match_count[MAX_FN_ARGS] = {0};

				for (int s = 0; s < COUNT_ARRAY(variant_suffixes); s++) {

					char namebuf[500] = {0};
					if (ssizeof(namebuf) <= snprintf(namebuf, sizeof(namebuf), "%s%c",
							it->name, variant_suffixes[s].suffix))
						die_at(args, it->loc, "function name too long");

					if (parse_file_look_for_function(p, namebuf, argsyms)) {

						char *vname = intern_string(args, storage, namebuf, 0);
						add_export(storage, args, vname, vname, it->opts.doc);

						variant_suffixes[s].found = 1;
						n_variants_found++;

						for (int j = 0; j < MAX_FN_ARGS; j++)
							arg_match_count[j] += compare_types_equal(
								argsyms[j].type, variant_suffixes[s].type, 0,0,0,0);
					}
				}

				if (n_variants_found == 0)
					die_at(args, it->loc, "found no variants of '%s' in '%s' following the "
					       "suffix convention", it->name, inputs[i]);

				emit_dispatch_wrapper(p, it->name, n_variants_found, arg_match_count,
					COUNT_ARRAY(variant_suffixes), variant_suffixes, argsyms);

				// the dispatcher is emitted as wrap_<prefix>; python sees the
				// prefix with any trailing underscore removed by default
				char dispname[500] = {0};
				int dlen = snprintf(dispname, sizeof(dispname), "%s", it->name);
				assert(ssizeof(dispname)-1 > dlen);
				if (it->opts.strip_underscore != 0 && dlen && dispname[dlen-1] == '_')
					dispname[dlen-1] = 0;

				add_export(storage, args,
					it->opts.py_name ? it->opts.py_name : intern_string(args, storage, dispname, 0),
					it->name, it->opts.doc);
			}

			args.opts = 0;
		}
	}

	emit_module(args, storage->nexports, storage->exports, num_enum_consts, enum_consts);

	cleanup: // free memory
	// read the storage pointer back out of the volatile record rather than
	// from the local, whose value is indeterminate after longjmp
	{
		StorageBuffers *st = (StorageBuffers *) _resources.ptrs[3];
		if (st) for (int i = 0; i < MAX_INPUTS; i++) free(st->cached[i]);
	}
	for(unsigned i = 0; i < sizeof(_resources.ptrs)/sizeof(_resources.ptrs[0]); i++)
		free(_resources.ptrs[i]);
	if(_resources.open_file) fclose(_resources.open_file);
	return ! _resources.success; // rtn zero on success
}

#if (_POSIX_C_SOURCE >= 200809L || defined(__APPLE__)) // requires fmemopen
int
cserpent_main_buffers(
	char *cserpent_argv[],

	long long stdin_buf_size,
	unsigned char *stdin_buf, 

	long long stdout_buf_size,
	unsigned char *stdout_buf,

	long long stderr_buf_size,
	unsigned char *stderr_buf)
{
	/*
		Run C-Serpent from in-memory buffers provided	

		RETURNS
			0 on success
			1 on failure
			1 on invalid arguments (no explanation is given)
	*/

	if(!cserpent_argv) return 1;

	int rtn = 1;
	FILE *in=0, *out=0, *err=0;
	
	if(stdin_buf) {
		in = fmemopen(stdin_buf, stdin_buf_size, "r");
		if(!in) goto bail;
	} else {
		in = stdin;
	}

	if(stdout_buf) {
		out = fmemopen(stdout_buf, stdout_buf_size, "w");
		if(!out) goto bail;
	} else {
		out = stdout;
	}

	if(stderr_buf) {
		err = fmemopen(stderr_buf, stderr_buf_size, "w");
		if(!err) goto bail;
	} else {
		err = stderr;
	}

	rtn = cserpent_main(cserpent_argv, in, out, err);

	bail:
	if(in && stdin_buf)   fclose(in);
	if(out && stdout_buf) fclose(out);
	if(err && stderr_buf) fclose(err);
	return rtn;
}
#endif

#ifndef CSERPENT_SUPPRESS_MAIN
int 
main (int argc, char *argv[])
{
	(void)argc;
	argv++; // strip off the conventional program name
	if(!*argv) {
		usage(); 
		exit(EXIT_FAILURE);
	}
	return cserpent_main(argv, stdin, stdout, stderr);
}
#endif
