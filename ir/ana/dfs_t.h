/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @author  Sebastian Hack
 * @date    21.04.2007
 * @brief
 *
 * depth first search internal stuff.
 */
#ifndef FIRM_ANA_DFS_T_H
#define FIRM_ANA_DFS_T_H

#include "hashptr.h"
#include "absgraph.h"
#include "obst.h"
#include "dfs.h"

struct dfs_node_t {
	int visited;
	const void *node;
	const void *ancestor;
	int pre_num;
	int max_pre_num;
	int post_num;
	int level;
};

struct dfs_edge_t {
	const void *src, *tgt;
	dfs_node_t *s, *t;
	dfs_edge_kind_t kind;
};

struct dfs_t {
	void *graph;
	const absgraph_t *graph_impl;
	struct obstack obst;

	set *nodes;
	set *edges;
	dfs_node_t **pre_order;
	dfs_node_t **post_order;

	int pre_num;
	int post_num;

	unsigned edges_classified : 1;
};

static dfs_node_t *_dfs_get_node(const dfs_t *self, const void *node)
{
	dfs_node_t templ;
	memset(&templ, 0, sizeof(templ));
	templ.node = node;
	return set_insert(dfs_node_t, self->nodes, &templ, sizeof(templ), hash_ptr(node));
}

#define _dfs_int_is_ancestor(n, m) ((m)->pre_num >= (n)->pre_num && (m)->pre_num <= (n)->max_pre_num)

static inline int _dfs_is_ancestor(const dfs_t *dfs, const void *a, const void *b)
{
	dfs_node_t *n = _dfs_get_node(dfs, a);
	dfs_node_t *m = _dfs_get_node(dfs, b);
	return _dfs_int_is_ancestor(n, m);
}

#define dfs_get_n_nodes(dfs)            ((dfs)->pre_num)
#define dfs_get_pre_num(dfs, node)      (_dfs_get_node((dfs), (node))->pre_num)
#define dfs_get_post_num(dfs, node)     (_dfs_get_node((dfs), (node))->post_num)
#define dfs_get_pre_num_node(dfs, num)  ((dfs)->pre_order[num]->node)
#define dfs_get_post_num_node(dfs, num) ((dfs)->post_order[num]->node)
#define dfs_is_ancestor(dfs, n, m)      _dfs_is_ancestor((n), (m))

#endif /* FIRM_ANA_DFS_T_H */
