/* Copyright (C) 1998 - 2000 by Universitaet Karlsruhe
** All rights reserved.
**
** Authors: Martin Trapp, Christian Schaefer
**
** ircons.c: basic and more detailed irnode constructors
**           store, block and parameter administration ,
** Adapted to extended FIRM nodes (exceptions...) and commented
**   by Goetz Lindenmaier
*/

# include "ircons.h"
# include "array.h"
# include "iropt.h"
/* memset belongs to string.h */
# include "string.h"

/* irnode constructor                                             */
/* create a new irnode in irg, with an op, mode, arity and        */
/* some incoming irnodes                                          */
/* this constructor is used in every specified irnode constructor */
inline ir_node *
new_ir_node (ir_graph *irg, ir_node *block, ir_op *op, ir_mode *mode,
	     int arity, ir_node **in)
{
  ir_node *res;
  int node_size = offsetof (ir_node, attr) +  op->attr_size;

  res = (ir_node *) obstack_alloc (irg->obst, node_size);

  res->kind = k_ir_node;
  res->op = op;
  res->mode = mode;
  res->visit = 0;
  res->link = NULL;
  if (arity < 0) {
    res->in = NEW_ARR_F (ir_node *, 1);
  } else {
    res->in = NEW_ARR_D (ir_node *, irg->obst, (arity+1));
    memcpy (&res->in[1], in, sizeof (ir_node *) * arity);
  }
  res->in[0] = block;
  return res;
}




/*********************************************** */
/** privat interfaces, for professional use only */

/*CS*/
inline ir_node *
new_r_Block (ir_graph *irg,  int arity, ir_node **in)
{
  ir_node *res;

  return res;

}

ir_node *
new_r_Start (ir_graph *irg, ir_node *block)
{
  ir_node *res;

  res = new_ir_node (irg, block, op_Start, mode_T, 0, NULL);

  ir_vrfy (res);
  return res;
}


ir_node *
new_r_End (ir_graph *irg, ir_node *block)
{
  ir_node *res;

  res = new_ir_node (irg, block, op_End, mode_X, -1, NULL);

  ir_vrfy (res);
  return res;

}


/* Creates a Phi node with 0 predecessors */
inline ir_node *
new_r_Phi0 (ir_graph *irg, ir_node *block, ir_mode *mode)
{
  ir_node *res;

  res = new_ir_node (irg, block, op_Phi, mode, 0, NULL);

  /* GL I'm not sure whether we should optimize this guy. *
     res = optimize (res); ??? */
  ir_vrfy (res);
  return res;
}

/* Creates a Phi node with all predecessors.  Calling this constructor
   is only allowed if the corresponding block is mature.  */
ir_node *
new_r_Phi (ir_graph *irg, ir_node *block, int arity, ir_node **in, ir_mode *mode)
{
  ir_node *res;

  assert( get_Block_matured(block) );
  assert( get_irn_arity(block) == arity );

  res = new_ir_node (irg, block, op_Phi, mode, arity, in);

  res = optimize (res);
  ir_vrfy (res);
  return res;
}

/* This is a stack used for allocating and deallocating nodes in
   new_r_Phi_in.  The original implementation used the obstack
   to model this stack, now it is explicit.  This reduces side effects.
*/
#if USE_EXPICIT_PHI_IN_STACK
Phi_in_stack *
new_Phi_in_stack() {
  Phi_in_stack *res;

  res = (Phi_in_stack *) malloc ( sizeof (Phi_in_stack));

  res->stack = NEW_ARR_F (ir_node *, 1);
  res->pos = 0;

  return res;
}


void free_to_Phi_in_stack(ir_node *phi) {
  assert(get_irn_opcode(phi) == iro_Phi);

  if (ARR_LEN(current_ir_graph->Phi_in_stack->stack) ==
      current_ir_graph->Phi_in_stack->pos)
    ARR_APP1 (ir_node *, current_ir_graph->Phi_in_stack->stack, phi);
  else
    current_ir_graph->Phi_in_stack->stack[current_ir_graph->Phi_in_stack->pos] = phi;

  (current_ir_graph->Phi_in_stack->pos)++;
}

ir_node *
alloc_or_pop_from_Phi_in_stack(ir_graph *irg, ir_node *block, ir_mode *mode,
	     int arity, ir_node **in) {
  ir_node *res;
  ir_node **stack = current_ir_graph->Phi_in_stack->stack;
  int pos = current_ir_graph->Phi_in_stack->pos;


  if (pos == 0) {
    /* We need to allocate a new node */
    res = new_ir_node (irg, block, op_Phi, mode, arity, in);
  } else {
    /* reuse the old node and initialize it again. */
    res = stack[pos-1];

    assert (res->kind == k_ir_node);
    assert (res->op == op_Phi);
    res->mode = mode;
    res->visit = 0;
    res->link = NULL;
    assert (arity >= 0);
    /* ???!!! How to free the old in array??  */
    res->in = NEW_ARR_D (ir_node *, irg->obst, (arity+1));
    res->in[0] = block;
    memcpy (&res->in[1], in, sizeof (ir_node *) * arity);

    (current_ir_graph->Phi_in_stack->pos)--;
  }
  return res;
}
#endif



/* Creates a Phi node with a given, fixed array **in of predecessors.
   If the Phi node is unnecessary, as the same value reaches the block
   through all control flow paths, it is eliminated and the value
   returned directly.  This constructor is only intended for use in
   the automatic Phi node generation triggered by get_value or mature.
   The implementation is quite tricky and depends on the fact, that
   the nodes are allocated on a stack:
   The in array contains predecessors and NULLs.  The NULLs appear,
   if get_r_value_internal, that computed the predecessors, reached
   the same block on two paths.  In this case the same value reaches
   this block on both paths, there is no definition in between.  We need
   not allocate a Phi where these path's merge, but we have to communicate
   this fact to the caller.  This happens by returning a pointer to the
   node the caller _will_ allocate.  (Yes, we predict the address. We can
   do so because the nodes are allocated on the obstack.)  The caller then
   finds a pointer to itself and, when this routine is called again,
   eliminates itself.
   */
inline ir_node *
new_r_Phi_in (ir_graph *irg, ir_node *block, ir_mode *mode,
	      ir_node **in, int ins)
{
  int i;
  ir_node *res, *known;

  /* allocate a new node on the obstack.
     This can return a node to which some of the pointers in the in-array
     already point.
     Attention: the constructor copies the in array, i.e., the later changes
     to the array in this routine do not affect the constructed node!  If
     the in array contains NULLs, there will be missing predecessors in the
     returned node.
     Is this a possible internal state of the Phi node generation? */
#if USE_EXPICIT_PHI_IN_STACK
  res = known = alloc_or_pop_from_Phi_in_stack(irg, block, mode, ins, in);
#else
  res = known = new_ir_node (irg, block, op_Phi, mode, ins, in);
#endif
  /* The in-array can contain NULLs.  These were returned by get_r_value_internal
     if it reached the same block/definition on a second path.
     The NULLs are replaced by the node itself to simplify the test in the
     next loop. */
  for (i=0;  i < ins;  ++i)
    if (in[i] == NULL) in[i] = res;

  /* This loop checks whether the Phi has more than one predecessor.
     If so, it is a real Phi node and we break the loop.  Else the
     Phi node merges the same definition on several paths and therefore
     is not needed. */
  for (i=0;  i < ins;  ++i)
  {
    if (in[i]==res || in[i]==known) continue;

    if (known==res)
      known = in[i];
    else
      break;
  }

  /* i==ins: there is at most one predecessor, we don't need a phi node. */
  if (i==ins) {
#if USE_EXPICIT_PHI_IN_STACK
    free_to_Phi_in_stack(res);
#else
    obstack_free (current_ir_graph->obst, res);
#endif
    res = known;
  } else {
    res = optimize (res);
    ir_vrfy (res);
  }

  /* return the pointer to the Phi node.  This node might be deallocated! */
  return res;
}

ir_node *
new_r_Const (ir_graph *irg, ir_node *block, ir_mode *mode, tarval *con)
{
  ir_node *res;
  res = new_ir_node (irg, block, op_Const, mode, 0, NULL);
  res->attr.con = con;
  res = optimize (res);
  ir_vrfy (res);

#if 0
  res = local_optimize_newby (res);
# endif

  return res;
}

ir_node *
new_r_Id (ir_graph *irg, ir_node *block, ir_node *val, ir_mode *mode)
{
  ir_node *in[1] = {val};
  ir_node *res;
  res = new_ir_node (irg, block, op_Id, mode, 1, in);
  res = optimize (res);
  ir_vrfy (res);
  return res;
}

ir_node *
new_r_Proj (ir_graph *irg, ir_node *block, ir_node *arg, ir_mode *mode, long proj)
{
  ir_node *in[1] = {arg};
  ir_node *res;
  res = new_ir_node (irg, block, op_Proj, mode, 1, in);
  res->attr.proj = proj;
  res = optimize (res);
  ir_vrfy (res);
  return res;

}

ir_node *
new_r_Conv (ir_graph *irg, ir_node *block, ir_node *op, ir_mode *mode)
{
  ir_node *in[1] = {op};
  ir_node *res;
  res = new_ir_node (irg, block, op_Conv, mode, 1, in);
  res = optimize (res);
  ir_vrfy (res);
  return res;

}

ir_node *
new_r_Tuple (ir_graph *irg, ir_node *block, int arity, ir_node **in)
{
  ir_node *res;

  res = new_ir_node (irg, block, op_Tuple, mode_T, arity, in);
  res = optimize (res);
  ir_vrfy (res);
  return res;
}

inline ir_node *
new_r_Add (ir_graph *irg, ir_node *block,
	   ir_node *op1, ir_node *op2, ir_mode *mode)
{
  ir_node *in[2] = {op1, op2};
  ir_node *res;
  res = new_ir_node (irg, block, op_Add, mode, 2, in);
  res = optimize (res);
  ir_vrfy (res);
  return res;
}

inline ir_node *
new_r_Sub (ir_graph *irg, ir_node *block,
	   ir_node *op1, ir_node *op2, ir_mode *mode)
{
  ir_node *in[2] = {op1, op2};
  ir_node *res;
  res = new_ir_node (irg, block, op_Sub, mode, 2, in);
  res = optimize (res);
  ir_vrfy (res);
  return res;
}

inline ir_node *
new_r_Minus (ir_graph *irg, ir_node *block,
	     ir_node *op,  ir_mode *mode)
{
  ir_node *in[1] = {op};
  ir_node *res;
  res = new_ir_node (irg, block, op_Minus, mode, 1, in);
  res = optimize (res);
  ir_vrfy (res);
  return res;
}

inline ir_node *
new_r_Mul (ir_graph *irg, ir_node *block,
	   ir_node *op1, ir_node *op2, ir_mode *mode)
{
  ir_node *in[2] = {op1, op2};
  ir_node *res;
  res = new_ir_node (irg, block, op_Mul, mode, 2, in);
  res = optimize (res);
  ir_vrfy (res);
  return res;
}

inline ir_node *
new_r_Quot (ir_graph *irg, ir_node *block,
     	    ir_node *memop, ir_node *op1, ir_node *op2)
{
  ir_node *in[3] = {memop, op1, op2};
  ir_node *res;
  res = new_ir_node (irg, block, op_Quot, mode_T, 2, in);
  res = optimize (res);
  ir_vrfy (res);
  return res;
}

inline ir_node *
new_r_DivMod (ir_graph *irg, ir_node *block,
	      ir_node *memop, ir_node *op1, ir_node *op2)
{
  ir_node *in[3] = {memop, op1, op2};
  ir_node *res;
  res = new_ir_node (irg, block, op_DivMod, mode_T, 2, in);
  res = optimize (res);
  ir_vrfy (res);
  return res;
}

inline ir_node *
new_r_Div (ir_graph *irg, ir_node *block,
    	   ir_node *memop, ir_node *op1, ir_node *op2)
{
  ir_node *in[3] = {memop, op1, op2};
  ir_node *res;
  res = new_ir_node (irg, block, op_Div, mode_T, 2, in);
  res = optimize (res);
  ir_vrfy (res);
  return res;
}

inline ir_node *
new_r_Mod (ir_graph *irg, ir_node *block,
     	   ir_node *memop, ir_node *op1, ir_node *op2)
{
  ir_node *in[3] = {memop, op1, op2};
  ir_node *res;
  res = new_ir_node (irg, block, op_Mod, mode_T, 2, in);
  res = optimize (res);
  ir_vrfy (res);
  return res;
}

inline ir_node *
new_r_And (ir_graph *irg, ir_node *block,
     	   ir_node *op1, ir_node *op2, ir_mode *mode)
{
  ir_node *in[2] = {op1, op2};
  ir_node *res;
  res = new_ir_node (irg, block, op_And, mode, 2, in);
  res = optimize (res);
  ir_vrfy (res);
  return res;
}

inline ir_node *
new_r_Or (ir_graph *irg, ir_node *block,
     	  ir_node *op1, ir_node *op2, ir_mode *mode)
{
  ir_node *in[2] = {op1, op2};
  ir_node *res;
  res = new_ir_node (irg, block, op_Or, mode, 2, in);
  res = optimize (res);
  ir_vrfy (res);
  return res;
}

inline ir_node *
new_r_Eor (ir_graph *irg, ir_node *block,
     	  ir_node *op1, ir_node *op2, ir_mode *mode)
{
  ir_node *in[2] = {op1, op2};
  ir_node *res;
  res = new_ir_node (irg, block, op_Eor, mode, 2, in);
  res = optimize (res);
  ir_vrfy (res);
  return res;
}

inline ir_node *
new_r_Not    (ir_graph *irg, ir_node *block,
	      ir_node *op, ir_mode *mode)
{
  ir_node *in[1] = {op};
  ir_node *res;
  res = new_ir_node (irg, block, op_Not, mode, 1, in);
  res = optimize (res);
  ir_vrfy (res);
  return res;
}

inline ir_node *
new_r_Shl (ir_graph *irg, ir_node *block,
     	  ir_node *op, ir_node *k, ir_mode *mode)
{
  ir_node *in[2] = {op, k};
  ir_node *res;
  res = new_ir_node (irg, block, op_Shl, mode, 2, in);
  res = optimize (res);
  ir_vrfy (res);
  return res;
}

inline ir_node *
new_r_Shr (ir_graph *irg, ir_node *block,
	   ir_node *op, ir_node *k, ir_mode *mode)
{
  ir_node *in[2] = {op, k};
  ir_node *res;
  res = new_ir_node (irg, block, op_Shr, mode, 2, in);
  res = optimize (res);
  ir_vrfy (res);
  return res;
}

inline ir_node *
new_r_Shrs (ir_graph *irg, ir_node *block,
	   ir_node *op, ir_node *k, ir_mode *mode)
{
  ir_node *in[2] = {op, k};
  ir_node *res;
  res = new_ir_node (irg, block, op_Shrs, mode, 2, in);
  res = optimize (res);
  ir_vrfy (res);
  return res;
}

inline ir_node *
new_r_Rot (ir_graph *irg, ir_node *block,
	   ir_node *op, ir_node *k, ir_mode *mode)
{
  ir_node *in[2] = {op, k};
  ir_node *res;
  res = new_ir_node (irg, block, op_Rot, mode, 2, in);
  res = optimize (res);
  ir_vrfy (res);
  return res;
}

inline ir_node *
new_r_Abs (ir_graph *irg, ir_node *block,
	   ir_node *op, ir_mode *mode)
{
  ir_node *in[1] = {op};
  ir_node *res;
  res = new_ir_node (irg, block, op_Abs, mode, 1, in);
  res = optimize (res);
  ir_vrfy (res);
  return res;
}

inline ir_node *
new_r_Cmp (ir_graph *irg, ir_node *block,
	   ir_node *op1, ir_node *op2)
{
  ir_node *in[2] = {op1, op2};
  ir_node *res;
  res = new_ir_node (irg, block, op_Cmp, mode_T, 2, in);
  res = optimize (res);
  ir_vrfy (res);
  return res;
}

inline ir_node *
new_r_Jmp (ir_graph *irg, ir_node *block)
{
  ir_node *in[0] = {};
  ir_node *res;
  res = new_ir_node (irg, block, op_Jmp, mode_X, 0, in);
  res = optimize (res);
  ir_vrfy (res);
  return res;
}

inline ir_node *
new_r_Cond (ir_graph *irg, ir_node *block, ir_node *c)
{
  ir_node *in[1] = {c};
  ir_node *res;
  res = new_ir_node (irg, block, op_Cond, mode_T, 1, in);
  res = optimize (res);
  ir_vrfy (res);
  return res;
}

ir_node *
new_r_Call (ir_graph *irg, ir_node *block, ir_node *store,
	    ir_node *callee, int arity, ir_node **in, type_method *type)
{
  ir_node **r_in;
  ir_node *res;
  int r_arity;

  r_arity = arity+2;
  NEW_ARR_A (ir_node *, r_in, r_arity);
  r_in[0] = store;
  r_in[1] = callee;
  memcpy (&r_in[2], in, sizeof (ir_node *) * arity);

  res = new_ir_node (irg, block, op_Call, mode_T, r_arity, r_in);

  set_Call_type(res, type);
  res = optimize (res);
  ir_vrfy (res);
  return res;
}

ir_node *
new_r_Return (ir_graph *irg, ir_node *block,
              ir_node *store, int arity, ir_node **in)
{
  ir_node **r_in;
  ir_node *res;
  int r_arity;

  r_arity = arity+1;

  NEW_ARR_A (ir_node *, r_in, r_arity);

  r_in[0] = store;

  memcpy (&r_in[1], in, sizeof (ir_node *) * arity);

  res = new_ir_node (irg, block, op_Return, mode_X, r_arity, r_in);

  res = optimize (res);

  ir_vrfy (res);
  return res;
}

inline ir_node *
new_r_Raise (ir_graph *irg, ir_node *block, ir_node *store, ir_node *obj)
{
  ir_node *in[2] = {store, obj};
  ir_node *res;
  res = new_ir_node (irg, block, op_Raise, mode_X, 2, in);

  res = optimize (res);
  ir_vrfy (res);
  return res;
}

inline ir_node *
new_r_Load (ir_graph *irg, ir_node *block,
	    ir_node *store, ir_node *adr)
{
  ir_node *in[2] = {store, adr};
  ir_node *res;
  res = new_ir_node (irg, block, op_Load, mode_T, 2, in);

  res = optimize (res);
  ir_vrfy (res);
  return res;
}

inline ir_node *
new_r_Store (ir_graph *irg, ir_node *block,
	     ir_node *store, ir_node *adr, ir_node *val)
{
  ir_node *in[3] = {store, adr, val};
  ir_node *res;
  res = new_ir_node (irg, block, op_Store, mode_T, 3, in);

  res = optimize (res);
  ir_vrfy (res);
  return res;
}

inline ir_node *
new_r_Alloc (ir_graph *irg, ir_node *block, ir_node *store,
	    ir_node *size, type *alloc_type, where_alloc where)
{
  ir_node *in[2] = {store, size};
  ir_node *res;
  res = new_ir_node (irg, block, op_Alloc, mode_T, 2, in);

  res->attr.a.where = where;
  res->attr.a.type = alloc_type;

  res = optimize (res);
  ir_vrfy (res);
  return res;
}

inline ir_node *
new_r_Free (ir_graph *irg, ir_node *block, ir_node *store,
	    ir_node *ptr, ir_node *size, type *free_type)
{
  ir_node *in[3] = {store, ptr, size};
  ir_node *res;
  res = new_ir_node (irg, block, op_Free, mode_T, 3, in);

  res->attr.f = free_type;

  res = optimize (res);
  ir_vrfy (res);
  return res;
}

inline ir_node *
new_r_Sel (ir_graph *irg, ir_node *block, ir_node *store, ir_node *objptr,
           int arity, ir_node **in, entity *ent)
{
  ir_node **r_in;
  ir_node *res;
  int r_arity;

  r_arity = arity + 2;
  NEW_ARR_A (ir_node *, r_in, r_arity);
  r_in[0] = store;
  r_in[1] = objptr;
  memcpy (&r_in[2], in, sizeof (ir_node *) * arity);
  res = new_ir_node (irg, block, op_Sel, mode_p, r_arity, r_in);

  res->attr.s.ltyp = static_linkage;
  res->attr.s.ent = ent;

  res = optimize (res);
  ir_vrfy (res);
  return res;
}

inline ir_node *
new_r_SymConst (ir_graph *irg, ir_node *block, type_or_id *value,
                symconst_kind symkind)
{
  ir_node *in[0] = {};
  ir_node *res;
  res = new_ir_node (irg, block, op_SymConst, mode_I, 0, in);

  res->attr.i.num = symkind;
  if (symkind == linkage_ptr_info) {
    res->attr.i.tori.ptrinfo = (ident *)value;
  } else {
    assert (   (   (symkind == type_tag)
	        || (symkind == size))
            && (is_type(value)));
    res->attr.i.tori.typ = (type *)value;
  }
  res = optimize (res);
  ir_vrfy (res);
  return res;
}

ir_node *
new_r_Sync (ir_graph *irg, ir_node *block, int arity, ir_node **in)
{
  ir_node *res;

  res = new_ir_node (irg, block, op_Sync, mode_M, arity, in);

  res = optimize (res);
  ir_vrfy (res);
  return res;
}


ir_node *
new_r_Bad (ir_node *block)
{
  return current_ir_graph->bad;
}

/***********************/
/** public interfaces  */
/** construction tools */

ir_node *
new_Start (void)
{
  ir_node *res;

  res = new_ir_node (current_ir_graph, current_ir_graph->current_block,
		     op_Start, mode_T, 0, NULL);

  res = optimize (res);
  ir_vrfy (res);
  return res;
}


ir_node *
new_End (void)
{
  ir_node *res;

  res = new_ir_node (current_ir_graph,  current_ir_graph->current_block,
		     op_End, mode_X, -1, NULL);

  res = optimize (res);
  ir_vrfy (res);
  return res;

}

ir_node *
new_Block (void)
{
  ir_node *res;

  res = new_ir_node (current_ir_graph, NULL, op_Block, mode_R, -1, NULL);
  current_ir_graph->current_block = res;
  res->attr.block.matured = 0;
  set_Block_block_visit(res, 0);

  /* forget this optimization. use this only if mature !!!!
  res = optimize (res); */
  ir_vrfy (res);

  /** create a new dynamic array, which stores all parameters in irnodes */
  /** using the same obstack as the whole irgraph */
  res->attr.block.graph_arr = NEW_ARR_D (ir_node *, current_ir_graph->obst,
                                         current_ir_graph->params);

  /** initialize the parameter array */
  memset(res->attr.block.graph_arr, 0, sizeof(ir_node *)*current_ir_graph->params);

  return res;
}


ir_node *
new_Phi (int arity, ir_node **in, ir_mode *mode)
{
  return new_r_Phi (current_ir_graph, current_ir_graph->current_block,
		    arity, in, mode);
}

ir_node *
new_Const (ir_mode *mode, tarval *con)
{
  return new_r_Const (current_ir_graph, current_ir_graph->start_block,
		      mode, con);
}

ir_node *
new_Id (ir_node *val, ir_mode *mode)
{
  return new_r_Id (current_ir_graph, current_ir_graph->current_block,
		   val, mode);
}

ir_node *
new_Proj (ir_node *arg, ir_mode *mode, long proj)
{
  return new_r_Proj (current_ir_graph, current_ir_graph->current_block,
		     arg, mode, proj);
}

ir_node *
new_Conv (ir_node *op, ir_mode *mode)
{
  return new_r_Conv (current_ir_graph, current_ir_graph->current_block,
		     op, mode);
}

ir_node *
new_Tuple (int arity, ir_node **in)
{
  return new_r_Tuple (current_ir_graph, current_ir_graph->current_block,
		      arity, in);
}

ir_node *
new_Add (ir_node *op1, ir_node *op2, ir_mode *mode)
{
  return new_r_Add (current_ir_graph, current_ir_graph->current_block,
		    op1, op2, mode);
}

ir_node *
new_Sub (ir_node *op1, ir_node *op2, ir_mode *mode)
{
  return new_r_Sub (current_ir_graph, current_ir_graph->current_block,
		    op1, op2, mode);
}


ir_node *
new_Minus  (ir_node *op,  ir_mode *mode)
{
  return new_r_Minus (current_ir_graph, current_ir_graph->current_block,
		      op, mode);
}

ir_node *
new_Mul (ir_node *op1, ir_node *op2, ir_mode *mode)
{
  return new_r_Mul (current_ir_graph, current_ir_graph->current_block,
		    op1, op2, mode);
}

ir_node *
new_Quot (ir_node *memop, ir_node *op1, ir_node *op2)
{
  return new_r_Quot (current_ir_graph, current_ir_graph->current_block,
		     memop, op1, op2);
}

ir_node *
new_DivMod (ir_node *memop, ir_node *op1, ir_node *op2)
{
  return new_r_DivMod (current_ir_graph, current_ir_graph->current_block,
		       memop, op1, op2);
}

ir_node *
new_Div (ir_node *memop, ir_node *op1, ir_node *op2)
{
  return new_r_Div (current_ir_graph, current_ir_graph->current_block,
		    memop, op1, op2);
}

ir_node *
new_Mod (ir_node *memop, ir_node *op1, ir_node *op2)
{
  return new_r_Mod (current_ir_graph, current_ir_graph->current_block,
		    memop, op1, op2);
}

ir_node *
new_And (ir_node *op1, ir_node *op2, ir_mode *mode)
{
  return new_r_And (current_ir_graph, current_ir_graph->current_block,
		    op1, op2, mode);
}

ir_node *
new_Or (ir_node *op1, ir_node *op2, ir_mode *mode)
{
  return new_r_Or (current_ir_graph, current_ir_graph->current_block,
		   op1, op2, mode);
}

ir_node *
new_Eor (ir_node *op1, ir_node *op2, ir_mode *mode)
{
  return new_r_Eor (current_ir_graph, current_ir_graph->current_block,
		    op1, op2, mode);
}

ir_node *
new_Not (ir_node *op, ir_mode *mode)
{
  return new_r_Not (current_ir_graph, current_ir_graph->current_block,
		    op, mode);
}

ir_node *
new_Shl (ir_node *op, ir_node *k, ir_mode *mode)
{
  return new_r_Shl (current_ir_graph, current_ir_graph->current_block,
		    op, k, mode);
}

ir_node *
new_Shr (ir_node *op, ir_node *k, ir_mode *mode)
{
  return new_r_Shr (current_ir_graph, current_ir_graph->current_block,
		    op, k, mode);
}

ir_node *
new_Shrs (ir_node *op, ir_node *k, ir_mode *mode)
{
  return new_r_Shrs (current_ir_graph, current_ir_graph->current_block,
		     op, k, mode);
}

ir_node *
new_Rotate (ir_node *op, ir_node *k, ir_mode *mode)
{
  return new_r_Rot (current_ir_graph, current_ir_graph->current_block,
		     op, k, mode);
}

ir_node *
new_Abs (ir_node *op, ir_mode *mode)
{
  return new_r_Abs (current_ir_graph, current_ir_graph->current_block,
		    op, mode);
}

ir_node *
new_Cmp (ir_node *op1, ir_node *op2)
{
  return new_r_Cmp (current_ir_graph, current_ir_graph->current_block,
		    op1, op2);
}

ir_node *
new_Jmp (void)
{
  return new_r_Jmp (current_ir_graph, current_ir_graph->current_block);
}

ir_node *
new_Cond (ir_node *c)
{
  return new_r_Cond (current_ir_graph, current_ir_graph->current_block, c);
}

ir_node *
new_Call (ir_node *store, ir_node *callee, int arity, ir_node **in,
	  type_method *type)
{
  return new_r_Call (current_ir_graph, current_ir_graph->current_block,
		     store, callee, arity, in, type);
}

/* make M parameter in call explicit:
new_Return (ir_node* store, int arity, ir_node **in) */
ir_node *
new_Return (ir_node* store, int arity, ir_node **in)
{
  return new_r_Return (current_ir_graph, current_ir_graph->current_block,
		       store, arity, in);
}

ir_node *
new_Raise (ir_node *store, ir_node *obj)
{
  return new_r_Raise (current_ir_graph, current_ir_graph->current_block,
		      store, obj);
}

ir_node *
new_Load (ir_node *store, ir_node *addr)
{
  return new_r_Load (current_ir_graph, current_ir_graph->current_block,
		     store, addr);
}

ir_node *
new_Store (ir_node *store, ir_node *addr, ir_node *val)
{
  return new_r_Store (current_ir_graph, current_ir_graph->current_block,
		      store, addr, val);
}

ir_node *
new_Alloc (ir_node *store, ir_node *size, type *alloc_type,
           where_alloc where)
{
  return new_r_Alloc (current_ir_graph, current_ir_graph->current_block,
		      store, size, alloc_type, where);
}

ir_node *
new_Free (ir_node *store, ir_node *ptr, ir_node *size, type *free_type)
{
  return new_r_Free (current_ir_graph, current_ir_graph->current_block,
		     store, ptr, size, free_type);
}

ir_node *
new_simpleSel (ir_node *store, ir_node *objptr, entity *ent)
/* GL: objptr was called frame before.  Frame was a bad choice for the name
   as the operand could as well be a pointer to a dynamic object. */
{
  return new_r_Sel (current_ir_graph, current_ir_graph->current_block,
		    store, objptr, 0, NULL, ent);
}

ir_node *
new_Sel (ir_node *store, ir_node *objptr, int n_index, ir_node **index, entity *sel)
{
  return new_r_Sel (current_ir_graph, current_ir_graph->current_block,
		    store, objptr, n_index, index, sel);
}

ir_node *
new_SymConst (type_or_id *value, symconst_kind kind)
{
  return new_r_SymConst (current_ir_graph, current_ir_graph->current_block,
                         value, kind);
}

ir_node *
new_Sync (int arity, ir_node** in)
{
  return new_r_Sync (current_ir_graph, current_ir_graph->current_block,
		     arity, in);
}


ir_node *
new_Bad (void)
{
  return current_ir_graph->bad;
}

#if 0
/************************/
/* ir block constructor */

/* GL: what's this good for? */

typedef struct ir_block {
  char closed;
  char matured;
  /* -1 = error, 0 = OK */
} ir_block;

ir_block *
new_ir_Block(void)
{
  ir_block *res;

  res->closed = -1;
  res->matured = -1;

  return res;
}
#endif

/* initialize */

/* call once for each run of the library */
void
init_cons (void)
{
}
