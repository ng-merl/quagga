/*
 * OSPF ABR functions.
 * Copyright (C) 1999, 2000 Alex Zinin, Toshiaki Takada
 *
 * This file is part of GNU Zebra.
 *
 * GNU Zebra is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 * 
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with GNU Zebra; see the file COPYING.  If not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */


#include <zebra.h>

#include "thread.h"
#include "memory.h"
#include "linklist.h"
#include "prefix.h"
#include "if.h"
#include "table.h"
#include "vty.h"
#include "filter.h"
#include "log.h"

#include "ospfd/ospfd.h"
#include "ospfd/ospf_interface.h"
#include "ospfd/ospf_ism.h"
#include "ospfd/ospf_asbr.h"
#include "ospfd/ospf_lsa.h"
#include "ospfd/ospf_lsdb.h"
#include "ospfd/ospf_neighbor.h"
#include "ospfd/ospf_nsm.h"
#include "ospfd/ospf_spf.h"
#include "ospfd/ospf_route.h"
#include "ospfd/ospf_ia.h"
#include "ospfd/ospf_flood.h"
#include "ospfd/ospf_abr.h"
#include "ospfd/ospf_ase.h"
#include "ospfd/ospf_zebra.h"
#include "ospfd/ospf_dump.h"


struct ospf_area_range *
ospf_area_range_lookup (struct ospf_area *area, struct in_addr *range_net)
{
  struct route_node *node;
  struct prefix_ipv4 p;
  struct ospf_area_range *range;

  p.family = AF_INET;
  p.prefixlen = IPV4_MAX_BITLEN;
  p.prefix = *range_net;

  node = route_node_match (area->ranges, (struct prefix *) &p);
  if (node)
    {
      range = node->info;
      route_unlock_node (node);
      return range;
    }
  return NULL;
}

struct ospf_area_range *
ospf_area_range_lookup_next (struct ospf_area *area, struct in_addr *range_net,
			     int first)
{
  struct route_node *rn;
  struct prefix_ipv4 p;
  struct ospf_area_range *find;

  p.family = AF_INET;
  p.prefixlen = IPV4_MAX_BITLEN;
  p.prefix = *range_net;

  if (first)
    rn = route_top (area->ranges);
  else
    {
      rn = route_node_get (area->ranges, (struct prefix *) &p);
      rn = route_next (rn);
    }

  for (; rn; rn = route_next (rn))
    if (rn->info)
      break;

  if (rn && rn->info)
    {
      find = rn->info;
      *range_net = rn->p.u.prefix4;
      route_unlock_node (rn);
      return find;
    }
  return NULL;
}

struct ospf_area_range *
ospf_area_range_match (struct ospf_area *area, struct prefix_ipv4 *p)
{
  struct route_node *node;

  node = route_node_match (area->ranges, (struct prefix *) p);
  if (node)
    {
      route_unlock_node (node);
      return node->info;
    }
  return NULL;
}

struct ospf_area_range *
ospf_some_area_range_match (struct prefix_ipv4 *p)
{
  listnode node;
  struct ospf_area_range * range;

  for (node = listhead (ospf_top->areas); node; nextnode (node))
    if ((range = ospf_area_range_match (node->data, p)))
      return range;

  return NULL;
}

int
ospf_range_active (struct ospf_area_range *range)
{
  return range->specifics;
}

int
ospf_area_actively_attached (struct ospf_area *area)
{
  return area->act_ints;
}

int
ospf_act_bb_connection ()
{
  if (ospf_top->backbone == NULL)
    return 0;

  return ospf_top->backbone->full_nbrs;
}

/* Check area border router status. */
void
ospf_check_abr_status ()
{
  struct ospf_area *area;
  listnode node;
  int bb_configured = 0;
  int bb_act_attached = 0;
  int areas_configured = 0;
  int areas_act_attached = 0;

  u_char new_flags = ospf_top->flags;

  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("ospf_check_abr_status(): Start");

  for (node = listhead (ospf_top->areas); node; nextnode (node))
    {
      area = getdata (node);

      if (listcount (area->iflist)) 
	{
	  areas_configured++;
	  
	  if (OSPF_IS_AREA_BACKBONE (area))
 	    bb_configured = 1;
	}

      if (ospf_area_actively_attached (area))
	{
	  areas_act_attached++;
	  
	  if (OSPF_IS_AREA_BACKBONE (area))
            bb_act_attached = 1;
	}
    }

  if (IS_DEBUG_OSPF_EVENT)
    {
      zlog_info ("ospf_check_abr_status(): looked through areas");
      zlog_info ("ospf_check_abr_status(): bb_configured: %d", bb_configured);
      zlog_info ("ospf_check_abr_status(): bb_act_attached: %d",
		 bb_act_attached);
      zlog_info ("ospf_check_abr_status(): areas_configured: %d",
		 areas_configured);
      zlog_info ("ospf_check_abr_status(): areas_act_attached: %d",
		 areas_act_attached);
    }

  switch (ospf_top->abr_type)
    {
    case OSPF_ABR_SHORTCUT:
    case OSPF_ABR_STAND:
      if (areas_act_attached > 1)
	SET_FLAG (new_flags, OSPF_FLAG_ABR);
      else
	UNSET_FLAG (new_flags, OSPF_FLAG_ABR);
      break;

    case OSPF_ABR_IBM:
      if ((areas_act_attached > 1) && bb_configured)
	SET_FLAG (new_flags, OSPF_FLAG_ABR);
      else
	UNSET_FLAG (new_flags, OSPF_FLAG_ABR);
      break;

    case OSPF_ABR_CISCO:
      if ((areas_configured > 1) && bb_act_attached)
	SET_FLAG (new_flags, OSPF_FLAG_ABR);
      else
	UNSET_FLAG (new_flags, OSPF_FLAG_ABR);
      break;
    default:
      break;
    }

  if (new_flags != ospf_top->flags)
    {
      ospf_spf_calculate_schedule ();
      if (IS_DEBUG_OSPF_EVENT)
	zlog_info ("ospf_check_abr_status(): new router flags: %x",new_flags);
      ospf_top->flags = new_flags;
      OSPF_TIMER_ON (ospf_top->t_router_lsa_update,
		     ospf_router_lsa_update_timer, OSPF_LSA_UPDATE_DELAY);
    }
}

void
ospf_abr_update_aggregate (struct ospf_area_range *range,
			   struct ospf_route *or)
{
  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("ospf_abr_update_aggregate(): Start");

  range->specifics++;

  if (or->cost > range->cost)
    {
      if (IS_DEBUG_OSPF_EVENT)
	zlog_info ("ospf_abr_update_aggregate(): worse cost, update");

      range->cost = or->cost;
    }
}

static void
set_metric (struct ospf_lsa *lsa, u_int32_t metric)
{
  struct summary_lsa *header;
  u_char *mp;
  metric = htonl (metric);
  mp = (char *) &metric;
  mp++;
  header = (struct summary_lsa *) lsa->data;
  memcpy(header->metric, mp, 3);
}

#ifdef HAVE_NSSA
int
ospf_abr_check_nssa_range (struct prefix_ipv4 *p, u_int32_t cost,
				   struct ospf_area *area)
{
  /* The Type-7 is tested against the aggregated prefix and forwarded
       for lsa installation and flooding */
  return 0;
}

/* ospf_abr_translate_nssa */
int
ospf_abr_translate_nssa (struct ospf_lsa *lsa, void *p_arg, int int_arg)
{
  /* Incoming Type-7 or later aggregated Type-7 

     LSA is skipped if P-bit is off.
     LSA is aggregated if within range.

     The Type-7 is translated, Installed/Approved as a Type-5 into
     global LSDB, then Flooded through AS

     Later, any Unapproved Translated Type-5's are flushed/discarded */

  struct ospf_lsa *dup;

  if (! CHECK_FLAG (lsa->data->options, OSPF_OPTION_NP))
    return 0;

  /* No more P-bit. */
  UNSET_FLAG (lsa->data->options, OSPF_OPTION_NP);

  /* Area where Aggregate testing will be inserted, just like summary
     advertisements */
  /* ospf_abr_check_nssa_range (p_arg, lsa-> cost, lsa -> area); */

  /* Follow thru here means no aggregation */
  dup = ospf_lsa_dup (lsa);	/* keep LSDB intact, lock = 1 */

  SET_FLAG (dup->flags, OSPF_LSA_LOCAL_XLT); /* Translated from 7  */
  SET_FLAG (dup->flags, OSPF_LSA_APPROVED); /* So, do not remove it */

  dup->data->type = OSPF_AS_EXTERNAL_LSA;  /* make Type-5 */

  ospf_lsa_install (NULL, dup); /* Install this Type-5 into LSDB, Lock = 2. */

  /* will LOCK it at value 2 */
  ospf_flood_through_as (NULL, dup); /* flood non-NSSA areas */
  
  /* This translated Type-5 will go to all non-NSSA areas connected to
     this ABR; The Type-5 could come from any of the NSSA's connected
     to this ABR.  */

  return 0;
}

void
ospf_abr_translate_nssa_range (struct prefix_ipv4 *p, u_int32_t cost)
{
  /* The Type-7 is created from the aggregated prefix and forwarded
     for lsa installation and flooding... to be added... */
}
#endif /* HAVE_NSSA */

void
ospf_abr_announce_network_to_area (struct prefix_ipv4 *p, u_int32_t cost,
				   struct ospf_area *area)
{
  struct ospf_lsa *lsa, *old = NULL;
  struct summary_lsa *sl = NULL;

  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("ospf_abr_announce_network_to_area(): Start");

  old = OSPF_SUMMARY_LSA_SELF_FIND_BY_PREFIX (area, p);

  if (old)
    {
      if (IS_DEBUG_OSPF_EVENT)
	zlog_info ("ospf_abr_announce_network_to_area(): old summary found");
      sl = (struct summary_lsa *) old->data;

      if (IS_DEBUG_OSPF_EVENT)
	zlog_info ("ospf_abr_announce_network_to_area(): "
		   "old metric: %d, new metric: %d",
		   GET_METRIC (sl->metric), cost);
    }

  if (old && (GET_METRIC (sl->metric) == cost))
    {
      if (IS_DEBUG_OSPF_EVENT)
	zlog_info ("ospf_abr_announce_network_to_area(): "
		   "old summary approved"); 
      SET_FLAG (old->flags, OSPF_LSA_APPROVED);
    }
  else
    {
      if (IS_DEBUG_OSPF_EVENT)
	zlog_info ("ospf_abr_announce_network_to_area(): "
		   "creating new summary");
      if (old)
	{

	  set_metric (old, cost);
	  lsa = ospf_summary_lsa_refresh (old);
	  /* This will flood through area. */
	}
      else
	{
	  lsa = ospf_summary_lsa_originate (p, cost, area);
	  /* This will flood through area. */
	}
      

      SET_FLAG (lsa->flags, OSPF_LSA_APPROVED);
      if (IS_DEBUG_OSPF_EVENT)
	zlog_info ("ospf_abr_announce_network_to_area(): "
		   "flooding new version of summary");

#ifndef HAVE_NSSA      
      ospf_flood_through_area (area, NULL, lsa);
#endif /* ! HAVE_NSSA */
    }

  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("ospf_abr_announce_network_to_area(): Stop");
}

int
ospf_abr_nexthops_belong_to_area (struct ospf_route *or,
				  struct ospf_area *area)
{
  listnode node;

  for (node = listhead (or->path); node; nextnode (node))
    {
      struct ospf_path *path = node->data;
      struct ospf_interface *oi = path->ifp->info;

      if (oi != NULL)
	if (oi->area == area)
	  return 1;
    }

  return 0;
}

int
ospf_abr_should_accept (struct prefix *p, struct ospf_area *area)
{
  if (IMPORT_NAME (area))
    {
      if (IMPORT_LIST (area) == NULL)
	IMPORT_LIST (area) = access_list_lookup (AF_INET, IMPORT_NAME (area));

      if (IMPORT_LIST (area))
        if (access_list_apply (IMPORT_LIST (area), p) == FILTER_DENY)
           return 0;
    }

 return 1;
}

void
ospf_abr_announce_network (struct route_node *n, struct ospf_route *or)
{
  listnode node;
  struct ospf_area_range *range;
  struct prefix_ipv4 *p;
  struct ospf_area *area, *or_area;

  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("ospf_abr_announce_network(): Start");
  p = (struct prefix_ipv4 *) &n->p;

  or_area = ospf_area_lookup_by_area_id (or->u.std.area_id); 
  assert (or_area);

  for (node = listhead (ospf_top->areas); node; nextnode (node))
    {
      area = getdata (node);

      if (IS_DEBUG_OSPF_EVENT)
	zlog_info ("ospf_abr_announce_network(): looking at area %s",
		   inet_ntoa (area->area_id));

      if (IPV4_ADDR_SAME (&or->u.std.area_id, &area->area_id))
	continue;

      if (ospf_abr_nexthops_belong_to_area (or, area))
	continue;

      if (!ospf_abr_should_accept (&n->p, area))
	{
	  if (IS_DEBUG_OSPF_EVENT)
	    zlog_info ("ospf_abr_announce_network(): "
		       "prefix %s/%d was denied by import-list",
		       inet_ntoa (p->prefix), p->prefixlen);
	  continue; 
	}

      if (area->external_routing != OSPF_AREA_DEFAULT && area->no_summary)
	{
	  if (IS_DEBUG_OSPF_EVENT)
	    zlog_info ("ospf_abr_announce_network(): "
		       "area %s is stub and no_summary",
		       inet_ntoa (area->area_id));
          continue;
	}

      if (or->path_type == OSPF_PATH_INTER_AREA)
	{
	  if (IS_DEBUG_OSPF_EVENT)
	    zlog_info ("ospf_abr_announce_network(): this is "
		       "inter-area route to %s/%d",
		       inet_ntoa (p->prefix), p->prefixlen);

          if (!OSPF_IS_AREA_BACKBONE (area))
	    ospf_abr_announce_network_to_area (p, or->cost, area);
	}

      if (or->path_type == OSPF_PATH_INTRA_AREA)
	{
	  if (IS_DEBUG_OSPF_EVENT)
	    zlog_info ("ospf_abr_announce_network(): "
		       "this is intra-area route to %s/%d",
		       inet_ntoa (p->prefix), p->prefixlen);
	  if ((range = ospf_area_range_match (or_area, p)) &&
              !ospf_area_is_transit (area))
	    ospf_abr_update_aggregate (range, or);
	  else
	    ospf_abr_announce_network_to_area (p, or->cost, area);
	}
    }
}

int
ospf_abr_should_announce (struct prefix *p, struct ospf_route *or)
{
  struct ospf_area *area = ospf_area_lookup_by_area_id (or->u.std.area_id);

  assert (area);
  
  if (EXPORT_NAME (area))
    {
      if (EXPORT_LIST (area) == NULL)
	EXPORT_LIST (area) = access_list_lookup (AF_INET, EXPORT_NAME (area));

      if (EXPORT_LIST (area))
        if (access_list_apply (EXPORT_LIST (area), p) == FILTER_DENY)
           return 0;
    }

  return 1;
}

#ifdef HAVE_NSSA
void
ospf_abr_process_nssa_translates ()
{
  /* Scan through all NSSA_LSDB records for all areas;

     If P-bit is on, translate all Type-7's to 5's and aggregate or
     flood install as approved in Type-5 LSDB with XLATE Flag on
     later, do same for all aggregates...  At end, DISCARD all
     remaining UNAPPROVED Type-5's (Aggregate is for future ) */
  listnode node;
  struct ospf_area *area;

  if (IS_DEBUG_OSPF_NSSA)
    zlog_info ("ospf_abr_process_nssa_translates(): Start");

  for (node = listhead (ospf_top->areas); node; nextnode (node))
    {
      area = getdata (node);

      if (! area->NSSATranslator)
	continue; /* skip if not translator */
      
      if (IS_DEBUG_OSPF_NSSA)
	zlog_info ("ospf_abr_process_nssa_translates(): "
		   "looking at area %s", inet_ntoa (area->area_id));
      
      foreach_lsa (NSSA_LSDB (area), area, 0, ospf_abr_translate_nssa);
    }
 
  if (IS_DEBUG_OSPF_NSSA)
    zlog_info ("ospf_abr_process_nssa_translates(): Stop");

}
#endif /* HAVE_NSSA */

void
ospf_abr_process_network_rt (struct route_table *rt)
{
  struct route_node *rn;
  struct ospf_route *or;

  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("ospf_abr_process_network_rt(): Start");
  RT_ITERATOR (rt, rn)
    {
      if ((or = rn->info) == NULL)
	continue;

      if (!ospf_area_lookup_by_area_id (or->u.std.area_id))
	{
	  if (IS_DEBUG_OSPF_EVENT)
	    zlog_info ("ospf_abr_process_network_rt(): area %s no longer exists",
		       inet_ntoa (or->u.std.area_id));
	  continue;
	}

      if (IS_DEBUG_OSPF_EVENT)
	zlog_info ("ospf_abr_process_network_rt(): this is a route to %s/%d",
		   inet_ntoa (rn->p.u.prefix4), rn->p.prefixlen);
      if (or->path_type >= OSPF_PATH_TYPE1_EXTERNAL)
	{
	  if (IS_DEBUG_OSPF_EVENT)
	    zlog_info ("ospf_abr_process_network_rt(): "
		       "this is an External router, skipping");
	  continue;
	}

      if (or->cost >= OSPF_LS_INFINITY)
	{
	  if (IS_DEBUG_OSPF_EVENT)
	    zlog_info ("ospf_abr_process_network_rt():"
		       " this route's cost is infinity, skipping");
	  continue;
	}

      if (or->type == OSPF_DESTINATION_DISCARD)
	{
	  if (IS_DEBUG_OSPF_EVENT)
	    zlog_info ("ospf_abr_process_network_rt():"
		       " this is a discard entry, skipping");
	  continue;
	}

      if ((or->path_type == OSPF_PATH_INTRA_AREA) &&
          (! ospf_abr_should_announce(&rn->p, or)) )
	{
	  if (IS_DEBUG_OSPF_EVENT)
	    zlog_info("ospf_abr_process_network_rt(): denied by export-list");
	  continue;
	}


      if ((or->path_type == OSPF_PATH_INTER_AREA) &&
          !OSPF_IS_AREA_ID_BACKBONE (or->u.std.area_id))
	{
	  if (IS_DEBUG_OSPF_EVENT)
	    zlog_info ("ospf_abr_process_network_rt():"
		       " this is route is not backbone one, skipping");
	  continue;
	}


      if ((ospf_top->abr_type == OSPF_ABR_CISCO) ||
          (ospf_top->abr_type == OSPF_ABR_IBM))

          if (!ospf_act_bb_connection () &&
              or->path_type != OSPF_PATH_INTRA_AREA)
	     {
	       if (IS_DEBUG_OSPF_EVENT)
		 zlog_info ("ospf_abr_process_network_rt(): ALT ABR: "
			    "No BB connection, skip not intra-area routes");
	       continue;
	     }

      if (IS_DEBUG_OSPF_EVENT)
	zlog_info ("ospf_abr_process_network_rt(): announcing");
      ospf_abr_announce_network (rn, or);
    }

  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("ospf_abr_process_network_rt(): Stop");
}

void
ospf_abr_announce_rtr_to_area (struct prefix_ipv4 *p, u_int32_t cost,
			       struct ospf_area *area)
{
  struct ospf_lsa *lsa, *old = NULL;
  struct summary_lsa *slsa = NULL;

  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("ospf_abr_announce_rtr_to_area(): Start");

  old = OSPF_SUMMARY_ASBR_LSA_SELF_FIND_BY_PREFIX (area, p);
  /* old = ospf_find_self_summary_asbr_lsa_by_prefix (area, p); */

  if (old)
    {
      if (IS_DEBUG_OSPF_EVENT)
	zlog_info ("ospf_abr_announce_rtr_to_area(): old summary found");
      slsa = (struct summary_lsa *) old->data;

      if (IS_DEBUG_OSPF_EVENT)
	zlog_info ("ospf_abr_announce_network_to_area(): "
		   "old metric: %d, new metric: %d",
		   GET_METRIC (slsa->metric), cost);
    }

  if (old && (GET_METRIC (slsa->metric) == cost))
    {
      if (IS_DEBUG_OSPF_EVENT)
	zlog_info ("ospf_abr_announce_rtr_to_area(): old summary approved");
      SET_FLAG (old->flags, OSPF_LSA_APPROVED);
    }
  else
    {
      if (IS_DEBUG_OSPF_EVENT)
	zlog_info ("ospf_abr_announce_rtr_to_area(): 2.2");
       
      if (old) 
	{ 
	  set_metric (old, cost);
	  lsa = ospf_summary_asbr_lsa_refresh (old);
	}
      else
	lsa = ospf_summary_asbr_lsa_originate (p, cost, area);

      if (IS_DEBUG_OSPF_EVENT)
	zlog_info ("ospf_abr_announce_rtr_to_area(): "
		   "flooding new version of summary");
      /*
      zlog_info ("ospf_abr_announce_rtr_to_area(): creating new summary");
      lsa = ospf_summary_asbr_lsa (p, cost, area, old); */

      SET_FLAG (lsa->flags, OSPF_LSA_APPROVED);
      /* ospf_flood_through_area (area, NULL, lsa);*/
    }

  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("ospf_abr_announce_rtr_to_area(): Stop");
}


void
ospf_abr_announce_rtr (struct prefix_ipv4 *p, struct ospf_route *or)
{
  listnode node;
  struct ospf_area *area;

  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("ospf_abr_announce_rtr(): Start");

  for (node = listhead (ospf_top->areas); node; nextnode (node))
    {
      area = getdata (node);

      if (IS_DEBUG_OSPF_EVENT)
	zlog_info ("ospf_abr_announce_rtr(): looking at area %s",
		   inet_ntoa (area->area_id));

      if (IPV4_ADDR_SAME (&or->u.std.area_id, &area->area_id))
	continue;

      if (ospf_abr_nexthops_belong_to_area (or, area))
	continue;

      if (area->external_routing != OSPF_AREA_DEFAULT)
	{
	  if (IS_DEBUG_OSPF_EVENT)
	    zlog_info ("ospf_abr_announce_network(): "
		       "area %s doesn't support external routing",
		       inet_ntoa(area->area_id));
          continue;
	}

      if (or->path_type == OSPF_PATH_INTER_AREA)
	{
	  if (IS_DEBUG_OSPF_EVENT)
	    zlog_info ("ospf_abr_announce_rtr(): "
		       "this is inter-area route to %s", inet_ntoa (p->prefix));
          if (!OSPF_IS_AREA_BACKBONE (area))
	    ospf_abr_announce_rtr_to_area (p, or->cost, area);
	}

      if (or->path_type == OSPF_PATH_INTRA_AREA)
	{
	  if (IS_DEBUG_OSPF_EVENT)
	    zlog_info ("ospf_abr_announce_rtr(): "
		       "this is intra-area route to %s", inet_ntoa (p->prefix));
          ospf_abr_announce_rtr_to_area (p, or->cost, area);
	}
    }

  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("ospf_abr_announce_rtr(): Stop");
}

void
ospf_abr_process_router_rt (struct route_table *rt)
{
  struct route_node *rn;
  struct ospf_route *or;
  struct list *l;

  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("ospf_abr_process_router_rt(): Start");

  RT_ITERATOR (rt, rn)
    {
      listnode node;
      char flag = 0;
      struct ospf_route *best = NULL;

      if (rn->info == NULL)
	continue;

      l = rn->info;

      if (IS_DEBUG_OSPF_EVENT)
	zlog_info ("ospf_abr_process_router_rt(): this is a route to %s",
		   inet_ntoa (rn->p.u.prefix4));

      LIST_ITERATOR (l, node)
	{
	  or = getdata (node);
	  if (or == NULL)
	    continue;

	  if (!ospf_area_lookup_by_area_id (or->u.std.area_id))
	    {
	      if (IS_DEBUG_OSPF_EVENT)
		zlog_info ("ospf_abr_process_router_rt(): area %s no longer exists",
			 inet_ntoa (or->u.std.area_id));
	      continue;
	    }


	  if (!CHECK_FLAG (or->u.std.flags, ROUTER_LSA_EXTERNAL))
	    {
	      if (IS_DEBUG_OSPF_EVENT)
		zlog_info ("ospf_abr_process_router_rt(): "
			   "This is not an ASBR, skipping");
	      continue;
	    }

	  if (!flag)
	    {
	      best = ospf_find_asbr_route (rt, (struct prefix_ipv4 *) &rn->p);
	      flag = 1;
	    }
	  
        if (best == NULL)
	  continue;
	
        if (or != best)
	  {
	    if (IS_DEBUG_OSPF_EVENT)
	      zlog_info ("ospf_abr_process_router_rt(): "
			 "This route is not the best among possible, skipping");
	    continue;
	  }
	
        if (or->path_type == OSPF_PATH_INTER_AREA &&
            !OSPF_IS_AREA_ID_BACKBONE (or->u.std.area_id))
	  {
	    if (IS_DEBUG_OSPF_EVENT)
	      zlog_info ("ospf_abr_process_router_rt(): "
			 "This route is not a backbone one, skipping");
	    continue;
	  }

        if (or->cost >= OSPF_LS_INFINITY)
	  {
	    if (IS_DEBUG_OSPF_EVENT)
	      zlog_info ("ospf_abr_process_router_rt(): "
			 "This route has LS_INFINITY metric, skipping");
	    continue;
	  }

        if (ospf_top->abr_type == OSPF_ABR_CISCO ||
            ospf_top->abr_type == OSPF_ABR_IBM)
	  if (!ospf_act_bb_connection () &&
	      or->path_type != OSPF_PATH_INTRA_AREA)
	    {
	      if (IS_DEBUG_OSPF_EVENT)
		zlog_info("ospf_abr_process_network_rt(): ALT ABR: "
			  "No BB connection, skip not intra-area routes");
	      continue;
	    }

        ospf_abr_announce_rtr ((struct prefix_ipv4 *) &rn->p, or);

	} /* LIST_ITERATOR */

    } /* RT_ITERATOR */

  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("ospf_abr_process_router_rt(): Stop");
}

#ifdef HAVE_NSSA
int
ospf_abr_unapprove_translates_apply (struct ospf_lsa *lsa, void *p_arg,
				    int int_arg)
{
  /* Could be a mix of Normal Type-5's, self-originated, or Type-7s
      that are Locally ABR Translated */

  if (CHECK_FLAG (lsa->flags, OSPF_LSA_LOCAL_XLT))
    UNSET_FLAG (lsa->flags, OSPF_LSA_APPROVED);
  
  return 0;
}

void
ospf_abr_unapprove_translates () /* For NSSA Translations */
{
  if (IS_DEBUG_OSPF_NSSA)
    zlog_info ("ospf_abr_unapprove_translates(): Start");

  /* NSSA Translator is not checked, because it may have gone away,
    and we would want to flush any residuals anyway */

  foreach_lsa (EXTERNAL_LSDB (ospf_top), NULL, 0,
	       ospf_abr_unapprove_translates_apply);

  if (IS_DEBUG_OSPF_NSSA)
    zlog_info ("ospf_abr_unapprove_translates(): Stop");
}
#endif /* HAVE_NSSA */

int
ospf_abr_unapprove_summaries_apply (struct ospf_lsa *lsa, void *p_arg,
				    int int_arg)
{
  if (ospf_lsa_is_self_originated (lsa))
    UNSET_FLAG (lsa->flags, OSPF_LSA_APPROVED);

  return 0;
}

void
ospf_abr_unapprove_summaries ()
{
  listnode node;
  struct ospf_area *area;

  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("ospf_abr_unapprove_summaries(): Start");

  for (node = listhead (ospf_top->areas); node; nextnode (node))
    {
      area = getdata (node);
      foreach_lsa (SUMMARY_LSDB (area), NULL, 0,
		   ospf_abr_unapprove_summaries_apply);
      foreach_lsa (SUMMARY_ASBR_LSDB (area), NULL, 0,
		   ospf_abr_unapprove_summaries_apply);
#if 0
      ospf_lsdb_iterator (SUMMARY_LSA (area), NULL, 0,
			  ospf_abr_unapprove_summaries_apply);
      
      ospf_lsdb_iterator (SUMMARY_LSA_ASBR (area), NULL, 0,
			  ospf_abr_unapprove_summaries_apply);
#endif
    }

  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("ospf_abr_unapprove_summaries(): Stop");
}

void
ospf_abr_prepare_aggregates ()
{
  listnode node;
  struct route_node *rn;
  struct ospf_area_range *range;

  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("ospf_abr_prepare_aggregates(): Start");

  for (node = listhead (ospf_top->areas); node; nextnode (node))
    {
      struct ospf_area *area = getdata (node);

      for (rn = route_top (area->ranges); rn; rn = route_next (rn))
	if ((range = rn->info) != NULL)
	  {
	    range->cost = 0;
	    range->specifics = 0;
	  }
    }

  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("ospf_abr_prepare_aggregates(): Stop");
}

void
ospf_abr_announce_aggregates ()
{
  listnode node, n;
  struct ospf_area *area, *ar;
  struct route_node *rn;
  struct ospf_area_range *range;
  struct prefix_ipv4 p;

  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("ospf_abr_announce_aggregates(): Start");

  for (node = listhead (ospf_top->areas); node; nextnode (node))
    {
      area = getdata (node);

      if (IS_DEBUG_OSPF_EVENT)
	zlog_info ("ospf_abr_announce_aggregates(): looking at area %s",
		   inet_ntoa (area->area_id));

      for (rn = route_top (area->ranges); rn; rn = route_next (rn))
	{
          if (rn->info == NULL)
	    continue;

	  range = rn->info;

          if (CHECK_FLAG (range->flags, OSPF_RANGE_SUPPRESS))
	    {
	      if (IS_DEBUG_OSPF_EVENT)
		zlog_info ("ospf_abr_announce_aggregates():"
			   " discarding suppress-ranges");
	      continue;
	    }

          p.family = AF_INET;
          p.prefix = range->node->p.u.prefix4;
          p.prefixlen = range->node->p.prefixlen;

	  if (IS_DEBUG_OSPF_EVENT)
	    zlog_info ("ospf_abr_announce_aggregates():"
		       " this is range: %s/%d",
		       inet_ntoa (p.prefix), p.prefixlen);

          if (CHECK_FLAG (range->flags, OSPF_RANGE_SUBST))
	    p = range->substitute;

          if (range->specifics)
	    {
	      if (IS_DEBUG_OSPF_EVENT)
		zlog_info ("ospf_abr_announce_aggregates(): active range");

	      for (n = listhead (ospf_top->areas); n; nextnode (n))
    		{
      		  ar = getdata (n);
                  if (ar == area)
		    continue;

                  /* We do not check nexthops here, because
                     intra-area routes can be associated with
		     one area only */

		  /* backbone routes are not summarized
		     when announced into transit areas */

                  if (ospf_area_is_transit (ar) &&
		      OSPF_IS_AREA_BACKBONE (area))
		    {
		      if (IS_DEBUG_OSPF_EVENT)
			zlog_info ("ospf_abr_announce_aggregates(): Skipping "
				   "announcement of BB aggregate into"
				   " a transit area");
		      continue; 
		    }
		  ospf_abr_announce_network_to_area (&p, range->cost, ar);
		}

	    } /* if (range->specifics)*/

	} /* all area ranges*/

    } /* all areas */

  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("ospf_abr_announce_aggregates(): Stop");
}

#ifdef HAVE_NSSA
void
ospf_abr_send_nssa_aggregates () /* temporarily turned off */
{
  listnode node; /*, n; */
  struct ospf_area *area; /*, *ar; */
  struct route_node *rn;
  struct ospf_area_range *range;
  struct prefix_ipv4 p;

  if (IS_DEBUG_OSPF_NSSA)
    zlog_info ("ospf_abr_send_nssa_aggregates(): Start");

  for (node = listhead (ospf_top->areas); node; nextnode (node))
    {
      area = getdata (node);

      if (! area->NSSATranslator)
	continue;

      if (IS_DEBUG_OSPF_NSSA)
	zlog_info ("ospf_abr_send_nssa_aggregates(): looking at area %s",
		   inet_ntoa (area->area_id));

      for (rn = route_top (area->ranges); rn; rn = route_next (rn))
	{
          if (rn->info == NULL)
	    continue;

	  range = rn->info;

          if (CHECK_FLAG (range->flags, OSPF_RANGE_SUPPRESS))
	    {
	      if (IS_DEBUG_OSPF_NSSA)
		zlog_info ("ospf_abr_send_nssa_aggregates():"
			   " discarding suppress-ranges");
	      continue;
	    }

          p.family = AF_INET;
          p.prefix = range->node->p.u.prefix4;
          p.prefixlen = range->node->p.prefixlen;

	  if (IS_DEBUG_OSPF_NSSA)
	    zlog_info ("ospf_abr_send_nssa_aggregates():"
		       " this is range: %s/%d",
		       inet_ntoa (p.prefix), p.prefixlen);

          if (CHECK_FLAG (range->flags, OSPF_RANGE_SUBST))
	    p = range->substitute;

          if (range->specifics)
	    {
	      if (IS_DEBUG_OSPF_NSSA)
		zlog_info ("ospf_abr_send_nssa_aggregates(): active range");

	      /* Fetch LSA-Type-7 from aggregate prefix, and then
                 translate, Install (as Type-5), Approve, and Flood */
		ospf_abr_translate_nssa_range (&p, range->cost);
	    } /* if (range->specifics)*/
	} /* all area ranges*/
    } /* all areas */

  if (IS_DEBUG_OSPF_NSSA)
    zlog_info ("ospf_abr_send_nssa_aggregates(): Stop");
}

void
ospf_abr_announce_nssa_defaults () /* By ABR-Translator */
{
  listnode node;
  struct ospf_area *area;
  struct prefix_ipv4 p;

  if (! OSPF_IS_ABR)
    return;

  if (IS_DEBUG_OSPF_NSSA)
    zlog_info ("ospf_abr_announce_stub_defaults(): Start");

  p.family = AF_INET;
  p.prefix.s_addr = OSPF_DEFAULT_DESTINATION;
  p.prefixlen = 0;

  for (node = listhead (ospf_top->areas); node; nextnode (node))
    {
      area = getdata (node);
      if (IS_DEBUG_OSPF_NSSA)
	zlog_info ("ospf_abr_announce_nssa_defaults(): looking at area %s",
		   inet_ntoa (area->area_id));

      if (area->external_routing != OSPF_AREA_NSSA)
	continue;

      if (OSPF_IS_AREA_BACKBONE (area))
	continue; /* Sanity Check */

      /* if (!TranslatorRole continue V 1.0 look for "always" conf */
      if (area->NSSATranslator)
	{
	  if (IS_DEBUG_OSPF_NSSA)
	    zlog_info ("ospf_abr_announce_nssa_defaults(): "
		       "announcing 0.0.0.0/0 to this nssa");
	  /* ospf_abr_announce_nssa_asbr_to_as (&p, area->default_cost, area); */
	}
    }
}
#endif /* HAVE_NSSA */

void
ospf_abr_announce_stub_defaults ()
{
  listnode node;
  struct ospf_area *area;
  struct prefix_ipv4 p;

  if (! OSPF_IS_ABR)
    return;

  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("ospf_abr_announce_stub_defaults(): Start");

  p.family = AF_INET;
  p.prefix.s_addr = OSPF_DEFAULT_DESTINATION;
  p.prefixlen = 0;

  for (node = listhead (ospf_top->areas); node; nextnode (node))
    {
      area = getdata (node);
      if (IS_DEBUG_OSPF_EVENT)
	zlog_info ("ospf_abr_announce_stub_defaults(): looking at area %s",
		   inet_ntoa (area->area_id));

#ifdef HAVE_NSSA
      if (area->external_routing != OSPF_AREA_STUB)
#else
      if (area->external_routing == OSPF_AREA_DEFAULT)
#endif /* HAVE_NSSA */
	continue;

      if (OSPF_IS_AREA_BACKBONE (area))
	continue; /* Sanity Check */

      if (IS_DEBUG_OSPF_EVENT)
	zlog_info ("ospf_abr_announce_stub_defaults(): "
		   "announcing 0.0.0.0/0 to this area");
      ospf_abr_announce_network_to_area (&p, area->default_cost, area);
    }

  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("ospf_abr_announce_stub_defaults(): Stop");
}

#ifdef HAVE_NSSA
int
ospf_abr_remove_unapproved_translates_apply (struct ospf_lsa *lsa, void *p_arg,
					     int int_arg)
{
  if (CHECK_FLAG (lsa->flags, OSPF_LSA_LOCAL_XLT)
      && ! CHECK_FLAG (lsa->flags, OSPF_LSA_APPROVED))
    {
      zlog_info ("ospf_abr_remove_unapproved_translates(): "
		 "removing unapproved translates, ID: %s",
		 inet_ntoa (lsa->data->id));

      /* FLUSH THROUGHOUT AS */
      ospf_lsa_flush_as (lsa);

      /* DISCARD from LSDB  */
    }
  return 0;
}

void
ospf_abr_remove_unapproved_translates () /* For NSSA Translations */
{
  /* All AREA PROCESS should have APPROVED necessary LSAs */
  /* Remove any left over and not APPROVED */
  if (IS_DEBUG_OSPF_NSSA)
    zlog_info ("ospf_abr_remove_unapproved_translates(): Start");

  foreach_lsa (EXTERNAL_LSDB (ospf_top), NULL, 0,
	       ospf_abr_remove_unapproved_translates_apply);
 
  if (IS_DEBUG_OSPF_NSSA)
    zlog_info ("ospf_abr_remove_unapproved_translates(): Stop");
}
#endif /* HAVE_NSSA */

int
ospf_abr_remove_unapproved_summaries_apply (struct ospf_lsa *lsa, void *p_arg,
					    int int_arg)
{
  struct ospf_area *area;

  area = (struct ospf_area *) p_arg;

  if (ospf_lsa_is_self_originated (lsa) &&
      !CHECK_FLAG (lsa->flags, OSPF_LSA_APPROVED))
    {
      if (IS_DEBUG_OSPF_EVENT)
	zlog_info ("ospf_abr_remove_unapproved_summaries(): "
		   "removing unapproved summary, ID: %s",
		   inet_ntoa (lsa->data->id));
      ospf_lsa_flush_area (lsa, area);
    }
  return 0;
}

void
ospf_abr_remove_unapproved_summaries ()
{
  listnode node;
  struct ospf_area *area;

  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("ospf_abr_remove_unapproved_summaries(): Start");

  for (node = listhead (ospf_top->areas); node; nextnode (node))
    {
      area = getdata (node);

      if (IS_DEBUG_OSPF_EVENT)
	zlog_info ("ospf_abr_remove_unapproved_summaries(): "
		   "looking at area %s", inet_ntoa (area->area_id));

      foreach_lsa (SUMMARY_LSDB (area), area, 0,
		   ospf_abr_remove_unapproved_summaries_apply);
      foreach_lsa (SUMMARY_ASBR_LSDB (area), area, 0,
		   ospf_abr_remove_unapproved_summaries_apply);
    }
 
  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("ospf_abr_remove_unapproved_summaries(): Stop");
}

void
ospf_abr_manage_discard_routes ()
{
  listnode node;
  struct route_node *rn;
  struct ospf_area *area;
  struct ospf_area_range *range;

  for (node = listhead (ospf_top->areas); node; nextnode (node))
    if ((area = node->data) != NULL)
      for (rn = route_top (area->ranges); rn; rn = route_next (rn))
	if ((range = rn->info) != NULL)
	  if (!CHECK_FLAG (range->flags, OSPF_RANGE_SUPPRESS))
	    {
	      if (range->specifics)
		ospf_add_discard_route (ospf_top->new_table, area,
					(struct prefix_ipv4 *) &rn->p);
	      else
		ospf_delete_discard_route ((struct prefix_ipv4 *) &rn->p);
	    }
}

#ifdef HAVE_NSSA
/* This is the function taking care about ABR NSSA, i.e.  NSSA
   Translator, -LSA aggregation and flooding. For all NSSAs

   Any SELF-AS-LSA is in the Type-5 LSDB and Type-7 LSDB.  These LSA's
   are refreshed from the Type-5 LSDB, installed into the Type-7 LSDB
   with the P-bit set.

   Any received Type-5s are legal for an ABR, else illegal for IR.
   Received Type-7s are installed, by area, with incoming P-bit.  They
   are flooded; if the Elected NSSA Translator, then P-bit off.

   Additionally, this ABR will place "translated type-7's" into the
   Type-5 LSDB in order to keep track of APPROVAL or not.

   It will scan through every area, looking for Type-7 LSAs with P-Bit
   SET. The Type-7's are either AS-FLOODED & 5-INSTALLED or
   AGGREGATED.  Later, the AGGREGATED LSAs are AS-FLOODED &
   5-INSTALLED.

   5-INSTALLED is into the Type-5 LSDB; Any UNAPPROVED Type-5 LSAs
   left over are FLUSHED and DISCARDED.

   For External Calculations, any NSSA areas use the Type-7 AREA-LSDB,
   any ABR-non-NSSA areas use the Type-5 GLOBAL-LSDB. */

void
ospf_abr_nssa_task () /* called only if any_nssa */
{
  if (! OSPF_IS_ABR)
    return;

  if (! ospf_top->anyNSSA)
    return;

  /* Each area must confirm TranslatorRole */
  if (IS_DEBUG_OSPF_NSSA)
    zlog_info ("ospf_abr_nssa_task(): Start");

  /* For all Global Entries flagged "local-translate", unset APPROVED */
  if (IS_DEBUG_OSPF_NSSA)
    zlog_info ("ospf_abr_nssa_task(): unapprove translates");

  ospf_abr_unapprove_translates ();

  /* RESET all Ranges in every Area, same as summaries */
  if (IS_DEBUG_OSPF_NSSA)
    zlog_info ("ospf_abr_nssa_task(): NSSA initialize aggregates");
  /*    ospf_abr_prepare_aggregates ();  TURNED OFF just for now */

  /* For all NSSAs, Type-7s, translate to 5's, INSTALL/FLOOD, or
     Aggregate as Type-7 */
  /* Install or Approve in Type-5 Global LSDB */
  if (IS_DEBUG_OSPF_NSSA)
    zlog_info ("ospf_abr_nssa_task(): process translates");

  ospf_abr_process_nssa_translates (ospf_top->new_table);

  /* Translate/Send any "ranged" aggregates, and also 5-Install and
     Approve */
  /* Scan Type-7's for aggregates, translate to Type-5's,
     Install/Flood/Approve */
  if (IS_DEBUG_OSPF_NSSA)
    zlog_info("ospf_abr_nssa_task(): send NSSA aggregates");
  /*       ospf_abr_send_nssa_aggregates ();  TURNED OFF FOR NOW */

  /* Send any NSSA defaults as Type-5 */
  if (IS_DEBUG_OSPF_NSSA)
    zlog_info ("ospf_abr_nssa_task(): announce nssa defaults");
  ospf_abr_announce_nssa_defaults ();
   
  /* Flush any unapproved previous translates from Global Data Base */
  if (IS_DEBUG_OSPF_NSSA)
    zlog_info ("ospf_abr_nssa_task(): remove unapproved translates");
  ospf_abr_remove_unapproved_translates ();

  ospf_abr_manage_discard_routes (); /* same as normal...discard */

  if (IS_DEBUG_OSPF_NSSA)
    zlog_info ("ospf_abr_nssa_task(): Stop");
}
#endif /* HAVE_NSSA */

/* This is the function taking care about ABR stuff, i.e.
   summary-LSA origination and flooding. */
void
ospf_abr_task ()
{
  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("ospf_abr_task(): Start");

  if (ospf_top->new_table == NULL || ospf_top->new_rtrs == NULL)
    {
      if (IS_DEBUG_OSPF_EVENT)
	zlog_info ("ospf_abr_task(): Routing tables are not yet ready");
      return;
    }

  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("ospf_abr_task(): unapprove summaries");
  ospf_abr_unapprove_summaries ();

  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("ospf_abr_task(): prepare aggregates");
  ospf_abr_prepare_aggregates ();

  if (OSPF_IS_ABR)
    {
      if (IS_DEBUG_OSPF_EVENT)
	zlog_info ("ospf_abr_task(): process network RT");
      ospf_abr_process_network_rt (ospf_top->new_table);

      if (IS_DEBUG_OSPF_EVENT)
	zlog_info ("ospf_abr_task(): process router RT");
      ospf_abr_process_router_rt (ospf_top->new_rtrs);

      if (IS_DEBUG_OSPF_EVENT)
	zlog_info ("ospf_abr_task(): announce aggregates");
      ospf_abr_announce_aggregates ();

      if (IS_DEBUG_OSPF_EVENT)
	zlog_info ("ospf_abr_task(): announce stub defaults");
      ospf_abr_announce_stub_defaults ();
    }

  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("ospf_abr_task(): remove unapproved summaries");
  ospf_abr_remove_unapproved_summaries ();

  ospf_abr_manage_discard_routes ();

#ifdef HAVE_NSSA
  ospf_abr_nssa_task(); /* if nssa-abr, then scan Type-7 LSDB */
#endif /* HAVE_NSSA */

  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("ospf_abr_task(): Stop");
}


int
ospf_abr_task_timer (struct thread *t)
{
  ospf_top->t_abr_task = 0;

  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("Running ABR task on timer");

  ospf_check_abr_status ();

  ospf_abr_task ();

 return 0;
}

void
ospf_schedule_abr_task ()
{
  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("Scheduling ABR task");
  if (! ospf_top->t_abr_task)
    ospf_top->t_abr_task = thread_add_timer (master, ospf_abr_task_timer,
					     0, OSPF_ABR_TASK_DELAY);
}
