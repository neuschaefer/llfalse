/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2012-2013  Jonathan Neuschäfer <j.neuschaefer@gmx.net>
 *
 * llfalse - a portable False compiler
 *
 * If you're looking for a type-safe, lightweight False interpreter, please use
 * Wouter's, it's in the package at <https://strlen.com/false-language>
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>

#include "util.h"

#include <llvm-c/Core.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Analysis.h> /* for LLVMVerifyModule */


/* The maximum number of items the false stack. */
#define DEFAULT_STACKSIZE 1024 /* 4kB */

/* options */
static struct {
	bool decode_latin1;
	bool decode_utf8;
	bool unsigned_mode;
	unsigned int stack_size;
	unsigned int int_width;
} options = {
	.decode_latin1 = true,
	.decode_utf8 = true,
	.unsigned_mode = false,
	.stack_size = DEFAULT_STACKSIZE,
	.int_width = sizeof(int) * CHAR_BIT /* FIXME */
};

static void parse_cmdline(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	/* nop */
}


enum linkage {
	LINKAGE_DATA,
	LINKAGE_CONST_DATA,
	LINKAGE_CODE,
};

/* set LLVM linkage and related attributes */
static void set_linkage(LLVMValueRef v, enum linkage lk)
{
	LLVMSetLinkage(v, LLVMPrivateLinkage);

	/* DATA, CONST_DATA: thread-local */
	/* CONST_DATA: const */
	/* CODE: anything special? */
}

/* lambdas are basically anonymous functions */
struct environment;
struct lambda {
	uint32_t id;
	struct lambda *prev;	/* linked list */
	struct environment *env;

	unsigned int line, column;

	/* the number of BBs allocated so far */
	unsigned int n_bb;

	LLVMValueRef fn;
	LLVMBasicBlockRef bb;	/* always valid */
	LLVMBuilderRef builder;
};

struct environment {
	FILE *fp;
	const char *file;
	struct lambda *last_lambda;
	unsigned int string_id;

	LLVMModuleRef module;
	LLVMTypeRef lambda_type;

	LLVMValueRef func_main, func_lambda_0;

	LLVMValueRef func_printnum, func_printstring, func_putchar,
		     func_getchar, func_flush;
	LLVMValueRef var_vars, var_stack, var_stackidx, var_lambdas;
};

static void l_init_llvm(struct lambda *l, const char *name)
{
	l->fn = LLVMAddFunction(l->env->module, name, l->env->lambda_type);

	/* Set private linkage to allow better optimization */
	set_linkage(l->fn, LINKAGE_CODE);

	l->bb = LLVMAppendBasicBlock(l->fn, "");
	l->n_bb = 1;
	l->builder = LLVMCreateBuilder();
	LLVMPositionBuilderAtEnd(l->builder, l->bb);
}

/* allocate a new lambda and add it to the linked list */
static struct lambda *l_new_child(struct lambda *parent)
{
	char buffer[sizeof("lambda_4000000000")];

	struct lambda *new_l = xmalloc(sizeof(*new_l));
	new_l->id = parent->env->last_lambda->id + 1;
	new_l->prev = parent->env->last_lambda;
	parent->env->last_lambda = new_l;
	new_l->env = parent->env;
	new_l->line = parent->line;
	new_l->column = parent->column;

	snprintf(buffer, sizeof(buffer), "lambda_%lu", (unsigned long) new_l->id);
	l_init_llvm(new_l, buffer);

	return new_l;
}

/* allocate the first lambda */
static struct lambda *l_new(struct environment *env)
{
	struct lambda *new_l = xmalloc(sizeof(*new_l));
	new_l->id = 0;
	new_l->prev = NULL;
	env->last_lambda = new_l;
	new_l->env = env;
	new_l->line = 1;
	new_l->column = 0;

	l_init_llvm(new_l, "lambda_0");

	return new_l;
}

/* allocate a new basic block */
static LLVMBasicBlockRef l_new_bb(struct lambda *l)
{
	char buffer[32];

	snprintf(buffer, sizeof(buffer), "b%u", l->n_bb++);
	return LLVMAppendBasicBlock(l->fn, buffer);
}

static int l_getchar(struct lambda *l)
{
	int ch = getc(l->env->fp);
	if (ch == '\n') {
		l->line++;
		l->column = 0;
	} else {
		l->column++;
	}

	return ch;
}

static void l_vmessage(struct lambda *l, const char *pre,
		const char *fmt, va_list ap)
{
	fprintf(stderr, "%s:%u:%u: %s", l->env->file, l->line, l->column, pre);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
}

static void l_warning(struct lambda *l, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	l_vmessage(l, "warning: ", fmt, ap);
	va_end(ap);
}

static void l_error(struct lambda *l, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	l_vmessage(l, "error: ", fmt, ap);
	va_end(ap);

	exit(EXIT_FAILURE);
}


static LLVMValueRef u32_value(uint32_t n)
{
	LLVMTypeRef i32t = LLVMInt32Type();
	return n? LLVMConstInt(i32t, n, false) : LLVMConstNull(i32t);
}

/* returns a pointer (value) to the requested stack element.
   0 selects the top element, 1 the element below the top etc. */
static LLVMValueRef index_stack_by_value(struct lambda *l, LLVMValueRef i)
{
	LLVMValueRef indices[2], stackidx;

	stackidx = LLVMBuildLoad(l->builder, l->env->var_stackidx, "");
	indices[0] = u32_value(0); /* We're accessing a global. */
	indices[1] = LLVMBuildSub(l->builder, stackidx, i, "");
	return LLVMBuildInBoundsGEP(l->builder, l->env->var_stack, indices, 2, "");
}
#define index_stack(l, i) index_stack_by_value((l), u32_value(i))

static void store_stack(struct lambda *l, uint32_t index, LLVMValueRef value)
{
	LLVMBuildStore(l->builder, value, index_stack(l, index));
}

static LLVMValueRef load_stack(struct lambda *l, uint32_t index)
{
	return LLVMBuildLoad(l->builder, index_stack(l, index), "");
}

/* positive deltas grow the stack, negative deltas shrink it */
static void grow_stack(struct lambda *l, int delta)
{
	LLVMValueRef old_size, new_size;

	if (delta < 0) {
		/* invalidate the newly free'd memory */
		LLVMValueRef undef = LLVMGetUndef(LLVMInt32Type());
		int i;
		for (i = 0; i < -delta; i++)
			LLVMBuildStore(l->builder, undef, index_stack(l, i));
	}

	old_size = LLVMBuildLoad(l->builder, l->env->var_stackidx, "");
	new_size = LLVMBuildAdd(l->builder, old_size, u32_value(delta), "");
	LLVMBuildStore(l->builder, new_size, l->env->var_stackidx);
}

static void push_stack(struct lambda *l, LLVMValueRef value)
{
	grow_stack(l, 1);
	store_stack(l, 0, value);
}

static LLVMValueRef pop_stack(struct lambda *l)
{
	LLVMValueRef ret;

	ret = load_stack(l, 0);
	grow_stack(l, -1);
	return ret;
}

static LLVMValueRef index_variables(struct lambda *l, LLVMValueRef ref)
{
	LLVMValueRef indices[2];

	indices[0] = u32_value(0);
	indices[1] = ref;
	return LLVMBuildInBoundsGEP(l->builder, l->env->var_vars, indices, 2, "");
}

static LLVMValueRef load_lambdas(struct lambda *l, LLVMValueRef index)
{
	LLVMValueRef ptr, gep;

	ptr = LLVMBuildLoad(l->builder, l->env->var_lambdas, "");
	gep = LLVMBuildGEP(l->builder, ptr, &index, 1, "");
	return LLVMBuildLoad(l->builder, gep, "");
}

static void build_string(struct lambda *l)
{
	struct growbuf *buf;
	LLVMValueRef str, global, indices[2];
	int ch;
	char tmp;
	/* four billion strings ought to be enough
	   for everyone :-) */
	char name_buf[sizeof("string_4000000000")];

	buf = growbuf_new();
	while((ch = l_getchar(l)) != EOF && ch != '"') {
		tmp = ch;
		growbuf_add(buf, &tmp, 1);
	}

	if (ch == EOF)
		l_error(l, "Unexpected end of file inside string.");

	str = LLVMConstString(growbuf_buf(buf),
			growbuf_len(buf), false);
	growbuf_free(buf);

	/* add global, init with str */
	snprintf(name_buf, sizeof(name_buf), "string_%lu",
			(unsigned long) l->env->string_id);
	l->env->string_id++;

	global = LLVMAddGlobal(l->env->module, LLVMTypeOf(str), name_buf);
	set_linkage(global, LINKAGE_CONST_DATA);
	LLVMSetInitializer(global, str);

	/* load from global, pass value to call inst */
	indices[0] = u32_value(0);
	indices[1] = indices[0];
	str = LLVMBuildGEP(l->builder, global, indices, 2, "");
	LLVMBuildCall(l->builder, l->env->func_printstring, &str, 1, "");
}

static void build_simple_binop(struct lambda *l, LLVMOpcode op)
{
	/* stack: a, b -> (a op b) */
	LLVMValueRef a, b, res;

	b = pop_stack(l);
	a = pop_stack(l);

	res = LLVMBuildBinOp(l->builder, op, a, b, "");
	push_stack(l, res);
}

static void build_icmp_op(struct lambda *l, LLVMIntPredicate op)
{
	/* stack: a, b -> sext(a op b) */
	LLVMValueRef a, b, res, sext;

	b = pop_stack(l);
	a = pop_stack(l);

	res = LLVMBuildICmp(l->builder, op, a, b, "");

	/* false -> 0, true -> 0xffffffff */
	sext = LLVMBuildSExt(l->builder, res, LLVMInt32Type(), "");

	push_stack(l, sext);
}

/*
 * The generated code is basically:
 * parent:
 *   pop body_fn
 *   pop cond
 *   br cond? body : out
 * body:
 *   call body_fn
 *   br out
 * out:
 *   (new empty bb)
 */
static void build_if(struct lambda *l)
{
	/* stack: bool,fn - */
	LLVMValueRef cond_v, cond, body_l, body_fn;
	LLVMBasicBlockRef body_bb, out_bb;

	body_l = pop_stack(l);
	cond_v = pop_stack(l);

	body_fn = load_lambdas(l, body_l);
	cond = LLVMBuildIsNotNull(l->builder, cond_v, "");

	body_bb = l_new_bb(l);
	out_bb = l_new_bb(l);

	LLVMBuildCondBr(l->builder, cond, body_bb, out_bb);

	LLVMPositionBuilderAtEnd(l->builder, body_bb);
	LLVMBuildCall(l->builder, body_fn, NULL, 0, "");
	LLVMBuildBr(l->builder, out_bb);

	LLVMPositionBuilderAtEnd(l->builder, out_bb);
	l->bb = out_bb;
}

/*
 * parent:
 *   pop body_fn
 *   pop cond_fn
 *   br head
 * head:
 *   call cond_fn
 *   pop cond
 *   br cond? body:out
 * body:
 *   call body_fn
 *   br head
 * out:
 *   (new empty bb)
 */
static void build_while(struct lambda *l)
{
	LLVMValueRef cond_l, body_l, cond_fn, body_fn, cond_v, cond;
	LLVMBasicBlockRef head_bb, body_bb, out_bb;

	head_bb = l_new_bb(l);
	body_bb = l_new_bb(l);
	out_bb = l_new_bb(l);

	body_l = pop_stack(l);
	cond_l = pop_stack(l);
	body_fn = load_lambdas(l, body_l);
	cond_fn = load_lambdas(l, cond_l);
	LLVMBuildBr(l->builder, head_bb);

	LLVMPositionBuilderAtEnd(l->builder, head_bb);
	LLVMBuildCall(l->builder, cond_fn, NULL, 0, "");
	cond_v = pop_stack(l);
	cond = LLVMBuildIsNotNull(l->builder, cond_v, "");
	LLVMBuildCondBr(l->builder, cond, body_bb, out_bb);

	LLVMPositionBuilderAtEnd(l->builder, body_bb);
	LLVMBuildCall(l->builder, body_fn, NULL, 0, "");
	LLVMBuildBr(l->builder, head_bb);

	LLVMPositionBuilderAtEnd(l->builder, out_bb);
	l->bb = out_bb;
}

static int ascii_isdigit(int x) { return x >= '0' && x <= '9'; }
static uint32_t ascii_digit_value(int x) { return (uint32_t) (x - '0'); }

static void parse_lambda(struct lambda *l)
{
	while(1) {
		int ch = l_getchar(l);
reparse:
		if (ch == EOF) {
			if (l->id != 0)
				l_error(l, "Unexpected end of file. Use ']' to terminate lambdas.");
			break;
		} else if (ch == ']') {
			if (l->id == 0)
				l_error(l, "']' unexpected.");
			break;
		}


		if (ch >= 'a' && ch <= 'z') {
			/* variable reference */
			push_stack(l, u32_value(ch - 'a'));
		} else if (ascii_isdigit(ch)) {
			/* number */
			uint32_t num = ascii_digit_value(ch);

			/* TODO: detect overflow */
			while (ascii_isdigit((ch = l_getchar(l))))
				num = 10 * num + ascii_digit_value(ch);

			push_stack(l, u32_value(num));

			/* we still have the first non-digit character in ch */
			goto reparse;
		} else switch (ch) {
		case ' ': /* ingore whitespace */
		case '\n':
		case '\t':
			break;
		case 0xc3: /* UTF-8 */
			if (!options.decode_utf8)
				goto default_label;
			ch = l_getchar(l);
			if (ch == 0x9f)
				ch = 'B';
			else if (ch == 0xb8)
				ch = 'O';
			else
				l_error(l, "Invalid UTF-8 seqence c3 %02x", ch);
			l->column--;
			goto reparse;
		case '{': /* comment */
			while ((ch = l_getchar(l)) != '}')
				if (ch == EOF)
					l_error(l, "Unexpected end of file. Use '}' to terminate comments");
			break;
		case '[': /* lambda */
			{
				struct lambda *new_l = l_new_child(l);
				parse_lambda(new_l);

				/* adjust lines and columns */
				l->line = new_l->line;
				l->column = new_l->column;

				push_stack(l, u32_value(new_l->id));
			} break;
		case '\'': /* char value */
			ch = l_getchar(l);
			if (ch == EOF)
				l_error(l, "Unexpected end of file after apostroph (')");
			push_stack(l, u32_value((uint32_t)(unsigned char) ch));
			break;
		case '`': /* inline assembly */
			l_warning(l, "Inline assembly isn't supported, ignoring.");
			break;
		case ':': /* store */
			{
				/* stack: val, ref -> (nothing) */
				LLVMValueRef val, ref;

				ref = pop_stack(l);
				val = pop_stack(l);

				LLVMBuildStore(l->builder, val, index_variables(l, ref));
			} break;
		case ';': /* load */
			{
				LLVMValueRef ref, ptr, val;

				ref = pop_stack(l);
				ptr = index_variables(l, ref);
				val = LLVMBuildLoad(l->builder, ptr, "");
				push_stack(l, val);
			} break;
		case '!': /* call */
			{ /* move code to call_lambda(index) */
				/* lambdas are stored on the stack as 32-bit
				   indices to a global array that contains pointers
				   to the actual functions */
				LLVMValueRef index, fn;

				index = pop_stack(l);
				fn = load_lambdas(l, index);
				LLVMBuildCall(l->builder, fn, NULL, 0, "");
			} break;
		case '+': /* add */
			build_simple_binop(l, LLVMAdd);
			break;
		case '-': /* sub */
			build_simple_binop(l, LLVMSub);
			break;
		case '*': /* mult */
			build_simple_binop(l, LLVMMul);
			break;
		case '/': /* div */
			build_simple_binop(l, options.unsigned_mode?
						LLVMUDiv : LLVMSDiv);
			break;
		case '&': /* and */
			build_simple_binop(l, LLVMAnd);
			break;
		case '|': /* or */
			build_simple_binop(l, LLVMOr);
			break;
		case '=': /* eq */
			build_icmp_op(l, LLVMIntEQ);
			break;
		case '>': /* gt */
			build_icmp_op(l, options.unsigned_mode?
						LLVMIntUGT : LLVMIntSGT);
			break;
		case '_': /* neg */
			store_stack(l, 0, LLVMBuildNeg(l->builder,
						load_stack(l, 0), ""));
			break;
		case '~': /* not (bitwise) */
			store_stack(l, 0, LLVMBuildNot(l->builder,
						load_stack(l, 0), ""));
			break;
		case '$': /* dup */
			push_stack(l, load_stack(l, 0));
			break;
		case '%': /* drop */
			grow_stack(l, -1);
			break;
		case '\\': /* swap */
			{
				/* a, b -> b, a */
				LLVMValueRef a, b;
				b = pop_stack(l);
				a = pop_stack(l);
				push_stack(l, b);
				push_stack(l, a);
			} break;
		case '@': /* rotate */
			{
				/* a, b, c -> b, c, a */
				LLVMValueRef a, b, c;
				a = load_stack(l, 2);
				b = load_stack(l, 1);
				c = load_stack(l, 0);
				store_stack(l, 2, b);
				store_stack(l, 1, c);
				store_stack(l, 0, a);
			} break;
		case 0xf8: /* ø in latin1 */
			if (!options.decode_latin1)
				goto default_label;
		case 'O': /* pick (ø) */
			/* Get the nth element of the stack, counted from the
			   top, and push it. The zeroth element is the one just
			   below the index, so "0ø" equals "$". */
			{
				LLVMValueRef index, value;

				index = pop_stack(l);
				value = LLVMBuildLoad(l->builder,
						index_stack_by_value(l, index), "pick");
				push_stack(l, value);
			} break;
		case '?': /* if */
			build_if(l);
			break;
		case '#': /* while */
			build_while(l);
			break;
		case '.': /* printnum */
			{
				/* TODO: consider options.unsigned_mode */
				LLVMValueRef arg = pop_stack(l);
				LLVMBuildCall(l->builder, l->env->func_printnum, &arg, 1, "");
			} break;
		case '"': /* string */
			build_string(l);
			break;
		case ',': /* putc */
			{
				LLVMValueRef arg = pop_stack(l);
				LLVMBuildCall(l->builder, l->env->func_putchar, &arg, 1, "");
			} break;
		case '^': /* getc */
			{
				LLVMValueRef res;

				res = LLVMBuildCall(l->builder,
						l->env->func_getchar, NULL, 0, "");
				push_stack(l, res);
			} break;
		case 0xdf: /* ß in latin1 */
			if (!options.decode_latin1)
				goto default_label;
		case 'B': /* flush (ß) */
			LLVMBuildCall(l->builder, l->env->func_flush, NULL, 0, "");
			break;
default_label: /* goto default; apparently doesn't work */
		default:
			if (isprint(ch))
				l_error(l, "Invalid character '%c'.", ch);
			else
				l_error(l, "Invalid character '\\x%02x'.", ch);
		}
	}

	/* add a return intruction */
	LLVMBuildRetVoid(l->builder);

	/* we don't need the builder anymore */
	LLVMDisposeBuilder(l->builder);
	l->builder = NULL;
	l->bb = NULL;
}

/* build the libfalse interface etc. */
static void prepare_env(struct environment *env)
{
	LLVMTypeRef voidt, i32t, strt, strpt, intt, lambdappt;
	LLVMTypeRef fnt_void_i32, fnt_void_str, fnt_i32_void, fnt_void_void;
	LLVMTypeRef fnt_main, parm_main[2];
	LLVMTypeRef art_vars, art_stack;

	voidt = LLVMVoidType();
	i32t = LLVMInt32Type(); /* LLVM doesn't have signedness at this level */
	strt = LLVMPointerType(LLVMInt8Type(), 0); /* no const, either (?) */
	strpt = LLVMPointerType(strt, 0); /* (char **) */

	fnt_void_i32 = LLVMFunctionType(voidt, &i32t, 1, false);
	fnt_void_str = LLVMFunctionType(voidt, &strt, 1, false);
	fnt_i32_void = LLVMFunctionType(i32t, NULL, 0, false);
	fnt_void_void = LLVMFunctionType(voidt, NULL, 0, false);

	/* define uint32_t vars[26]; */
	art_vars = LLVMArrayType(i32t, 26);
	env->var_vars = LLVMAddGlobal(env->module, art_vars, "vars");
	set_linkage(env->var_vars, LINKAGE_DATA);
	LLVMSetInitializer(env->var_vars, LLVMConstNull(art_vars));

	/* define uint32_t stack[STACKSIZE]; */
	art_stack = LLVMArrayType(i32t, options.stack_size);
	env->var_stack = LLVMAddGlobal(env->module, art_stack, "stack");
	set_linkage(env->var_stack, LINKAGE_DATA);
	LLVMSetInitializer(env->var_stack, LLVMConstNull(art_stack));

	/* define uint32_t stack_index; */
	env->var_stackidx = LLVMAddGlobal(env->module, i32t, "stack_index");
	set_linkage(env->var_stackidx, LINKAGE_DATA);
	LLVMSetInitializer(env->var_stackidx, LLVMConstNull(i32t));

	/* typedef void (*lambda_t)(void); */
	env->lambda_type = fnt_void_void;

	/* declare lambda_t lambdas[]; */
	lambdappt = LLVMPointerType(LLVMPointerType(env->lambda_type, 0), 0);
	env->var_lambdas = LLVMAddGlobal(env->module, lambdappt, "lambdas");
	set_linkage(env->var_lambdas, LINKAGE_CONST_DATA);

	/* extern void lf_printnum(uint32_t i); */
	env->func_printnum = LLVMAddFunction(env->module, "lf_printnum", fnt_void_i32);
	/* extern void lf_printstring(const char *str); */
	env->func_printstring = LLVMAddFunction(env->module, "lf_printstring", fnt_void_str);
	/* extern void lf_putchar(uint32_t ch); */
	env->func_putchar = LLVMAddFunction(env->module, "lf_putchar", fnt_void_i32);
	/* extern uint32_t lf_getchar(void); */
	env->func_getchar = LLVMAddFunction(env->module, "lf_getchar", fnt_i32_void);
	/* extern void lf_flush(void); */
	env->func_flush = LLVMAddFunction(env->module, "lf_flush", fnt_void_void);

	/* int main(int argc, char **argv); */
	intt = LLVMIntType(options.int_width);
	parm_main[0] = intt;
	parm_main[1] = strpt;
	fnt_main = LLVMFunctionType(intt, parm_main, 2, false);
	env->func_main = LLVMAddFunction(env->module, "main", fnt_main);
}

/* Getting this right wasn't quite easy, but compiling the following piece of
   C code with clang helped me find the way to go.

	typedef void(fn_t)(void);

	fn_t fn1, fn2;

	fn_t *a[] = {fn1, fn2};
	fn_t **p = a;

   The interesting part of the bitcode is this:

	@a = global [2 x void ()*] \
		[void ()* @fn1, void ()* @fn2], align 16
	@p = global void ()** getelementptr inbounds \
		([2 x void ()*]* @a, i32 0, i32 0), align 8

 */
static void fill_lambdas(struct environment *env)
{
	LLVMValueRef *values, array_const, anon_global, gep_ptr, indices[2];
	unsigned num;
	struct lambda *tmp;

	/* collect all lambda function values */
	num = env->last_lambda->id + 1;
	values = xmalloc(num * sizeof(*values));
	for (tmp = env->last_lambda; tmp; tmp = tmp->prev)
		values[tmp->id] = tmp->fn;

	/* make an array constant and initialize an anonymous global with it */
	array_const = LLVMConstArray(LLVMPointerType(env->lambda_type,0), values, num);
	free(values);
	anon_global = LLVMAddGlobal(env->module, LLVMTypeOf(array_const), "");
	set_linkage(anon_global, LINKAGE_CONST_DATA);
	LLVMSetInitializer(anon_global, array_const);

	/* make the "lambdas" global point to the array */
	indices[1] = indices[0] = u32_value(0);
	gep_ptr = LLVMConstInBoundsGEP(anon_global, indices, 2);
	LLVMSetInitializer(env->var_lambdas, gep_ptr);
}

static void finish_env(struct environment *env)
{
	LLVMBuilderRef builder;
	LLVMBasicBlockRef main_bb;
	LLVMTypeRef intt;

	fill_lambdas(env);

	/* build main */
	builder = LLVMCreateBuilder();
	main_bb = LLVMAppendBasicBlock(env->func_main, "");
	LLVMPositionBuilderAtEnd(builder, main_bb);

	LLVMBuildCall(builder, env->func_lambda_0, NULL, 0, "");
	intt = LLVMIntType(options.int_width);
	LLVMBuildRet(builder, LLVMConstNull(intt));

	LLVMDisposeBuilder(builder);
}

static void compile_file(const char *infile, const char *outfile)
{
	struct environment env;
	FILE *infp, *outfp;
	struct lambda *main_l;

	/* open files */
	if (infile) {
		infp = xfopen(infile, "r");
	} else {
		infp = stdin;
		infile = "<stdin>";
	}
	if (outfile) {
		outfp = xfopen(outfile, "w");
	} else {
		outfp = stdout;
		outfile = "<stdout>";
	}

	/* that saves us from a bit of work */
	memset(&env, 0, sizeof(env));

	env.fp = infp;
	env.file = infile;
	env.module = LLVMModuleCreateWithName("llfalse");

	prepare_env(&env);

	main_l = l_new(&env);
	parse_lambda(main_l);

	env.func_lambda_0 = main_l->fn;
	finish_env(&env);

	LLVMVerifyModule(env.module, LLVMPrintMessageAction, NULL);

	/* (0,0 means shouln't close, not unbuffered) */
	LLVMWriteBitcodeToFD(env.module, fileno(outfp), 0, 0);

	LLVMDisposeModule(env.module);
}

int main(int argc, char **argv)
{
	const char *infile, *outfile;

	/* options */
	parse_cmdline(argc, argv);
	infile = NULL; /* TODO */
	outfile = NULL;
	/* TODO: don't print bitcode to a terminal without being asked */

	compile_file(infile, outfile);

	return EXIT_SUCCESS;
}
