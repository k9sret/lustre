/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (C) 2002 Cluster File Systems, Inc.
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#define EXPORT_SYMTAB
#define DEBUG_SUBSYSTEM S_RPC

#include <linux/module.h>
#include <linux/lustre_net.h>

ptl_handle_eq_t request_out_eq, reply_in_eq, reply_out_eq, bulk_source_eq,
        bulk_sink_eq;
static const ptl_handle_ni_t *socknal_nip = NULL, *qswnal_nip = NULL;

/*
 *  Free the packet when it has gone out
 */
static int request_out_callback(ptl_event_t *ev, void *data)
{
        struct ptlrpc_request *req = ev->mem_desc.user_ptr;
        struct ptlrpc_client *cl = req->rq_client;

        ENTRY;

        if (ev->type == PTL_EVENT_SENT) {
                spin_lock(&req->rq_client->cli_lock);
                list_del(&req->rq_list);
                list_add(&req->rq_list, &cl->cli_sent_head);
                spin_unlock(&req->rq_client->cli_lock);
        } else {
                // XXX make sure we understand all events, including ACK's
                CERROR("Unknown event %d\n", ev->type);
                LBUG();
        }

        RETURN(1);
}


/*
 *  Free the packet when it has gone out
 */
static int reply_out_callback(ptl_event_t *ev, void *data)
{
        ENTRY;

        if (ev->type == PTL_EVENT_SENT) {
                OBD_FREE(ev->mem_desc.start, ev->mem_desc.length);
        } else {
                // XXX make sure we understand all events, including ACK's
                CERROR("Unknown event %d\n", ev->type);
                LBUG();
        }

        RETURN(1);
}

/*
 * Wake up the thread waiting for the reply once it comes in.
 */
static int reply_in_callback(ptl_event_t *ev, void *data)
{
        struct ptlrpc_request *rpc = ev->mem_desc.user_ptr;
        ENTRY;

        if (ev->type == PTL_EVENT_PUT) {
                rpc->rq_repmsg = ev->mem_desc.start + ev->offset;
                barrier();
                wake_up_interruptible(&rpc->rq_wait_for_rep);
        } else {
                // XXX make sure we understand all events, including ACK's
                CERROR("Unknown event %d\n", ev->type);
                LBUG();
        }

        RETURN(1);
}

int request_in_callback(ptl_event_t *ev, void *data)
{
        struct ptlrpc_service *service = data;
        int index;

        if (ev->rlength != ev->mlength)
                CERROR("Warning: Possibly truncated rpc (%d/%d)\n",
                       ev->mlength, ev->rlength);

        spin_lock(&service->srv_lock);
        for (index = 0; index < service->srv_ring_length; index++)
                if ( service->srv_buf[index] == ev->mem_desc.start)
                        break;

        if (index == service->srv_ring_length)
                LBUG();

        service->srv_ref_count[index]++;

        if (ptl_is_valid_handle(&ev->unlinked_me)) {
                int idx;

                for (idx = 0; idx < service->srv_ring_length; idx++)
                        if (service->srv_me_h[idx].handle_idx ==
                            ev->unlinked_me.handle_idx)
                                break;
                if (idx == service->srv_ring_length)
                        LBUG();

                CDEBUG(D_NET, "unlinked %d\n", idx);
                ptl_set_inv_handle(&(service->srv_me_h[idx]));

                if (service->srv_ref_count[idx] == 0)
                        ptlrpc_link_svc_me(service, idx);
        }

        spin_unlock(&service->srv_lock);
        if (ev->type == PTL_EVENT_PUT)
                wake_up(&service->srv_waitq);
        else
                CERROR("Unexpected event type: %d\n", ev->type);

        return 0;
}

static int bulk_source_callback(ptl_event_t *ev, void *data)
{
        struct ptlrpc_bulk_desc *bulk = ev->mem_desc.user_ptr;
        ENTRY;

        if (ev->type == PTL_EVENT_SENT) {
                CDEBUG(D_NET, "got SENT event\n");
        } else if (ev->type == PTL_EVENT_ACK) {
                CDEBUG(D_NET, "got ACK event\n");
                bulk->b_flags |= PTL_BULK_FL_SENT;
                wake_up_interruptible(&bulk->b_waitq);
        } else {
                CERROR("Unexpected event type!\n");
                LBUG();
        }

        RETURN(1);
}

static int bulk_sink_callback(ptl_event_t *ev, void *data)
{
        struct ptlrpc_bulk_desc *bulk = ev->mem_desc.user_ptr;
        ENTRY;

        if (ev->type == PTL_EVENT_PUT) {
                if (bulk->b_buf != ev->mem_desc.start + ev->offset)
                        CERROR("bulkbuf != mem_desc -- why?\n");
                bulk->b_flags |= PTL_BULK_FL_RCVD;
                if (bulk->b_cb != NULL)
                        bulk->b_cb(bulk, data);
                wake_up_interruptible(&bulk->b_waitq);
        } else {
                CERROR("Unexpected event type!\n");
                LBUG();
        }

        /* FIXME: This should happen unconditionally */
        if (bulk->b_cb != NULL)
                ptlrpc_free_bulk(bulk);

        RETURN(1);
}

int ptlrpc_init_portals(void)
{
        int rc;
        ptl_handle_ni_t ni;

        socknal_nip = inter_module_get_request("ksocknal_ni", "ksocknal");
        qswnal_nip = inter_module_get_request("kqswnal_ni", "kqswnal");
        if (socknal_nip == NULL && qswnal_nip == NULL) {
                CERROR("get_ni failed: is a NAL module loaded?\n");
                return -EIO;
        }

        /* Use the qswnal if it's there */
        if (qswnal_nip != NULL)
                ni = *qswnal_nip;
        else
                ni = *socknal_nip;

        rc = PtlEQAlloc(ni, 128, request_out_callback, NULL, &request_out_eq);
        if (rc != PTL_OK)
                CERROR("PtlEQAlloc failed: %d\n", rc);

        rc = PtlEQAlloc(ni, 128, reply_out_callback, NULL, &reply_out_eq);
        if (rc != PTL_OK)
                CERROR("PtlEQAlloc failed: %d\n", rc);

        rc = PtlEQAlloc(ni, 128, reply_in_callback, NULL, &reply_in_eq);
        if (rc != PTL_OK)
                CERROR("PtlEQAlloc failed: %d\n", rc);

        rc = PtlEQAlloc(ni, 128, bulk_source_callback, NULL, &bulk_source_eq);
        if (rc != PTL_OK)
                CERROR("PtlEQAlloc failed: %d\n", rc);

        rc = PtlEQAlloc(ni, 128, bulk_sink_callback, NULL, &bulk_sink_eq);
        if (rc != PTL_OK)
                CERROR("PtlEQAlloc failed: %d\n", rc);

        return rc;
}

void ptlrpc_exit_portals(void)
{
        PtlEQFree(request_out_eq);
        PtlEQFree(reply_out_eq);
        PtlEQFree(reply_in_eq);
        PtlEQFree(bulk_source_eq);
        PtlEQFree(bulk_sink_eq);

        if (qswnal_nip != NULL)
                inter_module_put("kqswnal_ni");
        if (socknal_nip != NULL)
                inter_module_put("ksocknal_ni");
}
