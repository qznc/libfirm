/*
 * Project:     libFIRM
 * File name:   testprograms/dead_block_example.c
 * Purpose:     Test unreachable code elimination.
 * Author:      Christian Schaefer, Goetz Lindenmaier
 * Modified by:
 * Created:
 * CVS-ID:      $Id$
 * Copyright:   (c) 1999-2003 Universitšt Karlsruhe
 * Licence:     This file protected by GPL -  GNU GENERAL PUBLIC LICENSE.
 */

# include <stdio.h>
# include <string.h>

# include "irvrfy.h"
# include "irdump.h"
# include "firm.h"

/*
 *   a dead block / unreachable code.
 */

/**
*  This file constructs a control flow of following shape:
*
*
*         firstBlock
*          /   \
*         /     \
*       |/_     _\|
*     Block1    Block2   deadBlock
*        \       |       /
*       \      |      /
*       _\|   \ /   |/_
*            nextBlock
*
*
*   This is a program as, e.g.,
*
*   if () then
*     { Jmp label1; } //  happens anyways
*   else
*     { Jmp label1; } //  happens anyways
* label1:
*   return();
*   Jmp label1;
*
**/

int main(int argc, char **argv)
{
  ir_graph *irg;          /* this variable contains the irgraph */
  type *owner;      /* the class in which this method is defined */
  type *proc_main; /* type information for the method main */
  type     *prim_t_int;
  entity *ent;            /* represents this method as entity of owner */
  ir_node *c1, *c2, *cond, *f, *t, *endBlock, *Block1, *jmp, *Block2,
          *deadBlock, *x;

  /* init library */
  init_firm (NULL);

  /*** Make basic type information for primitive type int. ***/
  prim_t_int = new_type_primitive(new_id_from_chars ("int", 3), mode_Is);

  /* FIRM was designed for oo languages where all methods belong to a class.
   * For imperative languages like C we view a file as a large class containing
   * all functions as methods in this file.
   * Therefore we define a class "empty" according to the file name
   * with a method main as an entity.
   */
#define CLASSNAME "DEAD_BLOCK"
#define METHODNAME "main"
#define NRARGS 0
#define NRES 1
  printf("\nCreating an IR graph: %s...\n", CLASSNAME);

  owner = new_type_class (new_id_from_chars (CLASSNAME, strlen(CLASSNAME)));
  proc_main = new_type_method(new_id_from_chars(METHODNAME, strlen(METHODNAME)),
                              NRARGS, NRES);
  set_method_res_type(proc_main, 0, prim_t_int);
  ent = new_entity (owner,
                    new_id_from_chars (METHODNAME, strlen(METHODNAME)),
                    proc_main);
  get_entity_ld_name(ent); /* To enforce name mangling for vcg graph name */
#define NUM_OF_LOCAL_VARS 1

  irg = new_ir_graph (ent, NUM_OF_LOCAL_VARS);

  /* to make a condition  */
  c1 = new_Const (mode_Is, new_tarval_from_long (1, mode_Is));
  c2 = new_Const (mode_Is, new_tarval_from_long (2, mode_Is));
  set_value(0, c2);

  cond = new_Cond(new_Proj(new_Cmp(c1, c2), mode_b, Eq));
  f = new_Proj(cond, mode_X, 0);
  t = new_Proj(cond, mode_X, 1);
  mature_immBlock(get_irg_current_block(irg));

  /* end block to add jmps */
  endBlock = new_immBlock();

  /* Block 1 */
  Block1 = new_immBlock();
  add_immBlock_pred(Block1, t);
  mature_immBlock(Block1);
  jmp = new_Jmp();
  add_immBlock_pred(endBlock, jmp);

  /* Block 2 */
  Block2 = new_immBlock();
  add_immBlock_pred(Block2, f);
  mature_immBlock(Block2);
  jmp = new_Jmp();
  add_immBlock_pred(endBlock, jmp);

  /* dead Block */
  deadBlock = new_immBlock();
  mature_immBlock(deadBlock);
  jmp = new_Jmp();
  add_immBlock_pred(endBlock, jmp);

  /* finish end block */
  set_cur_block(endBlock);
  {
    ir_node *in[1];
    in[0] = get_value(0, mode_Is);
    get_store();
    x = new_Return (get_store(), 1, in);
  }
  mature_immBlock (get_irg_current_block(irg));

  add_immBlock_pred (get_irg_end_block(irg), x);
  mature_immBlock (get_irg_end_block(irg));

  finalize_cons (irg);

  printf("Optimizing ...\n");
  local_optimize_graph (irg);
  dead_node_elimination (irg);

  /* verify the graph */
  irg_vrfy(irg);

  printf("Dumping the graph and a control flow graph.\n");
  char *dump_file_suffix = "";
  dump_ir_block_graph (irg, dump_file_suffix);
  dump_cfg (irg, dump_file_suffix);
  printf("Use xvcg to view these graphs:\n");
  printf("/ben/goetz/bin/xvcg GRAPHNAME\n\n");

  return (0);
}
