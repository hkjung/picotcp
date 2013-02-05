/*********************************************************************
PicoTCP. Copyright (c) 2012 TASS Belgium NV. Some rights reserved.
See LICENSE and COPYING for usage.
Do not redistribute without a written permission by the Copyright
holders.

Authors: Daniele Lacamera
*********************************************************************/


#include "pico_config.h"
#include "pico_arp.h"
#include "rb.h"
#include "pico_ipv4.h"
#include "pico_device.h"
#include "pico_stack.h"

const uint8_t PICO_ETHADDR_ALL[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
#define PICO_ARP_TIMEOUT 600000
#define PICO_ARP_RETRY 300


#define arp_dbg(...) do{}while(0)

static struct pico_queue pending;
static int pending_timer_on = 0;

void check_pending(unsigned long now, void *_unused)
{
  struct pico_frame *f = pico_dequeue(&pending);
  if (!f) {
    pending_timer_on = 0;
    return;
  }
  if(pico_ethernet_send(f) > 0)
    pico_frame_discard(f);
  pico_timer_add(PICO_ARP_RETRY, &check_pending, NULL);
}


struct
__attribute__ ((__packed__)) 
pico_arp_hdr
{
  uint16_t htype;
  uint16_t ptype;
  uint8_t hsize;
  uint8_t psize;
  uint16_t opcode;
  uint8_t s_mac[PICO_SIZE_ETH];
  struct pico_ip4 src;
  uint8_t d_mac[PICO_SIZE_ETH];
  struct pico_ip4 dst;
};


#define PICO_SIZE_ARPHDR ((sizeof(struct pico_arp_hdr)))



/*****************/
/**  ARP TREE **/
/*****************/

/* Routing destination */

RB_HEAD(arp_tree, pico_arp);
RB_PROTOTYPE_STATIC(arp_tree, pico_arp, node, arp_compare);


static int arp_compare(struct pico_arp *a, struct pico_arp *b)
{

  if (a->ipv4.addr < b->ipv4.addr)
    return -1;
  else if (a->ipv4.addr > b->ipv4.addr)
    return 1;
  return 0;
}

RB_GENERATE_STATIC(arp_tree, pico_arp, node, arp_compare);

static struct arp_tree Arp_table;

/*********************/
/**  END ARP TREE **/
/*********************/

struct pico_arp *pico_arp_get_entry(struct pico_ip4 *dst)
{
  struct pico_arp search, *found;
  search.ipv4.addr = dst->addr;
  found = RB_FIND(arp_tree, &Arp_table, &search);
  if (found && (found->arp_status != PICO_ARP_STATUS_STALE))
    return found;
  return NULL;
}

struct pico_arp *pico_arp_get_entry_by_mac(uint8_t *dst)
{
  struct pico_arp* search;
  RB_FOREACH(search, arp_tree, &Arp_table) {
    if(memcmp(&(search->eth.addr), dst, 6) == 0)
      return search;
  }
  return NULL;
}

struct pico_arp *pico_arp_get(struct pico_frame *f) {
  struct pico_arp *a4;
  struct pico_ip4 gateway;
  struct pico_ipv4_hdr *hdr = (struct pico_ipv4_hdr *) f->net_hdr;
  gateway = pico_ipv4_route_get_gateway(&hdr->dst);
  /* check if dst is local (gateway = 0), or if to use gateway */
  if (gateway.addr != 0)
    a4 = pico_arp_get_entry(&gateway);          /* check if gateway ip mac in cache */
  else
    a4 = pico_arp_get_entry(&hdr->dst);         /* check if local ip mac in cache */
  if (!a4) {
     if (++f->failure_count < 4) {
       dbg ("================= ARP REQUIRED: %d =============\n\n", f->failure_count);
       /* check if dst is local (gateway = 0), or if to use gateway */
       if (gateway.addr != 0)
         pico_arp_query(f->dev, &gateway);  /* arp to gateway */
       else
         pico_arp_query(f->dev, &hdr->dst); /* arp to dst */

       pico_enqueue(&pending, f);
       if (!pending_timer_on) {
        pending_timer_on++;
        pico_timer_add(PICO_ARP_RETRY, &check_pending, NULL);
       }
     } else {
      dbg("ARP: Destination Unreachable\n");
      pico_notify_dest_unreachable(f);
      pico_frame_discard(f);
    }
  }
  return a4;
}

void dbg_arp(void)
{
  struct pico_arp *a;
  RB_FOREACH(a, arp_tree, &Arp_table) {
    arp_dbg("ARP to  %08x, mac: %02x:%02x:%02x:%02x:%02x:%02x\n", a->ipv4.addr,a->eth.addr[0],a->eth.addr[1],a->eth.addr[2],a->eth.addr[3],a->eth.addr[4],a->eth.addr[5] );
  }
}

void arp_expire(unsigned long now, void *_stale)
{
  struct pico_arp *stale = (struct pico_arp *) _stale;
  stale->arp_status = PICO_ARP_STATUS_STALE;
  arp_dbg("ARP: Setting arp_status to STALE\n");
  pico_arp_query(stale->dev, &stale->ipv4);

}

int pico_arp_receive(struct pico_frame *f)
{
  struct pico_arp_hdr *hdr;
  struct pico_arp search, *found, *new = NULL;
  int ret = -1;
  hdr = (struct pico_arp_hdr *) f->net_hdr;

  if (!hdr)
    goto end;


  /* Populate a new arp entry */
  search.ipv4.addr = hdr->src.addr;
  memcpy(search.eth.addr, hdr->s_mac, PICO_SIZE_ETH);

  /* Search for already existing entry */
  found = RB_FIND(arp_tree, &Arp_table, &search);
  if (!found) {
    new = pico_zalloc(sizeof(struct pico_arp));
    if (!new)
      goto end;
    new->ipv4.addr = hdr->src.addr;
  }
  else if (found->arp_status == PICO_ARP_STATUS_STALE) {
    /* Replace if stale */
    new = found;
    RB_REMOVE(arp_tree, &Arp_table, new);
  }

  ret = 0;

  if (new) {
    memcpy(new->eth.addr, hdr->s_mac, PICO_SIZE_ETH);
    new->dev = f->dev;
    pico_arp_add_entry(new);
  }

  if (hdr->opcode == PICO_ARP_REQUEST) {
    struct pico_ip4 me;
    struct pico_eth_hdr *eh = (struct pico_eth_hdr *)f->datalink_hdr;
    struct pico_device *link_dev;
    me.addr = hdr->dst.addr;

    link_dev = pico_ipv4_link_find(&me);
    if (link_dev != f->dev)
      goto end;

    hdr->opcode = PICO_ARP_REPLY;
    memcpy(hdr->d_mac, hdr->s_mac, PICO_SIZE_ETH);
    memcpy(hdr->s_mac, f->dev->eth->mac.addr, PICO_SIZE_ETH);
    hdr->dst.addr = hdr->src.addr;
    hdr->src.addr = me.addr;

    /* Prepare eth header for arp reply */
    memcpy(eh->daddr, eh->saddr, PICO_SIZE_ETH);
    memcpy(eh->saddr, f->dev->eth->mac.addr, PICO_SIZE_ETH);
    f->start = f->datalink_hdr;
    f->len = PICO_SIZE_ETHHDR + PICO_SIZE_ARPHDR;
    f->dev->send(f->dev, f->start, f->len);
  }

  dbg_arp();

end:
  pico_frame_discard(f);
  return ret;
}

void pico_arp_add_entry(struct pico_arp *entry)
{
    entry->arp_status = PICO_ARP_STATUS_REACHABLE;
    entry->timestamp  = PICO_TIME();
    RB_INSERT(arp_tree, &Arp_table, entry);
    arp_dbg("ARP ## reachable.\n");
    pico_timer_add(PICO_ARP_TIMEOUT, arp_expire, entry);
}

int pico_arp_query(struct pico_device *dev, struct pico_ip4 *dst)
{
  struct pico_frame *q = pico_frame_alloc(PICO_SIZE_ETHHDR + PICO_SIZE_ARPHDR);
  struct pico_eth_hdr *eh;
  struct pico_arp_hdr *ah;
  struct pico_ip4 *src;
  int ret;

  src = pico_ipv4_source_find(dst);
  if (!src)
    return -1;

  arp_dbg("QUERY: %08x\n", dst->addr);

  if (!q)
    return -1;
  eh = (struct pico_eth_hdr *)q->start;
  ah = (struct pico_arp_hdr *) (q->start + PICO_SIZE_ETHHDR);

  /* Fill eth header */
  memcpy(eh->saddr, dev->eth->mac.addr, PICO_SIZE_ETH);
  memcpy(eh->daddr, PICO_ETHADDR_ALL, PICO_SIZE_ETH);
  eh->proto = PICO_IDETH_ARP;

  /* Fill arp header */
  ah->htype  = PICO_ARP_HTYPE_ETH;
  ah->ptype  = PICO_IDETH_IPV4;
  ah->hsize  = PICO_SIZE_ETH;
  ah->psize  = PICO_SIZE_IP4;
  ah->opcode = PICO_ARP_REQUEST;
  memcpy(ah->s_mac, dev->eth->mac.addr, PICO_SIZE_ETH);
  ah->src.addr = src->addr;
  ah->dst.addr = dst->addr;
  arp_dbg("Sending arp query.\n");
  ret = dev->send(dev, q->start, q->len);
  pico_frame_discard(q);
  return ret;
}
