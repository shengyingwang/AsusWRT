/*
 * Copyright (c) 2006 Mellanox Technologies. All rights reserved
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * $Id: ipoib_cm.c,v 1.1.1.1 2010/12/02 04:24:57 walf_wu Exp $
 */

#include <rdma/ib_cm.h>
#include <rdma/ib_cache.h>
#include <net/dst.h>
#include <net/icmp.h>
#include <linux/icmpv6.h>

#ifdef CONFIG_INFINIBAND_IPOIB_DEBUG_DATA
static int data_debug_level;

module_param_named(cm_data_debug_level, data_debug_level, int, 0644);
MODULE_PARM_DESC(cm_data_debug_level,
		 "Enable data path debug tracing for connected mode if > 0");
#endif

#include "ipoib.h"

#define IPOIB_CM_IETF_ID 0x1000000000000000ULL

#define IPOIB_CM_RX_UPDATE_TIME (256 * HZ)
#define IPOIB_CM_RX_TIMEOUT     (2 * 256 * HZ)
#define IPOIB_CM_RX_DELAY       (3 * 256 * HZ)
#define IPOIB_CM_RX_UPDATE_MASK (0x3)

struct ipoib_cm_id {
	struct ib_cm_id *id;
	int flags;
	u32 remote_qpn;
	u32 remote_mtu;
};

static int ipoib_cm_tx_handler(struct ib_cm_id *cm_id,
			       struct ib_cm_event *event);

static void ipoib_cm_dma_unmap_rx(struct ipoib_dev_priv *priv, int frags,
				  u64 mapping[IPOIB_CM_RX_SG])
{
	int i;

	ib_dma_unmap_single(priv->ca, mapping[0], IPOIB_CM_HEAD_SIZE, DMA_FROM_DEVICE);

	for (i = 0; i < frags; ++i)
		ib_dma_unmap_single(priv->ca, mapping[i + 1], PAGE_SIZE, DMA_FROM_DEVICE);
}

static int ipoib_cm_post_receive(struct net_device *dev, int id)
{
	struct ipoib_dev_priv *priv = netdev_priv(dev);
	struct ib_recv_wr *bad_wr;
	int i, ret;

	priv->cm.rx_wr.wr_id = id | IPOIB_CM_OP_SRQ;

	for (i = 0; i < IPOIB_CM_RX_SG; ++i)
		priv->cm.rx_sge[i].addr = priv->cm.srq_ring[id].mapping[i];

	ret = ib_post_srq_recv(priv->cm.srq, &priv->cm.rx_wr, &bad_wr);
	if (unlikely(ret)) {
		ipoib_warn(priv, "post srq failed for buf %d (%d)\n", id, ret);
		ipoib_cm_dma_unmap_rx(priv, IPOIB_CM_RX_SG - 1,
				      priv->cm.srq_ring[id].mapping);
		dev_kfree_skb_any(priv->cm.srq_ring[id].skb);
		priv->cm.srq_ring[id].skb = NULL;
	}

	return ret;
}

static struct sk_buff *ipoib_cm_alloc_rx_skb(struct net_device *dev, int id, int frags,
					     u64 mapping[IPOIB_CM_RX_SG])
{
	struct ipoib_dev_priv *priv = netdev_priv(dev);
	struct sk_buff *skb;
	int i;

	skb = dev_alloc_skb(IPOIB_CM_HEAD_SIZE + 12);
	if (unlikely(!skb))
		return NULL;

	/*
	 * IPoIB adds a 4 byte header. So we need 12 more bytes to align the
	 * IP header to a multiple of 16.
	 */
	skb_reserve(skb, 12);

	mapping[0] = ib_dma_map_single(priv->ca, skb->data, IPOIB_CM_HEAD_SIZE,
				       DMA_FROM_DEVICE);
	if (unlikely(ib_dma_mapping_error(priv->ca, mapping[0]))) {
		dev_kfree_skb_any(skb);
		return NULL;
	}

	for (i = 0; i < frags; i++) {
		struct page *page = alloc_page(GFP_ATOMIC);

		if (!page)
			goto partial_error;
		skb_fill_page_desc(skb, i, page, 0, PAGE_SIZE);

		mapping[i + 1] = ib_dma_map_page(priv->ca, skb_shinfo(skb)->frags[i].page,
						 0, PAGE_SIZE, DMA_FROM_DEVICE);
		if (unlikely(ib_dma_mapping_error(priv->ca, mapping[i + 1])))
			goto partial_error;
	}

	priv->cm.srq_ring[id].skb = skb;
	return skb;

partial_error:

	ib_dma_unmap_single(priv->ca, mapping[0], IPOIB_CM_HEAD_SIZE, DMA_FROM_DEVICE);

	for (; i >= 0; --i)
		ib_dma_unmap_single(priv->ca, mapping[i + 1], PAGE_SIZE, DMA_FROM_DEVICE);

	dev_kfree_skb_any(skb);
	return NULL;
}

static struct ib_qp *ipoib_cm_create_rx_qp(struct net_device *dev,
					   struct ipoib_cm_rx *p)
{
	struct ipoib_dev_priv *priv = netdev_priv(dev);
	struct ib_qp_init_attr attr = {
		.send_cq = priv->cq, /* does not matter, we never send anything */
		.recv_cq = priv->cq,
		.srq = priv->cm.srq,
		.cap.max_send_wr = 1, /* FIXME: 0 Seems not to work */
		.cap.max_send_sge = 1, /* FIXME: 0 Seems not to work */
		.sq_sig_type = IB_SIGNAL_ALL_WR,
		.qp_type = IB_QPT_RC,
		.qp_context = p,
	};
	return ib_create_qp(priv->pd, &attr);
}

static int ipoib_cm_modify_rx_qp(struct net_device *dev,
				  struct ib_cm_id *cm_id, struct ib_qp *qp,
				  unsigned psn)
{
	struct ipoib_dev_priv *priv = netdev_priv(dev);
	struct ib_qp_attr qp_attr;
	int qp_attr_mask, ret;

	qp_attr.qp_state = IB_QPS_INIT;
	ret = ib_cm_init_qp_attr(cm_id, &qp_attr, &qp_attr_mask);
	if (ret) {
		ipoib_warn(priv, "failed to init QP attr for INIT: %d\n", ret);
		return ret;
	}
	ret = ib_modify_qp(qp, &qp_attr, qp_attr_mask);
	if (ret) {
		ipoib_warn(priv, "failed to modify QP to INIT: %d\n", ret);
		return ret;
	}
	qp_attr.qp_state = IB_QPS_RTR;
	ret = ib_cm_init_qp_attr(cm_id, &qp_attr, &qp_attr_mask);
	if (ret) {
		ipoib_warn(priv, "failed to init QP attr for RTR: %d\n", ret);
		return ret;
	}
	qp_attr.rq_psn = psn;
	ret = ib_modify_qp(qp, &qp_attr, qp_attr_mask);
	if (ret) {
		ipoib_warn(priv, "failed to modify QP to RTR: %d\n", ret);
		return ret;
	}
	return 0;
}

static int ipoib_cm_send_rep(struct net_device *dev, struct ib_cm_id *cm_id,
			     struct ib_qp *qp, struct ib_cm_req_event_param *req,
			     unsigned psn)
{
	struct ipoib_dev_priv *priv = netdev_priv(dev);
	struct ipoib_cm_data data = {};
	struct ib_cm_rep_param rep = {};

	data.qpn = cpu_to_be32(priv->qp->qp_num);
	data.mtu = cpu_to_be32(IPOIB_CM_BUF_SIZE);

	rep.private_data = &data;
	rep.private_data_len = sizeof data;
	rep.flow_control = 0;
	rep.rnr_retry_count = req->rnr_retry_count;
	rep.target_ack_delay = 20; /* FIXME */
	rep.srq = 1;
	rep.qp_num = qp->qp_num;
	rep.starting_psn = psn;
	return ib_send_cm_rep(cm_id, &rep);
}

static int ipoib_cm_req_handler(struct ib_cm_id *cm_id, struct ib_cm_event *event)
{
	struct net_device *dev = cm_id->context;
	struct ipoib_dev_priv *priv = netdev_priv(dev);
	struct ipoib_cm_rx *p;
	unsigned long flags;
	unsigned psn;
	int ret;

	ipoib_dbg(priv, "REQ arrived\n");
	p = kzalloc(sizeof *p, GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->dev = dev;
	p->id = cm_id;
	p->qp = ipoib_cm_create_rx_qp(dev, p);
	if (IS_ERR(p->qp)) {
		ret = PTR_ERR(p->qp);
		goto err_qp;
	}

	psn = random32() & 0xffffff;
	ret = ipoib_cm_modify_rx_qp(dev, cm_id, p->qp, psn);
	if (ret)
		goto err_modify;

	ret = ipoib_cm_send_rep(dev, cm_id, p->qp, &event->param.req_rcvd, psn);
	if (ret) {
		ipoib_warn(priv, "failed to send REP: %d\n", ret);
		goto err_rep;
	}

	cm_id->context = p;
	p->jiffies = jiffies;
	spin_lock_irqsave(&priv->lock, flags);
	list_add(&p->list, &priv->cm.passive_ids);
	spin_unlock_irqrestore(&priv->lock, flags);
	queue_delayed_work(ipoib_workqueue,
			   &priv->cm.stale_task, IPOIB_CM_RX_DELAY);
	return 0;

err_rep:
err_modify:
	ib_destroy_qp(p->qp);
err_qp:
	kfree(p);
	return ret;
}

static int ipoib_cm_rx_handler(struct ib_cm_id *cm_id,
			       struct ib_cm_event *event)
{
	struct ipoib_cm_rx *p;
	struct ipoib_dev_priv *priv;
	unsigned long flags;
	int ret;

	switch (event->event) {
	case IB_CM_REQ_RECEIVED:
		return ipoib_cm_req_handler(cm_id, event);
	case IB_CM_DREQ_RECEIVED:
		p = cm_id->context;
		ib_send_cm_drep(cm_id, NULL, 0);
		/* Fall through */
	case IB_CM_REJ_RECEIVED:
		p = cm_id->context;
		priv = netdev_priv(p->dev);
		spin_lock_irqsave(&priv->lock, flags);
		if (list_empty(&p->list))
			ret = 0; /* Connection is going away already. */
		else {
			list_del_init(&p->list);
			ret = -ECONNRESET;
		}
		spin_unlock_irqrestore(&priv->lock, flags);
		if (ret) {
			ib_destroy_qp(p->qp);
			kfree(p);
			return ret;
		}
		return 0;
	default:
		return 0;
	}
}
/* Adjust length of skb with fragments to match received data */
static void skb_put_frags(struct sk_buff *skb, unsigned int hdr_space,
			  unsigned int length, struct sk_buff *toskb)
{
	int i, num_frags;
	unsigned int size;

	/* put header into skb */
	size = min(length, hdr_space);
	skb->tail += size;
	skb->len += size;
	length -= size;

	num_frags = skb_shinfo(skb)->nr_frags;
	for (i = 0; i < num_frags; i++) {
		skb_frag_t *frag = &skb_shinfo(skb)->frags[i];

		if (length == 0) {
			/* don't need this page */
			skb_fill_page_desc(toskb, i, frag->page, 0, PAGE_SIZE);
			--skb_shinfo(skb)->nr_frags;
		} else {
			size = min(length, (unsigned) PAGE_SIZE);

			frag->size = size;
			skb->data_len += size;
			skb->truesize += size;
			skb->len += size;
			length -= size;
		}
	}
}

void ipoib_cm_handle_rx_wc(struct net_device *dev, struct ib_wc *wc)
{
	struct ipoib_dev_priv *priv = netdev_priv(dev);
	unsigned int wr_id = wc->wr_id & ~IPOIB_CM_OP_SRQ;
	struct sk_buff *skb, *newskb;
	struct ipoib_cm_rx *p;
	unsigned long flags;
	u64 mapping[IPOIB_CM_RX_SG];
	int frags;

	ipoib_dbg_data(priv, "cm recv completion: id %d, op %d, status: %d\n",
		       wr_id, wc->opcode, wc->status);

	if (unlikely(wr_id >= ipoib_recvq_size)) {
		ipoib_warn(priv, "cm recv completion event with wrid %d (> %d)\n",
			   wr_id, ipoib_recvq_size);
		return;
	}

	skb  = priv->cm.srq_ring[wr_id].skb;

	if (unlikely(wc->status != IB_WC_SUCCESS)) {
		ipoib_dbg(priv, "cm recv error "
			   "(status=%d, wrid=%d vend_err %x)\n",
			   wc->status, wr_id, wc->vendor_err);
		++priv->stats.rx_dropped;
		goto repost;
	}

	if (!likely(wr_id & IPOIB_CM_RX_UPDATE_MASK)) {
		p = wc->qp->qp_context;
		if (time_after_eq(jiffies, p->jiffies + IPOIB_CM_RX_UPDATE_TIME)) {
			spin_lock_irqsave(&priv->lock, flags);
			p->jiffies = jiffies;
			/* Move this entry to list head, but do
			 * not re-add it if it has been removed. */
			if (!list_empty(&p->list))
				list_move(&p->list, &priv->cm.passive_ids);
			spin_unlock_irqrestore(&priv->lock, flags);
			queue_delayed_work(ipoib_workqueue,
					   &priv->cm.stale_task, IPOIB_CM_RX_DELAY);
		}
	}

	frags = PAGE_ALIGN(wc->byte_len - min(wc->byte_len,
					      (unsigned)IPOIB_CM_HEAD_SIZE)) / PAGE_SIZE;

	newskb = ipoib_cm_alloc_rx_skb(dev, wr_id, frags, mapping);
	if (unlikely(!newskb)) {
		/*
		 * If we can't allocate a new RX buffer, dump
		 * this packet and reuse the old buffer.
		 */
		ipoib_dbg(priv, "failed to allocate receive buffer %d\n", wr_id);
		++priv->stats.rx_dropped;
		goto repost;
	}

	ipoib_cm_dma_unmap_rx(priv, frags, priv->cm.srq_ring[wr_id].mapping);
	memcpy(priv->cm.srq_ring[wr_id].mapping, mapping, (frags + 1) * sizeof *mapping);

	ipoib_dbg_data(priv, "received %d bytes, SLID 0x%04x\n",
		       wc->byte_len, wc->slid);

	skb_put_frags(skb, IPOIB_CM_HEAD_SIZE, wc->byte_len, newskb);

	skb->protocol = ((struct ipoib_header *) skb->data)->proto;
	skb->mac.raw = skb->data;
	skb_pull(skb, IPOIB_ENCAP_LEN);

	dev->last_rx = jiffies;
	++priv->stats.rx_packets;
	priv->stats.rx_bytes += skb->len;

	skb->dev = dev;
	/* XXX get correct PACKET_ type here */
	skb->pkt_type = PACKET_HOST;
	netif_rx_ni(skb);

repost:
	if (unlikely(ipoib_cm_post_receive(dev, wr_id)))
		ipoib_warn(priv, "ipoib_cm_post_receive failed "
			   "for buf %d\n", wr_id);
}

static inline int post_send(struct ipoib_dev_priv *priv,
			    struct ipoib_cm_tx *tx,
			    unsigned int wr_id,
			    u64 addr, int len)
{
	struct ib_send_wr *bad_wr;

	priv->tx_sge.addr             = addr;
	priv->tx_sge.length           = len;

	priv->tx_wr.wr_id 	      = wr_id;

	return ib_post_send(tx->qp, &priv->tx_wr, &bad_wr);
}

void ipoib_cm_send(struct net_device *dev, struct sk_buff *skb, struct ipoib_cm_tx *tx)
{
	struct ipoib_dev_priv *priv = netdev_priv(dev);
	struct ipoib_tx_buf *tx_req;
	u64 addr;

	if (unlikely(skb->len > tx->mtu)) {
		ipoib_warn(priv, "packet len %d (> %d) too long to send, dropping\n",
			   skb->len, tx->mtu);
		++priv->stats.tx_dropped;
		++priv->stats.tx_errors;
		ipoib_cm_skb_too_long(dev, skb, tx->mtu - IPOIB_ENCAP_LEN);
		return;
	}

	ipoib_dbg_data(priv, "sending packet: head 0x%x length %d connection 0x%x\n",
		       tx->tx_head, skb->len, tx->qp->qp_num);

	/*
	 * We put the skb into the tx_ring _before_ we call post_send()
	 * because it's entirely possible that the completion handler will
	 * run before we execute anything after the post_send().  That
	 * means we have to make sure everything is properly recorded and
	 * our state is consistent before we call post_send().
	 */
	tx_req = &tx->tx_ring[tx->tx_head & (ipoib_sendq_size - 1)];
	tx_req->skb = skb;
	addr = ib_dma_map_single(priv->ca, skb->data, skb->len, DMA_TO_DEVICE);
	if (unlikely(ib_dma_mapping_error(priv->ca, addr))) {
		++priv->stats.tx_errors;
		dev_kfree_skb_any(skb);
		return;
	}

	tx_req->mapping = addr;

	if (unlikely(post_send(priv, tx, tx->tx_head & (ipoib_sendq_size - 1),
			        addr, skb->len))) {
		ipoib_warn(priv, "post_send failed\n");
		++priv->stats.tx_errors;
		ib_dma_unmap_single(priv->ca, addr, skb->len, DMA_TO_DEVICE);
		dev_kfree_skb_any(skb);
	} else {
		dev->trans_start = jiffies;
		++tx->tx_head;

		if (tx->tx_head - tx->tx_tail == ipoib_sendq_size) {
			ipoib_dbg(priv, "TX ring 0x%x full, stopping kernel net queue\n",
				  tx->qp->qp_num);
			netif_stop_queue(dev);
			set_bit(IPOIB_FLAG_NETIF_STOPPED, &tx->flags);
		}
	}
}

static void ipoib_cm_handle_tx_wc(struct net_device *dev, struct ipoib_cm_tx *tx,
				  struct ib_wc *wc)
{
	struct ipoib_dev_priv *priv = netdev_priv(dev);
	unsigned int wr_id = wc->wr_id;
	struct ipoib_tx_buf *tx_req;
	unsigned long flags;

	ipoib_dbg_data(priv, "cm send completion: id %d, op %d, status: %d\n",
		       wr_id, wc->opcode, wc->status);

	if (unlikely(wr_id >= ipoib_sendq_size)) {
		ipoib_warn(priv, "cm send completion event with wrid %d (> %d)\n",
			   wr_id, ipoib_sendq_size);
		return;
	}

	tx_req = &tx->tx_ring[wr_id];

	ib_dma_unmap_single(priv->ca, tx_req->mapping, tx_req->skb->len, DMA_TO_DEVICE);

	/* FIXME: is this right? Shouldn't we only increment on success? */
	++priv->stats.tx_packets;
	priv->stats.tx_bytes += tx_req->skb->len;

	dev_kfree_skb_any(tx_req->skb);

	spin_lock_irqsave(&priv->tx_lock, flags);
	++tx->tx_tail;
	if (unlikely(test_bit(IPOIB_FLAG_NETIF_STOPPED, &tx->flags)) &&
	    tx->tx_head - tx->tx_tail <= ipoib_sendq_size >> 1) {
		clear_bit(IPOIB_FLAG_NETIF_STOPPED, &tx->flags);
		netif_wake_queue(dev);
	}

	if (wc->status != IB_WC_SUCCESS &&
	    wc->status != IB_WC_WR_FLUSH_ERR) {
		struct ipoib_neigh *neigh;

		ipoib_dbg(priv, "failed cm send event "
			   "(status=%d, wrid=%d vend_err %x)\n",
			   wc->status, wr_id, wc->vendor_err);

		spin_lock(&priv->lock);
		neigh = tx->neigh;

		if (neigh) {
			neigh->cm = NULL;
			list_del(&neigh->list);
			if (neigh->ah)
				ipoib_put_ah(neigh->ah);
			ipoib_neigh_free(dev, neigh);

			tx->neigh = NULL;
		}

		/* queue would be re-started anyway when TX is destroyed,
		 * but it makes sense to do it ASAP here. */
		if (test_and_clear_bit(IPOIB_FLAG_NETIF_STOPPED, &tx->flags))
			netif_wake_queue(dev);

		if (test_and_clear_bit(IPOIB_FLAG_INITIALIZED, &tx->flags)) {
			list_move(&tx->list, &priv->cm.reap_list);
			queue_work(ipoib_workqueue, &priv->cm.reap_task);
		}

		clear_bit(IPOIB_FLAG_OPER_UP, &tx->flags);

		spin_unlock(&priv->lock);
	}

	spin_unlock_irqrestore(&priv->tx_lock, flags);
}

static void ipoib_cm_tx_completion(struct ib_cq *cq, void *tx_ptr)
{
	struct ipoib_cm_tx *tx = tx_ptr;
	int n, i;

	ib_req_notify_cq(cq, IB_CQ_NEXT_COMP);
	do {
		n = ib_poll_cq(cq, IPOIB_NUM_WC, tx->ibwc);
		for (i = 0; i < n; ++i)
			ipoib_cm_handle_tx_wc(tx->dev, tx, tx->ibwc + i);
	} while (n == IPOIB_NUM_WC);
}

int ipoib_cm_dev_open(struct net_device *dev)
{
	struct ipoib_dev_priv *priv = netdev_priv(dev);
	int ret;

	if (!IPOIB_CM_SUPPORTED(dev->dev_addr))
		return 0;

	priv->cm.id = ib_create_cm_id(priv->ca, ipoib_cm_rx_handler, dev);
	if (IS_ERR(priv->cm.id)) {
		printk(KERN_WARNING "%s: failed to create CM ID\n", priv->ca->name);
		return IS_ERR(priv->cm.id);
	}

	ret = ib_cm_listen(priv->cm.id, cpu_to_be64(IPOIB_CM_IETF_ID | priv->qp->qp_num),
			   0, NULL);
	if (ret) {
		printk(KERN_WARNING "%s: failed to listen on ID 0x%llx\n", priv->ca->name,
		       IPOIB_CM_IETF_ID | priv->qp->qp_num);
		ib_destroy_cm_id(priv->cm.id);
		return ret;
	}
	return 0;
}

void ipoib_cm_dev_stop(struct net_device *dev)
{
	struct ipoib_dev_priv *priv = netdev_priv(dev);
	struct ipoib_cm_rx *p;
	unsigned long flags;

	if (!IPOIB_CM_SUPPORTED(dev->dev_addr))
		return;

	ib_destroy_cm_id(priv->cm.id);
	spin_lock_irqsave(&priv->lock, flags);
	while (!list_empty(&priv->cm.passive_ids)) {
		p = list_entry(priv->cm.passive_ids.next, typeof(*p), list);
		list_del_init(&p->list);
		spin_unlock_irqrestore(&priv->lock, flags);
		ib_destroy_cm_id(p->id);
		ib_destroy_qp(p->qp);
		kfree(p);
		spin_lock_irqsave(&priv->lock, flags);
	}
	spin_unlock_irqrestore(&priv->lock, flags);

	cancel_delayed_work(&priv->cm.stale_task);
}

static int ipoib_cm_rep_handler(struct ib_cm_id *cm_id, struct ib_cm_event *event)
{
	struct ipoib_cm_tx *p = cm_id->context;
	struct ipoib_dev_priv *priv = netdev_priv(p->dev);
	struct ipoib_cm_data *data = event->private_data;
	struct sk_buff_head skqueue;
	struct ib_qp_attr qp_attr;
	int qp_attr_mask, ret;
	struct sk_buff *skb;
	unsigned long flags;

	p->mtu = be32_to_cpu(data->mtu);

	if (p->mtu < priv->dev->mtu + IPOIB_ENCAP_LEN) {
		ipoib_warn(priv, "Rejecting connection: mtu %d < device mtu %d + 4\n",
			   p->mtu, priv->dev->mtu);
		return -EINVAL;
	}

	qp_attr.qp_state = IB_QPS_RTR;
	ret = ib_cm_init_qp_attr(cm_id, &qp_attr, &qp_attr_mask);
	if (ret) {
		ipoib_warn(priv, "failed to init QP attr for RTR: %d\n", ret);
		return ret;
	}

	qp_attr.rq_psn = 0 /* FIXME */;
	ret = ib_modify_qp(p->qp, &qp_attr, qp_attr_mask);
	if (ret) {
		ipoib_warn(priv, "failed to modify QP to RTR: %d\n", ret);
		return ret;
	}

	qp_attr.qp_state = IB_QPS_RTS;
	ret = ib_cm_init_qp_attr(cm_id, &qp_attr, &qp_attr_mask);
	if (ret) {
		ipoib_warn(priv, "failed to init QP attr for RTS: %d\n", ret);
		return ret;
	}
	ret = ib_modify_qp(p->qp, &qp_attr, qp_attr_mask);
	if (ret) {
		ipoib_warn(priv, "failed to modify QP to RTS: %d\n", ret);
		return ret;
	}

	skb_queue_head_init(&skqueue);

	spin_lock_irqsave(&priv->lock, flags);
	set_bit(IPOIB_FLAG_OPER_UP, &p->flags);
	if (p->neigh)
		while ((skb = __skb_dequeue(&p->neigh->queue)))
			__skb_queue_tail(&skqueue, skb);
	spin_unlock_irqrestore(&priv->lock, flags);

	while ((skb = __skb_dequeue(&skqueue))) {
		skb->dev = p->dev;
		if (dev_queue_xmit(skb))
			ipoib_warn(priv, "dev_queue_xmit failed "
				   "to requeue packet\n");
	}

	ret = ib_send_cm_rtu(cm_id, NULL, 0);
	if (ret) {
		ipoib_warn(priv, "failed to send RTU: %d\n", ret);
		return ret;
	}
	return 0;
}

static struct ib_qp *ipoib_cm_create_tx_qp(struct net_device *dev, struct ib_cq *cq)
{
	struct ipoib_dev_priv *priv = netdev_priv(dev);
	struct ib_qp_init_attr attr = {};
	attr.recv_cq = priv->cq;
	attr.srq = priv->cm.srq;
	attr.cap.max_send_wr = ipoib_sendq_size;
	attr.cap.max_send_sge = 1;
	attr.sq_sig_type = IB_SIGNAL_ALL_WR;
	attr.qp_type = IB_QPT_RC;
	attr.send_cq = cq;
	return ib_create_qp(priv->pd, &attr);
}

static int ipoib_cm_send_req(struct net_device *dev,
			     struct ib_cm_id *id, struct ib_qp *qp,
			     u32 qpn,
			     struct ib_sa_path_rec *pathrec)
{
	struct ipoib_dev_priv *priv = netdev_priv(dev);
	struct ipoib_cm_data data = {};
	struct ib_cm_req_param req = {};

	data.qpn = cpu_to_be32(priv->qp->qp_num);
	data.mtu = cpu_to_be32(IPOIB_CM_BUF_SIZE);

	req.primary_path 	      = pathrec;
	req.alternate_path 	      = NULL;
	req.service_id                = cpu_to_be64(IPOIB_CM_IETF_ID | qpn);
	req.qp_num 		      = qp->qp_num;
	req.qp_type 		      = qp->qp_type;
	req.private_data 	      = &data;
	req.private_data_len 	      = sizeof data;
	req.flow_control 	      = 0;

	req.starting_psn              = 0; /* FIXME */

	/*
	 * Pick some arbitrary defaults here; we could make these
	 * module parameters if anyone cared about setting them.
	 */
	req.responder_resources	      = 4;
	req.remote_cm_response_timeout = 20;
	req.local_cm_response_timeout  = 20;
	req.retry_count 	      = 0; /* RFC draft warns against retries */
	req.rnr_retry_count 	      = 0; /* RFC draft warns against retries */
	req.max_cm_retries 	      = 15;
	req.srq 	              = 1;
	return ib_send_cm_req(id, &req);
}

static int ipoib_cm_modify_tx_init(struct net_device *dev,
				  struct ib_cm_id *cm_id, struct ib_qp *qp)
{
	struct ipoib_dev_priv *priv = netdev_priv(dev);
	struct ib_qp_attr qp_attr;
	int qp_attr_mask, ret;
	ret = ib_find_cached_pkey(priv->ca, priv->port, priv->pkey, &qp_attr.pkey_index);
	if (ret) {
		ipoib_warn(priv, "pkey 0x%x not in cache: %d\n", priv->pkey, ret);
		return ret;
	}

	qp_attr.qp_state = IB_QPS_INIT;
	qp_attr.qp_access_flags = IB_ACCESS_LOCAL_WRITE;
	qp_attr.port_num = priv->port;
	qp_attr_mask = IB_QP_STATE | IB_QP_ACCESS_FLAGS | IB_QP_PKEY_INDEX | IB_QP_PORT;

	ret = ib_modify_qp(qp, &qp_attr, qp_attr_mask);
	if (ret) {
		ipoib_warn(priv, "failed to modify tx QP to INIT: %d\n", ret);
		return ret;
	}
	return 0;
}

static int ipoib_cm_tx_init(struct ipoib_cm_tx *p, u32 qpn,
			    struct ib_sa_path_rec *pathrec)
{
	struct ipoib_dev_priv *priv = netdev_priv(p->dev);
	int ret;

	p->tx_ring = kzalloc(ipoib_sendq_size * sizeof *p->tx_ring,
				GFP_KERNEL);
	if (!p->tx_ring) {
		ipoib_warn(priv, "failed to allocate tx ring\n");
		ret = -ENOMEM;
		goto err_tx;
	}

	p->cq = ib_create_cq(priv->ca, ipoib_cm_tx_completion, NULL, p,
			     ipoib_sendq_size + 1);
	if (IS_ERR(p->cq)) {
		ret = PTR_ERR(p->cq);
		ipoib_warn(priv, "failed to allocate tx cq: %d\n", ret);
		goto err_cq;
	}

	ret = ib_req_notify_cq(p->cq, IB_CQ_NEXT_COMP);
	if (ret) {
		ipoib_warn(priv, "failed to request completion notification: %d\n", ret);
		goto err_req_notify;
	}

	p->qp = ipoib_cm_create_tx_qp(p->dev, p->cq);
	if (IS_ERR(p->qp)) {
		ret = PTR_ERR(p->qp);
		ipoib_warn(priv, "failed to allocate tx qp: %d\n", ret);
		goto err_qp;
	}

	p->id = ib_create_cm_id(priv->ca, ipoib_cm_tx_handler, p);
	if (IS_ERR(p->id)) {
		ret = PTR_ERR(p->id);
		ipoib_warn(priv, "failed to create tx cm id: %d\n", ret);
		goto err_id;
	}

	ret = ipoib_cm_modify_tx_init(p->dev, p->id,  p->qp);
	if (ret) {
		ipoib_warn(priv, "failed to modify tx qp to rtr: %d\n", ret);
		goto err_modify;
	}

	ret = ipoib_cm_send_req(p->dev, p->id, p->qp, qpn, pathrec);
	if (ret) {
		ipoib_warn(priv, "failed to send cm req: %d\n", ret);
		goto err_send_cm;
	}

	ipoib_dbg(priv, "Request connection 0x%x for gid " IPOIB_GID_FMT " qpn 0x%x\n",
		  p->qp->qp_num, IPOIB_GID_ARG(pathrec->dgid), qpn);

	return 0;

err_send_cm:
err_modify:
	ib_destroy_cm_id(p->id);
err_id:
	p->id = NULL;
	ib_destroy_qp(p->qp);
err_req_notify:
err_qp:
	p->qp = NULL;
	ib_destroy_cq(p->cq);
err_cq:
	p->cq = NULL;
err_tx:
	return ret;
}

static void ipoib_cm_tx_destroy(struct ipoib_cm_tx *p)
{
	struct ipoib_dev_priv *priv = netdev_priv(p->dev);
	struct ipoib_tx_buf *tx_req;

	ipoib_dbg(priv, "Destroy active connection 0x%x head 0x%x tail 0x%x\n",
		  p->qp ? p->qp->qp_num : 0, p->tx_head, p->tx_tail);

	if (p->id)
		ib_destroy_cm_id(p->id);

	if (p->qp)
		ib_destroy_qp(p->qp);

	if (p->cq)
		ib_destroy_cq(p->cq);

	if (test_bit(IPOIB_FLAG_NETIF_STOPPED, &p->flags))
		netif_wake_queue(p->dev);

	if (p->tx_ring) {
		while ((int) p->tx_tail - (int) p->tx_head < 0) {
			tx_req = &p->tx_ring[p->tx_tail & (ipoib_sendq_size - 1)];
			ib_dma_unmap_single(priv->ca, tx_req->mapping, tx_req->skb->len,
					 DMA_TO_DEVICE);
			dev_kfree_skb_any(tx_req->skb);
			++p->tx_tail;
		}

		kfree(p->tx_ring);
	}

	kfree(p);
}

static int ipoib_cm_tx_handler(struct ib_cm_id *cm_id,
			       struct ib_cm_event *event)
{
	struct ipoib_cm_tx *tx = cm_id->context;
	struct ipoib_dev_priv *priv = netdev_priv(tx->dev);
	struct net_device *dev = priv->dev;
	struct ipoib_neigh *neigh;
	unsigned long flags;
	int ret;

	switch (event->event) {
	case IB_CM_DREQ_RECEIVED:
		ipoib_dbg(priv, "DREQ received.\n");
		ib_send_cm_drep(cm_id, NULL, 0);
		break;
	case IB_CM_REP_RECEIVED:
		ipoib_dbg(priv, "REP received.\n");
		ret = ipoib_cm_rep_handler(cm_id, event);
		if (ret)
			ib_send_cm_rej(cm_id, IB_CM_REJ_CONSUMER_DEFINED,
				       NULL, 0, NULL, 0);
		break;
	case IB_CM_REQ_ERROR:
	case IB_CM_REJ_RECEIVED:
	case IB_CM_TIMEWAIT_EXIT:
		ipoib_dbg(priv, "CM error %d.\n", event->event);
		spin_lock_irqsave(&priv->tx_lock, flags);
		spin_lock(&priv->lock);
		neigh = tx->neigh;

		if (neigh) {
			neigh->cm = NULL;
			list_del(&neigh->list);
			if (neigh->ah)
				ipoib_put_ah(neigh->ah);
			ipoib_neigh_free(dev, neigh);

			tx->neigh = NULL;
		}

		if (test_and_clear_bit(IPOIB_FLAG_INITIALIZED, &tx->flags)) {
			list_move(&tx->list, &priv->cm.reap_list);
			queue_work(ipoib_workqueue, &priv->cm.reap_task);
		}

		spin_unlock(&priv->lock);
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		break;
	default:
		break;
	}

	return 0;
}

struct ipoib_cm_tx *ipoib_cm_create_tx(struct net_device *dev, struct ipoib_path *path,
				       struct ipoib_neigh *neigh)
{
	struct ipoib_dev_priv *priv = netdev_priv(dev);
	struct ipoib_cm_tx *tx;

	tx = kzalloc(sizeof *tx, GFP_ATOMIC);
	if (!tx)
		return NULL;

	neigh->cm = tx;
	tx->neigh = neigh;
	tx->path = path;
	tx->dev = dev;
	list_add(&tx->list, &priv->cm.start_list);
	set_bit(IPOIB_FLAG_INITIALIZED, &tx->flags);
	queue_work(ipoib_workqueue, &priv->cm.start_task);
	return tx;
}

void ipoib_cm_destroy_tx(struct ipoib_cm_tx *tx)
{
	struct ipoib_dev_priv *priv = netdev_priv(tx->dev);
	if (test_and_clear_bit(IPOIB_FLAG_INITIALIZED, &tx->flags)) {
		list_move(&tx->list, &priv->cm.reap_list);
		queue_work(ipoib_workqueue, &priv->cm.reap_task);
		ipoib_dbg(priv, "Reap connection for gid " IPOIB_GID_FMT "\n",
			  IPOIB_GID_ARG(tx->neigh->dgid));
		tx->neigh = NULL;
	}
}

static void ipoib_cm_tx_start(struct work_struct *work)
{
	struct ipoib_dev_priv *priv = container_of(work, struct ipoib_dev_priv,
						   cm.start_task);
	struct net_device *dev = priv->dev;
	struct ipoib_neigh *neigh;
	struct ipoib_cm_tx *p;
	unsigned long flags;
	int ret;

	struct ib_sa_path_rec pathrec;
	u32 qpn;

	spin_lock_irqsave(&priv->tx_lock, flags);
	spin_lock(&priv->lock);
	while (!list_empty(&priv->cm.start_list)) {
		p = list_entry(priv->cm.start_list.next, typeof(*p), list);
		list_del_init(&p->list);
		neigh = p->neigh;
		qpn = IPOIB_QPN(neigh->neighbour->ha);
		memcpy(&pathrec, &p->path->pathrec, sizeof pathrec);
		spin_unlock(&priv->lock);
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		ret = ipoib_cm_tx_init(p, qpn, &pathrec);
		spin_lock_irqsave(&priv->tx_lock, flags);
		spin_lock(&priv->lock);
		if (ret) {
			neigh = p->neigh;
			if (neigh) {
				neigh->cm = NULL;
				list_del(&neigh->list);
				if (neigh->ah)
					ipoib_put_ah(neigh->ah);
				ipoib_neigh_free(dev, neigh);
			}
			list_del(&p->list);
			kfree(p);
		}
	}
	spin_unlock(&priv->lock);
	spin_unlock_irqrestore(&priv->tx_lock, flags);
}

static void ipoib_cm_tx_reap(struct work_struct *work)
{
	struct ipoib_dev_priv *priv = container_of(work, struct ipoib_dev_priv,
						   cm.reap_task);
	struct ipoib_cm_tx *p;
	unsigned long flags;

	spin_lock_irqsave(&priv->tx_lock, flags);
	spin_lock(&priv->lock);
	while (!list_empty(&priv->cm.reap_list)) {
		p = list_entry(priv->cm.reap_list.next, typeof(*p), list);
		list_del(&p->list);
		spin_unlock(&priv->lock);
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		ipoib_cm_tx_destroy(p);
		spin_lock_irqsave(&priv->tx_lock, flags);
		spin_lock(&priv->lock);
	}
	spin_unlock(&priv->lock);
	spin_unlock_irqrestore(&priv->tx_lock, flags);
}

static void ipoib_cm_skb_reap(struct work_struct *work)
{
	struct ipoib_dev_priv *priv = container_of(work, struct ipoib_dev_priv,
						   cm.skb_task);
	struct net_device *dev = priv->dev;
	struct sk_buff *skb;
	unsigned long flags;

	unsigned mtu = priv->mcast_mtu;

	spin_lock_irqsave(&priv->tx_lock, flags);
	spin_lock(&priv->lock);
	while ((skb = skb_dequeue(&priv->cm.skb_queue))) {
		spin_unlock(&priv->lock);
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		if (skb->protocol == htons(ETH_P_IP))
			icmp_send(skb, ICMP_DEST_UNREACH, ICMP_FRAG_NEEDED, htonl(mtu));
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
		else if (skb->protocol == htons(ETH_P_IPV6))
			icmpv6_send(skb, ICMPV6_PKT_TOOBIG, 0, mtu, dev);
#endif
		dev_kfree_skb_any(skb);
		spin_lock_irqsave(&priv->tx_lock, flags);
		spin_lock(&priv->lock);
	}
	spin_unlock(&priv->lock);
	spin_unlock_irqrestore(&priv->tx_lock, flags);
}

void ipoib_cm_skb_too_long(struct net_device* dev, struct sk_buff *skb,
			   unsigned int mtu)
{
	struct ipoib_dev_priv *priv = netdev_priv(dev);
	int e = skb_queue_empty(&priv->cm.skb_queue);

	if (skb->dst)
		skb->dst->ops->update_pmtu(skb->dst, mtu);

	skb_queue_tail(&priv->cm.skb_queue, skb);
	if (e)
		queue_work(ipoib_workqueue, &priv->cm.skb_task);
}

static void ipoib_cm_stale_task(struct work_struct *work)
{
	struct ipoib_dev_priv *priv = container_of(work, struct ipoib_dev_priv,
						   cm.stale_task.work);
	struct ipoib_cm_rx *p;
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	while (!list_empty(&priv->cm.passive_ids)) {
		/* List if sorted by LRU, start from tail,
		 * stop when we see a recently used entry */
		p = list_entry(priv->cm.passive_ids.prev, typeof(*p), list);
		if (time_before_eq(jiffies, p->jiffies + IPOIB_CM_RX_TIMEOUT))
			break;
		list_del_init(&p->list);
		spin_unlock_irqrestore(&priv->lock, flags);
		ib_destroy_cm_id(p->id);
		ib_destroy_qp(p->qp);
		kfree(p);
		spin_lock_irqsave(&priv->lock, flags);
	}
	spin_unlock_irqrestore(&priv->lock, flags);
}


static ssize_t show_mode(struct device *d, struct device_attribute *attr, 
			 char *buf)
{
	struct ipoib_dev_priv *priv = netdev_priv(to_net_dev(d));

	if (test_bit(IPOIB_FLAG_ADMIN_CM, &priv->flags))
		return sprintf(buf, "connected\n");
	else
		return sprintf(buf, "datagram\n");
}

static ssize_t set_mode(struct device *d, struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct net_device *dev = to_net_dev(d);
	struct ipoib_dev_priv *priv = netdev_priv(dev);

	/* flush paths if we switch modes so that connections are restarted */
	if (IPOIB_CM_SUPPORTED(dev->dev_addr) && !strcmp(buf, "connected\n")) {
		set_bit(IPOIB_FLAG_ADMIN_CM, &priv->flags);
		ipoib_warn(priv, "enabling connected mode "
			   "will cause multicast packet drops\n");
		ipoib_flush_paths(dev);
		return count;
	}

	if (!strcmp(buf, "datagram\n")) {
		clear_bit(IPOIB_FLAG_ADMIN_CM, &priv->flags);
		dev->mtu = min(priv->mcast_mtu, dev->mtu);
		ipoib_flush_paths(dev);
		return count;
	}

	return -EINVAL;
}

static DEVICE_ATTR(mode, S_IWUSR | S_IRUGO, show_mode, set_mode);

int ipoib_cm_add_mode_attr(struct net_device *dev)
{
	return device_create_file(&dev->dev, &dev_attr_mode);
}

int ipoib_cm_dev_init(struct net_device *dev)
{
	struct ipoib_dev_priv *priv = netdev_priv(dev);
	struct ib_srq_init_attr srq_init_attr = {
		.attr = {
			.max_wr  = ipoib_recvq_size,
			.max_sge = IPOIB_CM_RX_SG
		}
	};
	int ret, i;

	INIT_LIST_HEAD(&priv->cm.passive_ids);
	INIT_LIST_HEAD(&priv->cm.reap_list);
	INIT_LIST_HEAD(&priv->cm.start_list);
	INIT_WORK(&priv->cm.start_task, ipoib_cm_tx_start);
	INIT_WORK(&priv->cm.reap_task, ipoib_cm_tx_reap);
	INIT_WORK(&priv->cm.skb_task, ipoib_cm_skb_reap);
	INIT_DELAYED_WORK(&priv->cm.stale_task, ipoib_cm_stale_task);

	skb_queue_head_init(&priv->cm.skb_queue);

	priv->cm.srq = ib_create_srq(priv->pd, &srq_init_attr);
	if (IS_ERR(priv->cm.srq)) {
		ret = PTR_ERR(priv->cm.srq);
		priv->cm.srq = NULL;
		return ret;
	}

	priv->cm.srq_ring = kzalloc(ipoib_recvq_size * sizeof *priv->cm.srq_ring,
				    GFP_KERNEL);
	if (!priv->cm.srq_ring) {
		printk(KERN_WARNING "%s: failed to allocate CM ring (%d entries)\n",
		       priv->ca->name, ipoib_recvq_size);
		ipoib_cm_dev_cleanup(dev);
		return -ENOMEM;
	}

	for (i = 0; i < IPOIB_CM_RX_SG; ++i)
		priv->cm.rx_sge[i].lkey	= priv->mr->lkey;

	priv->cm.rx_sge[0].length = IPOIB_CM_HEAD_SIZE;
	for (i = 1; i < IPOIB_CM_RX_SG; ++i)
		priv->cm.rx_sge[i].length = PAGE_SIZE;
	priv->cm.rx_wr.next = NULL;
	priv->cm.rx_wr.sg_list = priv->cm.rx_sge;
	priv->cm.rx_wr.num_sge = IPOIB_CM_RX_SG;

	for (i = 0; i < ipoib_recvq_size; ++i) {
		if (!ipoib_cm_alloc_rx_skb(dev, i, IPOIB_CM_RX_SG - 1,
					   priv->cm.srq_ring[i].mapping)) {
			ipoib_warn(priv, "failed to allocate receive buffer %d\n", i);
			ipoib_cm_dev_cleanup(dev);
			return -ENOMEM;
		}
		if (ipoib_cm_post_receive(dev, i)) {
			ipoib_warn(priv, "ipoib_ib_post_receive failed for buf %d\n", i);
			ipoib_cm_dev_cleanup(dev);
			return -EIO;
		}
	}

	priv->dev->dev_addr[0] = IPOIB_FLAGS_RC;
	return 0;
}

void ipoib_cm_dev_cleanup(struct net_device *dev)
{
	struct ipoib_dev_priv *priv = netdev_priv(dev);
	int i, ret;

	if (!priv->cm.srq)
		return;

	ipoib_dbg(priv, "Cleanup ipoib connected mode.\n");

	ret = ib_destroy_srq(priv->cm.srq);
	if (ret)
		ipoib_warn(priv, "ib_destroy_srq failed: %d\n", ret);

	priv->cm.srq = NULL;
	if (!priv->cm.srq_ring)
		return;
	for (i = 0; i < ipoib_recvq_size; ++i)
		if (priv->cm.srq_ring[i].skb) {
			ipoib_cm_dma_unmap_rx(priv, IPOIB_CM_RX_SG - 1,
					      priv->cm.srq_ring[i].mapping);
			dev_kfree_skb_any(priv->cm.srq_ring[i].skb);
			priv->cm.srq_ring[i].skb = NULL;
		}
	kfree(priv->cm.srq_ring);
	priv->cm.srq_ring = NULL;
}
