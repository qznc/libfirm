/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief    The main sparc backend driver file.
 * @author   Hannes Rapp, Matthias Braun
 */
#include "lc_opts.h"
#include "lc_opts_enum.h"

#include "irgwalk.h"
#include "irprog.h"
#include "ircons.h"
#include "irgmod.h"
#include "irgopt.h"
#include "iroptimize.h"
#include "irtools.h"
#include "irdump.h"
#include "iropt_t.h"
#include "lowering.h"
#include "lower_dw.h"
#include "lower_alloc.h"
#include "lower_builtins.h"
#include "lower_calls.h"
#include "lower_mode_b.h"
#include "lower_softfloat.h"
#include "debug.h"
#include "array_t.h"
#include "error.h"
#include "util.h"
#include "be_t.h"
#include "bearch.h"
#include "benode.h"
#include "belower.h"
#include "besched.h"
#include "bemodule.h"
#include "begnuas.h"
#include "belistsched.h"
#include "beflags.h"
#include "beutil.h"

#include "bearch_sparc_t.h"

#include "sparc_new_nodes.h"
#include "gen_sparc_regalloc_if.h"
#include "sparc_transform.h"
#include "sparc_emitter.h"
#include "sparc_cconv.h"

DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL;)

extern const arch_isa_if_t sparc_isa_if;
static sparc_isa_t sparc_isa_template = {
	{
		&sparc_isa_if,                       /* isa interface implementation */
		N_SPARC_REGISTERS,
		sparc_registers,
		N_SPARC_CLASSES,
		sparc_reg_classes,
		&sparc_registers[REG_SP],            /* stack pointer register */
		&sparc_registers[REG_FRAME_POINTER], /* base pointer register */
		3,                                   /* power of two stack alignment
		                                        for calls */
		7,                                   /* costs for a spill instruction */
		5,                                   /* costs for a reload instruction */
		true,                                /* custom abi handling */
	},
	NULL,                                  /* constants */
};

typedef enum {
	cpu_generic,
	cpu_v8plus,
	cpu_leon,
	cpu_supersparc,
	cpu_hypersparc,
} sparc_cpu_t;
static const lc_opt_enum_int_items_t cpu_items[] = {
	{ "generic",    cpu_generic    },
	{ "v8",         cpu_generic    },
	{ "v8plus",     cpu_v8plus     },
	{ "leon",       cpu_leon       },
	{ "supersparc", cpu_supersparc },
	{ "hypersparc", cpu_hypersparc },
	{ NULL,         0               },
};

static int cpu;
static lc_opt_enum_int_var_t cpu_var = {
	&cpu, cpu_items
};

sparc_codegen_config_t sparc_cg_config;

typedef enum {
	use_fpu_auto,
	use_fpu_yes,
	use_fpu_no
} sparc_use_fpu_t;
/* fpu set architectures. */
static const lc_opt_enum_int_items_t fpu_items[] = {
	{ "auto",      use_fpu_auto },
	{ "fpu",       use_fpu_yes  },
	{ "softfloat", use_fpu_no   },
	{ NULL,        0 }
};

static int fpu;
static lc_opt_enum_int_var_t arch_fpu_var = {
	&fpu, fpu_items
};

static int use_softfloat;

static const lc_opt_table_entry_t sparc_options[] = {
	LC_OPT_ENT_ENUM_INT("fpunit",     "select the floating point unit", &arch_fpu_var),
	LC_OPT_ENT_ENUM_INT("cpu",        "select architecture variant",    &cpu_var),
	LC_OPT_ENT_BOOL    ("soft-float", "equivalent to fpmath=softfloat", &use_softfloat),
	LC_OPT_LAST
};

static ir_entity *sparc_get_frame_entity(const ir_node *node)
{
	if (is_sparc_FrameAddr(node)) {
		const sparc_attr_t *attr = get_sparc_attr_const(node);
		return attr->immediate_value_entity;
	}

	if (sparc_has_load_store_attr(node)) {
		const sparc_load_store_attr_t *load_store_attr
			= get_sparc_load_store_attr_const(node);
		if (load_store_attr->is_frame_entity) {
			return load_store_attr->base.immediate_value_entity;
		}
	}

	return NULL;
}

/**
 * This function is called by the generic backend to correct offsets for
 * nodes accessing the stack.
 */
static void sparc_set_frame_offset(ir_node *node, int offset)
{
	sparc_attr_t *attr = get_sparc_attr(node);
	attr->immediate_value += offset;

	/* must be a FrameAddr or a load/store node with frame_entity */
	assert(is_sparc_FrameAddr(node) ||
			get_sparc_load_store_attr_const(node)->is_frame_entity);
}

static int sparc_get_sp_bias(const ir_node *node)
{
	if (is_sparc_Save(node)) {
		const sparc_attr_t *attr = get_sparc_attr_const(node);
		if (get_irn_arity(node) == 3)
			panic("no support for _reg variant yet");

		return -attr->immediate_value;
	} else if (is_sparc_RestoreZero(node)) {
		return SP_BIAS_RESET;
	}
	return 0;
}

/* fill register allocator interface */

const arch_irn_ops_t sparc_irn_ops = {
	sparc_get_frame_entity,
	sparc_set_frame_offset,
	sparc_get_sp_bias,
	NULL,    /* get_op_estimated_cost   */
	NULL,    /* possible_memory_operand */
	NULL,    /* perform_memory_operand  */
};

/**
 * Transforms the standard firm graph into
 * a SPARC firm graph
 */
static void sparc_prepare_graph(ir_graph *irg)
{
	sparc_transform_graph(irg);
}

static bool sparc_modifies_flags(const ir_node *node)
{
	be_foreach_out(node, o) {
		const arch_register_req_t *req = arch_get_irn_register_req_out(node, o);
		if (req->cls == &sparc_reg_classes[CLASS_sparc_flags_class])
			return true;
	}
	return false;
}

static bool sparc_modifies_fp_flags(const ir_node *node)
{
	be_foreach_out(node, o) {
		const arch_register_req_t *req = arch_get_irn_register_req_out(node, o);
		if (req->cls == &sparc_reg_classes[CLASS_sparc_fpflags_class])
			return true;
	}
	return false;
}

static void sparc_before_ra(ir_graph *irg)
{
	/* fixup flags register */
	be_sched_fix_flags(irg, &sparc_reg_classes[CLASS_sparc_flags_class],
	                   NULL, sparc_modifies_flags);
	be_sched_fix_flags(irg, &sparc_reg_classes[CLASS_sparc_fpflags_class],
	                   NULL, sparc_modifies_fp_flags);
}

/**
 * rewrite unsigned->float conversion.
 * Sparc has no instruction for this so instead we do the following:
 *
 *   int    signed_x = unsigned_value_x;
 *   double res      = signed_x;
 *   if (signed_x < 0)
 *       res += 4294967296. ;
 *   return (float) res;
 */
static void rewrite_unsigned_float_Conv(ir_node *node)
{
	ir_graph *irg         = get_irn_irg(node);
	dbg_info *dbgi        = get_irn_dbg_info(node);
	ir_node  *lower_block = get_nodes_block(node);

	part_block(node);

	ir_node   *block       = get_nodes_block(node);
	ir_node   *unsigned_x  = get_Conv_op(node);
	ir_mode   *mode_u      = get_irn_mode(unsigned_x);
	ir_mode   *mode_s      = find_signed_mode(mode_u);
	ir_mode   *mode_d      = mode_D;
	ir_node   *signed_x    = new_rd_Conv(dbgi, block, unsigned_x, mode_s);
	ir_node   *res         = new_rd_Conv(dbgi, block, signed_x, mode_d);
	ir_node   *zero        = new_r_Const(irg, get_mode_null(mode_s));
	ir_node   *cmp         = new_rd_Cmp(dbgi, block, signed_x, zero,
	                                    ir_relation_less);
	ir_node   *cond        = new_rd_Cond(dbgi, block, cmp);
	ir_node   *proj_true   = new_r_Proj(cond, mode_X, pn_Cond_true);
	ir_node   *proj_false  = new_r_Proj(cond, mode_X, pn_Cond_false);
	ir_node   *in_true[1]  = { proj_true };
	ir_node   *in_false[1] = { proj_false };
	ir_node   *true_block  = new_r_Block(irg, ARRAY_SIZE(in_true), in_true);
	ir_node   *false_block = new_r_Block(irg, ARRAY_SIZE(in_false),in_false);
	ir_node   *true_jmp    = new_r_Jmp(true_block);
	ir_node   *false_jmp   = new_r_Jmp(false_block);
	ir_tarval *correction  = new_tarval_from_double(4294967296., mode_d);
	ir_node   *c_const     = new_r_Const(irg, correction);
	ir_node   *fadd        = new_rd_Add(dbgi, true_block, res, c_const, mode_d);

	ir_node  *lower_in[2] = { true_jmp, false_jmp };
	ir_node  *phi_in[2]   = { fadd, res };
	ir_mode  *dest_mode   = get_irn_mode(node);

	set_irn_in(lower_block, ARRAY_SIZE(lower_in), lower_in);
	ir_node *phi = new_r_Phi(lower_block, ARRAY_SIZE(phi_in), phi_in, mode_d);
	assert(get_Block_phis(lower_block) == NULL);
	set_Block_phis(lower_block, phi);
	set_Phi_next(phi, NULL);

	ir_node *res_conv = new_rd_Conv(dbgi, lower_block, phi, dest_mode);
	exchange(node, res_conv);
}

/**
 * rewrite float->unsigned conversions.
 * Sparc has no instruction for this so instead we do the following:
 *
 * if (x >= 2147483648.) {
 *   converted ^= (int)(x-2147483648.) ^ 0x80000000;
 * } else {
 *   converted = (int)x;
 * }
 * return (unsigned)converted;
 */
static void rewrite_float_unsigned_Conv(ir_node *node)
{
	ir_graph *irg         = get_irn_irg(node);
	dbg_info *dbgi        = get_irn_dbg_info(node);
	ir_node  *lower_block = get_nodes_block(node);

	part_block(node);

	ir_node   *block       = get_nodes_block(node);
	ir_node   *float_x     = get_Conv_op(node);
	ir_mode   *mode_u      = get_irn_mode(node);
	ir_mode   *mode_s      = find_signed_mode(mode_u);
	ir_mode   *mode_f      = get_irn_mode(float_x);
	ir_tarval *limit       = new_tarval_from_double(2147483648., mode_f);
	ir_node   *limitc      = new_r_Const(irg, limit);
	ir_node   *cmp         = new_rd_Cmp(dbgi, block, float_x, limitc,
	                                    ir_relation_greater_equal);
	ir_node   *cond        = new_rd_Cond(dbgi, block, cmp);
	ir_node   *proj_true   = new_r_Proj(cond, mode_X, pn_Cond_true);
	ir_node   *proj_false  = new_r_Proj(cond, mode_X, pn_Cond_false);
	ir_node   *in_true[1]  = { proj_true };
	ir_node   *in_false[1] = { proj_false };
	ir_node   *true_block  = new_r_Block(irg, ARRAY_SIZE(in_true), in_true);
	ir_node   *false_block = new_r_Block(irg, ARRAY_SIZE(in_false),in_false);
	ir_node   *true_jmp    = new_r_Jmp(true_block);
	ir_node   *false_jmp   = new_r_Jmp(false_block);

	ir_tarval *correction  = new_tarval_from_long(0x80000000l, mode_s);
	ir_node   *c_const     = new_r_Const(irg, correction);
	ir_node   *sub         = new_rd_Sub(dbgi, true_block, float_x, limitc,
										mode_f);
	ir_node   *sub_conv    = new_rd_Conv(dbgi, true_block, sub, mode_s);
	ir_node   *xorn        = new_rd_Eor(dbgi, true_block, sub_conv, c_const,
										mode_s);

	ir_node   *converted   = new_rd_Conv(dbgi, false_block, float_x,mode_s);

	ir_node  *lower_in[2] = { true_jmp, false_jmp };
	ir_node  *phi_in[2]   = { xorn, converted };

	set_irn_in(lower_block, ARRAY_SIZE(lower_in), lower_in);
	ir_node *phi = new_r_Phi(lower_block, ARRAY_SIZE(phi_in), phi_in, mode_s);
	assert(get_Block_phis(lower_block) == NULL);
	set_Block_phis(lower_block, phi);
	set_Phi_next(phi, NULL);

	ir_node *res_conv = new_rd_Conv(dbgi, lower_block, phi, mode_u);
	exchange(node, res_conv);
}

static int sparc_rewrite_Conv(ir_node *node, void *ctx)
{
	(void) ctx;
	ir_mode *to_mode   = get_irn_mode(node);
	ir_node *op        = get_Conv_op(node);
	ir_mode *from_mode = get_irn_mode(op);

	if (mode_is_float(to_mode) && mode_is_int(from_mode)
	    && get_mode_size_bits(from_mode) == 32
	    && !mode_is_signed(from_mode)) {
		rewrite_unsigned_float_Conv(node);
		return 1;
	}
	if (mode_is_float(from_mode) && mode_is_int(to_mode)
	    && get_mode_size_bits(to_mode) <= 32
	    && !mode_is_signed(to_mode)) {
	    rewrite_float_unsigned_Conv(node);
	    return 1;
	}

	return 0;
}

static void sparc_handle_intrinsics(void)
{
	i_record records[3];
	size_t n_records = 0;

	/* we need to rewrite some forms of int->float conversions */
	i_instr_record *map_Conv = &records[n_records++].i_instr;
	map_Conv->kind     = INTRINSIC_INSTR;
	map_Conv->op       = op_Conv;
	map_Conv->i_mapper = sparc_rewrite_Conv;

	/* SPARC has no signed mod instruction ... */
	ir_type *int_tp = new_type_primitive(mode_Is);
	ir_type *mod_tp = new_type_method(2, 1);
	set_method_param_type(mod_tp, 0, int_tp);
	set_method_param_type(mod_tp, 1, int_tp);
	set_method_res_type(mod_tp, 0, int_tp);

	runtime_rt rt_iMod;
	ident *mod_id = new_id_from_str(".rem");
	rt_iMod.ent             = new_entity(get_glob_type(), mod_id, mod_tp);
	set_entity_ld_ident(rt_iMod.ent, mod_id);
	rt_iMod.mode            = mode_T;
	rt_iMod.res_mode        = mode_Is;
	rt_iMod.mem_proj_nr     = pn_Mod_M;
	rt_iMod.regular_proj_nr = pn_Mod_X_regular;
	rt_iMod.exc_proj_nr     = pn_Mod_X_except;
	rt_iMod.res_proj_nr     = pn_Mod_res;
	set_entity_visibility(rt_iMod.ent, ir_visibility_external);

	i_instr_record *map_imod = &records[n_records++].i_instr;
	map_imod->kind     = INTRINSIC_INSTR;
	map_imod->op       = op_Mod;
	map_imod->i_mapper = (i_mapper_func)i_mapper_RuntimeCall;
	map_imod->ctx      = &rt_iMod;

	/* ... nor an unsigned mod. */
	ir_type *umod_tp = new_type_method(2, 1);
	ir_type *uint_tp = new_type_primitive(mode_Iu);
	set_method_param_type(umod_tp, 0, uint_tp);
	set_method_param_type(umod_tp, 1, uint_tp);
	set_method_res_type(umod_tp, 0, uint_tp);

	runtime_rt rt_uMod;
	ident *umod_id = new_id_from_str(".urem");
	rt_uMod.ent             = new_entity(get_glob_type(), umod_id, umod_tp);
	set_entity_ld_ident(rt_uMod.ent, umod_id);
	rt_uMod.mode            = mode_T;
	rt_uMod.res_mode        = mode_Iu;
	rt_uMod.mem_proj_nr     = pn_Mod_M;
	rt_uMod.regular_proj_nr = pn_Mod_X_regular;
	rt_uMod.exc_proj_nr     = pn_Mod_X_except;
	rt_uMod.res_proj_nr     = pn_Mod_res;
	set_entity_visibility(rt_uMod.ent, ir_visibility_external);

	i_instr_record *map_umod = &records[n_records++].i_instr;
	map_umod->kind     = INTRINSIC_INSTR;
	map_umod->op       = op_Mod;
	map_umod->i_mapper = (i_mapper_func)i_mapper_RuntimeCall;
	map_umod->ctx      = &rt_uMod;

	assert(n_records <= ARRAY_SIZE(records));
	lower_intrinsics(records, n_records, /*part_block_used=*/ true);
}

static void sparc_setup_cg_config(void)
{
	memset(&sparc_cg_config, 0, sizeof(sparc_cg_config));
	bool has_fpu = false;
	switch ((sparc_cpu_t)cpu) {
	case cpu_v8plus:
		sparc_cg_config.use_cas = true;
		has_fpu = false;
		break;
	case cpu_leon:
		sparc_cg_config.use_cas = true;
		has_fpu = true;
		break;
	case cpu_hypersparc:
		has_fpu = true;
		break;
	case cpu_supersparc:
	case cpu_generic:
		has_fpu = false;
		break;
	}

	if (use_softfloat)
		fpu = use_fpu_no;

	if (fpu == use_fpu_auto) {
		sparc_cg_config.use_fpu = has_fpu;
	} else {
		sparc_cg_config.use_fpu = fpu == use_fpu_yes;
	}
}

static void sparc_init(void)
{
	sparc_init_asm_constraints();
	sparc_register_init();
	sparc_create_opcodes(&sparc_irn_ops);
	sparc_cconv_init();
	sparc_setup_cg_config();
}

static void sparc_finish(void)
{
	sparc_free_opcodes();
}

static arch_env_t *sparc_begin_codegeneration(void)
{
	sparc_isa_t *isa = XMALLOC(sparc_isa_t);
	*isa = sparc_isa_template;
	isa->constants = pmap_create();

	be_gas_elf_type_char = '#';
	be_gas_elf_variant   = ELF_VARIANT_SPARC;

	return &isa->base;
}

/**
 * Closes the output file and frees the ISA structure.
 */
static void sparc_end_codegeneration(void *self)
{
	sparc_isa_t *isa = (sparc_isa_t*)self;
	pmap_destroy(isa->constants);
	free(isa);
}

static void sparc_lower_for_target(void)
{
	lower_calls_with_compounds(LF_RETURN_HIDDEN);

	for (size_t i = 0, n_irgs = get_irp_n_irgs(); i < n_irgs; ++i) {
		ir_graph *irg = get_irp_irg(i);
		/* Turn all small CopyBs into loads/stores and all bigger CopyBs into
		 * memcpy calls. */
		lower_CopyB(irg, 31, 32, false);
	}

	if (!sparc_cg_config.use_fpu)
		lower_floating_point();

	ir_builtin_kind supported[8];
	size_t          s = 0;
	supported[s++] = ir_bk_saturating_increment;
	if (sparc_cg_config.use_cas)
		supported[s++] = ir_bk_compare_swap;
	assert(s < ARRAY_SIZE(supported));
	lower_builtins(s, supported);

	ir_mode *mode_gp = sparc_reg_classes[CLASS_sparc_gp].mode;
	for (size_t i = 0, n_irgs = get_irp_n_irgs(); i < n_irgs; ++i) {
		ir_graph *irg = get_irp_irg(i);
		lower_switch(irg, 4, 256, mode_gp);
	}

	sparc_lower_64bit();

	for (size_t i = 0, n_irgs = get_irp_n_irgs(); i < n_irgs; ++i) {
		ir_graph *irg = get_irp_irg(i);
		ir_lower_mode_b(irg, mode_Iu);
		/* TODO: Pass SPARC_MIN_STACKSIZE as addr_delta as soon as
		 * Alloc nodes are implemented more efficiently. */
		lower_alloc(irg, SPARC_STACK_ALIGNMENT, true, 0);
	}
}

static int sparc_is_mux_allowed(ir_node *sel, ir_node *mux_false,
                                ir_node *mux_true)
{
	return ir_is_optimizable_mux(sel, mux_false, mux_true);
}

/**
 * Returns the libFirm configuration parameter for this backend.
 */
static const backend_params *sparc_get_backend_params(void)
{
	static const ir_settings_arch_dep_t arch_dep = {
		1,     /* also_use_subs */
		1,     /* maximum_shifts */
		31,    /* highest_shift_amount */
		NULL,  /* evaluate_cost_func */
		1,     /* allow mulhs */
		1,     /* allow mulhu */
		32,    /* max_bits_for_mulh */
	};
	static backend_params p = {
		0,     /* no support for RotL nodes */
		1,     /* big endian */
		1,     /* modulo shift efficient */
		0,     /* non-modulo shift not efficient */
		&arch_dep,              /* will be set later */
		sparc_is_mux_allowed,   /* parameter for if conversion */
		32,    /* machine size */
		NULL,  /* float arithmetic mode */
		NULL,  /* long long type */
		NULL,  /* usigned long long type */
		NULL,  /* long double type */
		0,     /* no trampoline support: size 0 */
		0,     /* no trampoline support: align 0 */
		NULL,  /* no trampoline support: no trampoline builder */
		4      /* alignment of stack parameter: typically 4 (32bit) or 8 (64bit) */
	};

	ir_mode *mode_long_long
		= new_int_mode("long long", irma_twos_complement, 64, 1, 64);
	ir_type *type_long_long = new_type_primitive(mode_long_long);
	ir_mode *mode_unsigned_long_long
		= new_int_mode("unsigned long long", irma_twos_complement, 64, 0, 64);
	ir_type *type_unsigned_long_long
		= new_type_primitive(mode_unsigned_long_long);

	p.type_long_long          = type_long_long;
	p.type_unsigned_long_long = type_unsigned_long_long;

	ir_type *type_long_double = new_type_primitive(mode_Q);

	set_type_alignment_bytes(type_long_double, 8);
	set_type_size_bytes(type_long_double, 16);
	p.type_long_double = type_long_double;
	return &p;
}

static ir_node *sparc_new_spill(ir_node *value, ir_node *after)
{
	ir_node  *block = get_block(after);
	ir_graph *irg   = get_irn_irg(value);
	ir_node  *frame = get_irg_frame(irg);
	ir_node  *mem   = get_irg_no_mem(irg);
	ir_mode  *mode  = get_irn_mode(value);

	ir_node  *store;
	if (mode_is_float(mode)) {
		store = create_stf(NULL, block, value, frame, mem, mode, NULL, 0, true);
	} else {
		store = new_bd_sparc_St_imm(NULL, block, value, frame, mem, mode, NULL,
		                            0, true);
	}
	sched_add_after(after, store);
	return store;
}

static ir_node *sparc_new_reload(ir_node *value, ir_node *spill,
                                 ir_node *before)
{
	ir_node  *block = get_block(before);
	ir_graph *irg   = get_irn_irg(value);
	ir_node  *frame = get_irg_frame(irg);
	ir_mode  *mode  = get_irn_mode(value);

	ir_node  *load;
	if (mode_is_float(mode)) {
		load = create_ldf(NULL, block, frame, spill, mode, NULL, 0, true);
	} else {
		load = new_bd_sparc_Ld_imm(NULL, block, frame, spill, mode, NULL, 0,
		                           true);
	}
	sched_add_before(before, load);
	assert((long)pn_sparc_Ld_res == (long)pn_sparc_Ldf_res);
	ir_node *res = new_r_Proj(load, mode, pn_sparc_Ld_res);

	return res;
}

const arch_isa_if_t sparc_isa_if = {
	sparc_init,
	sparc_finish,
	sparc_get_backend_params,
	sparc_lower_for_target,
	sparc_is_valid_clobber,

	sparc_begin_codegeneration,
	sparc_end_codegeneration,
	NULL,
	NULL,                /* get call abi */
	NULL,                /* mark remat */
	NULL,                /* get_pic_base */
	sparc_new_spill,
	sparc_new_reload,
	NULL,                /* register_saved_by */

	sparc_handle_intrinsics,
	NULL,                /* before_abi */
	sparc_prepare_graph,
	sparc_before_ra,
	sparc_finish_graph,
	sparc_emit_routine,
};

BE_REGISTER_MODULE_CONSTRUCTOR(be_init_arch_sparc)
void be_init_arch_sparc(void)
{
	lc_opt_entry_t *be_grp = lc_opt_get_grp(firm_opt_get_root(), "be");
	lc_opt_entry_t *sparc_grp = lc_opt_get_grp(be_grp, "sparc");

	lc_opt_add_table(sparc_grp, sparc_options);

	be_register_isa_if("sparc", &sparc_isa_if);
	FIRM_DBG_REGISTER(dbg, "firm.be.sparc.cg");
	sparc_init_transform();
	sparc_init_emitter();
}
