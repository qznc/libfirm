/*
 * Copyright (C) 1995-2008 University of Karlsruhe.  All right reserved.
 *
 * This file is part of libFirm.
 *
 * This file may be distributed and/or modified under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation and appearing in the file LICENSE.GPL included in the
 * packaging of this file.
 *
 * Licensees holding valid libFirm Professional Edition licenses may use
 * this file in accordance with the libFirm Commercial License.
 * Agreement provided with the Software.
 *
 * This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
 * WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE.
 */

/**
 * @file
 * @brief   Various irnode constructors. Automatic construction of SSA
 *          representation.
 * @author  Martin Trapp, Christian Schaefer, Goetz Lindenmaier, Boris Boesler
            Michael Beck
 * @version $Id$
 */
#include "config.h"

#include "irprog_t.h"
#include "irgraph_t.h"
#include "irnode_t.h"
#include "irmode_t.h"
#include "ircons_t.h"
#include "firm_common_t.h"
#include "irvrfy.h"
#include "irop_t.h"
#include "iropt_t.h"
#include "irgmod.h"
#include "irhooks.h"
#include "array_t.h"
#include "irbackedge_t.h"
#include "irflag_t.h"
#include "iredges_t.h"
#include "irflag_t.h"

/* Uncomment to use original code instead of generated one */
// #define USE_ORIGINAL

/* when we need verifying */
#ifdef NDEBUG
# define IRN_VRFY_IRG(res, irg)
#else
# define IRN_VRFY_IRG(res, irg)  irn_vrfy_irg(res, irg)
#endif /* NDEBUG */

/**
 * Language dependent variable initialization callback.
 */
static uninitialized_local_variable_func_t *default_initialize_local_variable = NULL;

/* creates a bd constructor for a binop */
#define NEW_BD_BINOP(instr)                                     \
static ir_node *                                                \
new_bd_##instr(dbg_info *db, ir_node *block,                    \
       ir_node *op1, ir_node *op2, ir_mode *mode)               \
{                                                               \
  ir_node  *in[2];                                              \
  ir_node  *res;                                                \
  ir_graph *irg = current_ir_graph;                             \
  in[0] = op1;                                                  \
  in[1] = op2;                                                  \
  res = new_ir_node(db, irg, block, op_##instr, mode, 2, in);   \
  res = optimize_node(res);                                     \
  IRN_VRFY_IRG(res, irg);                                       \
  return res;                                                   \
}

/* creates a bd constructor for an unop */
#define NEW_BD_UNOP(instr)                                      \
static ir_node *                                                \
new_bd_##instr(dbg_info *db, ir_node *block,                    \
              ir_node *op, ir_mode *mode)                       \
{                                                               \
  ir_node  *res;                                                \
  ir_graph *irg = current_ir_graph;                             \
  res = new_ir_node(db, irg, block, op_##instr, mode, 1, &op);  \
  res = optimize_node(res);                                     \
  IRN_VRFY_IRG(res, irg);                                       \
  return res;                                                   \
}

/* creates a bd constructor for an divop */
#define NEW_BD_DIVOP(instr)                                     \
static ir_node *                                                \
new_bd_##instr(dbg_info *db, ir_node *block,                    \
            ir_node *memop, ir_node *op1, ir_node *op2, ir_mode *mode, op_pin_state state) \
{                                                               \
  ir_node  *in[3];                                              \
  ir_node  *res;                                                \
  ir_graph *irg = current_ir_graph;                             \
  in[0] = memop;                                                \
  in[1] = op1;                                                  \
  in[2] = op2;                                                  \
  res = new_ir_node(db, irg, block, op_##instr, mode_T, 3, in); \
  res->attr.divmod.exc.pin_state = state;                       \
  res->attr.divmod.res_mode = mode;                             \
  res->attr.divmod.no_remainder = 0;                            \
  res = optimize_node(res);                                     \
  IRN_VRFY_IRG(res, irg);                                       \
  return res;                                                   \
}

/* creates a rd constructor for a binop */
#define NEW_RD_BINOP(instr)                                     \
ir_node *                                                       \
new_rd_##instr(dbg_info *db, ir_graph *irg, ir_node *block,     \
       ir_node *op1, ir_node *op2, ir_mode *mode)               \
{                                                               \
  ir_node  *res;                                                \
  ir_graph *rem = current_ir_graph;                             \
  current_ir_graph = irg;                                       \
  res = new_bd_##instr(db, block, op1, op2, mode);              \
  current_ir_graph = rem;                                       \
  return res;                                                   \
}

/* creates a rd constructor for an unop */
#define NEW_RD_UNOP(instr)                                      \
ir_node *                                                       \
new_rd_##instr(dbg_info *db, ir_graph *irg, ir_node *block,     \
              ir_node *op, ir_mode *mode)                       \
{                                                               \
  ir_node  *res;                                                \
  ir_graph *rem = current_ir_graph;                             \
  current_ir_graph = irg;                                       \
  res = new_bd_##instr(db, block, op, mode);                    \
  current_ir_graph = rem;                                       \
  return res;                                                   \
}

/* creates a rd constructor for an divop */
#define NEW_RD_DIVOP(instr)                                     \
ir_node *                                                       \
new_rd_##instr(dbg_info *db, ir_graph *irg, ir_node *block,     \
            ir_node *memop, ir_node *op1, ir_node *op2, ir_mode *mode, op_pin_state state) \
{                                                               \
  ir_node  *res;                                                \
  ir_graph *rem = current_ir_graph;                             \
  current_ir_graph = irg;                                       \
  res = new_bd_##instr(db, block, memop, op1, op2, mode, state);\
  current_ir_graph = rem;                                       \
  return res;                                                   \
}

/* creates a d constructor for an binop */
#define NEW_D_BINOP(instr)                                                    \
ir_node *                                                                     \
new_d_##instr(dbg_info *db, ir_node *op1, ir_node *op2, ir_mode *mode) {      \
  return new_bd_##instr(db, current_ir_graph->current_block, op1, op2, mode); \
}

/* creates a d constructor for an unop */
#define NEW_D_UNOP(instr)                                                     \
ir_node *                                                                     \
new_d_##instr(dbg_info *db, ir_node *op, ir_mode *mode) {                     \
  return new_bd_##instr(db, current_ir_graph->current_block, op, mode);       \
}

#ifndef USE_ORIGINAL
#include "gen_ir_cons.c.inl"
#else

/**
 * Constructs a Block with a fixed number of predecessors.
 * Does not set current_block.  Cannot be used with automatic
 * Phi node construction.
 */
static ir_node *
new_bd_Block(dbg_info *db, int arity, ir_node **in) {
	ir_node  *res;
	ir_graph *irg = current_ir_graph;

	res = new_ir_node(db, irg, NULL, op_Block, mode_BB, arity, in);

	/* macroblock header */
	res->in[0] = res;

	res->attr.block.is_dead     = 0;
	res->attr.block.is_mb_head  = 1;
	res->attr.block.has_label   = 0;
	res->attr.block.irg         = irg;
	res->attr.block.backedge    = new_backedge_arr(irg->obst, arity);
	res->attr.block.in_cg       = NULL;
	res->attr.block.cg_backedge = NULL;
	res->attr.block.extblk      = NULL;
	res->attr.block.mb_depth    = 0;
	res->attr.block.label       = 0;

	set_Block_matured(res, 1);
	set_Block_block_visited(res, 0);

	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_Block */

static ir_node *
new_bd_Start(dbg_info *db, ir_node *block) {
	ir_node  *res;
	ir_graph *irg = current_ir_graph;

	res = new_ir_node(db, irg, block, op_Start, mode_T, 0, NULL);

	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_Start */

static ir_node *
new_bd_End(dbg_info *db, ir_node *block) {
	ir_node  *res;
	ir_graph *irg = current_ir_graph;

	res = new_ir_node(db, irg, block, op_End, mode_X, -1, NULL);

	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_End */
#endif

/**
 * Creates a Phi node with all predecessors.  Calling this constructor
 * is only allowed if the corresponding block is mature.
 */
static ir_node *
new_bd_Phi(dbg_info *db, ir_node *block, int arity, ir_node **in, ir_mode *mode) {
	ir_node  *res;
	ir_graph *irg = current_ir_graph;
	int i;
	int has_unknown = 0;

	/* Don't assert that block matured: the use of this constructor is strongly
	   restricted ... */
	if (get_Block_matured(block))
		assert(get_irn_arity(block) == arity);

	res = new_ir_node(db, irg, block, op_Phi, mode, arity, in);

	res->attr.phi.u.backedge = new_backedge_arr(irg->obst, arity);

	for (i = arity - 1; i >= 0; --i)
		if (is_Unknown(in[i])) {
			has_unknown = 1;
			break;
		}

	if (!has_unknown) res = optimize_node(res);
	IRN_VRFY_IRG(res, irg);

	/* Memory Phis in endless loops must be kept alive.
	   As we can't distinguish these easily we keep all of them alive. */
	if (is_Phi(res) && mode == mode_M)
		add_End_keepalive(get_irg_end(irg), res);
	return res;
}  /* new_bd_Phi */

#ifdef USE_ORIGINAL
static ir_node *
new_bd_Const_type(dbg_info *db, tarval *con, ir_type *tp) {
	ir_node  *res;
	ir_graph *irg = current_ir_graph;

	res = new_ir_node(db, irg, get_irg_start_block(irg), op_Const, get_tarval_mode(con), 0, NULL);
	res->attr.con.tv = con;
	set_Const_type(res, tp);  /* Call method because of complex assertion. */
	res = optimize_node (res);
	assert(get_Const_type(res) == tp);
	IRN_VRFY_IRG(res, irg);

	return res;
}  /* new_bd_Const_type */
#endif

static ir_node *
new_bd_Const(dbg_info *db, tarval *con) {
	ir_graph *irg = current_ir_graph;

	return new_rd_Const_type (db, irg, con, firm_unknown_type);
}  /* new_bd_Const */

static ir_node *
new_bd_Const_long(dbg_info *db, ir_mode *mode, long value) {
	ir_graph *irg = current_ir_graph;

	return new_rd_Const(db, irg, new_tarval_from_long(value, mode));
}  /* new_bd_Const_long */

#ifdef USE_ORIGINAL
static ir_node *
new_bd_Id(dbg_info *db, ir_node *block, ir_node *val, ir_mode *mode) {
	ir_node  *res;
	ir_graph *irg = current_ir_graph;

	res = new_ir_node(db, irg, block, op_Id, mode, 1, &val);
	res = optimize_node(res);
	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_Id */

static ir_node *
new_bd_Proj(dbg_info *db, ir_node *block, ir_node *arg, ir_mode *mode,
        long proj) {
	ir_node  *res;
	ir_graph *irg = current_ir_graph;

	res = new_ir_node (db, irg, block, op_Proj, mode, 1, &arg);
	res->attr.proj = proj;

	assert(res);
	assert(get_Proj_pred(res));
	assert(get_nodes_block(get_Proj_pred(res)));

	res = optimize_node(res);

	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_Proj */
#endif

static ir_node *
new_bd_defaultProj(dbg_info *db, ir_node *block, ir_node *arg,
           long max_proj) {
	ir_node  *res;
	ir_graph *irg = current_ir_graph;

	assert(arg->op == op_Cond);
	arg->attr.cond.kind = fragmentary;
	arg->attr.cond.default_proj = max_proj;
	res = new_rd_Proj (db, irg, block, arg, mode_X, max_proj);
	return res;
}  /* new_bd_defaultProj */

static ir_node *
new_bd_Conv(dbg_info *db, ir_node *block, ir_node *op, ir_mode *mode, int strict_flag) {
	ir_node  *res;
	ir_graph *irg = current_ir_graph;

	res = new_ir_node(db, irg, block, op_Conv, mode, 1, &op);
	res->attr.conv.strict = strict_flag;
	res = optimize_node(res);
	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_Conv */

#ifdef USE_ORIGINAL
static ir_node *
new_bd_Cast(dbg_info *db, ir_node *block, ir_node *op, ir_type *to_tp) {
	ir_node  *res;
	ir_graph *irg = current_ir_graph;

	assert(is_atomic_type(to_tp));

	res = new_ir_node(db, irg, block, op_Cast, get_irn_mode(op), 1, &op);
	res->attr.cast.totype = to_tp;
	res = optimize_node(res);
	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_Cast */

static ir_node *
new_bd_Tuple(dbg_info *db, ir_node *block, int arity, ir_node **in) {
	ir_node  *res;
	ir_graph *irg = current_ir_graph;

	res = new_ir_node(db, irg, block, op_Tuple, mode_T, arity, in);
	res = optimize_node (res);
	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_Tuple */

NEW_BD_BINOP(Add)
#endif
NEW_BD_BINOP(Sub)
NEW_BD_UNOP(Minus)
NEW_BD_BINOP(Mul)
NEW_BD_BINOP(Mulh)
NEW_BD_DIVOP(Quot)
NEW_BD_DIVOP(DivMod)
#ifdef USE_ORIGINAL
NEW_BD_DIVOP(Div)
#endif
NEW_BD_DIVOP(Mod)
NEW_BD_BINOP(And)
NEW_BD_BINOP(Or)
NEW_BD_BINOP(Eor)
NEW_BD_UNOP(Not)
NEW_BD_BINOP(Shl)
NEW_BD_BINOP(Shr)
NEW_BD_BINOP(Shrs)
NEW_BD_BINOP(Rotl)
NEW_BD_UNOP(Abs)
NEW_BD_BINOP(Carry)
NEW_BD_BINOP(Borrow)

/** Creates a remainderless Div node. */
static ir_node *new_bd_DivRL(dbg_info *db, ir_node *block,
            ir_node *memop, ir_node *op1, ir_node *op2, ir_mode *mode, op_pin_state state)
{
	ir_node  *in[3];
	ir_node  *res;
	ir_graph *irg = current_ir_graph;
	in[0] = memop;
	in[1] = op1;
	in[2] = op2;
	res = new_ir_node(db, irg, block, op_Div, mode_T, 3, in);
	res->attr.divmod.exc.pin_state = state;
	res->attr.divmod.res_mode = mode;
	res->attr.divmod.no_remainder = 1;
	res = optimize_node(res);
	IRN_VRFY_IRG(res, irg);
	return res;
}

#ifdef USE_ORIGINAL
static ir_node *
new_bd_Cmp(dbg_info *db, ir_node *block, ir_node *op1, ir_node *op2) {
	ir_node  *in[2];
	ir_node  *res;
	ir_graph *irg = current_ir_graph;
	in[0] = op1;
	in[1] = op2;
	res = new_ir_node(db, irg, block, op_Cmp, mode_T, 2, in);
	res = optimize_node(res);
	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_Cmp */

static ir_node *
new_bd_Jmp(dbg_info *db, ir_node *block) {
	ir_node  *res;
	ir_graph *irg = current_ir_graph;

	res = new_ir_node(db, irg, block, op_Jmp, mode_X, 0, NULL);
	res = optimize_node(res);
	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_Jmp */

static ir_node *
new_bd_IJmp(dbg_info *db, ir_node *block, ir_node *tgt) {
	ir_node  *res;
	ir_graph *irg = current_ir_graph;

	res = new_ir_node(db, irg, block, op_IJmp, mode_X, 1, &tgt);
	res = optimize_node(res);
	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_IJmp */

static ir_node *
new_bd_Cond(dbg_info *db, ir_node *block, ir_node *c) {
	ir_node  *res;
	ir_graph *irg = current_ir_graph;

	res = new_ir_node(db, irg, block, op_Cond, mode_T, 1, &c);
	res->attr.cond.kind         = dense;
	res->attr.cond.default_proj = 0;
	res->attr.cond.pred         = COND_JMP_PRED_NONE;
	res = optimize_node(res);
	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_Cond */
#endif

static ir_node *
new_bd_Call(dbg_info *db, ir_node *block, ir_node *store,
        ir_node *callee, int arity, ir_node **in, ir_type *tp) {
	ir_node  **r_in;
	ir_node  *res;
	int      r_arity;
	ir_graph *irg = current_ir_graph;

	r_arity = arity+2;
	NEW_ARR_A(ir_node *, r_in, r_arity);
	r_in[0] = store;
	r_in[1] = callee;
	memcpy(&r_in[2], in, sizeof(ir_node *) * arity);

	res = new_ir_node(db, irg, block, op_Call, mode_T, r_arity, r_in);

	assert((get_unknown_type() == tp) || is_Method_type(tp));
	set_Call_type(res, tp);
	res->attr.call.exc.pin_state = op_pin_state_pinned;
	res->attr.call.callee_arr    = NULL;
	res = optimize_node(res);
	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_Call */

static ir_node *
new_bd_Return(dbg_info *db, ir_node *block,
              ir_node *store, int arity, ir_node **in) {
	ir_node  **r_in;
	ir_node  *res;
	int      r_arity;
	ir_graph *irg = current_ir_graph;

	r_arity = arity+1;
	NEW_ARR_A (ir_node *, r_in, r_arity);
	r_in[0] = store;
	memcpy(&r_in[1], in, sizeof(ir_node *) * arity);
	res = new_ir_node(db, irg, block, op_Return, mode_X, r_arity, r_in);
	res = optimize_node(res);
	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_Return */

static ir_node *
new_bd_Load(dbg_info *db, ir_node *block,
        ir_node *store, ir_node *adr, ir_mode *mode) {
	ir_node  *in[2];
	ir_node  *res;
	ir_graph *irg = current_ir_graph;

	in[0] = store;
	in[1] = adr;
	res = new_ir_node(db, irg, block, op_Load, mode_T, 2, in);
	res->attr.load.exc.pin_state = op_pin_state_pinned;
	res->attr.load.load_mode     = mode;
	res->attr.load.volatility    = volatility_non_volatile;
	res->attr.load.aligned       = align_is_aligned;
	res = optimize_node(res);
	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_Load */

static ir_node *
new_bd_Store(dbg_info *db, ir_node *block,
         ir_node *store, ir_node *adr, ir_node *val) {
	ir_node  *in[3];
	ir_node  *res;
	ir_graph *irg = current_ir_graph;

	in[0] = store;
	in[1] = adr;
	in[2] = val;
	res = new_ir_node(db, irg, block, op_Store, mode_T, 3, in);
	res->attr.store.exc.pin_state = op_pin_state_pinned;
	res->attr.store.volatility    = volatility_non_volatile;
	res->attr.store.aligned       = align_is_aligned;
	res = optimize_node(res);
	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_Store */

static ir_node *
new_bd_Alloc(dbg_info *db, ir_node *block, ir_node *store,
        ir_node *size, ir_type *alloc_type, ir_where_alloc where) {
	ir_node  *in[2];
	ir_node  *res;
	ir_graph *irg = current_ir_graph;

	in[0] = store;
	in[1] = size;
	res = new_ir_node(db, irg, block, op_Alloc, mode_T, 2, in);
	res->attr.alloc.exc.pin_state = op_pin_state_pinned;
	res->attr.alloc.where         = where;
	res->attr.alloc.type          = alloc_type;
	res = optimize_node(res);
	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_Alloc */

static ir_node *
new_bd_Free(dbg_info *db, ir_node *block, ir_node *store,
        ir_node *ptr, ir_node *size, ir_type *free_type, ir_where_alloc where) {
	ir_node  *in[3];
	ir_node  *res;
	ir_graph *irg = current_ir_graph;

	in[0] = store;
	in[1] = ptr;
	in[2] = size;
	res = new_ir_node (db, irg, block, op_Free, mode_M, 3, in);
	res->attr.free.where = where;
	res->attr.free.type  = free_type;
	res = optimize_node(res);
	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_Free */

static ir_node *
new_bd_Sel(dbg_info *db, ir_node *block, ir_node *store, ir_node *objptr,
           int arity, ir_node **in, ir_entity *ent) {
	ir_node  **r_in;
	ir_node  *res;
	int      r_arity;
	ir_graph *irg = current_ir_graph;
	ir_mode  *mode = is_Method_type(get_entity_type(ent)) ? mode_P_code : mode_P_data;

	assert(ent != NULL && is_entity(ent) && "entity expected in Sel construction");

	r_arity = arity + 2;
	NEW_ARR_A(ir_node *, r_in, r_arity);  /* uses alloca */
	r_in[0] = store;
	r_in[1] = objptr;
	memcpy(&r_in[2], in, sizeof(ir_node *) * arity);
	/*
	 * Sel's can select functions which should be of mode mode_P_code.
	 */
	res = new_ir_node(db, irg, block, op_Sel, mode, r_arity, r_in);
	res->attr.sel.ent = ent;
	res = optimize_node(res);
	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_Sel */

static ir_node *
new_bd_SymConst_type(dbg_info *db, ir_node *block, ir_mode *mode,
                     symconst_symbol value,symconst_kind symkind, ir_type *tp) {
	ir_graph *irg = current_ir_graph;
	ir_node  *res = new_ir_node(db, irg, block, op_SymConst, mode, 0, NULL);

	res->attr.symc.kind = symkind;
	res->attr.symc.sym  = value;
	res->attr.symc.tp   = tp;

	res = optimize_node(res);
	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_SymConst_type */

static ir_node *
new_bd_Sync(dbg_info *db, ir_node *block) {
	ir_node  *res;
	ir_graph *irg = current_ir_graph;

	res = new_ir_node(db, irg, block, op_Sync, mode_M, -1, NULL);
	/* no need to call optimize node here, Sync are always created with no predecessors */
	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_Sync */

static ir_node *
new_bd_Confirm(dbg_info *db, ir_node *block, ir_node *val, ir_node *bound, pn_Cmp cmp) {
	ir_node  *in[2], *res;
	ir_graph *irg = current_ir_graph;

	in[0] = val;
	in[1] = bound;
	res = new_ir_node(db, irg, block, op_Confirm, get_irn_mode(val), 2, in);
	res->attr.confirm.cmp = cmp;
	res = optimize_node(res);
	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_Confirm */

static ir_node *
new_bd_Unknown(ir_mode *m) {
	ir_node  *res;
	ir_graph *irg = current_ir_graph;

	res = new_ir_node(NULL, irg, get_irg_start_block(irg), op_Unknown, m, 0, NULL);
	res = optimize_node(res);
	return res;
}  /* new_bd_Unknown */

static ir_node *
new_bd_CallBegin(dbg_info *db, ir_node *block, ir_node *call) {
	ir_node  *in[1];
	ir_node  *res;
	ir_graph *irg = current_ir_graph;

	in[0] = get_Call_ptr(call);
	res = new_ir_node(db, irg, block, op_CallBegin, mode_T, 1, in);
	/* res->attr.callbegin.irg = irg; */
	res->attr.callbegin.call = call;
	res = optimize_node(res);
	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_CallBegin */

static ir_node *
new_bd_EndReg(dbg_info *db, ir_node *block) {
	ir_node  *res;
	ir_graph *irg = current_ir_graph;

	res = new_ir_node(db, irg, block, op_EndReg, mode_T, -1, NULL);
	set_irg_end_reg(irg, res);
	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_EndReg */

static ir_node *
new_bd_EndExcept(dbg_info *db, ir_node *block) {
	ir_node  *res;
	ir_graph *irg = current_ir_graph;

	res = new_ir_node(db, irg, block, op_EndExcept, mode_T, -1, NULL);
	set_irg_end_except(irg, res);
	IRN_VRFY_IRG (res, irg);
	return res;
}  /* new_bd_EndExcept */

static ir_node *
new_bd_Break(dbg_info *db, ir_node *block) {
	ir_node  *res;
	ir_graph *irg = current_ir_graph;

	res = new_ir_node(db, irg, block, op_Break, mode_X, 0, NULL);
	res = optimize_node(res);
	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_Break */

static ir_node *
new_bd_Filter(dbg_info *db, ir_node *block, ir_node *arg, ir_mode *mode,
              long proj) {
	ir_node  *res;
	ir_graph *irg = current_ir_graph;

	res = new_ir_node(db, irg, block, op_Filter, mode, 1, &arg);
	res->attr.filter.proj = proj;
	res->attr.filter.in_cg = NULL;
	res->attr.filter.backedge = NULL;

	assert(res);
	assert(get_Proj_pred(res));
	assert(get_nodes_block(get_Proj_pred(res)));

	res = optimize_node(res);
	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_Filter */

static ir_node *
new_bd_Mux(dbg_info *db, ir_node *block,
           ir_node *sel, ir_node *ir_false, ir_node *ir_true, ir_mode *mode) {
	ir_node  *in[3];
	ir_node  *res;
	ir_graph *irg = current_ir_graph;

	in[0] = sel;
	in[1] = ir_false;
	in[2] = ir_true;

	res = new_ir_node(db, irg, block, op_Mux, mode, 3, in);
	assert(res);

	res = optimize_node(res);
	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_Mux */

static ir_node *
new_bd_CopyB(dbg_info *db, ir_node *block,
    ir_node *store, ir_node *dst, ir_node *src, ir_type *data_type) {
	ir_node  *in[3];
	ir_node  *res;
	ir_graph *irg = current_ir_graph;

	in[0] = store;
	in[1] = dst;
	in[2] = src;

	res = new_ir_node(db, irg, block, op_CopyB, mode_T, 3, in);

	res->attr.copyb.exc.pin_state = op_pin_state_pinned;
	res->attr.copyb.data_type     = data_type;
	res = optimize_node(res);
	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_CopyB */

static ir_node *
new_bd_InstOf(dbg_info *db, ir_node *block, ir_node *store,
              ir_node *objptr, ir_type *type) {
	ir_node  *in[2];
	ir_node  *res;
	ir_graph *irg = current_ir_graph;

	in[0] = store;
	in[1] = objptr;
	res = new_ir_node(db, irg, block, op_Sel, mode_T, 2, in);
	res->attr.instof.type = type;
	res = optimize_node(res);
	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_InstOf */

static ir_node *
new_bd_Raise(dbg_info *db, ir_node *block, ir_node *store, ir_node *obj) {
	ir_node  *in[2];
	ir_node  *res;
	ir_graph *irg = current_ir_graph;

	in[0] = store;
	in[1] = obj;
	res = new_ir_node(db, irg, block, op_Raise, mode_T, 2, in);
	res = optimize_node(res);
	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_Raise */

static ir_node *
new_bd_Bound(dbg_info *db, ir_node *block,
             ir_node *store, ir_node *idx, ir_node *lower, ir_node *upper) {
	ir_node  *in[4];
	ir_node  *res;
	ir_graph *irg = current_ir_graph;

	in[0] = store;
	in[1] = idx;
	in[2] = lower;
	in[3] = upper;
	res = new_ir_node(db, irg, block, op_Bound, mode_T, 4, in);
	res->attr.bound.exc.pin_state = op_pin_state_pinned;
	res = optimize_node(res);
	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_Bound */

static ir_node *
new_bd_Pin(dbg_info *db, ir_node *block, ir_node *node) {
	ir_node  *res;
	ir_graph *irg = current_ir_graph;

	res = new_ir_node(db, irg, block, op_Pin, get_irn_mode(node), 1, &node);
	res = optimize_node(res);
	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_Pin */

static ir_node *
new_bd_ASM(dbg_info *db, ir_node *block, int arity, ir_node *in[], ir_asm_constraint *inputs,
           int n_outs, ir_asm_constraint *outputs, int n_clobber, ident *clobber[], ident *asm_text) {
	ir_node  *res;
	ir_graph *irg = current_ir_graph;
	(void) clobber;

	res = new_ir_node(db, irg, block, op_ASM, mode_T, arity, in);
	res->attr.assem.pin_state = op_pin_state_pinned;
	res->attr.assem.inputs    = NEW_ARR_D(ir_asm_constraint, irg->obst, arity);
	res->attr.assem.outputs   = NEW_ARR_D(ir_asm_constraint, irg->obst, n_outs);
	res->attr.assem.clobber   = NEW_ARR_D(ident *, irg->obst, n_clobber);
	res->attr.assem.asm_text  = asm_text;

	memcpy(res->attr.assem.inputs,  inputs,  sizeof(inputs[0]) * arity);
	memcpy(res->attr.assem.outputs, outputs, sizeof(outputs[0]) * n_outs);
	memcpy(res->attr.assem.clobber, clobber, sizeof(clobber[0]) * n_clobber);

	res = optimize_node(res);
	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_bd_ASM */

/* --------------------------------------------- */
/* private interfaces, for professional use only */
/* --------------------------------------------- */

#ifdef USE_ORIGINAL
/* Constructs a Block with a fixed number of predecessors.
   Does not set current_block.  Can not be used with automatic
   Phi node construction. */
ir_node *
new_rd_Block(dbg_info *db, ir_graph *irg, int arity, ir_node **in) {
	ir_graph *rem = current_ir_graph;
	ir_node  *res;

	current_ir_graph = irg;
	res = new_bd_Block(db, arity, in);
	current_ir_graph = rem;

	return res;
}  /* new_rd_Block */

ir_node *
new_rd_Start(dbg_info *db, ir_graph *irg, ir_node *block) {
	ir_graph *rem = current_ir_graph;
	ir_node  *res;

	current_ir_graph = irg;
	res = new_bd_Start(db, block);
	current_ir_graph = rem;

	return res;
}  /* new_rd_Start */

ir_node *
new_rd_End(dbg_info *db, ir_graph *irg, ir_node *block) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_End(db, block);
	current_ir_graph = rem;

	return res;
}  /* new_rd_End */
#endif

/* Creates a Phi node with all predecessors.  Calling this constructor
   is only allowed if the corresponding block is mature.  */
ir_node *
new_rd_Phi(dbg_info *db, ir_graph *irg, ir_node *block, int arity, ir_node **in, ir_mode *mode) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_Phi(db, block,arity, in, mode);
	current_ir_graph = rem;

	return res;
}  /* new_rd_Phi */

#ifdef USE_ORIGINAL
ir_node *
new_rd_Const_type(dbg_info *db, ir_graph *irg, tarval *con, ir_type *tp) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_Const_type(db, con, tp);
	current_ir_graph = rem;

	return res;
}  /* new_rd_Const_type */
#endif

ir_node *
new_rd_Const(dbg_info *db, ir_graph *irg, tarval *con) {
	ir_node  *res;
#ifdef USE_ORIGINAL
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_Const_type(db, con, firm_unknown_type);
	current_ir_graph = rem;
#else
	res = new_rd_Const_type(db, irg, con, firm_unknown_type);
#endif

	return res;
}  /* new_rd_Const */

ir_node *
new_rd_Const_long(dbg_info *db, ir_graph *irg, ir_mode *mode, long value) {
	return new_rd_Const(db, irg, new_tarval_from_long(value, mode));
}  /* new_rd_Const_long */

#ifdef USE_ORIGINAL
ir_node *
new_rd_Id(dbg_info *db, ir_graph *irg, ir_node *block, ir_node *val, ir_mode *mode) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_Id(db, block, val, mode);
	current_ir_graph = rem;

	return res;
}  /* new_rd_Id */

ir_node *
new_rd_Proj(dbg_info *db, ir_graph *irg, ir_node *block, ir_node *arg, ir_mode *mode,
            long proj) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_Proj(db, block, arg, mode, proj);
	current_ir_graph = rem;

	return res;
}  /* new_rd_Proj */
#endif

ir_node *
new_rd_defaultProj(dbg_info *db, ir_graph *irg, ir_node *block, ir_node *arg,
                   long max_proj) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_defaultProj(db, block, arg, max_proj);
	current_ir_graph = rem;

	return res;
}  /* new_rd_defaultProj */

ir_node *
new_rd_Conv(dbg_info *db, ir_graph *irg, ir_node *block, ir_node *op, ir_mode *mode) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_Conv(db, block, op, mode, 0);
	current_ir_graph = rem;

	return res;
}  /* new_rd_Conv */

#ifdef USE_ORIGINAL
ir_node *
new_rd_Cast(dbg_info *db, ir_graph *irg, ir_node *block, ir_node *op, ir_type *to_tp) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_Cast(db, block, op, to_tp);
	current_ir_graph = rem;

	return res;
}  /* new_rd_Cast */

ir_node *
new_rd_Tuple(dbg_info *db, ir_graph *irg, ir_node *block, int arity, ir_node **in) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_Tuple(db, block, arity, in);
	current_ir_graph = rem;

	return res;
}  /* new_rd_Tuple */

NEW_RD_BINOP(Add)
#endif
NEW_RD_BINOP(Sub)
NEW_RD_UNOP(Minus)
NEW_RD_BINOP(Mul)
NEW_RD_BINOP(Mulh)
NEW_RD_DIVOP(Quot)
NEW_RD_DIVOP(DivMod)
#ifdef USE_ORIGINAL
NEW_RD_DIVOP(Div)
#endif
NEW_RD_DIVOP(Mod)
NEW_RD_BINOP(And)
NEW_RD_BINOP(Or)
NEW_RD_BINOP(Eor)
NEW_RD_UNOP(Not)
NEW_RD_BINOP(Shl)
NEW_RD_BINOP(Shr)
NEW_RD_BINOP(Shrs)
NEW_RD_BINOP(Rotl)
NEW_RD_UNOP(Abs)
NEW_RD_BINOP(Carry)
NEW_RD_BINOP(Borrow)

/* creates a rd constructor for an divRL */
ir_node *new_rd_DivRL(dbg_info *db, ir_graph *irg, ir_node *block,
            ir_node *memop, ir_node *op1, ir_node *op2, ir_mode *mode, op_pin_state state)
{
	ir_node  *res;
	ir_graph *rem = current_ir_graph;
	current_ir_graph = irg;
	res = new_bd_DivRL(db, block, memop, op1, op2, mode, state);
	current_ir_graph = rem;
	return res;
}

#ifdef USE_ORIGINAL
ir_node *
new_rd_Cmp(dbg_info *db, ir_graph *irg, ir_node *block,
           ir_node *op1, ir_node *op2) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_Cmp(db, block, op1, op2);
	current_ir_graph = rem;

	return res;
}  /* new_rd_Cmp */

ir_node *
new_rd_Jmp(dbg_info *db, ir_graph *irg, ir_node *block) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_Jmp(db, block);
	current_ir_graph = rem;

	return res;
}  /* new_rd_Jmp */

ir_node *
new_rd_IJmp(dbg_info *db, ir_graph *irg, ir_node *block, ir_node *tgt) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_IJmp(db, block, tgt);
	current_ir_graph = rem;

	return res;
}  /* new_rd_IJmp */

ir_node *
new_rd_Cond(dbg_info *db, ir_graph *irg, ir_node *block, ir_node *c) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_Cond(db, block, c);
	current_ir_graph = rem;

	return res;
}  /* new_rd_Cond */
#endif

ir_node *
new_rd_Call(dbg_info *db, ir_graph *irg, ir_node *block, ir_node *store,
            ir_node *callee, int arity, ir_node **in, ir_type *tp) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_Call(db, block, store, callee, arity, in, tp);
	current_ir_graph = rem;

	return res;
}  /* new_rd_Call */

ir_node *
new_rd_Return(dbg_info *db, ir_graph *irg, ir_node *block,
              ir_node *store, int arity, ir_node **in) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_Return(db, block, store, arity, in);
	current_ir_graph = rem;

	return res;
}  /* new_rd_Return */

ir_node *
new_rd_Load(dbg_info *db, ir_graph *irg, ir_node *block,
            ir_node *store, ir_node *adr, ir_mode *mode) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_Load(db, block, store, adr, mode);
	current_ir_graph = rem;

	return res;
}  /* new_rd_Load */

ir_node *
new_rd_Store(dbg_info *db, ir_graph *irg, ir_node *block,
             ir_node *store, ir_node *adr, ir_node *val) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_Store(db, block, store, adr, val);
	current_ir_graph = rem;

	return res;
}  /* new_rd_Store */

ir_node *
new_rd_Alloc(dbg_info *db, ir_graph *irg, ir_node *block, ir_node *store,
             ir_node *size, ir_type *alloc_type, ir_where_alloc where) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_Alloc(db, block, store, size, alloc_type, where);
	current_ir_graph = rem;

	return res;
}  /* new_rd_Alloc */

ir_node *
new_rd_Free(dbg_info *db, ir_graph *irg, ir_node *block, ir_node *store,
            ir_node *ptr, ir_node *size, ir_type *free_type, ir_where_alloc where) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_Free(db, block, store, ptr, size, free_type, where);
	current_ir_graph = rem;

	return res;
}  /* new_rd_Free */

ir_node *
new_rd_simpleSel(dbg_info *db, ir_graph *irg, ir_node *block,
                 ir_node *store, ir_node *objptr, ir_entity *ent) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_Sel(db, block, store, objptr, 0, NULL, ent);
	current_ir_graph = rem;

	return res;
}  /* new_rd_simpleSel */

ir_node *
new_rd_Sel(dbg_info *db, ir_graph *irg, ir_node *block, ir_node *store, ir_node *objptr,
           int arity, ir_node **in, ir_entity *ent) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_Sel(db, block, store, objptr, arity, in, ent);
	current_ir_graph = rem;

	return res;
}  /* new_rd_Sel */

ir_node *
new_rd_SymConst_type(dbg_info *db, ir_graph *irg, ir_node *block, ir_mode *mode,
                     symconst_symbol value, symconst_kind symkind, ir_type *tp) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_SymConst_type(db, block, mode, value, symkind, tp);
	current_ir_graph = rem;

	return res;
}  /* new_rd_SymConst_type */

ir_node *
new_rd_SymConst(dbg_info *db, ir_graph *irg, ir_node *block, ir_mode *mode,
                symconst_symbol value, symconst_kind symkind) {
	return new_rd_SymConst_type(db, irg, block, mode, value, symkind, firm_unknown_type);
}  /* new_rd_SymConst */

 ir_node *new_rd_SymConst_addr_ent(dbg_info *db, ir_graph *irg, ir_mode *mode, ir_entity *symbol, ir_type *tp) {
	symconst_symbol sym;
	sym.entity_p = symbol;
	return new_rd_SymConst_type(db, irg, get_irg_start_block(irg), mode, sym, symconst_addr_ent, tp);
}  /* new_rd_SymConst_addr_ent */

ir_node *new_rd_SymConst_ofs_ent(dbg_info *db, ir_graph *irg, ir_mode *mode, ir_entity *symbol, ir_type *tp) {
	symconst_symbol sym;
	sym.entity_p = symbol;
	return new_rd_SymConst_type(db, irg, get_irg_start_block(irg), mode, sym, symconst_ofs_ent, tp);
}  /* new_rd_SymConst_ofs_ent */

ir_node *new_rd_SymConst_addr_name(dbg_info *db, ir_graph *irg, ir_mode *mode, ident *symbol, ir_type *tp) {
	symconst_symbol sym;
	sym.ident_p = symbol;
	return new_rd_SymConst_type(db, irg, get_irg_start_block(irg), mode, sym, symconst_addr_name, tp);
}  /* new_rd_SymConst_addr_name */

ir_node *new_rd_SymConst_type_tag(dbg_info *db, ir_graph *irg, ir_mode *mode, ir_type *symbol, ir_type *tp) {
	symconst_symbol sym;
	sym.type_p = symbol;
	return new_rd_SymConst_type(db, irg, get_irg_start_block(irg), mode, sym, symconst_type_tag, tp);
}  /* new_rd_SymConst_type_tag */

ir_node *new_rd_SymConst_size(dbg_info *db, ir_graph *irg, ir_mode *mode, ir_type *symbol, ir_type *tp) {
	symconst_symbol sym;
	sym.type_p = symbol;
	return new_rd_SymConst_type(db, irg, get_irg_start_block(irg), mode, sym, symconst_type_size, tp);
}  /* new_rd_SymConst_size */

ir_node *new_rd_SymConst_align(dbg_info *db, ir_graph *irg, ir_mode *mode, ir_type *symbol, ir_type *tp) {
	symconst_symbol sym;
	sym.type_p = symbol;
	return new_rd_SymConst_type(db, irg, get_irg_start_block(irg), mode, sym, symconst_type_align, tp);
}  /* new_rd_SymConst_align */

ir_node *
new_rd_Sync(dbg_info *db, ir_graph *irg, ir_node *block, int arity, ir_node *in[]) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;
	int      i;

	current_ir_graph = irg;
	res = new_bd_Sync(db, block);
	current_ir_graph = rem;

	for (i = 0; i < arity; ++i)
		add_Sync_pred(res, in[i]);

	return res;
}  /* new_rd_Sync */

ir_node *
new_rd_Confirm(dbg_info *db, ir_graph *irg, ir_node *block, ir_node *val, ir_node *bound, pn_Cmp cmp) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_Confirm(db, block, val, bound, cmp);
	current_ir_graph = rem;

	return res;
}  /* new_rd_Confirm */

ir_node *
new_rd_Unknown(ir_graph *irg, ir_mode *m) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_Unknown(m);
	current_ir_graph = rem;

	return res;
}  /* new_rd_Unknown */

ir_node *
new_rd_CallBegin(dbg_info *db, ir_graph *irg, ir_node *block, ir_node *call) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_CallBegin(db, block, call);
	current_ir_graph = rem;

	return res;
}  /* new_rd_CallBegin */

ir_node *
new_rd_EndReg(dbg_info *db, ir_graph *irg, ir_node *block) {
	ir_node *res;

	res = new_ir_node(db, irg, block, op_EndReg, mode_T, -1, NULL);
	set_irg_end_reg(irg, res);
	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_rd_EndReg */

ir_node *
new_rd_EndExcept(dbg_info *db, ir_graph *irg, ir_node *block) {
	ir_node *res;

	res = new_ir_node(db, irg, block, op_EndExcept, mode_T, -1, NULL);
	set_irg_end_except(irg, res);
	IRN_VRFY_IRG (res, irg);
	return res;
}  /* new_rd_EndExcept */

ir_node *
new_rd_Break(dbg_info *db, ir_graph *irg, ir_node *block) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_Break(db, block);
	current_ir_graph = rem;

	return res;
}  /* new_rd_Break */

ir_node *
new_rd_Filter(dbg_info *db, ir_graph *irg, ir_node *block, ir_node *arg, ir_mode *mode,
              long proj) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_Filter(db, block, arg, mode, proj);
	current_ir_graph = rem;

	return res;
}  /* new_rd_Filter */

ir_node *
new_rd_Mux(dbg_info *db, ir_graph *irg, ir_node *block,
           ir_node *sel, ir_node *ir_false, ir_node *ir_true, ir_mode *mode) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_Mux(db, block, sel, ir_false, ir_true, mode);
	current_ir_graph = rem;

	return res;
}  /* new_rd_Mux */

ir_node *new_rd_CopyB(dbg_info *db, ir_graph *irg, ir_node *block,
                      ir_node *store, ir_node *dst, ir_node *src, ir_type *data_type) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_CopyB(db, block, store, dst, src, data_type);
	current_ir_graph = rem;

	return res;
}  /* new_rd_CopyB */

ir_node *
new_rd_InstOf(dbg_info *db, ir_graph *irg, ir_node *block, ir_node *store,
              ir_node *objptr, ir_type *type) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_InstOf(db, block, store, objptr, type);
	current_ir_graph = rem;

	return res;
}  /* new_rd_InstOf */

ir_node *
new_rd_Raise(dbg_info *db, ir_graph *irg, ir_node *block, ir_node *store, ir_node *obj) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_Raise(db, block, store, obj);
	current_ir_graph = rem;

	return res;
}  /* new_rd_Raise */

ir_node *new_rd_Bound(dbg_info *db, ir_graph *irg, ir_node *block,
                      ir_node *store, ir_node *idx, ir_node *lower, ir_node *upper) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_Bound(db, block, store, idx, lower, upper);
	current_ir_graph = rem;

	return res;
}  /* new_rd_Bound */

ir_node *new_rd_Pin(dbg_info *db, ir_graph *irg, ir_node *block, ir_node *node) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_Pin(db, block, node);
	current_ir_graph = rem;

	return res;
}  /* new_rd_Pin */

ir_node *new_rd_ASM(dbg_info *db, ir_graph *irg, ir_node *block,
                    int arity, ir_node *in[], ir_asm_constraint *inputs,
                    int n_outs, ir_asm_constraint *outputs,
                    int n_clobber, ident *clobber[], ident *asm_text) {
	ir_node  *res;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	res = new_bd_ASM(db, block, arity, in, inputs, n_outs, outputs, n_clobber, clobber, asm_text);
	current_ir_graph = rem;

	return res;
}  /* new_rd_ASM */


#ifdef USE_ORIGINAL
ir_node *new_r_Block(ir_graph *irg,  int arity, ir_node **in) {
	return new_rd_Block(NULL, irg, arity, in);
}
ir_node *new_r_Start(ir_graph *irg, ir_node *block) {
	return new_rd_Start(NULL, irg, block);
}
ir_node *new_r_End(ir_graph *irg, ir_node *block) {
	return new_rd_End(NULL, irg, block);
}
ir_node *new_r_Jmp(ir_graph *irg, ir_node *block) {
	return new_rd_Jmp(NULL, irg, block);
}
ir_node *new_r_IJmp(ir_graph *irg, ir_node *block, ir_node *tgt) {
	return new_rd_IJmp(NULL, irg, block, tgt);
}
ir_node *new_r_Cond(ir_graph *irg, ir_node *block, ir_node *c) {
	return new_rd_Cond(NULL, irg, block, c);
}
#endif
ir_node *new_r_Return(ir_graph *irg, ir_node *block,
                      ir_node *store, int arity, ir_node **in) {
	return new_rd_Return(NULL, irg, block, store, arity, in);
}
ir_node *new_r_Const(ir_graph *irg, tarval *con) {
	return new_rd_Const(NULL, irg, con);
}
ir_node *new_r_Const_long(ir_graph *irg, ir_mode *mode, long value) {
	return new_rd_Const_long(NULL, irg, mode, value);
}
#ifdef USE_ORIGINAL
ir_node *new_r_Const_type(ir_graph *irg, tarval *con, ir_type *tp) {
	return new_rd_Const_type(NULL, irg, con, tp);
}
#endif
ir_node *new_r_SymConst(ir_graph *irg, ir_node *block, ir_mode *mode,
                        symconst_symbol value, symconst_kind symkind) {
	return new_rd_SymConst(NULL, irg, block, mode, value, symkind);
}
ir_node *new_r_simpleSel(ir_graph *irg, ir_node *block, ir_node *store,
                         ir_node *objptr, ir_entity *ent) {
	return new_rd_Sel(NULL, irg, block, store, objptr, 0, NULL, ent);
}
ir_node *new_r_Sel(ir_graph *irg, ir_node *block, ir_node *store,
                   ir_node *objptr, int n_index, ir_node **index,
                   ir_entity *ent) {
	return new_rd_Sel(NULL, irg, block, store, objptr, n_index, index, ent);
}
ir_node *new_r_Call(ir_graph *irg, ir_node *block, ir_node *store,
                    ir_node *callee, int arity, ir_node **in,
                    ir_type *tp) {
	return new_rd_Call(NULL, irg, block, store, callee, arity, in, tp);
}
#ifdef USE_ORIGINAL
ir_node *new_r_Add(ir_graph *irg, ir_node *block,
                   ir_node *op1, ir_node *op2, ir_mode *mode) {
	return new_rd_Add(NULL, irg, block, op1, op2, mode);
}
#endif
ir_node *new_r_Sub(ir_graph *irg, ir_node *block,
                   ir_node *op1, ir_node *op2, ir_mode *mode) {
	return new_rd_Sub(NULL, irg, block, op1, op2, mode);
}
ir_node *new_r_Minus(ir_graph *irg, ir_node *block,
                     ir_node *op,  ir_mode *mode) {
	return new_rd_Minus(NULL, irg, block,  op, mode);
}
ir_node *new_r_Mul(ir_graph *irg, ir_node *block,
                   ir_node *op1, ir_node *op2, ir_mode *mode) {
	return new_rd_Mul(NULL, irg, block, op1, op2, mode);
}
ir_node *new_r_Mulh(ir_graph *irg, ir_node *block,
                   ir_node *op1, ir_node *op2, ir_mode *mode) {
	return new_rd_Mulh(NULL, irg, block, op1, op2, mode);
}
ir_node *new_r_Quot(ir_graph *irg, ir_node *block,
                    ir_node *memop, ir_node *op1, ir_node *op2, ir_mode *mode, op_pin_state state) {
	return new_rd_Quot(NULL, irg, block, memop, op1, op2, mode, state);
}
ir_node *new_r_DivMod(ir_graph *irg, ir_node *block,
                      ir_node *memop, ir_node *op1, ir_node *op2, ir_mode *mode, op_pin_state state) {
	return new_rd_DivMod(NULL, irg, block, memop, op1, op2, mode, state);
}
#ifdef USE_ORIGINAL
ir_node *new_r_Div(ir_graph *irg, ir_node *block,
                   ir_node *memop, ir_node *op1, ir_node *op2, ir_mode *mode, op_pin_state state) {
	return new_rd_Div(NULL, irg, block, memop, op1, op2, mode, state);
}
#endif
ir_node *new_r_DivRL(ir_graph *irg, ir_node *block,
                   ir_node *memop, ir_node *op1, ir_node *op2, ir_mode *mode, op_pin_state state) {
	return new_rd_DivRL(NULL, irg, block, memop, op1, op2, mode, state);
}
ir_node *new_r_Mod(ir_graph *irg, ir_node *block,
                   ir_node *memop, ir_node *op1, ir_node *op2, ir_mode *mode, op_pin_state state) {
	return new_rd_Mod(NULL, irg, block, memop, op1, op2, mode, state);
}
ir_node *new_r_Abs(ir_graph *irg, ir_node *block,
                   ir_node *op, ir_mode *mode) {
	return new_rd_Abs(NULL, irg, block, op, mode);
}
ir_node *new_r_And(ir_graph *irg, ir_node *block,
                   ir_node *op1, ir_node *op2, ir_mode *mode) {
	return new_rd_And(NULL, irg, block,  op1, op2, mode);
}
ir_node *new_r_Or(ir_graph *irg, ir_node *block,
                  ir_node *op1, ir_node *op2, ir_mode *mode) {
	return new_rd_Or(NULL, irg, block,  op1, op2, mode);
}
ir_node *new_r_Eor(ir_graph *irg, ir_node *block,
                   ir_node *op1, ir_node *op2, ir_mode *mode) {
	return new_rd_Eor(NULL, irg, block,  op1, op2, mode);
}
ir_node *new_r_Not(ir_graph *irg, ir_node *block,
                   ir_node *op, ir_mode *mode) {
	return new_rd_Not(NULL, irg, block, op, mode);
}
ir_node *new_r_Shl(ir_graph *irg, ir_node *block,
                   ir_node *op, ir_node *k, ir_mode *mode) {
	return new_rd_Shl(NULL, irg, block, op, k, mode);
}
ir_node *new_r_Shr(ir_graph *irg, ir_node *block,
                   ir_node *op, ir_node *k, ir_mode *mode) {
	return new_rd_Shr(NULL, irg, block, op, k, mode);
}
ir_node *new_r_Shrs(ir_graph *irg, ir_node *block,
                    ir_node *op, ir_node *k, ir_mode *mode) {
	return new_rd_Shrs(NULL, irg, block, op, k, mode);
}
ir_node *new_r_Rotl(ir_graph *irg, ir_node *block,
                   ir_node *op, ir_node *k, ir_mode *mode) {
	return new_rd_Rotl(NULL, irg, block, op, k, mode);
}
ir_node *new_r_Carry(ir_graph *irg, ir_node *block,
                     ir_node *op, ir_node *k, ir_mode *mode) {
	return new_rd_Carry(NULL, irg, block, op, k, mode);
}
ir_node *new_r_Borrow(ir_graph *irg, ir_node *block,
                      ir_node *op, ir_node *k, ir_mode *mode) {
	return new_rd_Borrow(NULL, irg, block, op, k, mode);
}
#ifdef USE_ORIGINAL
ir_node *new_r_Cmp(ir_graph *irg, ir_node *block,
                   ir_node *op1, ir_node *op2) {
	return new_rd_Cmp(NULL, irg, block, op1, op2);
}
#endif
ir_node *new_r_Conv(ir_graph *irg, ir_node *block,
                    ir_node *op, ir_mode *mode) {
	return new_rd_Conv(NULL, irg, block, op, mode);
}
#ifdef USE_ORIGINAL
ir_node *new_r_Cast(ir_graph *irg, ir_node *block, ir_node *op, ir_type *to_tp) {
	return new_rd_Cast(NULL, irg, block, op, to_tp);
}
#endif
ir_node *new_r_Phi(ir_graph *irg, ir_node *block, int arity,
                   ir_node **in, ir_mode *mode) {
	return new_rd_Phi(NULL, irg, block, arity, in, mode);
}
ir_node *new_r_Load(ir_graph *irg, ir_node *block,
                    ir_node *store, ir_node *adr, ir_mode *mode) {
	return new_rd_Load(NULL, irg, block, store, adr, mode);
}
ir_node *new_r_Store(ir_graph *irg, ir_node *block,
                     ir_node *store, ir_node *adr, ir_node *val) {
	return new_rd_Store(NULL, irg, block, store, adr, val);
}
ir_node *new_r_Alloc(ir_graph *irg, ir_node *block, ir_node *store,
                     ir_node *size, ir_type *alloc_type, ir_where_alloc where) {
	return new_rd_Alloc(NULL, irg, block, store, size, alloc_type, where);
}
ir_node *new_r_Free(ir_graph *irg, ir_node *block, ir_node *store,
                    ir_node *ptr, ir_node *size, ir_type *free_type, ir_where_alloc where) {
	return new_rd_Free(NULL, irg, block, store, ptr, size, free_type, where);
}
ir_node *new_r_Sync(ir_graph *irg, ir_node *block, int arity, ir_node *in[]) {
	return new_rd_Sync(NULL, irg, block, arity, in);
}
#ifdef USE_ORIGINAL
ir_node *new_r_Proj(ir_graph *irg, ir_node *block, ir_node *arg,
                    ir_mode *mode, long proj) {
	return new_rd_Proj(NULL, irg, block, arg, mode, proj);
}
#endif
ir_node *new_r_defaultProj(ir_graph *irg, ir_node *block, ir_node *arg,
                           long max_proj) {
	return new_rd_defaultProj(NULL, irg, block, arg, max_proj);
}
#ifdef USE_ORIGINAL
ir_node *new_r_Tuple(ir_graph *irg, ir_node *block,
                     int arity, ir_node **in) {
	return new_rd_Tuple(NULL, irg, block, arity, in );
}
ir_node *new_r_Id(ir_graph *irg, ir_node *block,
                  ir_node *val, ir_mode *mode) {
	return new_rd_Id(NULL, irg, block, val, mode);
}
#endif
ir_node *new_r_Bad(ir_graph *irg) {
	return get_irg_bad(irg);
}
ir_node *new_r_Confirm(ir_graph *irg, ir_node *block, ir_node *val, ir_node *bound, pn_Cmp cmp) {
	return new_rd_Confirm(NULL, irg, block, val, bound, cmp);
}
ir_node *new_r_Unknown(ir_graph *irg, ir_mode *m) {
	return new_rd_Unknown(irg, m);
}
ir_node *new_r_CallBegin(ir_graph *irg, ir_node *block, ir_node *callee) {
	return new_rd_CallBegin(NULL, irg, block, callee);
}
ir_node *new_r_EndReg(ir_graph *irg, ir_node *block) {
	return new_rd_EndReg(NULL, irg, block);
}
ir_node *new_r_EndExcept(ir_graph *irg, ir_node *block) {
	return new_rd_EndExcept(NULL, irg, block);
}
ir_node *new_r_Break(ir_graph *irg, ir_node *block) {
	return new_rd_Break(NULL, irg, block);
}
ir_node *new_r_Filter(ir_graph *irg, ir_node *block, ir_node *arg,
                      ir_mode *mode, long proj) {
	return new_rd_Filter(NULL, irg, block, arg, mode, proj);
}
ir_node *new_r_NoMem(ir_graph *irg) {
	return get_irg_no_mem(irg);
}
ir_node *new_r_Mux(ir_graph *irg, ir_node *block,
                   ir_node *sel, ir_node *ir_false, ir_node *ir_true, ir_mode *mode) {
	return new_rd_Mux(NULL, irg, block, sel, ir_false, ir_true, mode);
}
ir_node *new_r_CopyB(ir_graph *irg, ir_node *block,
                     ir_node *store, ir_node *dst, ir_node *src, ir_type *data_type) {
	return new_rd_CopyB(NULL, irg, block, store, dst, src, data_type);
}
ir_node *new_r_InstOf(ir_graph *irg, ir_node *block, ir_node *store, ir_node *objptr,
                      ir_type *type) {
	return new_rd_InstOf(NULL, irg, block, store, objptr, type);
}
ir_node *new_r_Raise(ir_graph *irg, ir_node *block,
                     ir_node *store, ir_node *obj) {
	return new_rd_Raise(NULL, irg, block, store, obj);
}
ir_node *new_r_Bound(ir_graph *irg, ir_node *block,
                     ir_node *store, ir_node *idx, ir_node *lower, ir_node *upper) {
	return new_rd_Bound(NULL, irg, block, store, idx, lower, upper);
}
ir_node *new_r_Pin(ir_graph *irg, ir_node *block, ir_node *node) {
	return new_rd_Pin(NULL, irg, block, node);
}
ir_node *new_r_ASM(ir_graph *irg, ir_node *block,
                   int arity, ir_node *in[], ir_asm_constraint *inputs,
                   int n_outs, ir_asm_constraint *outputs,
                   int n_clobber, ident *clobber[], ident *asm_text) {
	return new_rd_ASM(NULL, irg, block, arity, in, inputs, n_outs, outputs, n_clobber, clobber, asm_text);
}

/** ********************/
/** public interfaces  */
/** construction tools */

#ifdef USE_ORIGINAL

/**
 *
 *   - create a new Start node in the current block
 *
 *   @return s - pointer to the created Start node
 *
 *
 */
ir_node *
new_d_Start(dbg_info *db) {
	ir_node *res;

	res = new_ir_node(db, current_ir_graph, current_ir_graph->current_block,
	                  op_Start, mode_T, 0, NULL);

	res = optimize_node(res);
	IRN_VRFY_IRG(res, current_ir_graph);
	return res;
}  /* new_d_Start */

ir_node *
new_d_End(dbg_info *db) {
	ir_node *res;
	res = new_ir_node(db, current_ir_graph,  current_ir_graph->current_block,
	                  op_End, mode_X, -1, NULL);
	res = optimize_node(res);
	IRN_VRFY_IRG(res, current_ir_graph);

	return res;
}  /* new_d_End */

/* Constructs a Block with a fixed number of predecessors.
   Does set current_block.  Can be used with automatic Phi
   node construction. */
ir_node *
new_d_Block(dbg_info *db, int arity, ir_node **in) {
	ir_node *res;
	int i;
	int has_unknown = 0;

	res = new_bd_Block(db, arity, in);

	/* Create and initialize array for Phi-node construction. */
	if (get_irg_phase_state(current_ir_graph) == phase_building) {
		res->attr.block.graph_arr = NEW_ARR_D(ir_node *, current_ir_graph->obst,
		                                      current_ir_graph->n_loc);
		memset(res->attr.block.graph_arr, 0, sizeof(ir_node *)*current_ir_graph->n_loc);
	}

	for (i = arity-1; i >= 0; i--)
		if (is_Unknown(in[i])) {
			has_unknown = 1;
			break;
		}

	if (!has_unknown) res = optimize_node(res);
	current_ir_graph->current_block = res;

	IRN_VRFY_IRG(res, current_ir_graph);

	return res;
}  /* new_d_Block */
#endif

/* ***********************************************************************/
/* Methods necessary for automatic Phi node creation                     */
/*
  ir_node *phi_merge            (ir_node *block, int pos, ir_mode *mode, ir_node **nin, int ins)
  ir_node *get_r_value_internal (ir_node *block, int pos, ir_mode *mode);
  ir_node *new_rd_Phi0          (ir_graph *irg, ir_node *block, ir_mode *mode)
  ir_node *new_rd_Phi_in        (ir_graph *irg, ir_node *block, ir_mode *mode, ir_node **in, int ins)

  Call Graph:   ( A ---> B == A "calls" B)

       get_value         mature_immBlock
          |                   |
          |                   |
          |                   |
          |          ---> phi_merge
          |         /       /   \
          |        /       /     \
         \|/      /      |/_      \
       get_r_value_internal        |
                |                  |
                |                  |
               \|/                \|/
           new_rd_Phi0          new_rd_Phi_in

* *************************************************************************** */

/** Creates a Phi node with 0 predecessors. */
static inline ir_node *
new_rd_Phi0(ir_graph *irg, ir_node *block, ir_mode *mode) {
	ir_node *res;

	res = new_ir_node(NULL, irg, block, op_Phi, mode, 0, NULL);
	IRN_VRFY_IRG(res, irg);
	return res;
}  /* new_rd_Phi0 */


/**
 * Internal constructor of a Phi node by a phi_merge operation.
 *
 * @param irg    the graph on which the Phi will be constructed
 * @param block  the block in which the Phi will be constructed
 * @param mode   the mod eof the Phi node
 * @param in     the input array of the phi node
 * @param ins    number of elements in the input array
 * @param phi0   in non-NULL: the Phi0 node in the same block that represents
 *               the value for which the new Phi is constructed
 */
static inline ir_node *
new_rd_Phi_in(ir_graph *irg, ir_node *block, ir_mode *mode,
              ir_node **in, int ins, ir_node *phi0) {
	int i;
	ir_node *res, *known;

	/* Allocate a new node on the obstack.  The allocation copies the in
	   array. */
	res = new_ir_node(NULL, irg, block, op_Phi, mode, ins, in);
	res->attr.phi.u.backedge = new_backedge_arr(irg->obst, ins);

	/* This loop checks whether the Phi has more than one predecessor.
	   If so, it is a real Phi node and we break the loop.  Else the
	   Phi node merges the same definition on several paths and therefore
	   is not needed.
	   Note: We MUST consider Bad nodes, else we might get data flow cycles in dead loops! */
	known = res;
	for (i = ins - 1; i >= 0; --i) 	{
		assert(in[i]);

		in[i] = skip_Id(in[i]);  /* increases the number of freed Phis. */

		/* Optimize self referencing Phis:  We can't detect them yet properly, as
		they still refer to the Phi0 they will replace.  So replace right now. */
		if (phi0 && in[i] == phi0)
			in[i] = res;

		if (in[i] == res || in[i] == known)
			continue;

		if (known == res)
			known = in[i];
		else
			break;
	}

	/* i < 0: there is at most one predecessor, we don't need a phi node. */
	if (i < 0) {
		if (res != known) {
			edges_node_deleted(res, current_ir_graph);
			obstack_free(current_ir_graph->obst, res);
			if (is_Phi(known)) {
				/* If pred is a phi node we want to optimize it: If loops are matured in a bad
				   order, an enclosing Phi know may get superfluous. */
				res = optimize_in_place_2(known);
				if (res != known)
					exchange(known, res);
			}
			else
				res = known;
		} else {
			/* A undefined value, e.g., in unreachable code. */
			res = new_Bad();
		}
	} else {
		res = optimize_node(res);  /* This is necessary to add the node to the hash table for cse. */
		IRN_VRFY_IRG(res, irg);
		/* Memory Phis in endless loops must be kept alive.
		   As we can't distinguish these easily we keep all of them alive. */
		if (is_Phi(res) && mode == mode_M)
			add_End_keepalive(get_irg_end(irg), res);
	}

	return res;
}  /* new_rd_Phi_in */

static ir_node *
get_r_value_internal(ir_node *block, int pos, ir_mode *mode);

#if PRECISE_EXC_CONTEXT
static ir_node *
phi_merge(ir_node *block, int pos, ir_mode *mode, ir_node **nin, int ins);

/**
 * Construct a new frag_array for node n.
 * Copy the content from the current graph_arr of the corresponding block:
 * this is the current state.
 * Set ProjM(n) as current memory state.
 * Further the last entry in frag_arr of current block points to n.  This
 * constructs a chain block->last_frag_op-> ... first_frag_op of all frag ops in the block.
 */
static inline ir_node **new_frag_arr(ir_node *n) {
	ir_node **arr;
	int opt;

	arr = NEW_ARR_D (ir_node *, current_ir_graph->obst, current_ir_graph->n_loc);
	memcpy(arr, current_ir_graph->current_block->attr.block.graph_arr,
	       sizeof(ir_node *)*current_ir_graph->n_loc);

	/* turn off optimization before allocating Proj nodes, as res isn't
	   finished yet. */
	opt = get_opt_optimize(); set_optimize(0);
	/* Here we rely on the fact that all frag ops have Memory as first result! */
	if (is_Call(n)) {
		arr[0] = new_Proj(n, mode_M, pn_Call_M_except);
	} else if (is_CopyB(n)) {
		arr[0] = new_Proj(n, mode_M, pn_CopyB_M_except);
	} else {
		assert((pn_Quot_M == pn_DivMod_M) &&
		       (pn_Quot_M == pn_Div_M)    &&
		       (pn_Quot_M == pn_Mod_M)    &&
		       (pn_Quot_M == pn_Load_M)   &&
		       (pn_Quot_M == pn_Store_M)  &&
		       (pn_Quot_M == pn_Alloc_M)  &&
		       (pn_Quot_M == pn_Bound_M));
		arr[0] = new_Proj(n, mode_M, pn_Alloc_M);
	}
	set_optimize(opt);

	current_ir_graph->current_block->attr.block.graph_arr[current_ir_graph->n_loc-1] = n;
	return arr;
}  /* new_frag_arr */

/**
 * Returns the frag_arr from a node.
 */
static inline ir_node **get_frag_arr(ir_node *n) {
	switch (get_irn_opcode(n)) {
	case iro_Call:
		return n->attr.call.exc.frag_arr;
	case iro_Alloc:
		return n->attr.alloc.exc.frag_arr;
	case iro_Load:
		return n->attr.load.exc.frag_arr;
	case iro_Store:
		return n->attr.store.exc.frag_arr;
	default:
		return n->attr.except.frag_arr;
	}
}  /* get_frag_arr */

static void
set_frag_value(ir_node **frag_arr, int pos, ir_node *val) {
#ifdef DEBUG_libfirm
	int i;

	for (i = 1024; i >= 0; --i)
#else
	for (;;)
#endif
	{
		if (frag_arr[pos] == NULL)
			frag_arr[pos] = val;
		if (frag_arr[current_ir_graph->n_loc - 1] != NULL) {
			ir_node **arr = get_frag_arr(frag_arr[current_ir_graph->n_loc - 1]);
			assert(arr != frag_arr && "Endless recursion detected");
			frag_arr = arr;
		} else
			return;
	}
	assert(!"potential endless recursion in set_frag_value");
}  /* set_frag_value */

static ir_node *
get_r_frag_value_internal(ir_node *block, ir_node *cfOp, int pos, ir_mode *mode) {
	ir_node *res;
	ir_node **frag_arr;

	assert(is_fragile_op(cfOp) && !is_Bad(cfOp));

	frag_arr = get_frag_arr(cfOp);
	res = frag_arr[pos];
	if (res == NULL) {
		if (block->attr.block.graph_arr[pos] != NULL) {
			/* There was a set_value() after the cfOp and no get_value() before that
			   set_value().  We must build a Phi node now. */
			if (block->attr.block.is_matured) {
				int ins = get_irn_arity(block);
				ir_node **nin;
				NEW_ARR_A(ir_node *, nin, ins);
				res = phi_merge(block, pos, mode, nin, ins);
			} else {
				res = new_rd_Phi0(current_ir_graph, block, mode);
				res->attr.phi.u.pos    = pos;
				res->attr.phi.next     = block->attr.block.phis;
				block->attr.block.phis = res;
			}
			assert(res != NULL);
			/* It's a Phi, we can write this into all graph_arrs with NULL */
			set_frag_value(block->attr.block.graph_arr, pos, res);
		} else {
			res = get_r_value_internal(block, pos, mode);
			set_frag_value(block->attr.block.graph_arr, pos, res);
		}
	}
	return res;
}  /* get_r_frag_value_internal */
#endif /* PRECISE_EXC_CONTEXT */

/**
 * Check whether a control flownode  cf_pred represents an exception flow.
 *
 * @param cf_pred     the control flow node
 * @param prev_cf_op  if cf_pred is a Proj, the predecessor node, else equal to cf_pred
 */
static int is_exception_flow(ir_node *cf_pred, ir_node *prev_cf_op) {
	/*
	 * Note: all projections from a raise are "exceptional control flow" we we handle it
	 * like a normal Jmp, because there is no "regular" one.
	 * That's why Raise is no "fragile_op"!
	 */
	if (is_fragile_op(prev_cf_op)) {
		if (is_Proj(cf_pred)) {
			if (get_Proj_proj(cf_pred) == pn_Generic_X_regular) {
				/* the regular control flow, NO exception */
				return 0;
			}
			assert(get_Proj_proj(cf_pred) == pn_Generic_X_except);
			return 1;
		}
		/* Hmm, exception but not a Proj? */
		assert(!"unexpected condition: fragile op without a proj");
		return 1;
	}
	return 0;
}  /* is_exception_flow */

/**
 * Computes the predecessors for the real phi node, and then
 * allocates and returns this node.  The routine called to allocate the
 * node might optimize it away and return a real value.
 * This function must be called with an in-array of proper size.
 */
static ir_node *
phi_merge(ir_node *block, int pos, ir_mode *mode, ir_node **nin, int ins) {
	ir_node *prevBlock, *res, *phi0, *phi0_all;
	int i;

	/* If this block has no value at pos create a Phi0 and remember it
	   in graph_arr to break recursions.
	   Else we may not set graph_arr as there a later value is remembered. */
	phi0 = NULL;
	if (block->attr.block.graph_arr[pos] == NULL) {
		ir_graph *irg = current_ir_graph;

		if (block == get_irg_start_block(irg)) {
 			/* Collapsing to Bad tarvals is no good idea.
 			   So we call a user-supplied routine here that deals with this case as
 			   appropriate for the given language. Sorrily the only help we can give
 			   here is the position.

 			   Even if all variables are defined before use, it can happen that
 			   we get to the start block, if a Cond has been replaced by a tuple
 			   (bad, jmp).  In this case we call the function needlessly, eventually
 			   generating an non existent error.
 			   However, this SHOULD NOT HAPPEN, as bad control flow nodes are intercepted
 			   before recurring.
			 */
			if (default_initialize_local_variable != NULL) {
				ir_node *rem = get_cur_block();

				set_cur_block(block);
				block->attr.block.graph_arr[pos] = default_initialize_local_variable(irg, mode, pos - 1);
				set_cur_block(rem);
			}
			else
				block->attr.block.graph_arr[pos] = new_Unknown(mode);
			/* We don't need to care about exception ops in the start block.
			   There are none by definition. */
			return block->attr.block.graph_arr[pos];
		} else {
			phi0 = new_rd_Phi0(irg, block, mode);
			block->attr.block.graph_arr[pos] = phi0;
#if PRECISE_EXC_CONTEXT
			if (get_opt_precise_exc_context()) {
				/* Set graph_arr for fragile ops.  Also here we should break recursion.
				   We could choose a cyclic path through an cfop.  But the recursion would
				   break at some point. */
				set_frag_value(block->attr.block.graph_arr, pos, phi0);
			}
#endif
		}
	}

	/* This loop goes to all predecessor blocks of the block the Phi node
	   is in and there finds the operands of the Phi node by calling
	   get_r_value_internal.  */
	for (i = 1; i <= ins; ++i) {
		ir_node *cf_pred = block->in[i];
		ir_node *prevCfOp = skip_Proj(cf_pred);
		assert(prevCfOp);
		if (is_Bad(prevCfOp)) {
			/* In case a Cond has been optimized we would get right to the start block
			with an invalid definition. */
			nin[i-1] = new_Bad();
			continue;
		}
		prevBlock = prevCfOp->in[0]; /* go past control flow op to prev block */
		assert(prevBlock);
		if (!is_Bad(prevBlock)) {
#if PRECISE_EXC_CONTEXT
			if (get_opt_precise_exc_context() && is_exception_flow(cf_pred, prevCfOp)) {
				assert(get_r_frag_value_internal(prevBlock, prevCfOp, pos, mode));
				nin[i-1] = get_r_frag_value_internal(prevBlock, prevCfOp, pos, mode);
			} else
#endif
				nin[i-1] = get_r_value_internal(prevBlock, pos, mode);
		} else {
			nin[i-1] = new_Bad();
		}
	}

	/* We want to pass the Phi0 node to the constructor: this finds additional
	   optimization possibilities.
	   The Phi0 node either is allocated in this function, or it comes from
	   a former call to get_r_value_internal(). In this case we may not yet
	   exchange phi0, as this is done in mature_immBlock(). */
	if (phi0 == NULL) {
		phi0_all = block->attr.block.graph_arr[pos];
		if (! is_Phi0(phi0_all)            ||
		    get_irn_arity(phi0_all) != 0   ||
		    get_nodes_block(phi0_all) != block)
			phi0_all = NULL;
	} else {
		phi0_all = phi0;
	}

	/* After collecting all predecessors into the array nin a new Phi node
	   with these predecessors is created.  This constructor contains an
	   optimization: If all predecessors of the Phi node are identical it
	   returns the only operand instead of a new Phi node.  */
	res = new_rd_Phi_in(current_ir_graph, block, mode, nin, ins, phi0_all);

	/* In case we allocated a Phi0 node at the beginning of this procedure,
	   we need to exchange this Phi0 with the real Phi. */
	if (phi0 != NULL) {
		exchange(phi0, res);
		block->attr.block.graph_arr[pos] = res;
		/* Don't set_frag_value as it does not overwrite.  Doesn't matter, is
		   only an optimization. */
	}

	return res;
}  /* phi_merge */

/**
 * This function returns the last definition of a value.  In case
 * this value was last defined in a previous block, Phi nodes are
 * inserted.  If the part of the firm graph containing the definition
 * is not yet constructed, a dummy Phi node is returned.
 *
 * @param block   the current block
 * @param pos     the value number of the value searched
 * @param mode    the mode of this value (needed for Phi construction)
 */
static ir_node *
get_r_value_internal(ir_node *block, int pos, ir_mode *mode) {
	ir_node *res;
	/* There are 4 cases to treat.

	   1. The block is not mature and we visit it the first time.  We can not
	      create a proper Phi node, therefore a Phi0, i.e., a Phi without
	      predecessors is returned.  This node is added to the linked list (block
	      attribute "phis") of the containing block to be completed when this block is
	      matured. (Completion will add a new Phi and turn the Phi0 into an Id
	      node.)

	   2. The value is already known in this block, graph_arr[pos] is set and we
	      visit the block the first time.  We can return the value without
	      creating any new nodes.

	   3. The block is mature and we visit it the first time.  A Phi node needs
	      to be created (phi_merge).  If the Phi is not needed, as all it's
	      operands are the same value reaching the block through different
	      paths, it's optimized away and the value itself is returned.

	   4. The block is mature, and we visit it the second time.  Now two
	      subcases are possible:
	      * The value was computed completely the last time we were here. This
	        is the case if there is no loop.  We can return the proper value.
	      * The recursion that visited this node and set the flag did not
	        return yet.  We are computing a value in a loop and need to
	        break the recursion.  This case only happens if we visited
	    the same block with phi_merge before, which inserted a Phi0.
	    So we return the Phi0.
	*/

	/* case 4 -- already visited. */
	if (get_irn_visited(block) == get_irg_visited(current_ir_graph)) {
		/* As phi_merge allocates a Phi0 this value is always defined. Here
		is the critical difference of the two algorithms. */
		assert(block->attr.block.graph_arr[pos]);
		return block->attr.block.graph_arr[pos];
	}

	/* visited the first time */
	set_irn_visited(block, get_irg_visited(current_ir_graph));

	/* Get the local valid value */
	res = block->attr.block.graph_arr[pos];

	/* case 2 -- If the value is actually computed, return it. */
	if (res != NULL)
		return res;

	if (block->attr.block.is_matured) { /* case 3 */

		/* The Phi has the same amount of ins as the corresponding block. */
		int ins = get_irn_arity(block);
		ir_node **nin;
		NEW_ARR_A(ir_node *, nin, ins);

		/* Phi merge collects the predecessors and then creates a node. */
		res = phi_merge(block, pos, mode, nin, ins);

	} else {  /* case 1 */
		/* The block is not mature, we don't know how many in's are needed.  A Phi
		   with zero predecessors is created.  Such a Phi node is called Phi0
		   node.  The Phi0 is then added to the list of Phi0 nodes in this block
		   to be matured by mature_immBlock later.
		   The Phi0 has to remember the pos of it's internal value.  If the real
		   Phi is computed, pos is used to update the array with the local
		   values. */
		res = new_rd_Phi0(current_ir_graph, block, mode);
		res->attr.phi.u.pos    = pos;
		res->attr.phi.next     = block->attr.block.phis;
		block->attr.block.phis = res;
	}

	assert(is_ir_node(res) && "phi_merge() failed to construct a definition");

	/* The local valid value is available now. */
	block->attr.block.graph_arr[pos] = res;

	return res;
}  /* get_r_value_internal */

/* ************************************************************************** */

/*
 * Finalize a Block node, when all control flows are known.
 * Acceptable parameters are only Block nodes.
 */
void
mature_immBlock(ir_node *block) {
	int ins;
	ir_node *n, **nin;
	ir_node *next;

	assert(is_Block(block));
	if (!get_Block_matured(block)) {
		ir_graph *irg = current_ir_graph;

		ins = ARR_LEN(block->in) - 1;
		/* Fix block parameters */
		block->attr.block.backedge = new_backedge_arr(irg->obst, ins);

		/* An array for building the Phi nodes. */
		NEW_ARR_A(ir_node *, nin, ins);

		/* Traverse a chain of Phi nodes attached to this block and mature
		these, too. **/
		for (n = block->attr.block.phis; n; n = next) {
			inc_irg_visited(irg);
			next = n->attr.phi.next;
			exchange(n, phi_merge(block, n->attr.phi.u.pos, n->mode, nin, ins));
		}

		block->attr.block.is_matured = 1;

		/* Now, as the block is a finished Firm node, we can optimize it.
		   Since other nodes have been allocated since the block was created
		   we can not free the node on the obstack.  Therefore we have to call
		   optimize_in_place().
		   Unfortunately the optimization does not change a lot, as all allocated
		   nodes refer to the unoptimized node.
		   We can call optimize_in_place_2(), as global cse has no effect on blocks. */
		block = optimize_in_place_2(block);
		IRN_VRFY_IRG(block, irg);
	}
}  /* mature_immBlock */

ir_node *
new_d_Phi(dbg_info *db, int arity, ir_node **in, ir_mode *mode) {
	return new_bd_Phi(db, current_ir_graph->current_block, arity, in, mode);
}  /* new_d_Phi */

ir_node *
new_d_Const(dbg_info *db, tarval *con) {
	return new_bd_Const(db, con);
}  /* new_d_Const */

ir_node *
new_d_Const_long(dbg_info *db, ir_mode *mode, long value) {
	return new_bd_Const_long(db, mode, value);
}  /* new_d_Const_long */

#ifdef USE_ORIGINAL
ir_node *
new_d_Const_type(dbg_info *db, tarval *con, ir_type *tp) {
	return new_bd_Const_type(db, con, tp);
}  /* new_d_Const_type */


ir_node *
new_d_Id(dbg_info *db, ir_node *val, ir_mode *mode) {
	return new_bd_Id(db, current_ir_graph->current_block, val, mode);
}  /* new_d_Id */

ir_node *
new_d_Proj(dbg_info *db, ir_node *arg, ir_mode *mode, long proj) {
	return new_bd_Proj(db, current_ir_graph->current_block, arg, mode, proj);
}  /* new_d_Proj */
#endif

ir_node *
new_d_defaultProj(dbg_info *db, ir_node *arg, long max_proj) {
	ir_node *res;
	(void) db;
	assert(arg->op == op_Cond);
	arg->attr.cond.kind = fragmentary;
	arg->attr.cond.default_proj = max_proj;
	res = new_Proj(arg, mode_X, max_proj);
	return res;
}  /* new_d_defaultProj */

ir_node *
new_d_Conv(dbg_info *db, ir_node *op, ir_mode *mode) {
	return new_bd_Conv(db, current_ir_graph->current_block, op, mode, 0);
}  /* new_d_Conv */

ir_node *
new_d_strictConv(dbg_info *db, ir_node *op, ir_mode *mode) {
	return new_bd_Conv(db, current_ir_graph->current_block, op, mode, 1);
}  /* new_d_strictConv */

#ifdef USE_ORIGINAL
ir_node *
new_d_Cast(dbg_info *db, ir_node *op, ir_type *to_tp) {
	return new_bd_Cast(db, current_ir_graph->current_block, op, to_tp);
}  /* new_d_Cast */

ir_node *
new_d_Tuple(dbg_info *db, int arity, ir_node **in) {
	return new_bd_Tuple(db, current_ir_graph->current_block, arity, in);
}  /* new_d_Tuple */

NEW_D_BINOP(Add)
#endif
NEW_D_BINOP(Sub)
NEW_D_UNOP(Minus)
NEW_D_BINOP(Mul)
NEW_D_BINOP(Mulh)

/**
 * Allocate a frag array for a node if the current graph state is phase_building.
 *
 * @param irn         the node for which the frag array should be allocated
 * @param op          the opcode of the (original) node, if does not match opcode of irn,
 *                    nothing is done
 * @param frag_store  the address of the frag store in irn attributes, if this
 *                    address contains a value != NULL, does nothing
 */
void firm_alloc_frag_arr(ir_node *irn, ir_op *op, ir_node ***frag_store) {
	if (get_opt_precise_exc_context()) {
		if ((current_ir_graph->phase_state == phase_building) &&
		    (get_irn_op(irn) == op) && /* Could be optimized away. */
		    !*frag_store)    /* Could be a cse where the arr is already set. */ {
			*frag_store = new_frag_arr(irn);
		}
	}
}  /* firm_alloc_frag_arr */

ir_node *
new_d_Quot(dbg_info *db, ir_node *memop, ir_node *op1, ir_node *op2, ir_mode *mode, op_pin_state state) {
	ir_node *res;
	res = new_bd_Quot(db, current_ir_graph->current_block, memop, op1, op2, mode, state);
#if PRECISE_EXC_CONTEXT
	firm_alloc_frag_arr(res, op_Quot, &res->attr.except.frag_arr);
#endif

	return res;
}  /* new_d_Quot */

ir_node *
new_d_DivMod(dbg_info *db, ir_node *memop, ir_node *op1, ir_node *op2, ir_mode *mode, op_pin_state state) {
	ir_node *res;
	res = new_bd_DivMod(db, current_ir_graph->current_block, memop, op1, op2, mode, state);
#if PRECISE_EXC_CONTEXT
	firm_alloc_frag_arr(res, op_DivMod, &res->attr.except.frag_arr);
#endif

	return res;
}  /* new_d_DivMod */

#ifdef USE_ORIGINAL
ir_node *
new_d_Div(dbg_info *db, ir_node *memop, ir_node *op1, ir_node *op2, ir_mode *mode, op_pin_state state) {
	ir_node *res;
	res = new_bd_Div(db, current_ir_graph->current_block, memop, op1, op2, mode, state);
#if PRECISE_EXC_CONTEXT
	firm_alloc_frag_arr(res, op_Div, &res->attr.except.frag_arr);
#endif

	return res;
}  /* new_d_Div */
#endif

ir_node *
new_d_DivRL(dbg_info *db, ir_node *memop, ir_node *op1, ir_node *op2, ir_mode *mode, op_pin_state state) {
	ir_node *res;
	res = new_bd_DivRL(db, current_ir_graph->current_block, memop, op1, op2, mode, state);
#if PRECISE_EXC_CONTEXT
	firm_alloc_frag_arr(res, op_Div, &res->attr.except.frag_arr);
#endif

	return res;
}  /* new_d_DivRL */

ir_node *
new_d_Mod(dbg_info *db, ir_node *memop, ir_node *op1, ir_node *op2, ir_mode *mode, op_pin_state state) {
	ir_node *res;
	res = new_bd_Mod(db, current_ir_graph->current_block, memop, op1, op2, mode, state);
#if PRECISE_EXC_CONTEXT
	firm_alloc_frag_arr(res, op_Mod, &res->attr.except.frag_arr);
#endif

	return res;
}  /* new_d_Mod */

NEW_D_BINOP(And)
NEW_D_BINOP(Or)
NEW_D_BINOP(Eor)
NEW_D_UNOP(Not)
NEW_D_BINOP(Shl)
NEW_D_BINOP(Shr)
NEW_D_BINOP(Shrs)
NEW_D_BINOP(Rotl)
NEW_D_UNOP(Abs)
NEW_D_BINOP(Carry)
NEW_D_BINOP(Borrow)

#ifdef USE_ORIGINAL
ir_node *
new_d_Cmp(dbg_info *db, ir_node *op1, ir_node *op2) {
	return new_bd_Cmp(db, current_ir_graph->current_block, op1, op2);
}  /* new_d_Cmp */

ir_node *
new_d_Jmp(dbg_info *db) {
	return new_bd_Jmp(db, current_ir_graph->current_block);
}  /* new_d_Jmp */

ir_node *
new_d_IJmp(dbg_info *db, ir_node *tgt) {
	return new_bd_IJmp(db, current_ir_graph->current_block, tgt);
}  /* new_d_IJmp */

ir_node *
new_d_Cond(dbg_info *db, ir_node *c) {
	return new_bd_Cond(db, current_ir_graph->current_block, c);
}  /* new_d_Cond */
#endif

ir_node *
new_d_Call(dbg_info *db, ir_node *store, ir_node *callee, int arity, ir_node **in,
           ir_type *tp) {
	ir_node *res;
	res = new_bd_Call(db, current_ir_graph->current_block,
	                  store, callee, arity, in, tp);
#if PRECISE_EXC_CONTEXT
	firm_alloc_frag_arr(res, op_Call, &res->attr.call.exc.frag_arr);
#endif

	return res;
}  /* new_d_Call */

ir_node *
new_d_Return(dbg_info *db, ir_node* store, int arity, ir_node **in) {
	return new_bd_Return(db, current_ir_graph->current_block,
	                     store, arity, in);
}  /* new_d_Return */

ir_node *
new_d_Load(dbg_info *db, ir_node *store, ir_node *addr, ir_mode *mode) {
	ir_node *res;
	res = new_bd_Load(db, current_ir_graph->current_block,
	                  store, addr, mode);
#if PRECISE_EXC_CONTEXT
	firm_alloc_frag_arr(res, op_Load, &res->attr.load.exc.frag_arr);
#endif

	return res;
}  /* new_d_Load */

ir_node *
new_d_Store(dbg_info *db, ir_node *store, ir_node *addr, ir_node *val) {
	ir_node *res;
	res = new_bd_Store(db, current_ir_graph->current_block,
	                   store, addr, val);
#if PRECISE_EXC_CONTEXT
	firm_alloc_frag_arr(res, op_Store, &res->attr.store.exc.frag_arr);
#endif

	return res;
}  /* new_d_Store */

ir_node *
new_d_Alloc(dbg_info *db, ir_node *store, ir_node *size, ir_type *alloc_type,
            ir_where_alloc where) {
	ir_node *res;
	res = new_bd_Alloc(db, current_ir_graph->current_block,
	                   store, size, alloc_type, where);
#if PRECISE_EXC_CONTEXT
	firm_alloc_frag_arr(res, op_Alloc, &res->attr.alloc.exc.frag_arr);
#endif

	return res;
}  /* new_d_Alloc */

ir_node *
new_d_Free(dbg_info *db, ir_node *store, ir_node *ptr,
           ir_node *size, ir_type *free_type, ir_where_alloc where) {
	return new_bd_Free(db, current_ir_graph->current_block,
	                   store, ptr, size, free_type, where);
}

ir_node *
new_d_simpleSel(dbg_info *db, ir_node *store, ir_node *objptr, ir_entity *ent)
/* GL: objptr was called frame before.  Frame was a bad choice for the name
   as the operand could as well be a pointer to a dynamic object. */
{
	return new_bd_Sel(db, current_ir_graph->current_block,
	                  store, objptr, 0, NULL, ent);
}  /* new_d_simpleSel */

ir_node *
new_d_Sel(dbg_info *db, ir_node *store, ir_node *objptr, int n_index, ir_node **index, ir_entity *sel) {
	return new_bd_Sel(db, current_ir_graph->current_block,
	                  store, objptr, n_index, index, sel);
}  /* new_d_Sel */

ir_node *
new_d_SymConst_type(dbg_info *db, ir_mode *mode, symconst_symbol value, symconst_kind kind, ir_type *tp) {
	return new_bd_SymConst_type(db, get_irg_start_block(current_ir_graph), mode,
	                            value, kind, tp);
}  /* new_d_SymConst_type */

ir_node *
new_d_SymConst(dbg_info *db, ir_mode *mode, symconst_symbol value, symconst_kind kind) {
	return new_bd_SymConst_type(db, get_irg_start_block(current_ir_graph), mode,
	                            value, kind, firm_unknown_type);
}  /* new_d_SymConst */

ir_node *
new_d_Sync(dbg_info *db, int arity, ir_node *in[]) {
	return new_rd_Sync(db, current_ir_graph, current_ir_graph->current_block, arity, in);
}  /* new_d_Sync */

ir_node *
new_d_Confirm(dbg_info *db, ir_node *val, ir_node *bound, pn_Cmp cmp) {
	return new_bd_Confirm(db, current_ir_graph->current_block,
	                      val, bound, cmp);
}  /* new_d_Confirm */

ir_node *
new_d_Unknown(ir_mode *m) {
	return new_bd_Unknown(m);
}  /* new_d_Unknown */

ir_node *
new_d_CallBegin(dbg_info *db, ir_node *call) {
	return new_bd_CallBegin(db, current_ir_graph->current_block, call);
}  /* new_d_CallBegin */

ir_node *
new_d_EndReg(dbg_info *db) {
	return new_bd_EndReg(db, current_ir_graph->current_block);
}  /* new_d_EndReg */

ir_node *
new_d_EndExcept(dbg_info *db) {
	return new_bd_EndExcept(db, current_ir_graph->current_block);
}  /* new_d_EndExcept */

ir_node *
new_d_Break(dbg_info *db) {
	return new_bd_Break(db, current_ir_graph->current_block);
}  /* new_d_Break */

ir_node *
new_d_Filter(dbg_info *db, ir_node *arg, ir_mode *mode, long proj) {
	return new_bd_Filter(db, current_ir_graph->current_block,
	                     arg, mode, proj);
}  /* new_d_Filter */

ir_node *
new_d_Mux(dbg_info *db, ir_node *sel, ir_node *ir_false,
          ir_node *ir_true, ir_mode *mode) {
	return new_bd_Mux(db, current_ir_graph->current_block,
	                  sel, ir_false, ir_true, mode);
}  /* new_d_Mux */

ir_node *new_d_CopyB(dbg_info *db,ir_node *store,
    ir_node *dst, ir_node *src, ir_type *data_type) {
	ir_node *res;
	res = new_bd_CopyB(db, current_ir_graph->current_block,
	                   store, dst, src, data_type);
#if PRECISE_EXC_CONTEXT
	firm_alloc_frag_arr(res, op_CopyB, &res->attr.copyb.exc.frag_arr);
#endif
	return res;
}  /* new_d_CopyB */

ir_node *
new_d_InstOf(dbg_info *db, ir_node *store, ir_node *objptr, ir_type *type) {
	return new_bd_InstOf(db, current_ir_graph->current_block,
	                     store, objptr, type);
}  /* new_d_InstOf */

ir_node *
new_d_Raise(dbg_info *db, ir_node *store, ir_node *obj) {
	return new_bd_Raise(db, current_ir_graph->current_block, store, obj);
}  /* new_d_Raise */

ir_node *new_d_Bound(dbg_info *db,ir_node *store,
    ir_node *idx, ir_node *lower, ir_node *upper) {
	ir_node *res;
	res = new_bd_Bound(db, current_ir_graph->current_block,
	                   store, idx, lower, upper);
#if PRECISE_EXC_CONTEXT
	firm_alloc_frag_arr(res, op_Bound, &res->attr.bound.exc.frag_arr);
#endif
	return res;
}  /* new_d_Bound */

ir_node *
new_d_Pin(dbg_info *db, ir_node *node) {
	return new_bd_Pin(db, current_ir_graph->current_block, node);
}  /* new_d_Pin */

ir_node *
new_d_ASM(dbg_info *db, int arity, ir_node *in[], ir_asm_constraint *inputs,
          int n_outs, ir_asm_constraint *outputs,
          int n_clobber, ident *clobber[], ident *asm_text) {
	return new_bd_ASM(db, current_ir_graph->current_block, arity, in, inputs, n_outs, outputs, n_clobber, clobber, asm_text);
}  /* new_d_ASM */

/* ********************************************************************* */
/* Comfortable interface with automatic Phi node construction.           */
/* (Uses also constructors of ?? interface, except new_Block.            */
/* ********************************************************************* */

/*  Block construction */
/* immature Block without predecessors */
ir_node *
new_d_immBlock(dbg_info *db) {
	ir_node *res;

	assert(get_irg_phase_state(current_ir_graph) == phase_building);
	/* creates a new dynamic in-array as length of in is -1 */
	res = new_ir_node(db, current_ir_graph, NULL, op_Block, mode_BB, -1, NULL);

	/* macroblock head */
	res->in[0] = res;

	res->attr.block.is_matured  = 0;
	res->attr.block.is_dead     = 0;
	res->attr.block.is_mb_head  = 1;
	res->attr.block.has_label   = 0;
	res->attr.block.irg         = current_ir_graph;
	res->attr.block.backedge    = NULL;
	res->attr.block.in_cg       = NULL;
	res->attr.block.cg_backedge = NULL;
	res->attr.block.extblk      = NULL;
	res->attr.block.region      = NULL;
	res->attr.block.mb_depth    = 0;
	res->attr.block.label       = 0;

	set_Block_block_visited(res, 0);

	/* Create and initialize array for Phi-node construction. */
	res->attr.block.graph_arr = NEW_ARR_D(ir_node *, current_ir_graph->obst,
	                                      current_ir_graph->n_loc);
	memset(res->attr.block.graph_arr, 0, sizeof(ir_node *)*current_ir_graph->n_loc);

	/* Immature block may not be optimized! */
	IRN_VRFY_IRG(res, current_ir_graph);

	return res;
}  /* new_d_immBlock */

ir_node *
new_immBlock(void) {
	return new_d_immBlock(NULL);
}  /* new_immBlock */

/* immature PartBlock with its predecessors */
ir_node *
new_d_immPartBlock(dbg_info *db, ir_node *pred_jmp) {
	ir_node *res = new_d_immBlock(db);
	ir_node *blk = get_nodes_block(pred_jmp);

	res->in[0] = blk->in[0];
	assert(res->in[0] != NULL);
	add_immBlock_pred(res, pred_jmp);

	res->attr.block.is_mb_head = 0;
	res->attr.block.mb_depth = blk->attr.block.mb_depth + 1;

	return res;
}  /* new_d_immPartBlock */

ir_node *
new_immPartBlock(ir_node *pred_jmp) {
	return new_d_immPartBlock(NULL, pred_jmp);
}  /* new_immPartBlock */

/* add an edge to a jmp/control flow node */
void
add_immBlock_pred(ir_node *block, ir_node *jmp) {
	int n = ARR_LEN(block->in) - 1;

	assert(!block->attr.block.is_matured && "Error: Block already matured!\n");
	assert(block->attr.block.is_mb_head && "Error: Cannot add a predecessor to a PartBlock");
	assert(is_ir_node(jmp));

	ARR_APP1(ir_node *, block->in, jmp);
	/* Call the hook */
	hook_set_irn_n(block, n, jmp, NULL);
}  /* add_immBlock_pred */

/* changing the current block */
void
set_cur_block(ir_node *target) {
	current_ir_graph->current_block = target;
}  /* set_cur_block */

/* ************************ */
/* parameter administration */

/* get a value from the parameter array from the current block by its index */
ir_node *
get_d_value(dbg_info *db, int pos, ir_mode *mode) {
	ir_graph *irg = current_ir_graph;
	assert(get_irg_phase_state(irg) == phase_building);
	inc_irg_visited(irg);
	(void) db;

	assert(pos >= 0);

	return get_r_value_internal(irg->current_block, pos + 1, mode);
}  /* get_d_value */

/* get a value from the parameter array from the current block by its index */
ir_node *
get_value(int pos, ir_mode *mode) {
	return get_d_value(NULL, pos, mode);
}  /* get_value */

/* set a value at position pos in the parameter array from the current block */
void
set_value(int pos, ir_node *value) {
	ir_graph *irg = current_ir_graph;
	assert(get_irg_phase_state(irg) == phase_building);
	assert(pos >= 0);
	assert(pos+1 < irg->n_loc);
	assert(is_ir_node(value));
	irg->current_block->attr.block.graph_arr[pos + 1] = value;
}  /* set_value */

/* Find the value number for a node in the current block.*/
int
find_value(ir_node *value) {
	int i;
	ir_node *bl = current_ir_graph->current_block;

	for (i = ARR_LEN(bl->attr.block.graph_arr) - 1; i >= 1; --i)
		if (bl->attr.block.graph_arr[i] == value)
			return i - 1;
	return -1;
}  /* find_value */

/* get the current store */
ir_node *
get_store(void) {
	ir_graph *irg = current_ir_graph;

	assert(get_irg_phase_state(irg) == phase_building);
	/* GL: one could call get_value instead */
	inc_irg_visited(irg);
	return get_r_value_internal(irg->current_block, 0, mode_M);
}  /* get_store */

/* set the current store: handles automatic Sync construction for Load nodes */
void
set_store(ir_node *store) {
	ir_node *load, *pload, *pred, *in[2];

	assert(get_irg_phase_state(current_ir_graph) == phase_building);
	/* Beware: due to dead code elimination, a store might become a Bad node even in
	   the construction phase. */
	assert((get_irn_mode(store) == mode_M || is_Bad(store)) && "storing non-memory node");

	if (get_opt_auto_create_sync()) {
		/* handle non-volatile Load nodes by automatically creating Sync's */
		load = skip_Proj(store);
		if (is_Load(load) && get_Load_volatility(load) == volatility_non_volatile) {
			pred = get_Load_mem(load);

			if (is_Sync(pred)) {
				/* a Load after a Sync: move it up */
				ir_node *mem = skip_Proj(get_Sync_pred(pred, 0));

				set_Load_mem(load, get_memop_mem(mem));
				add_Sync_pred(pred, store);
				store = pred;
			} else {
				pload = skip_Proj(pred);
				if (is_Load(pload) && get_Load_volatility(pload) == volatility_non_volatile) {
					/* a Load after a Load: create a new Sync */
					set_Load_mem(load, get_Load_mem(pload));

					in[0] = pred;
					in[1] = store;
					store = new_Sync(2, in);
				}
			}
		}
	}
	current_ir_graph->current_block->attr.block.graph_arr[0] = store;
}  /* set_store */

void
keep_alive(ir_node *ka) {
	add_End_keepalive(get_irg_end(current_ir_graph), ka);
}  /* keep_alive */

/* --- Useful access routines --- */
/* Returns the current block of the current graph.  To set the current
   block use set_cur_block. */
ir_node *get_cur_block(void) {
	return get_irg_current_block(current_ir_graph);
}  /* get_cur_block */

/* Returns the frame type of the current graph */
ir_type *get_cur_frame_type(void) {
	return get_irg_frame_type(current_ir_graph);
}  /* get_cur_frame_type */


/* ********************************************************************* */
/* initialize */

/* call once for each run of the library */
void
firm_init_cons(uninitialized_local_variable_func_t *func) {
	default_initialize_local_variable = func;
}  /* firm_init_cons */

void
irp_finalize_cons(void) {
	int i;
	for (i = get_irp_n_irgs() - 1; i >= 0; --i) {
		irg_finalize_cons(get_irp_irg(i));
	}
	irp->phase_state = phase_high;
}  /* irp_finalize_cons */

#ifdef USE_ORIGINAL
ir_node *new_Block(int arity, ir_node **in) {
	return new_d_Block(NULL, arity, in);
}
ir_node *new_Start(void) {
	return new_d_Start(NULL);
}
ir_node *new_End(void) {
	return new_d_End(NULL);
}
ir_node *new_Jmp(void) {
	return new_d_Jmp(NULL);
}
ir_node *new_IJmp(ir_node *tgt) {
	return new_d_IJmp(NULL, tgt);
}
ir_node *new_Cond(ir_node *c) {
	return new_d_Cond(NULL, c);
}
#endif
ir_node *new_Return(ir_node *store, int arity, ir_node *in[]) {
	return new_d_Return(NULL, store, arity, in);
}
ir_node *new_Const(tarval *con) {
	return new_d_Const(NULL, con);
}

ir_node *new_Const_long(ir_mode *mode, long value) {
	return new_d_Const_long(NULL, mode, value);
}

#ifdef USE_ORIGINAL
ir_node *new_Const_type(tarval *con, ir_type *tp) {
	return new_d_Const_type(NULL, con, tp);
}
#endif

ir_node *new_SymConst_type(ir_mode *mode, symconst_symbol value, symconst_kind kind, ir_type *type) {
	return new_d_SymConst_type(NULL, mode, value, kind, type);
}
ir_node *new_SymConst(ir_mode *mode, symconst_symbol value, symconst_kind kind) {
	return new_d_SymConst(NULL, mode, value, kind);
}
ir_node *new_simpleSel(ir_node *store, ir_node *objptr, ir_entity *ent) {
	return new_d_simpleSel(NULL, store, objptr, ent);
}
ir_node *new_Sel(ir_node *store, ir_node *objptr, int arity, ir_node **in,
                 ir_entity *ent) {
	return new_d_Sel(NULL, store, objptr, arity, in, ent);
}
ir_node *new_Call(ir_node *store, ir_node *callee, int arity, ir_node **in,
                  ir_type *tp) {
	return new_d_Call(NULL, store, callee, arity, in, tp);
}
#ifdef USE_ORIGINAL
ir_node *new_Add(ir_node *op1, ir_node *op2, ir_mode *mode) {
	return new_d_Add(NULL, op1, op2, mode);
}
#endif
ir_node *new_Sub(ir_node *op1, ir_node *op2, ir_mode *mode) {
	return new_d_Sub(NULL, op1, op2, mode);
}
ir_node *new_Minus(ir_node *op,  ir_mode *mode) {
	return new_d_Minus(NULL, op, mode);
}
ir_node *new_Mul(ir_node *op1, ir_node *op2, ir_mode *mode) {
	return new_d_Mul(NULL, op1, op2, mode);
}
ir_node *new_Mulh(ir_node *op1, ir_node *op2, ir_mode *mode) {
	return new_d_Mulh(NULL, op1, op2, mode);
}
ir_node *new_Quot(ir_node *memop, ir_node *op1, ir_node *op2, ir_mode *mode, op_pin_state state) {
	return new_d_Quot(NULL, memop, op1, op2, mode, state);
}
ir_node *new_DivMod(ir_node *memop, ir_node *op1, ir_node *op2, ir_mode *mode, op_pin_state state) {
	return new_d_DivMod(NULL, memop, op1, op2, mode, state);
}
#ifdef USE_ORIGINAL
ir_node *new_Div(ir_node *memop, ir_node *op1, ir_node *op2, ir_mode *mode, op_pin_state state) {
	return new_d_Div(NULL, memop, op1, op2, mode, state);
}
#endif
ir_node *new_DivRL(ir_node *memop, ir_node *op1, ir_node *op2, ir_mode *mode, op_pin_state state) {
	return new_d_DivRL(NULL, memop, op1, op2, mode, state);
}
ir_node *new_Mod(ir_node *memop, ir_node *op1, ir_node *op2, ir_mode *mode, op_pin_state state) {
	return new_d_Mod(NULL, memop, op1, op2, mode, state);
}
ir_node *new_Abs(ir_node *op, ir_mode *mode) {
	return new_d_Abs(NULL, op, mode);
}
ir_node *new_And(ir_node *op1, ir_node *op2, ir_mode *mode) {
	return new_d_And(NULL, op1, op2, mode);
}
ir_node *new_Or(ir_node *op1, ir_node *op2, ir_mode *mode) {
	return new_d_Or(NULL, op1, op2, mode);
}
ir_node *new_Eor(ir_node *op1, ir_node *op2, ir_mode *mode) {
	return new_d_Eor(NULL, op1, op2, mode);
}
ir_node *new_Not(ir_node *op,                ir_mode *mode) {
	return new_d_Not(NULL, op, mode);
}
ir_node *new_Shl(ir_node *op,  ir_node *k,   ir_mode *mode) {
	return new_d_Shl(NULL, op, k, mode);
}
ir_node *new_Shr(ir_node *op,  ir_node *k,   ir_mode *mode) {
	return new_d_Shr(NULL, op, k, mode);
}
ir_node *new_Shrs(ir_node *op,  ir_node *k,   ir_mode *mode) {
	return new_d_Shrs(NULL, op, k, mode);
}
ir_node *new_Rotl(ir_node *op,  ir_node *k,   ir_mode *mode) {
	return new_d_Rotl(NULL, op, k, mode);
}
ir_node *new_Carry(ir_node *op1, ir_node *op2, ir_mode *mode) {
	return new_d_Carry(NULL, op1, op2, mode);
}
ir_node *new_Borrow(ir_node *op1, ir_node *op2, ir_mode *mode) {
	return new_d_Borrow(NULL, op1, op2, mode);
}
#ifdef USE_ORIGINAL
ir_node *new_Cmp(ir_node *op1, ir_node *op2) {
	return new_d_Cmp(NULL, op1, op2);
}
#endif
ir_node *new_Conv(ir_node *op, ir_mode *mode) {
	return new_d_Conv(NULL, op, mode);
}
ir_node *new_strictConv(ir_node *op, ir_mode *mode) {
	return new_d_strictConv(NULL, op, mode);
}
#ifdef USE_ORIGINAL
ir_node *new_Cast(ir_node *op, ir_type *to_tp) {
	return new_d_Cast(NULL, op, to_tp);
}
#endif
ir_node *new_Phi(int arity, ir_node **in, ir_mode *mode) {
	return new_d_Phi(NULL, arity, in, mode);
}
ir_node *new_Load(ir_node *store, ir_node *addr, ir_mode *mode) {
	return new_d_Load(NULL, store, addr, mode);
}
ir_node *new_Store(ir_node *store, ir_node *addr, ir_node *val) {
	return new_d_Store(NULL, store, addr, val);
}
ir_node *new_Alloc(ir_node *store, ir_node *size, ir_type *alloc_type,
                   ir_where_alloc where) {
	return new_d_Alloc(NULL, store, size, alloc_type, where);
}
ir_node *new_Free(ir_node *store, ir_node *ptr, ir_node *size,
                  ir_type *free_type, ir_where_alloc where) {
	return new_d_Free(NULL, store, ptr, size, free_type, where);
}
ir_node *new_Sync(int arity, ir_node *in[]) {
	return new_d_Sync(NULL, arity, in);
}
#ifdef USE_ORIGINAL
ir_node *new_Proj(ir_node *arg, ir_mode *mode, long proj) {
	return new_d_Proj(NULL, arg, mode, proj);
}
#endif
ir_node *new_defaultProj(ir_node *arg, long max_proj) {
	return new_d_defaultProj(NULL, arg, max_proj);
}
#ifdef USE_ORIGINAL
ir_node *new_Tuple(int arity, ir_node **in) {
	return new_d_Tuple(NULL, arity, in);
}
ir_node *new_Id(ir_node *val, ir_mode *mode) {
	return new_d_Id(NULL, val, mode);
}
#endif
ir_node *new_Bad(void) {
	return get_irg_bad(current_ir_graph);
}
ir_node *new_Confirm(ir_node *val, ir_node *bound, pn_Cmp cmp) {
	return new_d_Confirm(NULL, val, bound, cmp);
}
ir_node *new_Unknown(ir_mode *m) {
	return new_d_Unknown(m);
}
ir_node *new_CallBegin(ir_node *callee) {
	return new_d_CallBegin(NULL, callee);
}
ir_node *new_EndReg(void) {
	return new_d_EndReg(NULL);
}
ir_node *new_EndExcept(void) {
	return new_d_EndExcept(NULL);
}
ir_node *new_Break(void) {
	return new_d_Break(NULL);
}
ir_node *new_Filter(ir_node *arg, ir_mode *mode, long proj) {
	return new_d_Filter(NULL, arg, mode, proj);
}
ir_node *new_NoMem(void) {
	return get_irg_no_mem(current_ir_graph);
}
ir_node *new_Mux(ir_node *sel, ir_node *ir_false, ir_node *ir_true, ir_mode *mode) {
	return new_d_Mux(NULL, sel, ir_false, ir_true, mode);
}
ir_node *new_CopyB(ir_node *store, ir_node *dst, ir_node *src, ir_type *data_type) {
	return new_d_CopyB(NULL, store, dst, src, data_type);
}
ir_node *new_InstOf(ir_node *store, ir_node *objptr, ir_type *ent) {
	return new_d_InstOf(NULL, store, objptr, ent);
}
ir_node *new_Raise(ir_node *store, ir_node *obj) {
	return new_d_Raise(NULL, store, obj);
}
ir_node *new_Bound(ir_node *store, ir_node *idx, ir_node *lower, ir_node *upper) {
	return new_d_Bound(NULL, store, idx, lower, upper);
}
ir_node *new_Pin(ir_node *node) {
	return new_d_Pin(NULL, node);
}
ir_node *new_ASM(int arity, ir_node *in[], ir_asm_constraint *inputs,
                 int n_outs, ir_asm_constraint *outputs,
                 int n_clobber, ident *clobber[], ident *asm_text) {
	return new_d_ASM(NULL, arity, in, inputs, n_outs, outputs, n_clobber, clobber, asm_text);
}

/* create a new anchor node */
ir_node *new_Anchor(ir_graph *irg) {
	ir_node *in[anchor_last];
	memset(in, 0, sizeof(in));
	return new_ir_node(NULL, irg, NULL, op_Anchor, mode_ANY, anchor_last, in);
}
