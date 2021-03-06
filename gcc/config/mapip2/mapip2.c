#include "../newlib-stdint.h"
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
#include "tree-pass.h"
#include "function.h"
#include "expr.h"
#include "flags.h"
#include "reload.h"
#include "output.h"
#include "tm_p.h"
#include "ggc.h"
#include "diagnostic-core.h"
#include "df.h"

#define TARGET_ASM_NAMED_SECTION default_elf_asm_named_section
#define TARGET_LIBCALL_VALUE mapip2_target_libcall_value
#define TARGET_LEGITIMATE_ADDRESS_P mapip2_target_legitimate_address_p
#define TARGET_PROMOTE_PROTOTYPES hook_bool_const_tree_true
#define TARGET_ASM_FUNCTION_EPILOGUE mapip2_target_asm_function_epilogue
#define TARGET_ASM_FINAL_POSTSCAN_INSN mapip2_target_asm_final_postscan_insn
#define TARGET_PROMOTE_FUNCTION_MODE default_promote_function_mode_always_promote
#define TARGET_PRINT_OPERAND_ADDRESS mapip2_target_print_operand_address
#define TARGET_FUNCTION_ARG mapip2_target_function_arg
#define TARGET_FUNCTION_VALUE mapip2_target_function_value
#define TARGET_FUNCTION_VALUE_REGNO_P mapip2_target_function_value_regno_p
#define TARGET_FUNCTION_ARG_ADVANCE mapip2_target_function_arg_advance
#define TARGET_PRINT_OPERAND mapip2_target_print_operand
#define TARGET_ASM_INTEGER mapip2_target_asm_integer
#define TARGET_ASM_FUNCTION_END_PROLOGUE mapip2_target_asm_function_end_prologue

#include "target.h"
#include "target-def.h"

#define SHOULD_COMBINE_STORE_RESTORE 1

#define DEBUG_PROLOGUE 0

typedef struct save_info {
  int rmask;		/* mask of registers saved */
  int rblocks;		/* n of register blocks to store/restore */
  int first_reg; /* first saved register */
  int last_reg;  /* last saved register */
  int n_regs;		/* number of saved regs */
  int regs;		/* size of saved registers area, in bytes */
} save_info;

static struct mapip_frame_info {
  int valid;		/* 0 values are not valid */

	save_info isave, fsave;

  int regs;		/* size of saved registers area, in bytes */
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

static void compute_save_data(save_info* save, int rStart, int rEnd, int bytes_per_reg) {
	int rmask;
	int i;
	int rblocks;
	int first;
	int last;
	int regs;
	/* Calculate saved registers */

	regs = rmask = rblocks = 0;
	first = last = 0;

	for (i = rStart; i <= rEnd; i++)
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

	if (SHOULD_COMBINE_STORE_RESTORE && last && first)
		regs = bytes_per_reg * (1 + last - first);

	save->rmask = rmask;
	save->rblocks = rblocks;
	save->n_regs = regs / bytes_per_reg;
	save->first_reg = first;
	save->last_reg = last;
	save->regs = regs;
}

static int compute_frame_size(int locals)
{
	int total_size;
	int regs;

	compute_save_data(&frame_info.isave, RA_REGNUM, LAST_SAVED_REGNUM, UNITS_PER_WORD);
	compute_save_data(&frame_info.fsave, FR0_REGNUM, FR7_REGNUM, UNITS_PER_WORD * 2);

	regs = frame_info.isave.regs + frame_info.fsave.regs;

	/* Compute total size of stack frame */

	total_size = regs;
	total_size += locals;
	total_size += crtl->outgoing_args_size;
	total_size += crtl->args.pretend_args_size;
	frame_info.total_size = total_size;

	/* Store the size of the various parts of the stack frame */

	frame_info.outgoing = crtl->outgoing_args_size;
	frame_info.regs = regs;

	/* FIXME: ??? */

	frame_info.outgoing += crtl->args.pretend_args_size;
	frame_info.locals = locals;
	frame_info.valid = reload_completed;

	return frame_info.total_size;
}

int mapip2_regno_ok_for_base_p(unsigned regnum)
{
	return INT_REGNO_P(regnum) && regnum < FIRST_PSEUDO_REGISTER;
}

enum reg_class mapip2_regno_reg_class(unsigned regnum)
{
	if(regnum < FIRST_FLOAT_REGISTER)
		return GENERAL_REGS;
	if(regnum < FIRST_FAKE_REGISTER)
		return FLOAT_REGS;
	if(regnum < FIRST_PSEUDO_REGISTER)
		return GENERAL_REGS;
	return NO_REGS;
}

int mapip2_function_arg_regno_p(int regno)
{
	if(regno >= P0_REGNUM && regno <= P3_REGNUM)
		return 1;
	if(regno >= FR8_REGNUM && regno <= FR15_REGNUM)
		return 1;
	return 0;
}

int mapip2_hard_regno_mode_ok(int regno, int mode)
{
	int c = GET_MODE_CLASS(mode);
	if(FLOAT_REGNO_P(regno))
		return c == MODE_FLOAT || c == MODE_COMPLEX_FLOAT;
	/* can't store a multi-word value in the last word register. */
	else if(GET_MODE_SIZE(mode) > UNITS_PER_WORD && regno == 31)
		return 0;
	else
		return 1;
}

int mapip2_modes_tieable_p(int mode1, int mode2)
{
	/* make single and double floats tiable? */
	/* no, because a floats in int registers have different sizes. */
	return (GET_MODE_CLASS(mode1) == GET_MODE_CLASS(mode2) ||
		GET_MODE_SIZE(mode1) == GET_MODE_SIZE(mode2));
}

static int floatNRegs(int mode) {
	switch(mode) {
	case SFmode:
	case DFmode:
		return 1;
	case SCmode:
	case DCmode:
		return 2;
	default:
#if 0
		printf("Unhandled mode: %s\n", GET_MODE_NAME(mode));
		gcc_assert(false);
#else
		/* gcc actually calls HARD_REGNO_NREGS on every register with every mode, just to check. */
		return 1;
#endif
	}
}

int mapip2_hard_regno_nregs(int regno, int mode)
{
#if 0
	printf("mapip2_hard_regno_nregs(%s, %smode)\n", reg_names[regno], GET_MODE_NAME(mode));
#endif
	/* float */
	if(regno >= FR0_REGNUM && regno <= FR15_REGNUM)
		return floatNRegs(mode);
	/* integer */
	return ((GET_MODE_SIZE(mode) + UNITS_PER_WORD - 1) / UNITS_PER_WORD);
}

int mapip2_class_max_nregs(int CLASS, int MODE)
{
#if 0
	printf("mapip2_class_max_nregs(%s, %smode)\n", reg_class_names[CLASS], GET_MODE_NAME(MODE));
#endif
	/* float */
	if(CLASS == FLOAT_REGS)
		return floatNRegs(MODE);
	else	/* integer */
		return ((GET_MODE_SIZE(MODE) + UNITS_PER_WORD - 1) / UNITS_PER_WORD);
}


/***********************************
*  Test if a reg can be used as a
*		  base register
***********************************/

static int mapip2_reg_ok_for_base_p(rtx x, int strict)
{
	if (GET_MODE (x) == QImode || GET_MODE (x) == HImode)
		return 0;

	return (strict ?
		REGNO_OK_FOR_BASE_P (REGNO (x)) :
		INT_REGNO_P(REGNO(x)));
}

/*
* We support only one addressing mode: register + immediate.
*/
static bool mapip2_target_legitimate_address_p(enum machine_mode mode ATTRIBUTE_UNUSED, rtx x, bool strict)
{
	int valid;

	if (CONSTANT_ADDRESS_P (x))
		return 1;

	while (GET_CODE (x) == SUBREG)
		x = SUBREG_REG (x);

	if (GET_CODE (x) == REG && mapip2_reg_ok_for_base_p (x, strict))
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
			&& mapip2_reg_ok_for_base_p (x0, strict))
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

static rtx mapip2_target_function_value (const_tree type,
	const_tree fn_decl_or_type ATTRIBUTE_UNUSED, bool outgoing ATTRIBUTE_UNUSED)
{
	enum machine_mode mode = TYPE_MODE(type);
	if(FLOAT_MODE_P(mode))
		return gen_rtx_REG(mode, FR8_REGNUM);
	if(GET_MODE_SIZE(mode) < 4)
		mode = SImode;
	return gen_rtx_REG(mode, R0_REGNUM);
}

static bool mapip2_target_function_value_regno_p (const unsigned int regno)
{
	return regno == R0_REGNUM || regno == FR8_REGNUM;
}

#define FUNCTION_ARG_SIZE(MODE, TYPE) \
	((MODE) != BLKmode ? GET_MODE_SIZE (MODE) \
	: (unsigned) int_size_in_bytes (TYPE))

void mapip2_init_cumulative_args(CUMULATIVE_ARGS* c)
{
	c->i = c->f = c->s = 0;
}

static const unsigned MAX_INT_ARGS_IN_REGS = 4;
static const unsigned MAX_FLOAT_ARGS_IN_REGS = 8;

static void mapip2_target_function_arg_advance (CUMULATIVE_ARGS *ca,
	enum machine_mode mode,
	const_tree type,
	bool named)
{
	unsigned words = (3 + FUNCTION_ARG_SIZE (mode, type)) / 4;
	if(!named)
	{
		ca->s += words;
		return;
	}

	if(mode == DFmode || mode == SFmode)
	{
		if (ca->f < MAX_FLOAT_ARGS_IN_REGS)
		{
			ca->f++;
			return;
		}
	}
	else if(mode == DCmode || mode == SCmode)
	{
		if (ca->f < MAX_FLOAT_ARGS_IN_REGS - 1)
		{
			ca->f += 2;
			return;
		}
	} else {
		if(ca->i + words <= MAX_INT_ARGS_IN_REGS)
		{
			ca->i += words;
			return;
		}
	}
	ca->s += words;
}

static rtx mapip2_target_function_arg (CUMULATIVE_ARGS *ca,
	enum machine_mode mode,
	const_tree type,
	bool named)
{
	rtx x;
	unsigned words;

#if 0
	printf("TARGET_FUNCTION_ARG(%smode, named %i)\n", GET_MODE_NAME(mode), named);
#endif

	/* if we encounter an unnamed parameter, it is actually (gcc bug)
	 * the last named parameter before an ellipsis (...)
	 * so we want that to be passed in a register.
	 * testcase: void func(int,int,int,int, ...); */
	if(named == 0 && ca->s != 0)
		return 0;

#if 0
	if (type)
	{
		int unsignedp ATTRIBUTE_UNUSED;
		PROMOTE_MODE (mode, unsignedp, type);
	}
#endif

	if(mode == DFmode || mode == SFmode)
	{
		if (ca->f >= MAX_FLOAT_ARGS_IN_REGS)
			return 0;

		x = gen_rtx_REG (mode, FR8_REGNUM + ca->f);
	}
	else if(mode == DCmode || mode == SCmode)
	{
		if (ca->f >= MAX_FLOAT_ARGS_IN_REGS - 1)
			return 0;

		x = gen_rtx_REG (mode, FR8_REGNUM + ca->f);
	}
	else
	{
		words = ca->i;

		if (words >= MAX_INT_ARGS_IN_REGS)
			return 0;

		switch (mode)
		{
		default:
		case BLKmode:
			words += ((int_size_in_bytes (type) + UNITS_PER_WORD - 1) / UNITS_PER_WORD);
			break;

		case DImode:
			words += 2;
			break;

		case VOIDmode:
		case QImode:
		case HImode:
		case SImode:
			words++;
			break;
		}

		if (words > MAX_INT_ARGS_IN_REGS)
			return 0;

		x = gen_rtx_REG (mode, P0_REGNUM + ca->i);
	}

	if (x == 0)
		abort ();

	/* update ca, so the function's register usage count remains valid. */
	if(named == 0 && ca->s == 0)
		mapip2_target_function_arg_advance(ca, mode, type, 1);

	return x;
}

static rtx mapip2_target_libcall_value (enum machine_mode mode, const_rtx fun ATTRIBUTE_UNUSED)
{
	int regnum;
	if(FLOAT_MODE_P(mode))
		regnum = FR8_REGNUM;
	else
		regnum = R0_REGNUM;
#if 0
	printf("mapip2_target_libcall_value(%smode): %i\n", GET_MODE_NAME(mode), regnum);
#endif
	return gen_rtx_REG(mode, regnum);
}

/*
* warning: definition seems to have changed since 3.6.4. function may misbehave.
*/
static void mapip2_target_print_operand (FILE* file, rtx x, int letter)
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
			fputs("zr", file);
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
		fputs(reg_names[regno], file);
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
			union {
				long l;
				float f;
			} f;
			union {
				REAL_VALUE_TYPE r;
				double d;
			} d;

			fputc('#', file);
			REAL_VALUE_FROM_CONST_DOUBLE (d.r, x);
			REAL_VALUE_TO_TARGET_SINGLE (d.r, f.l);
			fprintf (file, "0x%x", (unsigned)f.l);
			fprintf(file, "\t\t// d(%.12g) f(%.6g)", d.d, f.f);
			return;
		}
		if (GET_MODE(x) == DFmode)
		{
			REAL_VALUE_TYPE r;
			long i[2];
			union {
				double d;
				HOST_WIDE_INT ll;
				int i[2];
			} d;

			fputc('#', file);
			REAL_VALUE_FROM_CONST_DOUBLE (r, x);
			REAL_VALUE_TO_TARGET_DOUBLE(r, i);
			fprintf (file, "0x%08x%08x", (unsigned)i[1], (unsigned)i[0]);
			d.i[0] = i[0];
			d.i[1] = i[1];
			fprintf(file, "\t\t// d(%.12g)", d.d);
			return;
		}

	/* !! End */

		/* output zero register in special cases where it is allowed. */
		if(code == CONST_INT && letter == 'z' && INTVAL (x) == 0)
		{
			fputs("zr", file);
			return;
		}

		/* unused */
#if 0
		if(code == CONST_INT && GET_MODE(x) == DImode)
#else
		/* movdi immediate */
		if(letter == 'D')
#endif
		{
			HOST_WIDE_INT hwi = INTVAL (x);
			fputc('#', file);
			fprintf (file, HOST_WIDE_INT_PRINT_HEX, hwi);
			fputs("\t\t// DImode", file);
			gcc_assert(sizeof(HOST_WIDE_INT) >= 8);
			return;
		}

		fputc('#', file);
		i = (int)INTVAL (x);
		if(i >= 0 || i < -256)
			fprintf (file, "0x%x", i);
		else
			fprintf(file, "%i", i);
		/* useless; always VOIDmode. */
		/*fprintf(file, "\t\t// %s", GET_MODE_NAME(GET_MODE(x)));*/
		return;
	}

	fputc('&', file);
	output_addr_const (file, x);
}


static void mapip2_target_print_operand_address(FILE *file, rtx addr)
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
			/*fprintf (file, "(%s)", GET_RTX_NAME(GET_CODE(offset)));*/
			if(reg && GET_CODE(offset) == CONST)
				fputc('&', file);
			output_addr_const (file, offset);
		}
	}
	break;

	case LABEL_REF:
	case SYMBOL_REF:
	case CONST:
		fputc('&', file);
	case CONST_INT:
		output_addr_const (file, addr);
		break;

	default:
		abort_with_insn (addr, "PRINT_OPERAND_ADDRESS, invalid insn #1");
	}
}

static bool mapip2_target_asm_integer (rtx x, unsigned int size, int aligned_p)
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

static void mapip2_target_asm_function_end_prologue (FILE *file)
{
	CUMULATIVE_ARGS* ca = &crtl->args.info;

	rtx x = crtl->return_rtx;
/*	const rtx func_rtx = current_function_decl; */

	register enum rtx_code code;
	const char *mtype = "void";

/*	c_dump_tree ( (void *)info, func_rtx); */
/*	dump_node (func_rtx, 1, file); */

	if (x)
	{
		code = GET_CODE (x);

		if (code == SIGN_EXTEND)
			x = XEXP (x, 0), code = GET_CODE (x);

		switch(GET_MODE(x))
		{
			case SFmode: mtype = "float"; break;
			case DFmode: mtype = "double"; break;
			case SImode: mtype = "int"; break;
			case DImode: mtype = "long"; break;
			case SCmode:
			case DCmode: mtype = "complexFloat"; break;
			default: mtype = GET_MODE_NAME(GET_MODE(x)); break;
		}
	}

	/* emit the function directive with the number of input params*/
#define N_MOSYNC 0xfa
	fprintf (file, "\t.stabs\t\"%s,%d,%d\",%d,0,0,0\n",
		mtype, ca->i, ca->f, N_MOSYNC);
}

#define DEBUG_POSTSCAN 0
#define STAB_USE_DOT 0

static void mapip2_target_asm_final_postscan_insn (FILE *file, rtx insn, rtx *opvec ATTRIBUTE_UNUSED, int noperands ATTRIBUTE_UNUSED)
{
	if(GET_CODE(insn) == CALL_INSN)
	{
		rtx usage = CALL_INSN_FUNCTION_USAGE(insn);
		int iregs = 0;
		int fregs = 0;
		const char* returnType;
#if !STAB_USE_DOT
		static int labelNum = 0;
		ASM_OUTPUT_DEBUG_LABEL(file, "LMOSYNC", ++labelNum);
#endif
		/*print_rtl(file, insn);*/
		fprintf(file, "\t.stabs\t\"");
		while(usage)
		{
			/* count parameter registers */
			rtx use, reg;
			int regno;
			enum machine_mode mode;
			/* assert that all the RTX:s have the correct code before trying to access them. */
#if DEBUG_POSTSCAN
			print_rtl(file, usage);
#endif
			gcc_assert(GET_CODE(usage) == EXPR_LIST);
#if DEBUG_POSTSCAN
			fprintf(file, "looks %s\n", (GET_CODE(usage) == EXPR_LIST) ? "good" : "bad");
#endif
			use = XEXP(usage, 0);
			gcc_assert(GET_CODE(use) == USE);
			reg = XEXP(use, 0);
#if 0
			gcc_assert(GET_CODE(reg) == REG);
#else
			if(GET_CODE(reg) != REG)
			{
				usage = XEXP(usage, 1);
				continue;
			}
#endif
#if DEBUG_POSTSCAN
			fprintf(file, "reg: ");
			print_rtl(file, reg);
#endif

			regno = REGNO(reg);
			mode = GET_MODE(reg);

#if DEBUG_POSTSCAN
			fprintf(file, "\nregno %i, mode %s\n", regno, GET_MODE_NAME(mode));
#endif

			switch(mode) {
			case TImode:
			case CDImode:
				regno += 2;
				/* intentional fallthrough */
			case DImode:
			case CSImode:
				regno++;
				/* intentional fallthrough */
			case SImode:
			case HImode:
			case QImode:
			case CQImode:
			case CHImode:
				if(!(regno >= P0_REGNUM && regno <= P3_REGNUM))
					printf("Bad register %i in %smode\n", regno, GET_MODE_NAME(mode));
				gcc_assert(regno >= P0_REGNUM && regno <= P3_REGNUM);
				iregs = MAX(iregs, (regno - P0_REGNUM) + 1);
				break;
			case SFmode:
			case DFmode:
				gcc_assert(regno >= FR8_REGNUM && regno <= FR15_REGNUM);
				fregs = MAX(fregs, (regno - FR8_REGNUM) + 1);
				break;
			case DCmode:
			case SCmode:
				if(!(regno >= FR8_REGNUM && regno < FR15_REGNUM))
					printf("Bad register %i in %smode\n", regno, GET_MODE_NAME(mode));
				gcc_assert(regno >= FR8_REGNUM && regno < FR15_REGNUM);
				fregs = MAX(fregs, (regno - FR8_REGNUM) + 2);
				break;
			default:
				printf("Unrecognized mode: %s\n", GET_MODE_NAME(mode));
				gcc_assert(false);
			}

			usage = XEXP(usage, 1);
#if DEBUG_POSTSCAN
			fprintf(file, "next: ");
			print_rtl(file, usage);
#endif
		}

		/* find the return type. */
		{
			rtx pattern = PATTERN(insn);
			gcc_assert(pattern);
#if DEBUG_POSTSCAN
			fprintf(file, "\npattern:\n");
			print_rtl(file, pattern);
#endif
			if(GET_CODE(pattern) == PARALLEL)
			{
				pattern = XVECEXP(pattern, 0, 0);
				gcc_assert(pattern);
#if DEBUG_POSTSCAN
				fprintf(file, "\ndeparl'd:\n");
				print_rtl(file, pattern);
#endif
			}
			if(GET_CODE(pattern) == SET)
			{
				rtx reg = XEXP(pattern, 0);
				gcc_assert(GET_CODE(reg) == REG);
				returnType = GET_MODE_NAME(GET_MODE(reg));
			}
			else
			{
				gcc_assert(GET_CODE(pattern) == CALL);
#if 0
				print_rtl(file, pattern);
				fprintf(file, "\n%s", (GET_CODE(pattern) == CALL) ? "" : "weird one\n");
#endif
				returnType = "VOID";
			}
		}

		fprintf(file, "%s,%i,%i", returnType, iregs, fregs);

#if STAB_USE_DOT
		fprintf(file, "\",%d,1,0,.\n",
			N_MOSYNC);
#else
		fprintf(file, "\",%d,1,0,LMOSYNC%i\n",
			N_MOSYNC, labelNum);
#endif
	}
}

static void mapip2_target_asm_function_epilogue (FILE *file ATTRIBUTE_UNUSED, HOST_WIDE_INT size ATTRIBUTE_UNUSED)
{
#if DEBUG_PROLOGUE
	fprintf(file, "// epilogue %li\n", size);
	printf("epilogue %li\n", size);
#endif
	/* because mapip2_expand_epilogue() doesn't always get called. */
	frame_info.valid = 0;
}


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

void mapip2_asm_declare_function_name(FILE* stream, const char* name, tree decl ATTRIBUTE_UNUSED)
{
	ASM_OUTPUT_TYPE_DIRECTIVE(stream, name, "function");
	ASM_OUTPUT_FUNCTION_LABEL(stream, name, decl);
}

void mapip2_asm_declare_function_size(FILE* stream, const char* name, tree decl ATTRIBUTE_UNUSED)
{
	if(!flag_inhibit_size_directive)
		ASM_OUTPUT_MEASURED_SIZE(stream, name);
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


/* Return the difference, in bytes, between FP and SP, as it would be after the prologue. */
int mapip2_initial_frame_pointer_offset(void)
{
	long framesize = (frame_info.valid ?
		frame_info.total_size :
		compute_frame_size(get_frame_size()));
	return framesize;
}

int mapip2_initial_elimination_offset(int from, int to)
{
	int framesize = (frame_info.valid ?
		frame_info.total_size :
		compute_frame_size (get_frame_size ()));

	if (from == ARG_POINTER_REGNUM)
	{
		if(to == STACK_POINTER_REGNUM)
			return framesize;
		if (to == HARD_FRAME_POINTER_REGNUM)
			return 0;
	}
	if (from == FRAME_POINTER_REGNUM)
	{
		if (to == STACK_POINTER_REGNUM)
			return frame_info.outgoing + frame_info.locals;
		if(to == HARD_FRAME_POINTER_REGNUM)
			return -frame_info.regs;
	}
	if (from == RETURN_ADDRESS_POINTER_REGNUM)
	{
		if(to == RA_REGNUM)
			return 0;
		if (to == HARD_FRAME_POINTER_REGNUM)
			return -UNITS_PER_WORD;
		if (to == STACK_POINTER_REGNUM)
			return framesize - UNITS_PER_WORD;
	}

	fprintf(stderr, "from %s, to %s\n", reg_names[from], reg_names[to]);
	fancy_abort (__FILE__, __LINE__, __FUNCTION__);
}


static void save_registers(const save_info* save)
{
	/* Save the necessary registers */
	if (SHOULD_COMBINE_STORE_RESTORE)
	{
#if DEBUG_PROLOGUE
		printf("push %i to %i\n", save->first_reg, save->last_reg);
#endif
		if(save->first_reg && save->last_reg)
			emit_insn (gen_store_regs (gen_rtx_REG (SImode, save->first_reg),
				gen_rtx_REG (SImode, save->last_reg)));
	}
	else
	{
		int rmask = save->rmask;
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
#if DEBUG_PROLOGUE
	printf("prologue: %s. local: %li, total %li\n", current_function_name(), get_frame_size(), framesize);
#endif

	if (framesize > 0 || frame_pointer_needed)
		sp = gen_rtx_REG (SImode, SP_REGNUM);

	if (frame_pointer_needed)
		fp = gen_rtx_REG (SImode, FP_REGNUM);

	save_registers(&frame_info.isave);
	save_registers(&frame_info.fsave);

	adjust = framesize - frame_info.regs;
	if (adjust != 0)
	{
		/* Adjust $sp to make room for locals */
		emit_insn (gen_subsi3 (sp, sp, GEN_INT (adjust)));
	}

	if (frame_pointer_needed)
	{
#if 1
		/* Adjust frame pointer to point to arguments. */
		/* FP should, after this, have the same value as SP had before the PUSH. */
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
}


int simple_return(void)
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

#define INVALIDATE_FRAME_INFO_IN_EPILOGUE /*frame_info.valid = 0*/

static void restore_registers(const save_info* save)
{
	int i, j;
	int rblocks = save->rblocks;
	int rmask = save->rmask;

#if DEBUG_PROLOGUE
	printf("pop %i to %i\n", save->first_reg, save->last_reg);
#endif

	if (SHOULD_COMBINE_STORE_RESTORE)
	{
		if(save->first_reg && save->last_reg)
			emit_insn (gen_restore_regs (gen_rtx_REG (SImode, save->first_reg),
				gen_rtx_REG (SImode, save->last_reg)));
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
				emit_insn (gen_restore_regs (gen_rtx_REG (SImode, save->first_reg),
					gen_rtx_REG (SImode, save->last_reg)));
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
}

void mapip2_expand_epilogue(void)
{
	long framesize;
	long adjust;

	framesize = (frame_info.valid ? frame_info.total_size: compute_frame_size (get_frame_size ()));

	if (simple_return ())
	{
		emit_jump_insn (gen_return_internal());
		INVALIDATE_FRAME_INFO_IN_EPILOGUE;
		return;
	}

	adjust = framesize - frame_info.regs;

	if (adjust != 0)
	{
		rtx sp = gen_rtx_REG (SImode, SP_REGNUM);
		emit_insn (gen_addsi3 (sp, sp, GEN_INT (adjust)));
	}

	if (frame_info.regs != 0)
	{
		/* Restore pushed registers in reverse order */
		restore_registers(&frame_info.fsave);
		restore_registers(&frame_info.isave);
	}

	emit_jump_insn (gen_return_internal());

	INVALIDATE_FRAME_INFO_IN_EPILOGUE;
}

void mapip2_indirect_jump(rtx reg ATTRIBUTE_UNUSED) {
	printf("mapip2_indirect_jump\n");
	gcc_assert(false);
}

void mapip2_tablejump(rtx reg ATTRIBUTE_UNUSED, rtx label ATTRIBUTE_UNUSED) {
	printf("mapip2_tablejump\n");
	gcc_assert(false);
}
