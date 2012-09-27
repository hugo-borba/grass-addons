/* PURPOSE:      Develop the image segments */

/* Currently only region growing is implemented */

#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <math.h>
#include <time.h>
#include <grass/gis.h>
#include <grass/glocale.h>
#include <grass/raster.h>
#include <grass/segment.h>	/* segmentation library */
#include <grass/rbtree.h>	/* Red Black Tree library functions */
#include "iseg.h"

#define EPSILON 1.0e-12

/* internal functions */
static int merge_regions(struct ngbr_stats *, struct reg_stats *, /* Ri */
                         struct ngbr_stats *, struct reg_stats *, /* Rk */
			 int,
			 struct globals *);
static int search_neighbors(struct ngbr_stats *,    /* Ri */
			    struct reg_stats *,
                            struct NB_TREE *,       /* Ri's neighbors */ 
			    double *,               /* highest similarity */ 
			    struct ngbr_stats *,    /* Ri's best neighbor */
			    struct reg_stats *,
		            struct globals *);
static int set_candidate_flag(struct ngbr_stats *, int , struct globals *);
static int find_best_neighbor(struct ngbr_stats *, struct reg_stats *,
			      struct NB_TREE *, struct ngbr_stats *,
			      struct reg_stats *, double *, int,
			      struct globals *);
static int calculate_reg_stats(int, int, struct reg_stats *, 
                         struct globals *);

/* function used by binary tree to compare items */
static int compare_rc(const void *first, const void *second)
{
    struct rc *a = (struct rc *)first, *b = (struct rc *)second;

    if (a->row < b->row)
	return -1;
    if (a->row > b->row)
	return 1;

    /* same row */
    if (a->col < b->col)
	return -1;
    if (a->col > b->col)
	return 1;
    /* same row and col */
    return 0;
}

static int compare_ints(const void *first, const void *second)
{
    int a = *(int *)first, b = *(int *)second;

    return (a < b ? -1 : (a > b));
}

static int compare_double(double first, double second)
{

    /* standard comparison, gives good results */
    if (first < second)
	return -1;
    return (first > second);

    /* fuzzy comparison, 
     * can give weird results if EPSILON is too large or 
     * if the formula is changed because this is operating at the 
     * limit of double fp precision */
    if (first < second && first + first * EPSILON < second)
	    return -1;
    if (first > second && first > second + second * EPSILON)
	    return 1;
    return 0;
}

static int dump_Ri(struct ngbr_stats *Ri, struct reg_stats *Ri_rs, double *Ri_sim,
                   double *Rk_sim, int *Ri_nn, int *Rk_nn, struct globals *globals)
{
    int i;

    G_debug(0, "Ri, Ri_rs ID: %d, %d", Ri->id, Ri_rs->id);
    G_debug(0, "Ri, Ri_rs count: %d, %d", Ri->count, Ri_rs->count);

    for (i = 0; i < globals->nbands; i++) {
	G_debug(0, "Ri, Ri_rs mean %d: %g, %g", i, Ri->mean[i], Ri_rs->mean[i]);
	G_debug(0, "Ri_rs sum %d: %g", i, Ri_rs->sum[i]);
    }
    G_debug(0, "Ri nn: %d", *Ri_nn);
    if (Rk_nn)
	G_debug(0, "Rk nn: %d", *Rk_nn);
    G_debug(0, "Ri similarity: %g", *Ri_sim);
    if (Rk_sim)
	G_debug(0, "Rk similarity: %g", *Rk_sim);

    return 1;
}

int create_isegs(struct globals *globals)
{
    int row, col;
    int successflag = 1;
    int have_bound;
    CELL current_bound, bounds_val;

    G_debug(1, "Threshold: %g", globals->threshold);
    G_debug(1, "segmentation method: %d", globals->method);

    if (globals->bounds_map == NULL) {
	/* just one time through loop */
	successflag = region_growing(globals);
    }
    else {
	/* outer processing loop for polygon constraints */
	for (current_bound = globals->lower_bound;
	     current_bound <= globals->upper_bound; current_bound++) {

	    G_debug(1, "current_bound = %d", current_bound);

	    have_bound = 0;

	    /* get min/max row/col to narrow the processing window */
	    globals->row_min = globals->nrows;
	    globals->row_max = 0;
	    globals->col_min = globals->ncols;
	    globals->col_max = 0;
	    for (row = 0; row < globals->nrows; row++) {
		for (col = 0; col < globals->ncols; col++) {
		    segment_get(&globals->bounds_seg, &bounds_val,
				row, col);

		    if (bounds_val == current_bound) {
			have_bound = 1;

			FLAG_UNSET(globals->null_flag, row, col);

			if (globals->row_min > row)
			    globals->row_min = row;
			if (globals->row_max < row)
			    globals->row_max = row;
			if (globals->col_min > col)
			    globals->col_min = col;
			if (globals->col_max < col)
			    globals->col_max = col;
		    }
		    else
			FLAG_SET(globals->null_flag, row, col);
		}
	    }
	    globals->row_max++;
	    globals->col_max++;

	    if (have_bound)
		successflag = region_growing(globals);
	}    /* end outer loop for processing polygons */
    }

    return successflag;
}


int region_growing(struct globals *globals)
{
    int row, col, t;
    double threshold, adjthresh, Ri_similarity, Rk_similarity;
    double alpha2, divisor;		/* threshold parameters */
    int n_merges, do_merge;		/* number of merges on that iteration */
    int pathflag;		/* =1 if we didn't find mutually best neighbors, continue with Rk */
    int candidates_only;
    struct ngbr_stats Ri,
                      Rk,
	              Rk_bestn,         /* Rk's best neighbor */
		      *next;
    int Ri_nn, Rk_nn; /* number of neighbors for Ri/Rk */
    struct NB_TREE *Ri_ngbrs, *Rk_ngbrs;
    struct NB_TRAV travngbr;
    /* not all regions are in the tree, but we always need reg_stats for Ri and Rk */
    struct reg_stats Ri_rs, Rk_rs, Rk_bestn_rs;
    double *dp;
    struct NB_TREE *tmpnbtree;

    G_verbose_message("Running region growing algorithm");

    /* init neighbor stats */
    Ri.mean = G_malloc(globals->datasize);
    Rk.mean = G_malloc(globals->datasize);
    Rk_bestn.mean = G_malloc(globals->datasize);

    Ri_ngbrs = nbtree_create(globals->nbands, globals->datasize);
    Rk_ngbrs = nbtree_create(globals->nbands, globals->datasize);

    /* init region stats */
    Ri_rs.mean = G_malloc(globals->datasize);
    Ri_rs.sum = G_malloc(globals->datasize);
    Rk_rs.mean = G_malloc(globals->datasize);
    Rk_rs.sum = G_malloc(globals->datasize);
    Rk_bestn_rs.mean = G_malloc(globals->datasize);
    Rk_bestn_rs.sum = G_malloc(globals->datasize);
    
    t = 0;
    n_merges = 1;

    /* threshold calculation */
    alpha2 = globals->alpha * globals->alpha;
    threshold = alpha2 * globals->threshold;
    G_debug(1, "Squared threshold: %g", threshold);

    /* make the divisor a constant ? */
    divisor = globals->nrows + globals->ncols;

    while (t++ < globals->end_t && n_merges > 0) {

	G_message(_("Pass %d:"), t);

	n_merges = 0;
	globals->candidate_count = 0;
	flag_clear_all(globals->candidate_flag);

	/* Set candidate flag to true/1 for all non-NULL cells */
	for (row = globals->row_min; row < globals->row_max; row++) {
	    for (col = globals->col_min; col < globals->col_max; col++) {
		if (!(FLAG_GET(globals->null_flag, row, col))) {
		    
		    FLAG_SET(globals->candidate_flag, row, col);
		    globals->candidate_count++;
		}
	    }
	}

	G_debug(4, "Starting to process %d candidate cells",
		globals->candidate_count);

	/*process candidate cells */
	for (row = globals->row_min; row < globals->row_max; row++) {
	    G_percent(row, globals->row_max, 4);
	    for (col = globals->col_min; col < globals->col_max; col++) {
		if (!(FLAG_GET(globals->candidate_flag, row, col)))
		    continue;

		pathflag = TRUE;
		candidates_only = TRUE;

		nbtree_clear(Ri_ngbrs);
		nbtree_clear(Rk_ngbrs);

		G_debug(4, "Next starting cell: row, %d, col, %d",
			row, col);

		/* First cell in Ri is current row/col */
		Ri.row = row;
		Ri.col = col;

		/* get Ri's segment ID */
		segment_get(&globals->rid_seg, (void *)&Ri.id, Ri.row, Ri.col);
		
		if (Ri.id < 0)
		    continue;

		/* find segment neighbors */
		/* find Ri's best neighbor, clear candidate flag */
		Ri_similarity = globals->threshold + 1;

		Ri_rs.id = Ri.id;
		fetch_reg_stats(Ri.row, Ri.col, &Ri_rs, globals);
		memcpy(Ri.mean, Ri_rs.mean, globals->datasize);
		Ri.count = Ri_rs.count;

		/* Ri is now complete */
		G_debug(4, "Ri is now complete");

		/* find best neighbor, get region stats for Rk */
		Ri_nn = find_best_neighbor(&Ri, &Ri_rs, Ri_ngbrs,
					   &Rk, &Rk_rs, &Ri_similarity,
					   1, globals);
		/* Rk is now complete */
		G_debug(4, "Rk is now complete");

		if (Ri_nn == 0) {
		    /* this can only happen if only one segment is left */
		    G_debug(4, "Segment had no valid neighbors");
		    pathflag = FALSE;
		    Ri.count = 0;
		}

		if (/* !(t & 1) && */ Ri_nn == 1 &&
		    !(FLAG_GET(globals->candidate_flag, Rk.row, Rk.col)) &&
		    compare_double(Ri_similarity, threshold) == -1) {
		    /* this is slow ??? */
		    int smaller = Rk.count;

		    if (Ri.count < Rk.count)
			smaller = Ri.count;

		    adjthresh = pow(alpha2, 1. + (double) smaller / divisor) *
				globals->threshold;

		    if (compare_double(Ri_similarity, adjthresh) == -1) {
			G_debug(4, "Ri nn == 1");
			if (Rk.count < 2)
			    G_fatal_error("Rk count too low");
			if (Rk.count < Ri.count)
			    G_debug(4, "Rk count lower than Ri count");

			merge_regions(&Ri, &Ri_rs, &Rk, &Rk_rs, 1, globals);
			n_merges++;
		    }

		    pathflag = FALSE;
		}
		/* this is slow ??? */
		if (/* t & */ 1) {
		    if ((globals->nn < 8 && Rk.count <= 8) || 
		        (globals->nn >= 8 && Rk.count <= globals->nn))
		    candidates_only = FALSE;
		}

		while (pathflag) {
		    pathflag = FALSE;
		    
		    /* optional check if Rk is candidate
		     * to prevent backwards merging */
		    if (candidates_only && 
		        !(FLAG_GET(globals->candidate_flag, Rk.row, Rk.col))) {

			Ri_similarity = globals->threshold + 1;
		    }

		    candidates_only = TRUE;

		    if (compare_double(Ri_similarity, threshold) == -1) {
			do_merge = 1;

			/* we'll have the neighbor pixel to start with. */
			G_debug(4, "Working with Rk");

			/* find Rk's best neighbor, do not clear candidate flag */
			/* Rk_similarity = globals->threshold + 1; */
			Rk_similarity = Ri_similarity;
			Rk_bestn_rs.count = 0;
			/* Rk_rs is already complete */
			Rk_nn = find_best_neighbor(&Rk, &Rk_rs, Rk_ngbrs,
						   &Rk_bestn,
						   &Rk_bestn_rs,
						   &Rk_similarity, 0,
						   globals);

			/* not mutually best neighbors */
			if (Rk_similarity != Ri_similarity) {
			    /* important for fp precision limit
			     * because region stats may be calculated 
			     * in two slightly different ways */
			    if (fabs(Ri_similarity / Rk_similarity  - 1) > EPSILON)
				do_merge = 0;
			}
			/* Ri has only one neighbor, merge */
			if (Ri_nn == 1 && Rk_nn > 1)
			    do_merge = 1;

			/* adjust threshold */
			if (do_merge) {
			    int smaller = Rk.count;

			    if (Ri.count < Rk.count)
				smaller = Ri.count;

			    adjthresh = pow(alpha2, 1. + (double) smaller / divisor) *
					globals->threshold;

			    do_merge = 0;
			    if (compare_double(Ri_similarity, adjthresh) == -1) {
				do_merge = 1;
			    }
			}

			if (do_merge) {
			    
			    G_debug(4, "merge neighbor trees");

			    Ri_nn -= Ri_ngbrs->count;
			    Ri_nn += (Rk_nn - Rk_ngbrs->count);
			    globals->ns.id = Rk.id;
			    nbtree_remove(Ri_ngbrs, &(globals->ns));

			    nbtree_init_trav(&travngbr, Rk_ngbrs);
			    while ((next = nbtree_traverse(&travngbr))) {
				if (!nbtree_find(Ri_ngbrs, next) && next->id != Ri.id)
				    nbtree_insert(Ri_ngbrs, next);
			    }
			    nbtree_clear(Rk_ngbrs);
			    Ri_nn += Ri_ngbrs->count;

			    merge_regions(&Ri, &Ri_rs, &Rk, &Rk_rs, 1, globals);
			    /* Ri is now updated, Rk no longer usable */

			    /* made a merge, need another iteration */
			    n_merges++;

			    Ri_similarity = globals->threshold + 1;
			    Rk_similarity = globals->threshold + 1;

			    /* we have checked the neighbors of Ri, Rk already
			     * use faster version of finding the best neighbor
			     */
			    
			    /* use neighbor tree to find Ri's new best neighbor */
			    search_neighbors(&Ri, &Ri_rs, Ri_ngbrs, &Ri_similarity,
					     &Rk, &Rk_rs, globals);

			    if (Ri_nn > 0 && compare_double(Ri_similarity, threshold) == -1) {

				pathflag = TRUE;
				/* candidates_only:
				 * FALSE: less passes, takes a bit longer, but less memory
				 * TRUE: more passes, is a bit faster */
				candidates_only = FALSE;
			    }
			    /* else end of Ri -> Rk chain since we merged Ri and Rk
			     * go to next row, col */
			}
			else {
			    if (compare_double(Rk_similarity, threshold) == -1) {
				pathflag = TRUE;
			    }
			    /* test this: can it cause an infinite loop ? */
			    if (!(FLAG_GET(globals->candidate_flag, Rk.row, Rk.col)))
				pathflag = FALSE;

			    if (Rk_nn < 2)
				pathflag = FALSE;

			    if (Rk.id < 1)
				pathflag = FALSE;

			    if (pathflag) {
				
				/* clear candidate flag for Rk */
				if (FLAG_GET(globals->candidate_flag, Rk.row, Rk.col)) {
				    set_candidate_flag(&Rk, FALSE, globals);
				}

				/* Use Rk as next Ri:  
				 * this is the eCognition technique. */
				G_debug(4, "do ecog");
				Ri_nn = Rk_nn;
				Ri_similarity = Rk_similarity;

				dp = Ri.mean;
				Ri = Rk;
				Rk = Rk_bestn;
				Rk_bestn.mean = dp;

				Ri_rs.id = Rk_rs.id;
				Rk_rs.id = Rk_bestn_rs.id;
				Rk_bestn_rs.id = 0;
				Ri_rs.count = Rk_rs.count;
				Rk_rs.count = Rk_bestn_rs.count;
				Rk_bestn_rs.count = 0;
				dp = Ri_rs.mean;
				Ri_rs.mean = Rk_rs.mean;
				Rk_rs.mean = Rk_bestn_rs.mean;
				Rk_bestn_rs.mean = dp;
				dp = Ri_rs.sum;
				Ri_rs.sum = Rk_rs.sum;
				Rk_rs.sum = Rk_bestn_rs.sum;
				Rk_bestn_rs.sum = dp;

				tmpnbtree = Ri_ngbrs;
				Ri_ngbrs = Rk_ngbrs;
				Rk_ngbrs = tmpnbtree;
				nbtree_clear(Rk_ngbrs);
			    }
			}
		    }    /* end if < threshold */
		}    /* end pathflag */
	    }    /* next col */
	}    /* next row */

	/* finished one pass for processing candidate pixels */
	G_verbose_message("%d merges", n_merges);

	G_debug(4, "Finished pass %d", t);
    }

    /*end t loop *//*TODO, should there be a max t that it can iterate for?  Include t in G_message? */
    if (n_merges > 0)
	G_message(_("Segmentation processes stopped at %d due to reaching max iteration limit, more merges may be possible"), t);
    else
	G_message(_("Segmentation converged after %d iterations."), t);


    /* ****************************************************************************************** */
    /* final pass, ignore threshold and force a merge for small segments with their best neighbor */
    /* ****************************************************************************************** */
    
    if (globals->min_segment_size > 1) {
	G_message(_("Merging segments smaller than %d cells"), globals->min_segment_size);

	threshold = globals->alpha * globals->alpha * globals->threshold;

	flag_clear_all(globals->candidate_flag);
	
	n_merges = 0;

	/* Set candidate flag to true/1 for all non-NULL cells */
	for (row = globals->row_min; row < globals->row_max; row++) {
	    for (col = globals->col_min; col < globals->col_max; col++) {
		if (!(FLAG_GET(globals->null_flag, row, col))) {
		    FLAG_SET(globals->candidate_flag, row, col);

		    globals->candidate_count++;
		}
	    }
	}

	G_debug(4, "Starting to process %d candidate cells",
		globals->candidate_count);

	/* process candidate cells */
	for (row = globals->row_min; row < globals->row_max; row++) {
	    G_percent(row, globals->row_max, 9);
	    for (col = globals->col_min; col < globals->col_max; col++) {
		int do_merge = 1;
		
		if (!(FLAG_GET(globals->candidate_flag, row, col)))
		    continue;
		
		nbtree_clear(Ri_ngbrs);
		nbtree_clear(Rk_ngbrs);

		Ri.row = row;
		Ri.col = col;

		/* get segment id */
		segment_get(&globals->rid_seg, (void *) &Ri.id, row, col);
		
		if (Ri.id < 0)		
		    continue;

		Ri_rs.id = Ri.id;

		/* get segment size */

		fetch_reg_stats(Ri.row, Ri.col, &Ri_rs, globals);
		memcpy(Ri.mean, Ri_rs.mean, globals->datasize);
		Ri.count = Ri_rs.count;

		while (do_merge) {

		    do_merge = 0;

		    /* merge all smaller than min size */
		    if (Ri.count < globals->min_segment_size)
			do_merge = 1;

		    Ri_nn = 0;
		    Ri_similarity = globals->threshold + 1;

		    if (do_merge) {

			segment_get(&globals->bands_seg, (void *)Ri.mean,
				    Ri.row, Ri.col);

			/* find Ri's best neighbor, clear candidate flag */
			Ri_nn = find_best_neighbor(&Ri, &Ri_rs, Ri_ngbrs,
						   &Rk, &Rk_rs,
						   &Ri_similarity, 1,
						   globals);
		    }

		    if (do_merge) {

			nbtree_clear(Ri_ngbrs);
			
			/* merge Ri with Rk */
			/* do not clear candidate flag for Rk */
			merge_regions(&Ri, &Ri_rs, &Rk, &Rk_rs, 0, globals);
			n_merges++;

			if (Ri_nn <= 0 || Ri.count >= globals->min_segment_size)
			    do_merge = 0;
		    }
		}
	    }
	}
	G_percent(1, 1, 1);

	/* finished one pass for processing candidate pixels */
	G_verbose_message("%d merges", n_merges);
    }
    
    /* free neighbor stats */
    G_free(Ri.mean);
    G_free(Rk.mean);
    G_free(Rk_bestn.mean);
    
    nbtree_clear(Ri_ngbrs);
    nbtree_clear(Rk_ngbrs);
    free(Ri_ngbrs);
    free(Rk_ngbrs);

    /* free region stats */
    G_free(Ri_rs.mean);
    G_free(Ri_rs.sum);
    G_free(Rk_rs.mean);
    G_free(Rk_rs.sum);
    G_free(Rk_bestn_rs.mean);
    G_free(Rk_bestn_rs.sum);

    return TRUE;
}

static int find_best_neighbor(struct ngbr_stats *Ri,
			      struct reg_stats *Ri_rs,
			      struct NB_TREE *Ri_ngbrs, 
			      struct ngbr_stats *Rk, 
			      struct reg_stats *Rk_rs,
			      double *sim, int clear_cand,
			      struct globals *globals)
{
    int n, n_ngbrs, no_check, cmp;
    struct rc ngbr_rc, next;
    struct rclist rilist;
    double tempsim;
    int neighbors[8][2];
    struct RB_TREE *no_check_tree;	/* cells already checked */
    struct reg_stats *rs_found;

    G_debug(4, "find_best_neighbor()");

    /* dynamics of the region growing algorithm
     * some regions are growing fast, often surrounded by many small regions
     * not all regions are equally growing, some will only grow at a later stage ? */

    /* *** initialize data *** */

    no_check_tree = rbtree_create(compare_rc, sizeof(struct rc));
    ngbr_rc.row = Ri->row;
    ngbr_rc.col = Ri->col;
    rbtree_insert(no_check_tree, &ngbr_rc);

    n_ngbrs = 0;
    /* TODO: add size of largest region to reg_tree, use this as min */
    Rk->count = globals->ncells;

    /* go through segment, spreading outwards from head */
    rclist_init(&rilist);

    /* check neighbors of start cell */
    next.row = Ri->row;
    next.col = Ri->col;
    do {
	/* remove from candidates */
	if (clear_cand)
	    FLAG_UNSET(globals->candidate_flag, next.row, next.col);

	G_debug(5, "find_pixel_neighbors for row: %d , col %d",
		next.row, next.col);

	globals->find_neighbors(next.row, next.col, neighbors);
	
	n = globals->nn - 1;
	do {

	    globals->ns.row = ngbr_rc.row = neighbors[n][0];
	    globals->ns.col = ngbr_rc.col = neighbors[n][1];

	    no_check = (ngbr_rc.row < globals->row_min ||
	                ngbr_rc.row >= globals->row_max ||
		        ngbr_rc.col < globals->col_min ||
			ngbr_rc.col >= globals->col_max);

	    n_ngbrs += no_check;

	    if (!no_check) {

		no_check = ((FLAG_GET(globals->null_flag, ngbr_rc.row,
							  ngbr_rc.col)) != 0);
		n_ngbrs += no_check;

		if (!no_check) {

		    if (!rbtree_find(no_check_tree, &ngbr_rc)) {

			/* not yet checked, don't check it again */
			rbtree_insert(no_check_tree, &ngbr_rc);

			/* get neighbor ID */
			segment_get(&globals->rid_seg,
				    (void *) &(globals->ns.id),
				    ngbr_rc.row, ngbr_rc.col);

			if (globals->ns.id == Ri->id) {

			    /* want to check this neighbor's neighbors */
			    rclist_add(&rilist, ngbr_rc.row, ngbr_rc.col);
			}
			else {

			    /* new neighbor ? */
			    if (nbtree_find(Ri_ngbrs, &globals->ns) == NULL) {

				/* get values for Rk */
				globals->rs.id = globals->ns.id;
				rs_found = rgtree_find(globals->reg_tree,
						       &(globals->rs));
				if (!rs_found) {
				    /* region stats are not in search tree */
				    rs_found = &(globals->rs);
				    calculate_reg_stats(ngbr_rc.row, ngbr_rc.col,
							rs_found, globals);
				}
				globals->ns.mean = rs_found->mean;
				globals->ns.count = rs_found->count;
				/* globals->ns is now complete */

				tempsim = (globals->calculate_similarity)(Ri, &globals->ns, globals);

				cmp = compare_double(tempsim, *sim);
				if (cmp == -1) {
				    *sim = tempsim;
				    /* copy temp Rk to Rk */
				    Rk->row = ngbr_rc.row;
				    Rk->col = ngbr_rc.col;

				    Rk->id = rs_found->id;
				    Rk->count = rs_found->count;
				    memcpy(Rk->mean, rs_found->mean,
				           globals->datasize);

				    Rk_rs->id = Rk->id;
				    Rk_rs->count = Rk->count;
				    memcpy(Rk_rs->mean, rs_found->mean,
				           globals->datasize);
				    memcpy(Rk_rs->sum, rs_found->sum,
				           globals->datasize);
				}
				else if (cmp == 0) {
				    /* resolve ties: prefer smaller regions */

				    if (Rk->count > globals->ns.count) {
					/* copy temp Rk to Rk */
					Rk->row = ngbr_rc.row;
					Rk->col = ngbr_rc.col;

					Rk->id = rs_found->id;
					Rk->count = rs_found->count;
					memcpy(Rk->mean, rs_found->mean,
					       globals->datasize);

					Rk_rs->id = Rk->id;
					Rk_rs->count = Rk->count;
					memcpy(Rk_rs->mean, rs_found->mean,
					       globals->datasize);
					memcpy(Rk_rs->sum, rs_found->sum,
					       globals->datasize);
				    }
				}

				n_ngbrs++;
				nbtree_insert(Ri_ngbrs, &globals->ns);
			    }
			}
		    }
		}
	    }
	} while (n--);    /* end do loop - next neighbor */
    } while (rclist_drop(&rilist, &next));   /* while there are cells to check */

    /* clean up */
    rbtree_destroy(no_check_tree);

    return n_ngbrs;
}

void find_four_neighbors(int p_row, int p_col,
			        int neighbors[8][2])
{
    /* north */
    neighbors[0][0] = p_row - 1;
    neighbors[0][1] = p_col;

    /* east */
    neighbors[1][0] = p_row;
    neighbors[1][1] = p_col + 1;

    /* south */
    neighbors[2][0] = p_row + 1;
    neighbors[2][1] = p_col;

    /* west */
    neighbors[3][0] = p_row;
    neighbors[3][1] = p_col - 1;

    return;
}

void find_eight_neighbors(int p_row, int p_col,
			         int neighbors[8][2])
{
    /* get the 4 orthogonal neighbors */
    find_four_neighbors(p_row, p_col, neighbors);

    /* get the 4 diagonal neighbors */
    /* north-west */
    neighbors[4][0] = p_row - 1;
    neighbors[4][1] = p_col - 1;

    /* north-east */
    neighbors[5][0] = p_row - 1;
    neighbors[5][1] = p_col + 1;

    /* south-west */
    neighbors[6][0] = p_row + 1;
    neighbors[6][1] = p_col - 1;

    /* south-east */
    neighbors[7][0] = p_row + 1;
    neighbors[7][1] = p_col + 1;

    return;
}

/* similarity / distance between two points based on their input raster values */
/* assumes first point values already saved in files->bands_seg - only run segment_get once for that value... */
/* TODO: segment_get already happened for a[] values in the main function.  Could remove a[] from these parameters */
double calculate_euclidean_similarity(struct ngbr_stats *Ri,
                                      struct ngbr_stats *Rk,
				      struct globals *globals)
{
    double val = 0., diff;
    int n = globals->nbands - 1;

    /* squared euclidean distance, sum the square differences for each dimension */
    do {
	diff = Ri->mean[n] - Rk->mean[n];
	    
	val += diff * diff;
    } while (n--);

    return val;
}


static int search_neighbors(struct ngbr_stats *Ri,
			    struct reg_stats *Ri_rs,
                            struct NB_TREE *Ri_ngbrs, 
			    double *sim,
			    struct ngbr_stats *Rk,
			    struct reg_stats *Rk_rs,
		            struct globals *globals)
{
    double tempsim, *dp;
    struct NB_TRAV travngbr;
    struct ngbr_stats *next;
    int cmp, i;

    G_debug(4, "search_neighbors");

    nbtree_init_trav(&travngbr, Ri_ngbrs);
    Rk->count = globals->ncells + 1;

    while ((next = nbtree_traverse(&travngbr))) {
	tempsim = (globals->calculate_similarity)(Ri, next, globals);

	cmp = compare_double(tempsim, *sim);
	if (cmp == -1) {
	    *sim = tempsim;

	    dp = Rk->mean;
	    *Rk = *next;
	    Rk->mean = dp;
	    memcpy(Rk->mean, next->mean, globals->datasize);
	}
	else if (cmp == 0) {
	    /* resolve ties, prefer smaller regions */
	    G_debug(4, "resolve ties");

	    if (Rk->count > next->count) {
		dp = Rk->mean;
		*Rk = *next;
		Rk->mean = dp;
		memcpy(Rk->mean, next->mean, globals->datasize);
	    }
	}
    }
    Rk_rs->id = Rk->id;

    /* faster, but with fp error:
     * calculate sum from mean and count */
    /*
    Rk_rs->count = Rk->count;
    memcpy(Rk_rs->mean, Rk->mean, globals->datasize);
    i = globals->nbands - 1;
    do {
	Rk_rs->sum[i] = Rk_rs->mean[i] * Rk_rs->count;
    } while (i--);
    */

    /* a bit slower but correct: */
    fetch_reg_stats(Rk->row, Rk->col, Rk_rs, globals);

    return 1;
}

static int merge_regions(struct ngbr_stats *Ri, struct reg_stats *Ri_rs,
		         struct ngbr_stats *Rk, struct reg_stats *Rk_rs,
		         int do_cand, struct globals *globals)
{
    int n;
    int R_id;
    struct rc next, ngbr_rc;
    struct rclist rlist;
    int neighbors[8][2];
    struct reg_stats *new_rs;

    G_debug(4, "merge_regions");

    /* Ri ID must always be positive */
    if (Ri_rs->id < 1)
	G_fatal_error("Ri id is negative: %d", Ri_rs->id);
    /* if Rk ID is negative (no seed), Rk count must be 1  */
    if (Rk_rs->id < 1 && Rk_rs->count > 1)
	G_fatal_error("Rk id is negative: %d, but count is > 1: %d",
	              Rk_rs->id, Rk_rs->count);

    /* update segment id and clear candidate flag */
    
    /* cases
     * Ri, Rk are not in the tree
     * Ri, Rk are both in the tree
     * Ri is in the tree, Rk is not
     * Rk is in the tree, Ri is not
     */

    /* Ri_rs, Rk_rs must always be set */
    /* add Rk */
    Ri_rs->count += Rk_rs->count;
    n = globals->nbands - 1;
    do {
	Ri_rs->sum[n] += Rk_rs->sum[n];
	Ri_rs->mean[n] = Ri_rs->sum[n] / Ri_rs->count;
    } while (n--);

    if (Ri->count >= Rk->count) {

	if (Rk->count >= globals->min_reg_size) {
	    if (rgtree_find(globals->reg_tree, Rk_rs) == NULL)
		G_fatal_error("merge regions: Rk should be in tree");
	    /* remove from tree */
	    rgtree_remove(globals->reg_tree, Rk_rs);
	}
    }
    else {

	if (Ri->count >= globals->min_reg_size) {
	    if (rgtree_find(globals->reg_tree, Ri_rs) == NULL)
		G_fatal_error("merge regions: Ri should be in tree");
	    /* remove from tree */
	    rgtree_remove(globals->reg_tree, Ri_rs);
	}

	/* magic switch */
	Ri_rs->id = Rk->id;
    }

    if ((new_rs = rgtree_find(globals->reg_tree, Ri_rs)) != NULL) {
	/* update stats for tree item */
	new_rs->count = Ri_rs->count;
	memcpy(new_rs->mean, Ri_rs->mean, globals->datasize);
	memcpy(new_rs->sum, Ri_rs->sum, globals->datasize);
    }
    else if (Ri_rs->count >= globals->min_reg_size) {
	/* add to tree */
	rgtree_insert(globals->reg_tree, Ri_rs);
    }

    Ri->count = Ri_rs->count;
    memcpy(Ri->mean, Ri_rs->mean, globals->datasize);

    if (Ri->id == Ri_rs->id) {
	/* Ri is already updated, including candidate flags
	 * need to clear candidate flag for Rk and set new id */
	 
	/* the actual merge: change region id */
	segment_put(&globals->rid_seg, (void *) &Ri->id, Rk->row, Rk->col);

	if (do_cand) {
	    do_cand = 0;
	    if (FLAG_GET(globals->candidate_flag, Rk->row, Rk->col)) {
		/* clear candidate flag */
		FLAG_UNSET(globals->candidate_flag, Rk->row, Rk->col);
		globals->candidate_count--;
		do_cand = 1;
	    }
	}

	rclist_init(&rlist);
	rclist_add(&rlist, Rk->row, Rk->col);

	while (rclist_drop(&rlist, &next)) {

	    if (do_cand) {
		/* clear candidate flag */
		FLAG_UNSET(globals->candidate_flag, next.row, next.col);
		globals->candidate_count--;
	    }

	    globals->find_neighbors(next.row, next.col, neighbors);
	    
	    n = globals->nn - 1;
	    do {

		ngbr_rc.row = neighbors[n][0];
		ngbr_rc.col = neighbors[n][1];

		if (ngbr_rc.row >= globals->row_min &&
		    ngbr_rc.row < globals->row_max &&
		    ngbr_rc.col >= globals->col_min &&
		    ngbr_rc.col < globals->col_max) {

		    if (!(FLAG_GET(globals->null_flag, ngbr_rc.row, ngbr_rc.col))) {

			segment_get(&globals->rid_seg, (void *) &R_id,
			            ngbr_rc.row, ngbr_rc.col);

			if (R_id == Rk->id) {
			    /* the actual merge: change region id */
			    segment_put(&globals->rid_seg, (void *) &Ri->id, ngbr_rc.row, ngbr_rc.col);

			    /* want to check this neighbor's neighbors */
			    rclist_add(&rlist, ngbr_rc.row, ngbr_rc.col);
			}
		    }
		}
	    } while (n--);
	}
    }
    else {
	/* Rk was larger than Ri */

	/* clear candidate flag for Rk */
	if (do_cand && FLAG_GET(globals->candidate_flag, Rk->row, Rk->col)) {
	    set_candidate_flag(Rk, FALSE, globals);
	}

	/* update region id for Ri */

	/* the actual merge: change region id */
	segment_put(&globals->rid_seg, (void *) &Rk->id, Ri->row, Ri->col);

	rclist_init(&rlist);
	rclist_add(&rlist, Ri->row, Ri->col);

	while (rclist_drop(&rlist, &next)) {

	    globals->find_neighbors(next.row, next.col, neighbors);
	    
	    n = globals->nn - 1;
	    do {

		ngbr_rc.row = neighbors[n][0];
		ngbr_rc.col = neighbors[n][1];

		if (ngbr_rc.row >= globals->row_min &&
		    ngbr_rc.row < globals->row_max &&
		    ngbr_rc.col >= globals->col_min &&
		    ngbr_rc.col < globals->col_max) {


		    if (!(FLAG_GET(globals->null_flag, ngbr_rc.row, ngbr_rc.col))) {

			segment_get(&globals->rid_seg, (void *) &R_id, ngbr_rc.row, ngbr_rc.col);

			if (R_id == Ri->id) {
			    /* the actual merge: change region id */
			    segment_put(&globals->rid_seg, (void *) &Rk->id, ngbr_rc.row, ngbr_rc.col);

			    /* want to check this neighbor's neighbors */
			    rclist_add(&rlist, ngbr_rc.row, ngbr_rc.col);
			}
		    }
		}
	    } while (n--);
	}
	Ri->id = Ri_rs->id;   /* == Rk->id */
	if (Ri->id != Rk->id)
	    G_fatal_error("Ri ID should be set to Rk ID");
    }
    
    if (Rk->id > 0)
	globals->n_regions--;

    /* disable Rk */
    Rk->id = Rk_rs->id = 0;
    Rk->count = Rk_rs->count = 0;

    return TRUE;
}

static int set_candidate_flag(struct ngbr_stats *head, int value, struct globals *globals)
{
    int n, R_id;
    struct rc next, ngbr_rc;
    struct rclist rlist;
    int neighbors[8][2];

    G_debug(4, "set_candidate_flag");

    if (!(FLAG_GET(globals->candidate_flag, head->row, head->col)) != value) {
	G_warning(_("Candidate flag is already %s"), value ? _("set") : _("unset"));
	return FALSE;
    }

    rclist_init(&rlist);
    rclist_add(&rlist, head->row, head->col);

    /* (un)set candidate flag */
    if (value == TRUE) {
	FLAG_SET(globals->candidate_flag, head->row, head->col);
	globals->candidate_count++;
    }
    else {
	FLAG_UNSET(globals->candidate_flag, head->row, head->col);
	globals->candidate_count--;
    }

    while (rclist_drop(&rlist, &next)) {

	globals->find_neighbors(next.row, next.col, neighbors);
	
	n = globals->nn - 1;
	do {

	    ngbr_rc.row = neighbors[n][0];
	    ngbr_rc.col = neighbors[n][1];

	    if (ngbr_rc.row >= globals->row_min &&
	        ngbr_rc.row < globals->row_max &&
		ngbr_rc.col >= globals->col_min &&
		ngbr_rc.col < globals->col_max) {

		if (!(FLAG_GET(globals->null_flag, ngbr_rc.row, ngbr_rc.col))) {

		    if (!(FLAG_GET(globals->candidate_flag, ngbr_rc.row, ngbr_rc.col)) == value) {

			segment_get(&globals->rid_seg, (void *) &R_id, ngbr_rc.row, ngbr_rc.col);

			if (R_id == head->id) {
			    /* want to check this neighbor's neighbors */
			    rclist_add(&rlist, ngbr_rc.row, ngbr_rc.col);

			    /* (un)set candidate flag */
			    if (value == TRUE) {
				FLAG_SET(globals->candidate_flag, ngbr_rc.row, ngbr_rc.col);
				globals->candidate_count++;
			    }
			    else {
				FLAG_UNSET(globals->candidate_flag, ngbr_rc.row, ngbr_rc.col);
				globals->candidate_count--;
			    }
			}
		    }
		}
	    }
	} while (n--);
    }

    return TRUE;
}

int fetch_reg_stats(int row, int col, struct reg_stats *rs, 
                           struct globals *globals)
{
    struct reg_stats *rs_found;

    if ((rs_found = rgtree_find(globals->reg_tree, rs)) != NULL) {

	memcpy(rs->mean, rs_found->mean, globals->datasize);
	memcpy(rs->sum, rs_found->sum, globals->datasize);
	rs->count = rs_found->count;

	return 1;
    }

    calculate_reg_stats(row, col, rs, globals);

    return 2;
}

static int calculate_reg_stats(int row, int col, struct reg_stats *rs, 
                         struct globals *globals)
{
    G_debug(4, "calculate_reg_stats()");

    segment_get(&globals->bands_seg, (void *)globals->bands_val,
		row, col);
    rs->count = 1;
    memcpy(rs->mean, globals->bands_val, globals->datasize);
    memcpy(rs->sum, globals->bands_val, globals->datasize);

    if (globals->min_reg_size < 3)
	return 1;

    if (globals->min_reg_size == 3) {
	int n, i, rid;
	struct rc ngbr_rc;
	int neighbors[8][2];

	globals->find_neighbors(row, col, neighbors);

	for (n = 0; n < globals->nn; n++) {

	    ngbr_rc.row = neighbors[n][0];
	    ngbr_rc.col = neighbors[n][1];

	    if (ngbr_rc.row < 0 || ngbr_rc.row >= globals->nrows ||
		ngbr_rc.col < 0 || ngbr_rc.col >= globals->ncols) {
		continue;
	    }

	    if ((FLAG_GET(globals->null_flag, ngbr_rc.row, ngbr_rc.col)) == 0) {

		segment_get(&globals->rid_seg, (void *) &rid,
			    ngbr_rc.row, ngbr_rc.col);
		
		if (rid == rs->id) {

		    /* update region stats */
		    segment_get(&globals->bands_seg, (void *)globals->bands_val,
				ngbr_rc.row, ngbr_rc.col);

		    i = globals->nbands - 1;
		    do {
			rs->sum[i] += globals->bands_val[i];
		    } while (i--);
		    rs->count++;

		    /* only one other neighbor can have the same ID */
		    break;
		}
	    }
	}

	/* band mean */
	i = globals->nbands - 1;
	do {
	    rs->mean[i] = rs->sum[i] / rs->count;
	} while (i--);
	
	return 2;
    }

    if (globals->min_reg_size > 3) {
	/* rs->id must be set */
	struct RB_TREE *rc_check_tree;	/* cells already checked */
	int n, i, rid;
	struct rc ngbr_rc, next;
	struct rclist rilist;
	int neighbors[8][2];
	int no_check;
	
	/* go through region, spreading outwards from head */
	rclist_init(&rilist);

	rc_check_tree = rbtree_create(compare_rc, sizeof(struct rc));
	ngbr_rc.row = row;
	ngbr_rc.col = col;
	rbtree_insert(rc_check_tree, &ngbr_rc);

	next.row = row;
	next.col = col;
	do {
	    G_debug(5, "find_pixel_neighbors for row: %d , col %d",
		    next.row, next.col);

	    globals->find_neighbors(next.row, next.col, neighbors);

	    n = globals->nn - 1;
	    n = 0;
	    do {

		ngbr_rc.row = neighbors[n][0];
		ngbr_rc.col = neighbors[n][1];

		no_check = (ngbr_rc.row < 0 || ngbr_rc.row >= globals->nrows ||
		    ngbr_rc.col < 0 || ngbr_rc.col >= globals->ncols);

		if (!no_check) {
		    if ((FLAG_GET(globals->null_flag, ngbr_rc.row, ngbr_rc.col)) == 0) {
		    
			/* already checked ? */
			if (!rbtree_find(rc_check_tree, &ngbr_rc)) {

			    /* not yet checked, don't check it again */
			    rbtree_insert(rc_check_tree, &ngbr_rc);

			    segment_get(&globals->rid_seg, (void *) &rid,
					ngbr_rc.row, ngbr_rc.col);
			    
			    if (rid == rs->id) {

				/* want to check this neighbor's neighbors */
				rclist_add(&rilist, ngbr_rc.row, ngbr_rc.col);

				/* update region stats */
				segment_get(&globals->bands_seg,
					    (void *)globals->bands_val,
					    ngbr_rc.row, ngbr_rc.col);

				i = globals->nbands - 1;
				do {
				    rs->sum[i] += globals->bands_val[i];
				} while (i--);
				rs->count++;
			    }
			}
		    }
		}
	    } while (n++ < globals->nn - 1); /* (n--); */
	} while (rclist_drop(&rilist, &next));
	/* band mean */
	i = globals->nbands - 1;
	do {
	    rs->mean[i] = rs->sum[i] / rs->count;
	} while (i--);

	/* clean up */
	rbtree_destroy(rc_check_tree);
	
	return 3;
    }
    
    return 0;
}
