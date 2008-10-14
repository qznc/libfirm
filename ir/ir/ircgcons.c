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
 * @brief   Construction and removal of interprocedural representation
 *          (explicit interprocedural dependencies).
 * @author  Hubert Schmid
 * @date    09.06.2002
 * @version $Id$
 */
#include "config.h"

#ifdef INTERPROCEDURAL_VIEW

#include <string.h>
#include <stdbool.h>
#include "ircgcons.h"

#include "array.h"
#include "irprog.h"
#include "irnode_t.h"
#include "irprog_t.h"
#include "ircons_t.h"
#include "irgmod.h"
#include "irgwalk.h"
#include "irflag_t.h"
#include "irtools.h"

/* Return the current state of the interprocedural view. */
ip_view_state get_irp_ip_view_state(void) {
  return irp->ip_view;
}

/* Set the current state of the interprocedural view. */
static void set_irp_ip_view(ip_view_state state) {
  irp->ip_view = state;
}

/* Set the state of the interprocedural view to invalid. */
void set_irp_ip_view_invalid(void) {
  set_irp_ip_view(ip_view_invalid);
}


/* Data for each method */
typedef struct {
  int count;                      /* Number of calleers. */
  bool open;                      /* Open method: called by an unknown caller */
  ir_node * reg, * mem, ** res;   /* EndReg, Mem and Method return values */
  ir_node * except, * except_mem; /* EndExcept and Mem for exception return */
} irg_data_t;

static irg_data_t * irg_data_create(void)
{
  return XMALLOCZ(irg_data_t);
}

/** Count the number of callers of each method and mark open methods.
 *
 *  Fills the irg_data data structure.
 *  Open methods are methods with an unknown caller, I.e., methods that
 *   - are external visible
 *   - are dereferenced somewhere within the program (i.e., the address of the
 *     method is stored somewhere). */
static void caller_init(int arr_length, ir_entity ** free_methods) {
  int i, j;
  for (i = get_irp_n_irgs() - 1; i >= 0; --i) {
    set_entity_link(get_irg_entity(get_irp_irg(i)), irg_data_create());
  }
  for (i = arr_length - 1; i >= 0; --i) {
    irg_data_t * data = get_entity_link(free_methods[i]);
    data->open = true;
  }
  for (i = get_irp_n_irgs() - 1; i >= 0; --i) {
    ir_graph * irg = get_irp_irg(i);
    ir_node * call;
    /* We collected all call nodes in a link list at the end node. */
    for (call = get_irn_link(get_irg_end(irg)); call; call = get_irn_link(call)) {
      if (!is_Call(call)) continue;
      for (j = get_Call_n_callees(call) - 1; j >= 0; --j) {
        ir_entity * ent = get_Call_callee(call, j);
        if (get_entity_irg(ent)) {
          irg_data_t * data = get_entity_link(ent);
# ifndef CATE_jni
          assert(get_entity_irg(ent) && data);
          ++data->count;
# endif /* ndef CATE_jni */
        } else {
          set_entity_link(ent, NULL);
        }
      }
    }
  }
}

/*
static inline ir_node * tail(ir_node * node) {
  ir_node * link;
  for (; (link = get_irn_link(node)); node = link) ;
  return node;
}
*/

/* Call-Operationen an die "link"-Liste von "call_tail" anh�ngen (und
 * "call_tail" aktualisieren), Proj-Operationen in die Liste ihrer Definition
 * (auch bei Proj->Call Operationen) und Phi-Operationen in die Liste ihres
 * Grundblocks einf�gen. */
static void collect_phicallproj_walker(ir_node * node, ir_node ** call_tail) {
  if (is_Call(node)) {
    /* Die Liste von Call an call_tail anh�ngen. */
    ir_node * link;
    assert(get_irn_link(*call_tail) == NULL);
    set_irn_link(*call_tail, node);
    /* call_tail aktualisieren: */
    for (link = get_irn_link(*call_tail); link; *call_tail = link, link = get_irn_link(link)) ;
  } else if (get_irn_op(node) == op_Proj) {
    ir_node * head = skip_Proj(get_Proj_pred(node));
    set_irn_link(node, get_irn_link(head));
    set_irn_link(head, node);
    /* call_tail gegebenenfalls aktualisieren: */
    if (head == *call_tail) {
      *call_tail = node;
    }
  } else if (get_irn_op(node) == op_Phi) {
    ir_node * block = get_nodes_block(node);
    set_irn_link(node, get_irn_link(block));
    set_irn_link(block, node);
  }
}


static void link(ir_node * head, ir_node * node) {
  if (node) {
    set_irn_link(node, get_irn_link(head));
    set_irn_link(head, node);
  }
}


/* Die Call-Operationen aller Graphen an den End-Operationen
 * verlinken, die Proj-Operationen an ihren Definitionen und die
 * Phi-Operationen an ihren Grundbl�cken. Die Liste der Calls sieht
 * dann so aus: End -> Call -> Proj -> ... -> Proj -> Call -> Proj ->
 * ... -> Proj -> NULL. */
static void collect_phicallproj(void) {
  int i;

  for (i = get_irp_n_irgs() - 1; i >= 0; --i) {
    ir_graph * irg = get_irp_irg(i);
    ir_node * start = get_irg_start(irg);
    ir_node * end = get_irg_end(irg);
    current_ir_graph = irg;
    assert(irg && start);

	set_using_irn_link(irg);

    /* Die speziellen Parameter der Start-Operation extra verlinken,
     * auch wenn sie nicht im intraprozeduralen Graphen erreichbar
     * sind. */
    link(start, get_irg_frame(irg));
    /* walk */
    irg_walk_graph(irg, firm_clear_link, (irg_walk_func *) collect_phicallproj_walker, &end);

	clear_using_irn_link(irg);
  }
}


/* Proj-Operation durch Filter-Operation im aktuellen Block ersetzen. */
static ir_node * exchange_proj(ir_node * proj) {
  ir_node * filter;
  assert(get_irn_op(proj) == op_Proj);
  filter = new_Filter(get_Proj_pred(proj), get_irn_mode(proj), get_Proj_proj(proj));
  /* Die Proj- (Id-) Operation sollte im gleichen Grundblock stehen, wie die
   * Filter-Operation. */
  set_nodes_block(proj, get_nodes_block(filter));
  exchange(proj, filter);
  return filter;
}


/* Echt neue Block-Operation erzeugen. CSE abschalten! */
static ir_node * create_Block(int n, ir_node ** in) {
  /* Turn off optimizations so that blocks are not merged again. */
  int rem_opt = get_opt_optimize();
  ir_node * block;
  set_optimize(0);
  block = new_Block(n, in);
  set_optimize(rem_opt);
  return block;
}


static void prepare_irg_end(ir_graph * irg, irg_data_t * data);
static void prepare_irg_end_except(ir_graph * irg, irg_data_t * data);


/* If we use new_Unknown we get the Unknown of a graph.  This can
 * cause cycles we don't want to see, as Unknwon is in the Start Block
 * of the procedure. Use unknown of outermost irg where the start
 * block has no predecessors. */
static inline ir_node *get_cg_Unknown(ir_mode *m) {
  assert((get_Block_n_cfgpreds(get_irg_start_block(get_irp_main_irg())) == 1) &&
	 (get_nodes_block(get_Block_cfgpred(get_irg_start_block(get_irp_main_irg()), 0)) ==
	  get_irg_start_block(get_irp_main_irg())));
  return new_r_Unknown(get_irp_main_irg(), m);
}


/* IRG vorbereiten. Proj-Operationen der Start-Operation in Filter-Operationen
 * umwandeln. Die k�nstlichen Steuerzusammenfl�sse EndReg und EndExcept
 * einf�gen. An der Start-Operation h�ngt nach dem Aufruf eine Liste der
 * entsprechenden Filter-Knoten. */
static void prepare_irg(ir_graph * irg, irg_data_t * data) {
  ir_node * start_block = get_irg_start_block(irg);
  ir_node * link, * proj;
  int n_callers = data->count + (data->open ? 1 : 0);
  ir_node ** in = NEW_ARR_F(ir_node *, n_callers);

  current_ir_graph = irg;
  set_irg_current_block(irg, start_block);

  /* Grundblock interprozedural machen. */
  /* "in" ist nicht initialisiert. Das passiert erst in "construct_start". */
  set_Block_cg_cfgpred_arr(start_block, n_callers, in);
  /* Proj-Operationen durch Filter-Operationen ersetzen und (sonst) in
   * den Start-Block verschieben. */
  for (proj = get_irn_link(get_irg_start(irg)); proj; proj = get_irn_link(proj)) {
    if (get_Proj_pred(proj) != get_irg_start(irg)
    || (get_Proj_proj(proj) != pn_Start_X_initial_exec && get_Proj_proj(proj) != pn_Start_T_args)) {
      ir_node * filter = exchange_proj(proj);
      set_Filter_cg_pred_arr(filter, n_callers, in);
    } else {
      set_nodes_block(proj, start_block);
    }
  }

  DEL_ARR_F(in);

  /* Liste der Filter-Operationen herstellen. Dabei muss man beachten,
   * dass oben f�r "verschiedene" Proj-Operationen wegen CSE nur eine
   * Filter-Operation erzeugt worden sein kann. */
  for (link = get_irg_start(irg), proj = get_irn_link(link); proj; proj = get_irn_link(proj)) {
    if (is_Id(proj)) { /* replaced with filter */
      ir_node * filter = get_Id_pred(proj);
      assert(is_Filter(filter));
      if (filter != link && get_irn_link(filter) == NULL) {
    set_irn_link(link, filter);
    link = filter;
      }
    }
  }
  /* Globle Eintr�ge f�r ersetzte Operationen korrigieren. */
  set_irg_initial_exec(irg, skip_Id(get_irg_initial_exec(irg)));
  set_irg_frame       (irg, skip_Id(get_irg_frame(irg)));
  set_irg_initial_mem (irg, skip_Id(get_irg_initial_mem(irg)));

  /* Unbekannten Aufrufer sofort eintragen. */
  if (data->open) {
    set_Block_cg_cfgpred(start_block, 0, get_cg_Unknown(mode_X));
    for (proj = get_irn_link(get_irg_start(irg)); proj; proj = get_irn_link(proj)) {
      if (is_Filter(proj)) {
    set_Filter_cg_pred(proj, 0, get_cg_Unknown(get_irn_mode(proj)));
      }
    }
    data->count = 1;
  } else {
    data->count = 0;
  }

  prepare_irg_end(irg, data);
  prepare_irg_end_except(irg, data);
}


/* K�nstlicher Steuerzusammenfluss EndReg einf�gen. */
static void prepare_irg_end(ir_graph * irg, irg_data_t * data) {
  ir_node * end_block   = get_irg_end_block(irg);
  ir_node * end         = get_irg_end(irg);
  ir_node **ret_arr     = NULL;
  ir_node **cfgpred_arr = get_Block_cfgpred_arr(end_block);
  int i, j;
  int n_ret = 0;

  for (i = get_Block_n_cfgpreds(end_block) - 1; i >= 0; --i) {
    if (is_Return(cfgpred_arr[i])) {
      if (ret_arr) {
        ARR_APP1(ir_node *, ret_arr, cfgpred_arr[i]);
      } else {
        ret_arr = NEW_ARR_F(ir_node *, 1);
        ret_arr[0] = cfgpred_arr[i];
      }
      ++n_ret;
    }
  }

  if (n_ret > 0) {
    int n_res = get_method_n_ress(get_entity_type(get_irg_entity(irg)));
    ir_node ** in = NEW_ARR_F(ir_node *, n_ret);

    /* block */
    for (i = n_ret - 1; i >= 0; --i) {
      set_irg_current_block(irg, get_nodes_block(ret_arr[i]));
      in[i] = new_Jmp();
    }
    create_Block(n_ret, in);

    /* end */
    data->reg = new_EndReg();

    /* mem */
    for (i = n_ret - 1; i >= 0; --i) {
      in[i] = get_Return_mem(ret_arr[i]);
    }
    data->mem = new_Phi(n_ret, in, mode_M);
    /* This Phi is a merge, therefore needs not be kept alive.
       It might be optimized away, though.  */
    if (get_End_keepalive(end, get_End_n_keepalives(end)-1 ) == data->mem)
      set_End_keepalive(end, get_End_n_keepalives(end)-1, new_Bad());

    /* res */
    data->res = NEW_ARR_F(ir_node *, n_res);
    for (j = n_res - 1; j >= 0; --j) {
      ir_mode *mode = NULL;
      /* In[0] could be a Bad node with wrong mode. */
      for (i = n_ret - 1; i >= 0; --i) {
	in[i] = get_Return_res(ret_arr[i], j);
	if (!mode && get_irn_mode(in[i]) != mode_T)
	  mode = get_irn_mode(in[i]);
      }
      if (mode)
	data->res[j] = new_Phi(n_ret, in, mode);
      else  /* All preds are Bad */
	data->res[j] = new_Bad();
    }

    DEL_ARR_F(in);
  }

  if (ret_arr) DEL_ARR_F(ret_arr);
}


/* K�nstlicher Steuerzusammenfluss EndExcept einf�gen. */
static void prepare_irg_end_except(ir_graph * irg, irg_data_t * data) {
  ir_node * end_block = get_irg_end_block(irg);
  ir_node * end = get_irg_end(irg);
  ir_node ** except_arr = NULL;
  int i;
  int n_except = 0;
  ir_node ** cfgpred_arr = get_Block_cfgpred_arr(end_block);
  for (i = get_Block_n_cfgpreds(end_block) - 1; i >= 0; --i) {
    if (! is_Return(cfgpred_arr[i])) {
      if (except_arr) {
        ARR_APP1(ir_node *, except_arr, cfgpred_arr[i]);
      } else {
        except_arr = NEW_ARR_F(ir_node *, 1);
        except_arr[0] = cfgpred_arr[i];
      }
      ++n_except;
    }
  }
  if (n_except > 0) {
    ir_node ** in = NEW_ARR_F(ir_node *, n_except);
    /* block */
    create_Block(n_except, except_arr);
    /* end_except */
    data->except = new_EndExcept();
    /* mem */
    for (i = n_except - 1; i >= 0; --i) {
      ir_node *node = skip_Proj(skip_Tuple(except_arr[i]));
      ir_op *op = get_irn_op(node);
      if (op == op_Call) {
        in[i] = new_r_Proj(irg, get_nodes_block(node), node, mode_M, pn_Call_M_except);
      } else if (op == op_Raise) {
        in[i] = new_r_Proj(irg, get_nodes_block(node), node, mode_M, pn_Raise_M);
      } else if (op == op_CopyB) {
        in[i] = new_r_Proj(irg, get_nodes_block(node), node, mode_M, pn_CopyB_M_except);
      } else {
        assert(is_fragile_op(node));
        /* We rely that all cfops have the memory output at the same position. */
        in[i] = new_r_Proj(irg, get_nodes_block(node), node, mode_M, 0);
      }
    }
    data->except_mem = new_Phi(n_except, in, mode_M);
    /* This Phi is a merge, therefor needs not be kept alive.
       It might be optimized away, though.  */
    if (get_End_keepalive(end, get_End_n_keepalives(end)-1 )
    == data->except_mem)
      set_End_keepalive(end, get_End_n_keepalives(end)-1, new_Bad());
    DEL_ARR_F(in);
  }
  if (except_arr) DEL_ARR_F(except_arr);
}


/* Zwischengespeicherte Daten wieder freigeben. */
static void cleanup_irg(ir_graph * irg) {
  ir_entity * ent = get_irg_entity(irg);
  irg_data_t * data = get_entity_link(ent);
  assert(data);
  if (data->res) DEL_ARR_F(data->res);
  set_entity_link(ent, NULL);
  free(data);
}


/* Alle Phi-Operationen aus "from_block" nach "to_block"
 * verschieben. Die Phi-Operationen m�ssen am zugeh�rigen Grundblock
 * verlinkt sein. Danach sind sie am neuen Grundblock verlinkt. */
static void move_phis(ir_node * from_block, ir_node * to_block) {
  ir_node * phi;
  for (phi = get_irn_link(from_block); phi != NULL; phi = get_irn_link(phi)) {
    set_nodes_block(phi, to_block);
  }
  assert(get_irn_link(to_block) == NULL);
  set_irn_link(to_block, get_irn_link(from_block));
  set_irn_link(from_block, NULL);
}


/* Rekursiv die Operation "node" und alle ihre Vorg�nger aus dem Block
 * "from_block" nach "to_block" verschieben.
 * Verschiebe ebenfalls die Projs aus diesen Operationen. */
static void move_nodes(ir_node * from_block, ir_node * to_block, ir_node * node) {
  int i,  arity = get_irn_arity(node);
  ir_node *proj;

  for (i = arity - 1; i >= 0; --i) {
    ir_node * pred = get_irn_n(node, i);
    if (get_nodes_block(pred) == from_block) {
      move_nodes(from_block, to_block, pred);
    }
  }
  set_nodes_block(node, to_block);

  /* Move projs of this node. */
  proj = get_irn_link(node);
  for (; proj; proj = skip_Id(get_irn_link(proj))) {
    if (get_irn_op(proj) != op_Proj && !is_Filter(proj)) continue;
    if ((get_nodes_block(proj) == from_block) && (skip_Proj(get_irn_n(proj, 0)) == node))
      set_nodes_block(proj, to_block);
  }
}


/* Abh�ngigkeiten vom Start-Block und den Filter-Operationen im
 * Start-Block auf den Aufrufer hinzuf�gen. */
static void construct_start(ir_entity * caller, ir_entity * callee,
                            ir_node * call, ir_node * exec)
{
  irg_data_t *data  = get_entity_link(callee);
  ir_graph   *irg   = get_entity_irg(callee);
  ir_node    *start = get_irg_start(irg);
  ir_node    *filter;
  (void) caller;

  assert(irg);
  assert(get_entity_peculiarity(callee) == peculiarity_existent); /* Else data is not initalized. */
  assert((0 <= data->count) &&
	 (data->count < get_Block_cg_n_cfgpreds(get_nodes_block(start))));

  set_Block_cg_cfgpred(get_nodes_block(start), data->count, exec);
  for (filter = get_irn_link(start); filter; filter = get_irn_link(filter)) {
    if (!is_Filter(filter)) continue;
    if (get_Proj_pred(filter) == start) {
      switch ((int) get_Proj_proj(filter)) {
      case pn_Start_M:
    set_Filter_cg_pred(filter, data->count, get_Call_mem(call));
    break;
      case  pn_Start_P_frame_base:
    /* "frame_base" wird nur durch Unknown dargestellt. Man kann ihn aber
     * auch explizit darstellen, wenn sich daraus Vorteile f�r die
     * Datenflussanalyse ergeben. */
    set_Filter_cg_pred(filter, data->count, get_cg_Unknown(get_irn_mode(filter)));
    break;
      case pn_Start_P_globals:
    /* "globals" wird nur durch Unknown dargestellt. Man kann ihn aber auch
     * explizit darstellen, wenn sich daraus Vorteile f�r die
     * Datenflussanalyse ergeben. */
    set_Filter_cg_pred(filter, data->count, get_cg_Unknown(get_irn_mode(filter)));
    break;
      default:
    /* not reached */
    assert(0 && "not reached");
    break;
      }
    } else {
      set_Filter_cg_pred(filter, data->count, get_Call_param(call, get_Proj_proj(filter)));
    }
  }
  ++data->count;
}


/* Abh�ngigkeiten f�r den Speicherzustand �ber alle aufgerufenen
 * Methoden bestimmen. */
static void fill_mem(int length, irg_data_t * data[], ir_node * in[]) {
  int i;
  for (i = 0; i < length; ++i) {
    if (data[i]) { /* explicit */
      if (data[i]->reg) {
	in[i] = data[i]->mem;
      } else {
	in[i] = new_Bad();
      }
    } else { /* unknown */
      in[i] = get_cg_Unknown(mode_M);
    }
  }
}


/* Abh�ngigkeiten f�r den Ausnahme-Speicherzustand �ber alle
 * aufgerufenen Methoden bestimmen. */
static void fill_except_mem(int length, irg_data_t * data[], ir_node * in[]) {
  int i;
  for (i = 0; i < length; ++i) {
    if (data[i]) { /* explicit */
      if (data[i]->except) {
	in[i] = data[i]->except_mem;
      } else {
	in[i] = new_Bad();
      }
    } else { /* unknown */
      in[i] = get_cg_Unknown(mode_M);
    }
  }
}


/* Abh�ngigkeiten f�r ein Ergebnis �ber alle aufgerufenen Methoden
 * bestimmen. */
static void fill_result(int pos, int length, irg_data_t * data[], ir_node * in[], ir_mode *m) {
  int i;
  for (i = 0; i < length; ++i) {
    if (data[i]) { /* explicit */
      if (data[i]->reg) {
	in[i] = data[i]->res[pos];
      } else {
	in[i] = new_Bad();
      }
    } else { /* unknown */
      in[i] = get_cg_Unknown(m);
    }
  }
}


/* Proj auf Except-X einer Call-Operation (aus der Link-Liste) bestimmen. */
static ir_node * get_except(ir_node * call) {
  /* Mit CSE k�nnte man das effizienter machen! Die Methode wird aber f�r jede
   * Aufrufstelle nur ein einziges Mal aufgerufen. */
  ir_node * proj;
  for (proj = get_irn_link(call); proj && get_irn_op(proj) == op_Proj; proj = get_irn_link(proj)) {
    if (get_Proj_proj(proj) == 1 && is_Call(get_Proj_pred(proj))) {
      return proj;
    }
  }
  return NULL;
}

/* Returns true if control flow operation exc is predecessor of end
   block in irg.  Works also for Return nodes, not only exceptions. */
static bool exc_branches_to_end(ir_graph *irg, ir_node *exc) {
  int i;
  ir_node *end = get_irg_end_block(irg);
  for (i = get_Block_n_cfgpreds(end) -1; i >= 0; --i)
    if (get_Block_cfgpred(end, i) == exc) return true;
  return false;
}

/* Returns true if only caller of irg is "Unknown". */
static bool is_outermost_graph(ir_graph *irg) {
  irg_data_t * data = get_entity_link(get_irg_entity(irg));
  if (data->count) {
    return false;
  } else if (data->open) {
    /* Die Methode wird nur von "der" unbekannten Aufrufstelle
     * aufgerufen. Darstellung wird f�r diese Methode nicht
     * ge�ndert. */
  } else {
    /* Methode kann nicht aufgerufen werden. Die Darstellung wird
     * f�r diese Methode nicht ge�ndert. Das kann nicht vorkommen,
     * wenn zuvor "gc_irgs()" aufgerufen wurde. */
  }
  return true;
}

#ifdef INTERPROCEDURAL_VIEW
/* Grundblock der Call-Operation aufteilen. CallBegin- und Filter-Operationen
 * einf�gen. Die Steuer- und Datenflussabh�ngigkeiten von den aufgerufenen
 * Methoden auf die CallBegin-Operation, und von der Aufrufstelle auf die
 * aufgerufenen Methoden eintragen. */
static void construct_call(ir_node * call) {
  int i, n_callees;
  ir_node *post_block, *pre_block, *except_block, * proj, *jmp, *call_begin;
  ir_node ** in;
  ir_entity * caller;
  ir_entity ** callees;
  ir_graph ** irgs;
  irg_data_t ** data;

  n_callees  = get_Call_n_callees(call);
  post_block = get_nodes_block(call); /* block nach dem Aufruf */
  pre_block  = create_Block(get_Block_n_cfgpreds(post_block),
	              get_Block_cfgpred_arr(post_block)); /* block vor dem Aufruf (mit CallBegin) */
  except_block = NULL;
  jmp = new_Break(); /* Sprung f�r intraprozedurale Darstellung (in	* pre_block) */
  call_begin = new_CallBegin(call); /* (in pre_block) */
  /* CallBegin might be entry to endless recursion. */
  add_End_keepalive(get_irg_end(get_irn_irg(pre_block)), pre_block);

  in = NEW_ARR_F(ir_node *, n_callees);
  caller = get_irg_entity(current_ir_graph); /* entity des aktuellen ir_graph */
  callees = NEW_ARR_F(ir_entity *, n_callees); /* aufgerufene Methoden: entity */
  irgs = NEW_ARR_F(ir_graph *, n_callees); /* aufgerufene Methoden: ir_graph */
  data = NEW_ARR_F(irg_data_t *, n_callees); /* aufgerufene Methoden: irg_data_t */

  /* post_block kann bereits interprozedurale Steuerflussvorg�nger
   * besitzen. Diese m�ssen dann auch noch f�r den pre_block gesetzt werden. */
  if (get_Block_cg_cfgpred_arr(post_block)) {
    set_Block_cg_cfgpred_arr(pre_block, get_Block_cg_n_cfgpreds(post_block),
                 get_Block_cg_cfgpred_arr(post_block));
    remove_Block_cg_cfgpred_arr(post_block);
  }

  /* Operationen verschieben */
  move_phis(post_block, pre_block);
  move_nodes(post_block, pre_block, call);
  set_irn_in(post_block, 1, &jmp);

  /* Wiederverwendete Daten initialisieren. */
  for (i = 0; i < n_callees; ++i) {
    callees[i] = get_Call_callee(call, i);
    irgs[i] = get_entity_irg(callees[i]);
    data[i] = get_entity_link(callees[i]);
    /* Only entities that have an irg got a irg_data data structure.
       In others there is some arbitrary garbage in the link field. */
    if (!irgs[i]) { assert(!data[i]); data[i] = NULL; }
  }

  /*
   * Set flag to suppress verifying placement on proper irg:
   * optimization can return block on other irg.
   */
  set_interprocedural_view(1);

  /* Die interprozeduralen Steuerflussvorg�nger des post_block
   * bestimmen. */
  for (i = 0; i < n_callees; ++i) {
    if (data[i]) { /* explicit */
      if (data[i]->reg) {
	    in[i] = new_r_Proj(irgs[i], get_nodes_block(data[i]->reg),
			       data[i]->reg, mode_X, data[i]->count);
      } else {
	    in[i] = new_Bad();
      }
    } else { /* unknown */
      in[i] = get_cg_Unknown(mode_X);
    }
  }
  set_interprocedural_view(0);

  set_Block_cg_cfgpred_arr(post_block, n_callees, in);

  /* Die interprozeduralen Steuerflussvorg�nger des except_block
   * bestimmen. */
  if ((proj = get_except(call)) != NULL) {
    int preds = 0;
    bool exc_to_end = false;
    if (exc_branches_to_end(current_ir_graph, proj)) {
      /* The Call aborts the procedure if it returns with an exception.
	 If this is an outermost procedure, the normal handling of exceptions
	 will generate a Break that goes to the end block.  This is illegal
	 Frim. So directly branch to the end block with all exceptions. */
      exc_to_end = true;
      if (is_outermost_graph(current_ir_graph)) {
	    except_block = get_irg_end_block(current_ir_graph);
      } else {
	    irg_data_t * tmp_data = get_entity_link(get_irg_entity(current_ir_graph));
	    except_block = get_nodes_block(tmp_data->except);
      }
    } else {
      except_block = create_Block(1, &proj);
      set_nodes_block(proj, except_block);
      exchange(proj, new_Break());
      set_irg_current_block(current_ir_graph, pre_block);
      set_irn_n(except_block, 0, new_Proj(call, mode_X, pn_Call_X_except));
      set_irg_current_block(current_ir_graph, post_block);
    }

    /*
     * Set flag to suppress verifying placement on proper irg:
     * optimization can return block on other irg.
     */
    set_interprocedural_view(1);

    for (i = 0; i < n_callees; ++i) {
      ir_entity * callee = get_Call_callee(call, i);
      if (data[i]) { /* explicit */
	    if (data[i]->except) {
	      in[i] = new_r_Proj(get_entity_irg(callee), get_nodes_block(data[i]->except),
			         data[i]->except, mode_X, data[i]->count);
	    } else {
	      in[i] = new_Bad();
	    }
      } else { /* unknown */
	    in[i] = get_cg_Unknown(mode_X);
      }
    }

    preds = n_callees;
    if (exc_to_end) {
      /* append all existing preds of the end block to new in array.
       * Normal access routine guarantees that with first visit we
       * get the normal preds, and from then on the _cg_ preds.
       * (interprocedural view is set!)
       * Do not add the exc pred of end we are replacing! */
      for (i = get_Block_n_cfgpreds(except_block)-1; i >= 0; --i) {
	    ir_node *pred = get_Block_cfgpred(except_block, i);
	    if (pred != proj) {
	      ARR_APP1(ir_node *, in, pred);
	      preds++;
	    }
      }
    }
    set_Block_cg_cfgpred_arr(except_block, preds, in);
  }
  set_interprocedural_view(0);

  /* Diesen Vorg�nger in den Start-Bl�cken der aufgerufenen Methoden
   * eintragen. */
  set_irg_current_block(current_ir_graph, pre_block);
  for (i = 0; i < n_callees; ++i) {
    if (irgs[i]) /* Else there is not graph to call */
      construct_start(caller, callees[i], call, new_Proj(call_begin, mode_X, i));
  }

  /* Proj-Operationen in Filter-Operationen umwandeln und
   * interprozedurale Vorg�nger einf�gen. */
  set_irg_current_block(current_ir_graph, post_block);
  for (proj = get_irn_link(call); proj; proj = get_irn_link(proj)) {
    if (get_irn_op(proj) != op_Proj) continue;
    if (skip_Proj(get_Proj_pred(proj)) != call) continue;
    if (get_Proj_pred(proj) == call) {
      if (get_Proj_proj(proj) == pn_Call_M_regular) { /* memory */
	    ir_node * filter;

	    set_nodes_block(proj, post_block);
	    filter = exchange_proj(proj);
	    /* filter in die Liste der Phis aufnehmen */
	    if (get_irn_link(filter) == NULL) { /* note CSE */
	      set_irn_link(filter, get_irn_link(post_block));
	      set_irn_link(post_block, filter);
	    }
	    fill_mem(n_callees, data, in);
	    set_Filter_cg_pred_arr(filter, n_callees, in);
      } else if (get_Proj_proj(proj) == pn_Call_X_except) { /* except */
        /* nothing: siehe oben */
      } else if (get_Proj_proj(proj) == pn_Call_T_result) { /* results */
	    /* nothing */
      } else if (get_Proj_proj(proj) == pn_Call_M_except) { /* except_mem */
        ir_node * filter;

	    set_nodes_block(proj, post_block);
	    assert(except_block);
	    set_irg_current_block(current_ir_graph, except_block);
	    filter = exchange_proj(proj);
	    /* filter in die Liste der Phis aufnehmen */
	    if (get_irn_link(filter) == NULL) { /* note CSE */
	      set_irn_link(filter, get_irn_link(except_block));
	      set_irn_link(except_block, filter);
	    }
	    set_irg_current_block(current_ir_graph, post_block);
	    fill_except_mem(n_callees, data, in);
	    set_Filter_cg_pred_arr(filter, n_callees, in);
      } else {
        assert(0 && "not reached");
      }
    } else { /* result */
      ir_node * filter;

      assert(is_Proj(get_Proj_pred(proj)) && get_Proj_pred(get_Proj_pred(proj)) == call);
      set_nodes_block(proj, post_block);
      filter = exchange_proj(proj);
      /* filter in die Liste der Phis aufnehmen */
      if (get_irn_link(filter) == NULL) { /* not CSE */
	    set_irn_link(filter, get_irn_link(post_block));
	    set_irn_link(post_block, filter);
      }
      fill_result(get_Proj_proj(filter), n_callees, data, in, get_irn_mode(filter));
      set_Filter_cg_pred_arr(filter, n_callees, in);
    }
  }
  DEL_ARR_F(in);
  DEL_ARR_F(callees);
  DEL_ARR_F(irgs);
  DEL_ARR_F(data);
}
#endif


void cg_construct(int arr_len, ir_entity ** free_methods_arr) {
  int i;

  if (get_irp_ip_view_state() == ip_view_valid) return;
  if (get_irp_ip_view_state() == ip_view_invalid) cg_destruct();
  set_irp_ip_view(ip_view_valid);

  collect_phicallproj();

  /* count callers */
  caller_init(arr_len, free_methods_arr);

  /* prepare irgs */
  for (i = get_irp_n_irgs() - 1; i >= 0; --i) {
    ir_graph * irg = get_irp_irg(i);
    ir_entity * ent = get_irg_entity(irg);
    irg_data_t * data = get_entity_link(ent);
    if (data->count) {
      prepare_irg(irg, data);
    } else if (data->open) {
      /* Die Methode wird nur von "der" unbekannten Aufrufstelle
       * aufgerufen. Darstellung wird f�r diese Methode nicht
       * ge�ndert. */
    } else {
      /* Methode kann nicht aufgerufen werden. Die Darstellung wird
       * f�r diese Methode nicht ge�ndert. Das kann nicht vorkommen,
       * wenn zuvor "gc_irgs()" aufgerufen wurde. */
    }
  }

  /* construct calls */
  for (i = get_irp_n_irgs() - 1; i >= 0; --i) {
    ir_node * node;

    current_ir_graph = get_irp_irg(i);
    for (node = get_irn_link(get_irg_end(current_ir_graph)); node; node = get_irn_link(node)) {
      if (is_Call(node)) {
        int j, n_callees = get_Call_n_callees(node);
        for (j = 0; j < n_callees; ++j)
          if (get_entity_irg(get_Call_callee(node, j)))
            break;
          if (j < n_callees)  /* There is an entity with a graph */
            construct_call(node);
      }
    }
  }

  /* cleanup irgs: Abschlussarbeiten: Die Vorg�nger der Methoden noch
   * explizit setzen und die zwischengespeicherten Daten wieder
   * freigeben. */
  for (i = get_irp_n_irgs() - 1; i >= 0; --i) {
    cleanup_irg(get_irp_irg(i));
  }
}



static void destruct_walker(ir_node * node, void * env)
{
  (void) env;
  if (is_Block(node)) {
    remove_Block_cg_cfgpred_arr(node);
    /* Do not turn Break into Jmp.  Better: merge blocks right away.
       Well, but there are Breaks left.
       See exc1 from ajacs-rts/Exceptions.java.  */
    if (get_Block_n_cfgpreds(node) == 1) {
      ir_node *pred = get_Block_cfgpred(node, 0);
      if (get_irn_op(pred) == op_Break)
	exchange(node, get_nodes_block(pred));
    }
  } else if (is_Filter(node)) {
    set_irg_current_block(current_ir_graph, get_nodes_block(node));
    exchange(node, new_Proj(get_Filter_pred(node), get_irn_mode(node), get_Filter_proj(node)));
  } else if (get_irn_op(node) == op_Break) {
    set_irg_current_block(current_ir_graph, get_nodes_block(node));
    exchange(node, new_Jmp());
  } else if (is_Call(node)) {
    remove_Call_callee_arr(node);
  } else if (get_irn_op(node) == op_Proj) {
    /*  some ProjX end up in strange blocks. */
    set_nodes_block(node, get_nodes_block(get_Proj_pred(node)));
  }
}


void cg_destruct(void) {
  int i;
  if (get_irp_ip_view_state() != ip_view_no) {
    for (i = get_irp_n_irgs() - 1; i >= 0; --i) {
      ir_graph * irg = get_irp_irg(i);
      irg_walk_graph(irg, destruct_walker, firm_clear_link, NULL);

	  set_irg_initial_exec(irg, skip_Id(get_irg_initial_exec(irg)));
      set_irg_frame       (irg, skip_Id(get_irg_frame(irg)));
      set_irg_initial_mem (irg, skip_Id(get_irg_initial_mem(irg)));
      set_irg_end_reg     (irg, get_irg_end(irg));
      set_irg_end_except  (irg, get_irg_end(irg));

      set_irg_callee_info_state(irg, irg_callee_info_none);
    }

    set_irp_ip_view(ip_view_no);
  }
}

#endif
