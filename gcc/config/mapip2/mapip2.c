#include "config.h"
#include "system.h"
#include <signal.h>
#include "coretypes.h"
#include "mapip2.h"
#include "rtl.h"
#include "regs.h"
#include "hard-reg-set.h"
#include "real.h"
#include "insn-config.h"
#include "insn-constants.h"
#include "conditions.h"
#include "insn-attr.h"
#include "recog.h"
#include "toplev.h"
#include "output.h"

#include "basic-block.h"

#include "tree.h"
#include "function.h"
#include "expr.h"
#include "flags.h"
#include "reload.h"
#include "output.h"
#include "tm_p.h"
#include "ggc.h"
#include "diagnostic-core.h"
#include "df.h"

#include "target.h"
#include "target-def.h"

#define SHOULD_COMBINE_STORE_RESTORE 1

static struct mapip_frame_info {
  int valid;		/* 0 values are not valid */
  int rmask;		/* mask of registers saved */
  int rblocks;		/* n of register blocks to store/restore */
  int first_reg; /* first saved register */
  int last_reg;  /* last saved register */
  int n_regs;		/* number of saved regs */
  int regs;		/* size of saved registers area */
  long total_size;	/* total size in bytes */
  long outgoing;	/* size of outgoing args area */
  long locals;		/* size locals area */
} frame_info;


static void print_mem_expr_old (FILE *file, rtx op);

static void
abort_with_insn (rtx insn, const char * reason)
{
	error ("%s", reason);
	debug_rtx (insn);
	fancy_abort (__FILE__, __LINE__, __FUNCTION__);
}


/****************************************

****************************************/

#define SAVE_REGISTER_P(REGNO) \
	((df_regs_ever_live_p(REGNO) && !call_used_regs[REGNO]) \
	|| (REGNO == HARD_FRAME_POINTER_REGNUM && frame_pointer_needed) \
	|| (REGNO == RA_REGNUM && df_regs_ever_live_p(REGNO) && !current_function_is_leaf) \
	/*|| (REGNO == RA_REGNUM && current_opt_level==0)*/ )

static int compute_frame_size(int locals)
{
	int total_size;
	int regs;
	int rmask;
	int i;
	int rblocks;
	int first;
	int last;

	/* Calculate saved registers */

	regs = rmask = rblocks = 0;
	first = last = 0;

	for (i = RA_REGNUM; i <= LAST_SAVED_REGNUM; i++)
	{
		if (SAVE_REGISTER_P (i))
		{
			if (!first)
				first = i;

			last = i;

			regs += UNITS_PER_WORD;

			if ((rmask & (1 << (i-1))) == 0)
				rblocks++;

			rmask |= 1 << i;
		}
	}

	if (SHOULD_COMBINE_STORE_RESTORE)
		regs = UNITS_PER_WORD * (1 + last - first);

	/* Compute total size of stack frame */

	total_size = regs;
	total_size += locals;
	total_size += crtl->outgoing_args_size;
	total_size += crtl->args.pretend_args_size;
	frame_info.total_size = total_size;

	/* Store the size of the various parts of the stack frame */

	frame_info.rmask = rmask;
	frame_info.rblocks = rblocks;
	frame_info.regs = regs;
	frame_info.n_regs = regs / UNITS_PER_WORD;
	frame_info.first_reg = first;
	frame_info.last_reg = last;
	frame_info.outgoing = crtl->outgoing_args_size;

	/* FIXME: ??? */

	frame_info.outgoing += crtl->args.pretend_args_size;
	frame_info.locals = locals;
	frame_info.valid = reload_completed;

	return frame_info.total_size;
}

/***********************************
*  Test if a reg can be used as a
*		  base register
***********************************/

static int mapip_reg_ok_for_base_p(rtx x, int strict)
{
	if (GET_MODE (x) == QImode || GET_MODE (x) == HImode)
		return 0;

	return (strict
		? REGNO_OK_FOR_BASE_P (REGNO (x))
		: (REGNO (x) < FIRST_PSEUDO_REGISTER || REGNO (x) >= FIRST_PSEUDO_REGISTER));
}

/*
* We support only one addressing mode: register + immediate.
*/
#undef TARGET_LEGITIMATE_ADDRESS_P
static bool TARGET_LEGITIMATE_ADDRESS_P(enum machine_mode mode ATTRIBUTE_UNUSED, rtx x, bool strict)
{
	int valid;

	if (CONSTANT_ADDRESS_P (x))
		return 1;

	while (GET_CODE (x) == SUBREG)
		x = SUBREG_REG (x);

	if (GET_CODE (x) == REG && mapip_reg_ok_for_base_p (x, strict))
		return 1;

	valid = 0;
	if (GET_CODE (x) == PLUS)
	{
		rtx x0 = XEXP (x, 0);
		rtx x1 = XEXP (x, 1);

		enum rtx_code c0;
		enum rtx_code c1;

		while (GET_CODE (x0) == SUBREG)
			x0 = SUBREG_REG (x0);

		c0 = GET_CODE (x0);

		while (GET_CODE (x1) == SUBREG)
			x1 = SUBREG_REG (x1);

		c1 = GET_CODE (x1);

		if (c0 == REG
			&& mapip_reg_ok_for_base_p (x0, strict))
		{
			if (c1 == CONST_INT || CONSTANT_ADDRESS_P (x1))
				valid = 1;
		}

#if 0
		if (!valid)
		{
			fprintf (stderr, "INVALID(%d): ", reload_completed);
			debug_rtx (x);
		}
#endif
	}

  return valid;
}

#undef TARGET_FUNCTION_VALUE
static rtx TARGET_FUNCTION_VALUE (const_tree type,
	const_tree fn_decl_or_type ATTRIBUTE_UNUSED, bool outgoing ATTRIBUTE_UNUSED)
{
	enum machine_mode mode = TYPE_MODE(type);
	if(GET_MODE_SIZE(mode) < 4)
		mode = SImode;
	return gen_rtx_REG(mode, R0_REGNUM);
}

#undef TARGET_FUNCTION_VALUE_REGNO_P
static bool TARGET_FUNCTION_VALUE_REGNO_P (const unsigned int regno)
{
	return regno == R0_REGNUM;
}

#define FUNCTION_ARG_SIZE(MODE, TYPE) \
	((MODE) != BLKmode ? GET_MODE_SIZE (MODE) \
	: (unsigned) int_size_in_bytes (TYPE))

#undef TARGET_FUNCTION_ARG_ADVANCE
static void TARGET_FUNCTION_ARG_ADVANCE (CUMULATIVE_ARGS *ca ATTRIBUTE_UNUSED,
	enum machine_mode mode ATTRIBUTE_UNUSED,
	const_tree type ATTRIBUTE_UNUSED,
	bool named ATTRIBUTE_UNUSED)
{
	*ca += (3 + FUNCTION_ARG_SIZE (mode, type)) / 4;
}

static const int MAX_ARGS_IN_REGS = 4;

#undef TARGET_FUNCTION_ARG
static rtx TARGET_FUNCTION_ARG (CUMULATIVE_ARGS *ca,
	enum machine_mode mode,
	const_tree type ATTRIBUTE_UNUSED,
	bool named ATTRIBUTE_UNUSED)
{
	rtx x;
	int words;

	words = *ca;

	if (words > MAX_ARGS_IN_REGS || named == 0)
		return 0;

	if (type)
	{
		int unsignedp ATTRIBUTE_UNUSED;
		PROMOTE_MODE (mode, unsignedp, type);
	}

	switch (mode)
	{
	default:
	case BLKmode:
		words += ((int_size_in_bytes (type) + UNITS_PER_WORD - 1) / UNITS_PER_WORD);
		break;

	case DFmode:
	case DImode:
		words += 2;
		break;

	case VOIDmode:
	case SFmode:
	case QImode:
	case HImode:
	case SImode:
		words++;
		break;
	}

	if (words > MAX_ARGS_IN_REGS)
		return 0;

	x = gen_rtx_REG (mode, P0_REGNUM + *ca);

	if (x == 0)
		abort ();

	return x;
}

#undef TARGET_LIBCALL_VALUE
static rtx TARGET_LIBCALL_VALUE (enum machine_mode mode, const_rtx fun ATTRIBUTE_UNUSED)
{
	return gen_rtx_REG (mode, R0_REGNUM);
}

#undef TARGET_PRINT_OPERAND
/*
* warning: definition seems to have changed since 3.6.4. function may misbehave.
*/
static void TARGET_PRINT_OPERAND (FILE* file, rtx x, int letter)
{
	enum rtx_code code;

	if (!x)
	{
		error("PRINT_OPERAND null pointer");
		return;
	}

	code = GET_CODE (x);

	if(letter == 'Z') {
		if(code == CONST_INT && INTVAL (x) == 0) {
			/* output zero register in special cases where it is allowed. */
			fprintf(file, "zr");
			return;
		} else if (code != REG) {
			fprintf(stderr, "code: %s\n", GET_RTX_NAME(code));
			if(code == CONST_INT) {
				fprintf(stderr, "intval %li\n", INTVAL (x));
			}
			/* letter z is allowed for reg or const_int only. */
			abort_with_insn (x, "PRINT_OPERAND, invalid operand for %Z");
		}
	}

	if (code == SIGN_EXTEND)
		x = XEXP (x, 0), code = GET_CODE (x);

	if (letter == 'C' || letter == 'N')
	{
		switch (code)
		{
		case EQ: fputs ("eq", file); return;
		case NE: fputs ("ne", file); return;
		case GT: fputs ("gt", file); return;
		case GE: fputs ("ge", file); return;
		case LT: fputs ("lt", file); return;
		case LE: fputs ("le", file); return;
		case GTU: fputs ("gtu", file); return;
		case GEU: fputs ("geu", file); return;
		case LTU: fputs ("ltu", file); return;
		case LEU: fputs ("leu", file); return;
		default: abort_with_insn (x, "PRINT_OPERAND, invalid insn for %%C");
		}

		return;
	}

	if (code == REG)
	{
		int regno = REGNO(x);

		if (letter == 'D')					/* High part of double */
			regno++;

		fprintf (file, "%s", reg_names[regno]);
		return;
	}

	if (code == MEM)
	{
		output_address (XEXP (x, 0));
		return;
	}

	if (code == CONST_INT || code == CONST_DOUBLE)
	{
		int i;
	/* !! Added ARH 20-04-08 Fixed problem with immediate floats */

		if (GET_MODE(x) == SFmode)
		{
			REAL_VALUE_TYPE d;
			long l;
			union {
				REAL_VALUE_TYPE r;
				double d;
			} u;

			fprintf (file, "#");
			REAL_VALUE_FROM_CONST_DOUBLE (d, x);
			REAL_VALUE_TO_TARGET_SINGLE (d, l);
			fprintf (file, HOST_WIDE_INT_PRINT_HEX, l);
			u.r = d;
			fprintf(file, "\t\t// %.12g", u.d);
			return;
		}

	/* !! End */

		/* output zero register in special cases where it is allowed. */
		if(code == CONST_INT && letter == 'z' && INTVAL (x) == 0)
		{
			fprintf(file, "zr");
			return;
		}

		fprintf (file, "#");
		i = (int)INTVAL (x);
		if(i >= 0 || i < -256)
			fprintf (file, "0x%x", i);
		else
			fprintf(file, "%i", i);
		return;
	}

	fprintf (file, "&");
	output_addr_const (file, x);
}


#undef TARGET_PRINT_OPERAND_ADDRESS
static void TARGET_PRINT_OPERAND_ADDRESS(FILE *file, rtx addr)
{
	if (!addr)
	{
		error ("PRINT_OPERAND_ADDRESS, null pointer");
		return;
	}

	switch (GET_CODE (addr))
	{
	case REG:
		fputs (reg_names[REGNO (addr)], file);
		break;

	case PLUS:
	{
		rtx reg    = (rtx)0;
		rtx offset = (rtx)0;
		rtx arg0   = XEXP (addr, 0);
		rtx arg1   = XEXP (addr, 1);

		if (GET_CODE (arg0) == REG)
		{
			reg = arg0;
			offset = arg1;
			if (GET_CODE (offset) == REG)
				abort_with_insn (addr, "PRINT_OPERAND_ADDRESS, 2 regs");
		}
		else if (GET_CODE (arg1) == REG)
		{
			reg = arg1;
			offset = arg0;
		}
		else if (CONSTANT_P (arg0) && CONSTANT_P (arg1))
		{
			output_addr_const (file, addr);
			break;
		}
		else if (GET_CODE (arg0) == MEM)
		{
			print_mem_expr_old (file, arg0);
			offset = arg1;
		}
		else
			abort_with_insn (addr, "PRINT_OPERAND_ADDRESS, no regs");

		if (reg)
			fprintf (file, "%s,", reg_names[REGNO (reg)]);

		if (offset)
		{
			if (!CONSTANT_P (offset))
				abort_with_insn (addr, "PRINT_OPERAND_ADDRESS, invalid insn #2");
			output_addr_const (file, offset);
		}
	}
	break;

	case LABEL_REF:
	case SYMBOL_REF:
	case CONST_INT:
	case CONST:
		fprintf (file, "&");
		output_addr_const (file, addr);
		break;

	default:
		abort_with_insn (addr, "PRINT_OPERAND_ADDRESS, invalid insn #1");
	}
}

#undef TARGET_ASM_NAMED_SECTION
static void TARGET_ASM_NAMED_SECTION (const char *name, unsigned int flags, tree decl ATTRIBUTE_UNUSED)
{
	fprintf (asm_out_file, ".section %s,0x%x\n",
		name, flags);
}

#undef TARGET_ASM_INTEGER
static bool TARGET_ASM_INTEGER (rtx x, unsigned int size, int aligned_p)
{
	/* work around a bug in assemble_integer() */
	if (size == UNITS_PER_WORD)
	{
		fputs (".long ", asm_out_file);
		output_addr_const (asm_out_file, x);
		fputs ("\n", asm_out_file);
		return true;
	}

	return default_assemble_integer (x, size, aligned_p);
}

#undef TARGET_PROMOTE_FUNCTION_MODE
#define TARGET_PROMOTE_FUNCTION_MODE default_promote_function_mode_always_promote



/* Initialize the GCC target structure.  */

struct gcc_target targetm = TARGET_INITIALIZER;


void mapip2_asm_output_labelref(FILE* stream, const char* name)
{
	fputc('_', stream);
	fputs(name, stream);
}

void mapip2_asm_weaken_label(FILE* stream, const char* name)
{
	fputs(".weak ", stream);
	assemble_name(stream, name);
	fputc('\n', stream);
}

void mapip2_asm_output_weak_alias(FILE* stream, const char* name, const char* value)
{
	mapip2_asm_weaken_label(stream, name);
	fputs(".set ", stream);
	assemble_name(stream, name);
	fputc(',', stream);
	assemble_name(stream, value);
	fputc('\n', stream);
}


void mapip2_asm_generate_internal_label(char *buf, const char *prefix, int num)
{
	if (strcmp(prefix, "L") == 0)			/* instruction labels */
	{
		sprintf (buf, "*L%d", num);
		return;
	}

	if (strcmp(prefix, "LTHUNK") == 0)			/* Trap thunk labels */
	{
		sprintf (buf, "*%%%d", num);
		return;
	}

	sprintf (buf, "*%s%d", prefix, num);
}

void mapip2_asm_output_align(FILE* stream, int power)
{
	fprintf(stream, ".align %i\n", 1 << power);
}

void mapip2_asm_output_common(FILE* stream, const char* name, int size, int rounded)
{
	fprintf(stream, ".comm\t");
	assemble_name(stream, name);
	fprintf(stream, ", %d\t// size=%d\n", rounded, size);
}

void mapip2_asm_output_skip(FILE* stream, int nbytes)
{
	fprintf(stream, ".space\t%u\t//(ASM_OUTPUT_SKIP)\n", (nbytes));
}

void mapip2_asm_output_local(FILE* stream, const char* name, int size, int rounded)
{
	fprintf(stream, ".lcomm\t");
	assemble_name(stream, name);
	fprintf(stream, ", %d\t// size=%d\n", rounded, size);
}

void default_globalize_label(FILE* stream, const char* name)
{
	fputs(".global\t", stream);
	assemble_name(stream, name);
	putc('\n', stream);
}

static void print_mem_expr_old (FILE *file, rtx op)
{
	rtx arg0;
	arg0 = XEXP (op, 0);
	switch (GET_CODE (arg0))
	{
	case PLUS:
		TARGET_PRINT_OPERAND_ADDRESS (file, arg0);
		fprintf (file, "+");
		break;
	case REG:
		TARGET_PRINT_OPERAND_ADDRESS (file, arg0);
		fputc (',', file);
		break;
	case MEM:
		print_mem_expr_old (file, arg0);
		break;
	default:
		TARGET_PRINT_OPERAND (file, op, 0);
	}
}

void mapip2_asm_output_addr_diff_elt(FILE* stream, rtx body ATTRIBUTE_UNUSED, int value, int rel)
{
	fprintf(stream, ".long L%d-L%d\n", value, rel);
}

void mapip2_asm_output_addr_vec_elt(FILE* stream, int value)
{
	fprintf(stream, ".long L%d\n", value);
}


/* Emit RTL for a function prologue */
void mapip2_expand_prologue(void)
{
	long framesize;
	long adjust;
	rtx sp = NULL_RTX;
	rtx fp = NULL_RTX;

	framesize = (frame_info.valid
		? frame_info.total_size
		: compute_frame_size (get_frame_size ()));

	if (framesize > 0 || frame_pointer_needed)
		sp = gen_rtx_REG (SImode, SP_REGNUM);

	if (frame_pointer_needed)
		fp = gen_rtx_REG (SImode, FP_REGNUM);

	/* Save the necessary registers */
	if (SHOULD_COMBINE_STORE_RESTORE)
	{
		emit_insn (gen_store_regs (gen_rtx_REG (SImode, frame_info.first_reg),
			gen_rtx_REG (SImode, frame_info.last_reg)));
	}
	else
	{
		int rmask = frame_info.rmask;
		int i,j;

		for (i = 0; rmask != 0; )
		{
			if (rmask & 1)
			{
				for (j = i; rmask & 1; j++)
					rmask >>= 1;

				emit_insn (gen_store_regs (gen_rtx_REG (SImode, i),
					gen_rtx_REG (SImode, j-1)));
				i = j;
			}
			else
			{
				rmask >>= 1;
				i++;
			}
		}
	}

	if (frame_pointer_needed)
	{
#if 0
		/* Adjust frame pointer to point to arguments */
		if (framesize > 0)
		{
			emit_move_insn (fp, sp);
			emit_insn (gen_addsi3 (fp, fp, GEN_INT (framesize)));
		}
		else
#endif
		{
			emit_move_insn (fp, sp);
		}
	}

	adjust = framesize - frame_info.regs;
	if (adjust != 0)
	{
		/* Adjust $sp to make room for locals */
		emit_insn (gen_subsi3 (sp, sp, GEN_INT (adjust)));
	}
}


static int simple_return(void)
{
	int i;

	if (frame_pointer_needed)
		return 0;
	else
	{
		i = (frame_info.valid
			? frame_info.total_size
			: compute_frame_size (get_frame_size ()));

		return (i == 0);
	}
}

void mapip2_expand_epilogue(void)
{
	int i, j;
	long framesize;
	long adjust;
	int rblocks = frame_info.rblocks;
	int rmask = frame_info.rmask;

	framesize = (frame_info.valid ? frame_info.total_size: compute_frame_size (get_frame_size ()));

	if (simple_return ())
	{
		emit_jump_insn (gen_return());
		frame_info.valid = 0;
		return;
	}

	adjust = framesize - frame_info.regs;

	if (adjust != 0)
	{
		rtx sp = gen_rtx_REG (SImode, SP_REGNUM);
		emit_insn (gen_addsi3 (sp, sp, GEN_INT (adjust)));
	}

	/* Restore pushed registers in reverse order */

	if (frame_info.regs == 0)
	{
		emit_jump_insn (gen_return());
		frame_info.valid = 0;
		return;
	}

	/* Must be a frame */

	if (SHOULD_COMBINE_STORE_RESTORE)
	{
		emit_insn (gen_restore_regs (gen_rtx_REG (SImode, frame_info.first_reg),
			gen_rtx_REG (SImode, frame_info.last_reg)));

		emit_jump_insn (gen_return());

		frame_info.valid = 0;
		return;
	}

	for (i = LAST_SAVED_REGNUM; i >= 0; )
	{
		j = i;

		while (rmask & (1 << j))
			j--;

		if (j < i)
		{
			if (--rblocks == 0)
			{
				emit_insn (gen_restore_regs (gen_rtx_REG (SImode, frame_info.first_reg),
					gen_rtx_REG (SImode, frame_info.last_reg)));

				emit_jump_insn (gen_return());

				frame_info.valid = 0;
				return;
			}
			else
			{
				emit_insn (gen_restore_regs (gen_rtx_REG (SImode, j+1),
					gen_rtx_REG (SImode, i)));
			}

			i = j;
		}
		else
			i--;
	}

	frame_info.valid = 0;
}
