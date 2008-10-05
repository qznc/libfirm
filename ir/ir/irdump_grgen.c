/*
* Copyright (C) 1995-2007 University of Karlsruhe.  All right reserved.
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
* @brief   Write ir graph as a grgen construction rule
* @author  Andreas Schoesser
* @version $Id$
*/

/*
 * THIS IS A COMPLETE QUICK HACK! USE WITH CARE
 * NOT FOR PRODUCTION BUILD ;-)
 */

#define MAX_NODENAME_LEN 100

#include <assert.h>
#include <stdio.h>
#include <obst.h>


#include "irgraph.h"
#include "firm_types.h"
#include "pmap.h"
#include "tv.h"
#include "irgwalk.h"
#include "firm_types.h"
#include "irdump_grgen.h"


typedef struct
{
	ir_graph *irg;
	struct pmap *mode_edge_map;
	struct pmap *edge_name_map;
	struct pmap *node_name_map;  // Contains the mapping firm node -> node name
	struct obstack node_names;   // Contains the node name data
	struct pmap *mode_name_map;  // Contains the mapping firm mode -> mode node name
	struct obstack mode_names;   // Contains the "mode node name" data
	struct pmap *nodes_to_dump;  // Contains firm nodes, that have to be dumped
} grgen_dumpinfo_t;

typedef struct			// Holds information needed througout the usage of a grgen dumper instance
{
	FILE *output_file;	// The file the grgen rules will be dumped to
} irg_grgen_dumper_env_t;

static void dump_grg_node(ir_node *n, grgen_dumpinfo_t *dump_info, FILE *fp);
static void dump_grg_egde(ir_node *n, int n_edge, grgen_dumpinfo_t *dump_info, FILE *fp);
static void dump_grgen_mode(ir_node *n, grgen_dumpinfo_t *dump_info, FILE *fp, ir_mode *alt_mode);
static char *dump_grgen_mode_node(ir_mode *irn_mode, grgen_dumpinfo_t *dump_info, FILE *fp);
static void dump_grgen_eval(ir_node *n, grgen_dumpinfo_t *dump_info, FILE *fp);
static int dump_pattern(grgen_dumpinfo_t *dump_info, FILE *fp);
static void set_indent(int i);



/*****************************************************************************
* Program:		grgen_dumper.c
* Function:		Dumps parts of a firm graph (those which have to be extracted
*				as search and replace patterns) as a grGen rule.
* Depends on:	Needs the analysis info generated by the pattern creator
* Author:		Andreas Schoesser
* Date:		2006-12-07
*****************************************************************************/


// ---------------------------- INCLUDES --------------------------------




/* #include "grgen_dumper__t.h"
#include "create_pattern_t.h"
#include "firm_node_ext.h" */

// ----------------------------- GLOABALS --------------------------------

// Saves the current indent value and keeps spaces in a string
#define MAX_INDENT 100
static char indent[MAX_INDENT] = "";

// Saves the current node number to generate node names
static int node_counter;
static int edge_counter;




/************************************************************************
* Initializes the grgen_dumper module and dumps the GrGen File header
* Returns:		An environment to be passed to all functions of the GrGen
*				dumper.
* Parameters:	file:	filename of the file to dump to
*				append:	1 if the previous file content should be
*						maintained.
************************************************************************/

irg_grgen_dumper_env_t *init_irg_grgen_dumper(char *file, int append)
{
	irg_grgen_dumper_env_t *const grgen_dumper_env = XMALLOC(irg_grgen_dumper_env_t);
	FILE *fp;

	if(append)
		fp = fopen(file, "at");
	else
	{
		fp = fopen(file, "wt");

		// *** Dump header
		fprintf(fp, "%susing Firm;\n\n", indent);
	}

	grgen_dumper_env -> output_file = fp;
	return(grgen_dumper_env);
}



/************************************************************************
* Frees information used by the grgen_dumper and closes the output file
************************************************************************/

void deinit_irg_grgen_dumper(irg_grgen_dumper_env_t *grgen_dumper_env)
{
	fclose(grgen_dumper_env->output_file);
	xfree(grgen_dumper_env);
}

static void collect_nodes(ir_node *n, void * env)
{
	pmap *nodes_to_dump = (pmap *) env;

	pmap_insert(nodes_to_dump, n, NULL);
}




/************************************************************************
 * Starts dumping
 ************************************************************************/

void dump_irg_grgen_file(ir_graph *irg, char *filename, int append)
{
	FILE *fp;
	grgen_dumpinfo_t dump_info;
	int uses_memory = 0;


	irg_grgen_dumper_env_t *grgen_dumper_env = init_irg_grgen_dumper(filename, append);
	fp = grgen_dumper_env -> output_file;

	// Do initialization
	dump_info.irg = irg;			// Basically copy the graph_ana_struct, ugly
	dump_info.mode_edge_map = pmap_create();		// Create some additional pmaps to hold
	dump_info.edge_name_map = pmap_create();		// node and edge name information etc.
	dump_info.node_name_map = pmap_create();
	dump_info.mode_name_map = pmap_create();
	dump_info.nodes_to_dump = pmap_create();

	// Node and egde count start at 0 for each pattern to be dumped.
	node_counter = 0;
	edge_counter = 0;
	obstack_init(&(dump_info.node_names));
	obstack_init(&(dump_info.mode_names));

	// Dump rule header
	set_indent(0);
	fprintf(fp, "\n\n%srule %s\n%s{\n", indent, get_entity_name(get_irg_entity(irg)), indent);
	set_indent(2);

	fprintf(fp, "%spattern { }\n", indent);			 // Empty pattern
	fprintf(fp, "%sreplace\n%s{\n", indent, indent); // Graph is contrcuted in the replacement part

	set_indent(4);

	irg_walk_graph(irg, collect_nodes, NULL, dump_info.nodes_to_dump);
	uses_memory = dump_pattern(&dump_info, fp);

	// *** Dump footer
	set_indent(0);
	fprintf(fp, "%s}\n", indent);

	// Clean up
	pmap_destroy(dump_info.mode_edge_map);
	pmap_destroy(dump_info.edge_name_map);
	pmap_destroy(dump_info.node_name_map);
	pmap_destroy(dump_info.mode_name_map);
	obstack_free(&(dump_info.node_names), NULL);
	obstack_finish(&(dump_info.node_names));
	obstack_free(&(dump_info.mode_names), NULL);
	obstack_finish(&(dump_info.mode_names));

	deinit_irg_grgen_dumper(grgen_dumper_env);
}


void dump_irg_grgen(ir_graph *irg, char *suffix)
{
  char filename[100] = "";

  strncat(filename, get_entity_name(get_irg_entity(irg)), 100);
  strncat(filename, suffix, 100);
  strncat(filename, ".grg", 100);

  dump_irg_grgen_file(irg, filename, 0);
}


/************************************************************************
 * Dumps the left hand side of the rule
 ************************************************************************/

static int dump_pattern(grgen_dumpinfo_t *dump_info, FILE *fp)
{
	struct pmap *nodes_to_dump = dump_info->nodes_to_dump;
	pmap_entry *entry;
	int uses_memory = 0;

	// Dump all nodes
	foreach_pmap(nodes_to_dump, entry)
	{
		ir_node *n = (ir_node *) entry->key;

		// Dump node
		if(get_irn_opcode(n) == iro_Proj && get_irn_modecode(n) == irm_M)
			uses_memory = 1;
		dump_grg_node(n, dump_info, fp);
		dump_grgen_mode(n, dump_info, fp, NULL);
	}

	// Dump all edges
	foreach_pmap(nodes_to_dump, entry)
	{
		ir_node *n = (ir_node *) entry->key;
		int i;

		// Dump edges
		for(i = is_Block(n) ? 0 : -1; i < get_irn_arity(n); i++)
			dump_grg_egde(n, i, dump_info, fp);
	}

	fprintf(fp, "%seval {\n", indent);
	set_indent(6);
	foreach_pmap(nodes_to_dump, entry)
	{
		ir_node *n = (ir_node *) entry->key;
		dump_grgen_eval(n, dump_info, fp);
	}
	set_indent(4);
	fprintf(fp, "%s}\n", indent);


	set_indent(2);
	fprintf(fp, "%s} /* Replacement */\n", indent);
	return(uses_memory);
}



/************************************************************************
* Dumps a node in GrGen Format
************************************************************************/

static void dump_grg_node(ir_node *n, grgen_dumpinfo_t *dump_info, FILE *fp)
{
	char *node_name;

	// Already dumped the node? Then do nothing
	if(pmap_contains(dump_info -> node_name_map, n))
		return;

	// Else generate new node name and dump the node

	node_name = obstack_alloc(&(dump_info -> node_names), MAX_NODENAME_LEN);

	sprintf(node_name, "%s%ld", get_op_name(get_irn_op(n)), get_irn_node_nr(n));
	fprintf(fp, "%s%s : %s;\n", indent, node_name, get_op_name(get_irn_op(n)));

	pmap_insert(dump_info -> node_name_map, n, node_name);
	node_counter++;
}



/************************************************************************
* Dumps an edge in GrGen format
************************************************************************/

static void dump_grg_egde(ir_node *n, int n_edge, grgen_dumpinfo_t *dump_info, FILE *fp)
{
	ir_node *to_node;
	char *from_node_name, *to_node_name;
	char **nodes_edge_names;


	// Check if to_node has also to be dumped. If not, skip this edge
	// We have to dump to_node here, because to_node has to be known by grgen before
	// connecting an edge to it.
	to_node =  get_irn_n(n, n_edge);
	if(!pmap_contains(dump_info -> nodes_to_dump, to_node))
		return;

	if((nodes_edge_names = pmap_get(dump_info -> edge_name_map, n)) == NULL)
	{
		nodes_edge_names = (char **) obstack_alloc(&(dump_info->node_names), (get_irn_arity(n) + 1) * sizeof(char *));
		memset(nodes_edge_names, 0, (get_irn_arity(n) + 1) * sizeof(char *));
		pmap_insert(dump_info->edge_name_map, n, nodes_edge_names);
	}

	assert(pmap_contains(dump_info -> node_name_map, n));
	assert(pmap_contains(dump_info -> node_name_map, to_node));
	from_node_name = (char *) pmap_get(dump_info -> node_name_map, n);
	to_node_name = (char *) pmap_get(dump_info -> node_name_map, to_node);

	{

	char edge_name[50], *edge_name_obst;

	sprintf(edge_name, "pos%d_%d", n_edge + 1, edge_counter++);
	edge_name_obst = obstack_alloc(&(dump_info->node_names), strlen(edge_name) + 1);
	strcpy(edge_name_obst, edge_name);
	nodes_edge_names[n_edge + 1] = edge_name_obst;

	fprintf(fp, "%s%s -%s:df-> %s;\n", indent, from_node_name, edge_name_obst, to_node_name);
	}


}



/************************************************************************
* Dumps an FIRM Mode as GrGen Code
* If source_node_name == NULL, that name of n that was already
* generated is used.
* If source_node_name != NULL, this given source will be used
* (useful for retyped nodes)
************************************************************************/

static void dump_grgen_mode(ir_node *n, grgen_dumpinfo_t *dump_info, FILE *fp, ir_mode *alt_mode)
{
	char *node_name = (char *) pmap_get(dump_info -> node_name_map, n);
	ir_mode *irn_mode = (alt_mode != NULL) ? alt_mode : get_irn_mode(n);
	char edge_name[50];
	char *mode_node_name;

	mode_node_name = dump_grgen_mode_node(irn_mode, dump_info, fp);

	//mode_code =  get_mode_modecode(irn_mode);
	//mode_name =  get_mode_name(irn_mode);

	// Yes, use the given mode-node
	//mode_node_name = pmap_get(dump_info -> mode_name_map, (void *) mode_code);
	sprintf(edge_name, "m%d", edge_counter++);

	if(pmap_get(dump_info->mode_edge_map, n) == NULL)
	{
		char *edge_name_obst = obstack_alloc(&(dump_info->node_names), strlen(edge_name) + 1);
		strcpy(edge_name_obst, edge_name);
		pmap_insert(dump_info->mode_edge_map, n, edge_name_obst);
	}

	// Dump the edge from the current node to it's mode node
	fprintf(fp, "%s%s -%s:has_mode-> %s;\n", indent, node_name, edge_name, mode_node_name);
}



/************************************************************************
* Dumps a node representing a node
************************************************************************/

static char *dump_grgen_mode_node(ir_mode *irn_mode, grgen_dumpinfo_t *dump_info, FILE *fp)
{
	ir_modecode mode_code = get_mode_modecode(irn_mode);
	const char *mode_name =  get_mode_name(irn_mode);
	char *mode_node_name;

	if(!pmap_contains(dump_info -> mode_name_map, (void *) mode_code))
	{
		// No, create a new mode-node
		mode_node_name = obstack_alloc(&(dump_info -> mode_names), MAX_NODENAME_LEN);
		sprintf(mode_node_name, "mode_%s_node", mode_name);
		pmap_insert(dump_info -> mode_name_map, (void *) mode_code, mode_node_name);
		fprintf(fp, "%s%s : Mode_%s;\n", indent, mode_node_name, mode_name);
		return(mode_node_name);
	}
	else
	{
		return((char *) pmap_get(dump_info -> mode_name_map, (void *) mode_code));
	}

}



/************************************************************************
* Dumps the condition for the given node, depending on the node's
* attributes and the node's opcode
************************************************************************/

static void dump_grgen_eval(ir_node *n, grgen_dumpinfo_t *dump_info, FILE *fp)
{
	char *node_name;
	ir_opcode code = get_irn_opcode(n);

	if(code == iro_Const)
	{
		node_name = pmap_get(dump_info->node_name_map, n);
		fprintf(fp, "%s%s.value = \"%ld\";\n", indent, node_name, get_tarval_long(get_Const_tarval(n)));
	}


	if(code == iro_Proj)
	{
		node_name = pmap_get(dump_info->node_name_map, n);
		fprintf(fp, "%s%s.proj = %ld;\n", indent, node_name, get_Proj_proj(n));
	}

	/*if(code == iro_Block)
	{
		node_name = pmap_get(dump_info->node_name_map, n);
		fprintf(fp, "%s%s.pos = %d;\n", indent, node_name, ??);
	}

	if(code == iro_Phi)
	{
		node_name = pmap_get(dump_info->node_name_map, n);
		fprintf(fp, "%s%s.pos = %d;\n", indent, node_name, ??);
	}*/

	// TODO: Dump Block evals: edge numbers


	if(code == iro_Phi || code == iro_Block)
	{
		char **edge_names;
		int i;

		//assert((get_irn_arity(n) == get_irn_arity(phi_block)) && "Phi has other arity than it's block! Pattern seems to be broken.");

		// Load the edge names that have been saved
		edge_names = pmap_get(dump_info->edge_name_map, n);
		assert(edge_names && "Some edge names have not been dumped!");

		// Correlate the matched phi edges with the matched block edges
		// Caution: Position 0 in the edge_names array is the block edge, so start at 1
		for(i = code == iro_Block; i < get_irn_arity(n) + 1; i++)
		{
			assert(edge_names[i] != NULL && "Some edges have not been dumped!");

			fprintf(fp, "%s%s.pos = %d;\n", indent, edge_names[i], i);
		}
		return;
	}
}





/************************************************************************
* Sets current indent
************************************************************************/

static void set_indent(int i)
{
	int j;

	// Generate a string containing i blank characters
	if(i < MAX_INDENT - 1)
	{
		for(j = 0; j < i; j++)
			indent[j] = ' ';
		indent[j] = 0x0;
	}
}
