/* Redistribution Handler
 * Copyright (C) 1998 Kunihiro Ishiguro
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

#include "vector.h"
#include "vty.h"
#include "command.h"
#include "prefix.h"
#include "table.h"
#include "stream.h"
#include "zclient.h"
#include "linklist.h"

#include "zebra/rib.h"
#include "zebra/zserv.h"
#include "zebra/redistribute.h"

int
zebra_check_addr (struct prefix *p)
{
  if (p->family == AF_INET)
    {
      u_int32_t addr;

      addr = p->u.prefix4.s_addr;
      addr = ntohl (addr);

      if (IPV4_NET127 (addr))
	return 0;
    }
#ifdef HAVE_IPV6
  if (p->family == AF_INET6)
    {
      if (IN6_IS_ADDR_LOOPBACK (&p->u.prefix6))
	return 0;
      if (IN6_IS_ADDR_LINKLOCAL(&p->u.prefix6))
	return 0;
    }
#endif /* HAVE_IPV6 */
  return 1;
}

int
is_default (struct prefix *p)
{
  if (p->family == AF_INET)
    if (p->u.prefix4.s_addr == 0 && p->prefixlen == 0)
      return 1;
#ifdef HAVE_IPV6
#if 0  /* IPv6 default separation is now pending until protocol daemon
          can handle that. */
  if (p->family == AF_INET6)
    if (IN6_IS_ADDR_UNSPECIFIED (&p->u.prefix6) && p->prefixlen == 0)
      return 1;
#endif /* 0 */
#endif /* HAVE_IPV6 */
  return 0;
}

void
zebra_redistribute_default (struct zserv *client)
{
  struct prefix_ipv4 p;
  struct route_node *rn;
  struct rib *rib;
#ifdef HAVE_IPV6
  struct prefix_ipv6 p6;
#endif /* HAVE_IPV6 */

  /* Lookup default route. */
  memset (&p, 0, sizeof (struct prefix_ipv4));
  p.family = AF_INET;

  rn = route_node_lookup (ipv4_rib_table, (struct prefix *)&p);

  if (rn)
    {
      for (rib = rn->info; rib; rib = rib->next)
	if (IS_RIB_FIB (rib))
	  zsend_ipv4_add (client, rib->type, 0, (struct prefix_ipv4 *)&rn->p,
			  &rib->u.gate4, rib->u.ifindex);
      route_unlock_node (rn);
    }

#ifdef HAVE_IPV6
  /* Lookup default route. */
  memset (&p6, 0, sizeof (struct prefix_ipv6));
  p6.family = AF_INET6;

  rn = route_node_lookup (ipv6_rib_table, (struct prefix *)&p6);

  if (rn)
    {
      for (rib = rn->info; rib; rib = rib->next)
	if (IS_RIB_FIB (rib))
	  zsend_ipv6_add (client, rib->type, 0, (struct prefix_ipv6 *)&rn->p,
			  &rib->u.gate6, rib->u.ifindex);
      route_unlock_node (rn);
    }
#endif /* HAVE_IPV6 */
}

/* Redistribute routes. */
#ifdef OLD_RIB
void
zebra_redistribute (struct zserv *client, int type)
{
  struct rib *rib;
  struct route_node *np;

  for (np = route_top (ipv4_rib_table); np; np = route_next (np))
    for (rib = np->info; rib; rib = rib->next)
      if (IS_RIB_FIB (rib) && rib->type == type && zebra_check_addr (&np->p))
	zsend_ipv4_add (client, type, 0, (struct prefix_ipv4 *)&np->p,
			&rib->u.gate4, rib->u.ifindex);
  
#ifdef HAVE_IPV6
  for (np = route_top (ipv6_rib_table); np; np = route_next (np))
    for (rib = np->info; rib; rib = rib->next)
      if (IS_RIB_FIB (rib) && rib->type == type && zebra_check_addr (&np->p))
	zsend_ipv6_add (client, type, 0, (struct prefix_ipv6 *)&np->p,
			&rib->u.gate6, rib->u.ifindex);
#endif /* HAVE_IPV6 */
}
#else
void
zebra_redistribute (struct zserv *client, int type)
{
#ifdef HAVE_IPV6
  struct rib *rib = NULL;
#endif /* HAVE_IPV6 */
  struct new_rib *newrib;
  struct route_node *rn;

  for (rn = route_top (ipv4_rib_table); rn; rn = route_next (rn))
    for (newrib = rn->info; newrib; newrib = newrib->next)
      if (CHECK_FLAG (newrib->flags, RIB_FLAG_SELECTED) 
	  && newrib->type == type 
	  && zebra_check_addr (&rn->p))
	zsend_ipv4_add_multipath (client, rn, newrib);
  
#ifdef HAVE_IPV6
  for (rn = route_top (ipv6_rib_table); rn; rn = route_next (rn))
    for (rib = rn->info; rib; rib = rib->next)
      if (IS_RIB_FIB (rib) && rib->type == type && zebra_check_addr (&rn->p))
	zsend_ipv6_add (client, type, 0, (struct prefix_ipv6 *)&rn->p,
			&rib->u.gate6, rib->u.ifindex);
#endif /* HAVE_IPV6 */
}
#endif /* OLD_RIB */

extern list client_list;

#ifndef OLD_RIB
void
redistribute_add_multipath (struct route_node *rn, struct new_rib *rib)
{
  listnode node;
  struct zserv *client;

  for (node = listhead (client_list); node; nextnode (node))
    if ((client = getdata (node)) != NULL)
      {
	if (is_default (&rn->p))
	  {
	    if (client->redist_default || client->redist[rib->type])
	      {
		if (rn->p.family == AF_INET)
		  zsend_ipv4_add_multipath (client, rn, rib);
#if 0
#ifdef HAVE_IPV6
		if (rn->p.family == AF_INET6)
		  zsend_ipv6_add_multipath (client, rn, rib);
#endif /* HAVE_IPV6 */	  
#endif /* 0 */
	      }
	  }
	else if (client->redist[rib->type])
	  {
	    if (rn->p.family == AF_INET)
	      zsend_ipv4_add_multipath (client, rn, rib);
#if 0
#ifdef HAVE_IPV6
	    if (rn->p.family == AF_INET6)
	      zsend_ipv6_add_multipath (client, rn, rib);
#endif /* HAVE_IPV6 */	  
#endif /* 0 */
	  }
      }
}
#endif /* OLD_RIB */

void
redistribute_add (struct route_node *rn, struct rib *rib)
{
  listnode node;
  struct zserv *client;

  for (node = listhead (client_list); node; nextnode (node))
    if ((client = getdata (node)) != NULL)
      {
	if (is_default (&rn->p))
	  {
	    if (client->redist_default || client->redist[rib->type])
	      {
		if (rn->p.family == AF_INET)
		  zsend_ipv4_add (client, rib->type, 0,
				  (struct prefix_ipv4 *)&rn->p, &rib->u.gate4,
				  rib->u.ifindex);
#ifdef HAVE_IPV6
		if (rn->p.family == AF_INET6)
		  zsend_ipv6_add (client, rib->type, 0,
				  (struct prefix_ipv6 *)&rn->p, &rib->u.gate6,
				  rib->u.ifindex);
#endif /* HAVE_IPV6 */	  
	      }
	  }
	else if (client->redist[rib->type])
	  {
	    if (rn->p.family == AF_INET)
	      zsend_ipv4_add (client, rib->type, 0,
			      (struct prefix_ipv4 *)&rn->p, &rib->u.gate4,
			      rib->u.ifindex);
#ifdef HAVE_IPV6
	    if (rn->p.family == AF_INET6)
	      zsend_ipv6_add (client, rib->type, 0,
			      (struct prefix_ipv6 *)&rn->p, &rib->u.gate6,
			      rib->u.ifindex);
#endif /* HAVE_IPV6 */	  
	  }
      }
}

#ifndef OLD_RIB
void
redistribute_delete_multipath (struct route_node *rn, struct new_rib *rib)
{
  listnode node;
  struct zserv *client;

  for (node = listhead (client_list); node; nextnode (node))
    if ((client = getdata (node)) != NULL)
      {
	if (is_default (&rn->p))
	  {
	    if (client->redist_default || client->redist[rib->type])
	      {
		if (rn->p.family == AF_INET)
		  zsend_ipv4_delete_multipath (client, rn, rib);
#if 0
#ifdef HAVE_IPV6
		if (rn->p.family == AF_INET6)
		  zsend_ipv6_delete (client, rib->type, 0,
				     (struct prefix_ipv6 *)&rn->p,
				     &rib->u.gate6,
				     rib->u.ifindex);
#endif /* HAVE_IPV6 */	  
#endif /* 0 */
	      }
	  }
	else if (client->redist[rib->type])
	  {
	    if (rn->p.family == AF_INET)
	      zsend_ipv4_delete_multipath (client, rn, rib);
#if 0
#ifdef HAVE_IPV6
	    if (rn->p.family == AF_INET6)
	      zsend_ipv6_delete (client, rib->type, 0,
				 (struct prefix_ipv6 *)&rn->p, &rib->u.gate6,
				 rib->u.ifindex);
#endif /* HAVE_IPV6 */	  
#endif /* 0 */
	  }
      }
}
#endif /* ! OLD_RIB */

void
redistribute_delete (struct route_node *rn, struct rib *rib)
{
  listnode node;
  struct zserv *client;

  for (node = listhead (client_list); node; nextnode (node))
    if ((client = getdata (node)) != NULL)
      {
	if (is_default (&rn->p))
	  {
	    if (client->redist_default || client->redist[rib->type])
	      {
		if (rn->p.family == AF_INET)
		  zsend_ipv4_delete (client, rib->type, 0, 
				     (struct prefix_ipv4 *)&rn->p,
				     &rib->u.gate4,
				     rib->u.ifindex);
#ifdef HAVE_IPV6
		if (rn->p.family == AF_INET6)
		  zsend_ipv6_delete (client, rib->type, 0,
				     (struct prefix_ipv6 *)&rn->p,
				     &rib->u.gate6,
				     rib->u.ifindex);
#endif /* HAVE_IPV6 */	  
	      }
	  }
	else if (client->redist[rib->type])
	  {
	    if (rn->p.family == AF_INET)
	      zsend_ipv4_delete (client, rib->type, 0, 
				 (struct prefix_ipv4 *)&rn->p, &rib->u.gate4,
				 rib->u.ifindex);
#ifdef HAVE_IPV6
	    if (rn->p.family == AF_INET6)
	      zsend_ipv6_delete (client, rib->type, 0,
				 (struct prefix_ipv6 *)&rn->p, &rib->u.gate6,
				 rib->u.ifindex);
#endif /* HAVE_IPV6 */	  
	  }
      }
}

void
zebra_redistribute_add (int command, struct zserv *client, int length)
{
  int type;

  type = stream_getc (client->ibuf);

  switch (type)
    {
    case ZEBRA_ROUTE_KERNEL:
    case ZEBRA_ROUTE_CONNECT:
    case ZEBRA_ROUTE_STATIC:
    case ZEBRA_ROUTE_RIP:
    case ZEBRA_ROUTE_RIPNG:
    case ZEBRA_ROUTE_OSPF:
    case ZEBRA_ROUTE_OSPF6:
    case ZEBRA_ROUTE_BGP:
      if (! client->redist[type])
	{
	  client->redist[type] = 1;
	  zebra_redistribute (client, type);
	}
      break;
    default:
      break;
    }
}     

void
zebra_redistribute_delete (int command, struct zserv *client, int length)
{
  int type;

  type = stream_getc (client->ibuf);

  switch (type)
    {
    case ZEBRA_ROUTE_KERNEL:
    case ZEBRA_ROUTE_CONNECT:
    case ZEBRA_ROUTE_STATIC:
    case ZEBRA_ROUTE_RIP:
    case ZEBRA_ROUTE_RIPNG:
    case ZEBRA_ROUTE_OSPF:
    case ZEBRA_ROUTE_OSPF6:
    case ZEBRA_ROUTE_BGP:
      client->redist[type] = 0;
      break;
    default:
      break;
    }
}     

void
zebra_redistribute_default_add (int command, struct zserv *client, int length)
{
  client->redist_default = 1;
  zebra_redistribute_default (client);
}     

void
zebra_redistribute_default_delete (int command, struct zserv *client,
				   int length)
{
  client->redist_default = 0;;
}     

/* Interface information update. */
void
zebra_interface_add_update (struct interface *ifp)
{
  listnode node;
  struct zserv *client;

  for (node = listhead (client_list); node; nextnode (node))
    if ((client = getdata (node)) != NULL)
      if (client->ifinfo)
	zsend_interface_add (client, ifp);
}

void
zebra_interface_delete_update (struct interface *ifp)
{
  listnode node;
  struct zserv *client;

  for (node = listhead (client_list); node; nextnode (node))
    if ((client = getdata (node)) != NULL)
      if (client->ifinfo)
	zsend_interface_delete (client, ifp);
}

/* Interface address addition. */
void
zebra_interface_address_add_update (struct interface *ifp, struct connected *c)
{
  listnode node;
  struct zserv *client;

  for (node = listhead (client_list); node; nextnode (node))
    if ((client = getdata (node)) != NULL)
      if (client->ifinfo)
	zsend_interface_address_add (client, ifp, c);
}

/* Interface address deletion. */
void
zebra_interface_address_delete_update (struct interface *ifp,
				       struct connected *c)
{
  listnode node;
  struct zserv *client;

  for (node = listhead (client_list); node; nextnode (node))
    if ((client = getdata (node)) != NULL)
      if (client->ifinfo)
	zsend_interface_address_delete (client, ifp, c);
}
