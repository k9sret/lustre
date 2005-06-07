/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (c) 2002, 2003 Cluster File Systems, Inc.
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

#define DEBUG_SUBSYSTEM S_RPC
#ifndef __KERNEL__
#include <liblustre.h>
#endif
#include <linux/obd_class.h>
#include <linux/obd_support.h>
#include <linux/lustre_net.h>
#include <linux/lustre_lib.h>
#include <linux/obd.h>
#include <linux/lustre_sec.h>
#include "ptlrpc_internal.h"

static int ptl_send_buf (ptl_handle_md_t *mdh, void *base, int len, 
                         ptl_ack_req_t ack, struct ptlrpc_cb_id *cbid,
                         struct ptlrpc_connection *conn, int portal, __u64 xid)
{
        int              rc;
        ptl_md_t         md;
        char str[PTL_NALFMT_SIZE];
        ENTRY;

        LASSERT (portal != 0);
        LASSERT (conn != NULL);
        CDEBUG (D_INFO, "conn=%p ni %s id %s on %s\n",
                conn, conn->c_peer.peer_ni->pni_name,
                ptlrpc_id2str(&conn->c_peer, str),
                conn->c_peer.peer_ni->pni_name);
        md.start     = base;
        md.length    = len;
        md.threshold = (ack == PTL_ACK_REQ) ? 2 : 1;
        md.options   = PTLRPC_MD_OPTIONS;
        md.user_ptr  = cbid;
        md.eq_handle    = conn->c_peer.peer_ni->pni_eq_h;

        if (ack == PTL_ACK_REQ &&
            OBD_FAIL_CHECK(OBD_FAIL_PTLRPC_ACK | OBD_FAIL_ONCE)) {
                /* don't ask for the ack to simulate failing client */
                ack = PTL_NOACK_REQ;
                obd_fail_loc |= OBD_FAIL_ONCE | OBD_FAILED;
        }

        rc = PtlMDBind (conn->c_peer.peer_ni->pni_ni_h, md, 
                        PTL_UNLINK, mdh);
        if (rc != PTL_OK) {
                CERROR ("PtlMDBind failed: %d\n", rc);
                LASSERT (rc == PTL_NO_SPACE);
                RETURN (-ENOMEM);
        }

        CDEBUG(D_NET, "Sending %d bytes to portal %d, xid "LPD64"\n",
               len, portal, xid);

        rc = PtlPut (*mdh, ack, conn->c_peer.peer_id, portal, 0, xid, 0, 0);
        if (rc != PTL_OK) {
                int rc2;   
                /* We're going to get an UNLINK event when I unlink below,
                 * which will complete just like any other failed send, so
                 * I fall through and return success here! */
                CERROR("PtlPut(%s, %d, "LPD64") failed: %d\n",
                       ptlrpc_id2str(&conn->c_peer, str),
                       portal, xid, rc);
                rc2 = PtlMDUnlink(*mdh);
                LASSERTF(rc2 == PTL_OK, "rc2 = %d\n", rc2);
        }

        RETURN (0);
}

int ptlrpc_start_bulk_transfer (struct ptlrpc_bulk_desc *desc)
{
        int                 rc;
        int                 rc2;
        struct ptlrpc_peer *peer;
        ptl_md_t            md;
        __u64               xid;
        char                str[PTL_NALFMT_SIZE];
        ENTRY;

        if (OBD_FAIL_CHECK_ONCE(OBD_FAIL_PTLRPC_BULK_PUT_NET)) 
                RETURN(0);

        /* NB no locking required until desc is on the network */
        LASSERT (!desc->bd_network_rw);
        LASSERT (desc->bd_type == BULK_PUT_SOURCE ||
                 desc->bd_type == BULK_GET_SINK);
        desc->bd_success = 0;
        peer = &desc->bd_export->exp_connection->c_peer;

        md.user_ptr = &desc->bd_cbid;
        md.eq_handle = peer->peer_ni->pni_eq_h;
        md.threshold = 2; /* SENT and ACK/REPLY */
        md.options = PTLRPC_MD_OPTIONS;
        ptlrpc_fill_bulk_md(&md, desc);

        LASSERT (desc->bd_cbid.cbid_fn == server_bulk_callback);
        LASSERT (desc->bd_cbid.cbid_arg == desc);

        /* NB total length may be 0 for a read past EOF, so we send a 0
         * length bulk, since the client expects a bulk event. */

        rc = PtlMDBind(peer->peer_ni->pni_ni_h, md,
                       PTL_UNLINK, &desc->bd_md_h);
        if (rc != PTL_OK) {
                CERROR("PtlMDBind failed: %d\n", rc);
                LASSERT (rc == PTL_NO_SPACE);
                RETURN(-ENOMEM);
        }

        /* Client's bulk and reply matchbits are the same */
        xid = desc->bd_req->rq_xid;
        CDEBUG(D_NET, "Transferring %u pages %u bytes via portal %d on %s "
               "nid %s pid %d xid "LPX64"\n", desc->bd_iov_count,
               desc->bd_nob, desc->bd_portal, peer->peer_ni->pni_name,
               ptlrpc_id2str(peer, str), peer->peer_id.pid, xid);

        /* Network is about to get at the memory */
        desc->bd_network_rw = 1;

        if (desc->bd_type == BULK_PUT_SOURCE)
                rc = PtlPut (desc->bd_md_h, PTL_ACK_REQ, peer->peer_id,
                             desc->bd_portal, 0, xid, 0, 0);
        else
                rc = PtlGet (desc->bd_md_h, peer->peer_id,
                             desc->bd_portal, 0, xid, 0);

        if (rc != PTL_OK) {
                /* Can't send, so we unlink the MD bound above.  The UNLINK
                 * event this creates will signal completion with failure,
                 * so we return SUCCESS here! */
                CERROR("Transfer(%s, %d, "LPX64") failed: %d\n",
                       ptlrpc_id2str(peer, str),
                       desc->bd_portal, xid, rc);
                rc2 = PtlMDUnlink(desc->bd_md_h);
                LASSERT (rc2 == PTL_OK);
        }

        RETURN(0);
}

void ptlrpc_abort_bulk (struct ptlrpc_bulk_desc *desc)
{
        /* Server side bulk abort. Idempotent. Not thread-safe (i.e. only
         * serialises with completion callback) */
        struct l_wait_info lwi;
        int                rc;

        LASSERT (!in_interrupt ());             /* might sleep */

        if (!ptlrpc_bulk_active(desc))          /* completed or */
                return;                         /* never started */
        
        /* The unlink ensures the callback happens ASAP and is the last
         * one.  If it fails, it must be because completion just happened,
         * but we must still l_wait_event() in this case, to give liblustre
         * a chance to run server_bulk_callback()*/

        PtlMDUnlink (desc->bd_md_h);

        for (;;) {
                /* Network access will complete in finite time but the HUGE
                 * timeout lets us CWARN for visibility of sluggish NALs */
                lwi = LWI_TIMEOUT (300 * HZ, NULL, NULL);
                rc = l_wait_event(desc->bd_waitq, 
                                  !ptlrpc_bulk_active(desc), &lwi);
                if (rc == 0)
                        return;

                LASSERT(rc == -ETIMEDOUT);
                CWARN("Unexpectedly long timeout: desc %p\n", desc);
        }
}

int ptlrpc_register_bulk (struct ptlrpc_request *req)
{
        struct ptlrpc_bulk_desc *desc = req->rq_bulk;
        struct ptlrpc_peer *peer;
        int rc;
        int rc2;
        ptl_handle_me_t  me_h;
        ptl_md_t         md;
        ENTRY;

        if (OBD_FAIL_CHECK_ONCE(OBD_FAIL_PTLRPC_BULK_GET_NET)) 
                RETURN(0);

        /* NB no locking required until desc is on the network */
        LASSERT (desc->bd_nob > 0);
        LASSERT (!desc->bd_network_rw);
        LASSERT (desc->bd_iov_count <= PTLRPC_MAX_BRW_PAGES);
        LASSERT (desc->bd_req != NULL);
        LASSERT (desc->bd_type == BULK_PUT_SINK ||
                 desc->bd_type == BULK_GET_SOURCE);

        desc->bd_success = 0;

        peer = &desc->bd_import->imp_connection->c_peer;

        md.user_ptr = &desc->bd_cbid;
        md.eq_handle = peer->peer_ni->pni_eq_h;
        md.threshold = 1;                       /* PUT or GET */
        md.options = PTLRPC_MD_OPTIONS | 
                     ((desc->bd_type == BULK_GET_SOURCE) ? 
                      PTL_MD_OP_GET : PTL_MD_OP_PUT);
        ptlrpc_fill_bulk_md(&md, desc);

        LASSERT (desc->bd_cbid.cbid_fn == client_bulk_callback);
        LASSERT (desc->bd_cbid.cbid_arg == desc);

        /* XXX Registering the same xid on retried bulk makes my head
         * explode trying to understand how the original request's bulk
         * might interfere with the retried request -eeb */
        LASSERT (!desc->bd_registered || req->rq_xid != desc->bd_last_xid);
        desc->bd_registered = 1;
        desc->bd_last_xid = req->rq_xid;

        rc = PtlMEAttach(peer->peer_ni->pni_ni_h, desc->bd_portal,
                         desc->bd_import->imp_connection->c_peer.peer_id, 
                         req->rq_xid, 0, PTL_UNLINK, PTL_INS_AFTER, &me_h);
        if (rc != PTL_OK) {
                CERROR("PtlMEAttach failed: %d\n", rc);
                LASSERT (rc == PTL_NO_SPACE);
                RETURN (-ENOMEM);
        }

        /* About to let the network at it... */
        desc->bd_network_rw = 1;
        rc = PtlMDAttach(me_h, md, PTL_UNLINK, &desc->bd_md_h);
        if (rc != PTL_OK) {
                CERROR("PtlMDAttach failed: %d\n", rc);
                LASSERT (rc == PTL_NO_SPACE);
                desc->bd_network_rw = 0;
                rc2 = PtlMEUnlink (me_h);
                LASSERT (rc2 == PTL_OK);
                RETURN (-ENOMEM);
        }

        CDEBUG(D_NET, "Setup bulk %s buffers: %u pages %u bytes, xid "LPX64", "
               "portal %u on %s\n",
               desc->bd_type == BULK_GET_SOURCE ? "get-source" : "put-sink",
               desc->bd_iov_count, desc->bd_nob,
               req->rq_xid, desc->bd_portal, peer->peer_ni->pni_name);
        RETURN(0);
}

void ptlrpc_unregister_bulk (struct ptlrpc_request *req)
{
        /* Disconnect a bulk desc from the network. Idempotent. Not
         * thread-safe (i.e. only interlocks with completion callback). */
        struct ptlrpc_bulk_desc *desc = req->rq_bulk;
        wait_queue_head_t       *wq;
        struct l_wait_info       lwi;
        int                      rc;

        LASSERT (!in_interrupt ());             /* might sleep */

        if (!ptlrpc_bulk_active(desc))          /* completed or */
                return;                         /* never registered */
        
        LASSERT (desc->bd_req == req);          /* bd_req NULL until registered */

        /* the unlink ensures the callback happens ASAP and is the last
         * one.  If it fails, it must be because completion just happened,
         * but we must still l_wait_event() in this case to give liblustre
         * a chance to run client_bulk_callback() */

        PtlMDUnlink (desc->bd_md_h);
       
        if (req->rq_set != NULL) 
                wq = &req->rq_set->set_waitq;
        else
                wq = &req->rq_reply_waitq;

        for (;;) {
                /* Network access will complete in finite time but the HUGE
                 * timeout lets us CWARN for visibility of sluggish NALs */
                lwi = LWI_TIMEOUT (300 * HZ, NULL, NULL);
                rc = l_wait_event(*wq, !ptlrpc_bulk_active(desc), &lwi);
                if (rc == 0)
                        return;
                
                LASSERT (rc == -ETIMEDOUT);
                CWARN("Unexpectedly long timeout: desc %p\n", desc);
        }
}

int ptlrpc_send_reply (struct ptlrpc_request *req, int may_be_difficult)
{
        struct ptlrpc_service     *svc = req->rq_rqbd->rqbd_srv_ni->sni_service;
        struct ptlrpc_reply_state *rs = req->rq_reply_state;
        struct ptlrpc_connection  *conn;
        int                        rc;

        /* We must already have a reply buffer (only ptlrpc_error() may be
         * called without one).  We usually also have a request buffer which
         * is either the actual (swabbed) incoming request, or a saved copy
         * if this is a req saved in target_queue_final_reply(). but this
         * will not be true since some security handling may skip the reqmsg
         * setting and prepare reply under normal ptlrpc layer */
        LASSERT (rs != NULL);
        LASSERT (req->rq_repmsg != NULL);
        LASSERT (may_be_difficult || !rs->rs_difficult);
        LASSERT (req->rq_repmsg == rs->rs_msg);
        LASSERT (rs->rs_cb_id.cbid_fn == reply_out_callback);
        LASSERT (rs->rs_cb_id.cbid_arg == rs);

        LASSERT (req->rq_repmsg != NULL);
        if (req->rq_type != PTL_RPC_MSG_ERR)
                req->rq_type = PTL_RPC_MSG_REPLY;

        req->rq_repmsg->type   = req->rq_type;
        req->rq_repmsg->status = req->rq_status;
        req->rq_repmsg->opc    = req->rq_reqmsg ? req->rq_reqmsg->opc : 0;

        if (req->rq_export == NULL) 
                conn = ptlrpc_get_connection(&req->rq_peer, NULL);
        else
                conn = ptlrpc_connection_addref(req->rq_export->exp_connection);

        atomic_inc (&svc->srv_outstanding_replies);

        rc = svcsec_authorize(req);
        if (rc) {
                CERROR("Error wrap reply message "LPX64"\n", req->rq_xid);
                goto out;
        }

        rc = ptl_send_buf (&rs->rs_md_h, rs->rs_repbuf, rs->rs_repdata_len,
                           rs->rs_difficult ? PTL_ACK_REQ : PTL_NOACK_REQ,
                           &rs->rs_cb_id, conn,
                           svc->srv_rep_portal, req->rq_xid);
out:
        if (rc != 0) {
                atomic_dec (&svc->srv_outstanding_replies);

                if (!rs->rs_difficult) {
                        /* Callers other than target_send_reply() expect me
                         * to clean up on a comms error */
                        lustre_free_reply_state (rs);
                        req->rq_reply_state = NULL;
                        req->rq_repmsg = NULL;
                }
        }
        ptlrpc_put_connection(conn);
        return rc;
}

int ptlrpc_reply (struct ptlrpc_request *req)
{
        return (ptlrpc_send_reply (req, 0));
}

int ptlrpc_error(struct ptlrpc_request *req)
{
        int rc;
        ENTRY;

        if (!req->rq_repmsg) {
                rc = lustre_pack_reply(req, 0, NULL, NULL);
                if (rc)
                        RETURN(rc);
        }

        req->rq_type = PTL_RPC_MSG_ERR;

        rc = ptlrpc_send_reply (req, 0);
        RETURN(rc);
}

int ptl_send_rpc(struct ptlrpc_request *request)
{
        int rc;
        int rc2;
        struct ptlrpc_connection *connection;
        unsigned long flags;
        ptl_handle_me_t  reply_me_h;
        ptl_md_t         reply_md;
        ENTRY;

        LASSERT (request->rq_type == PTL_RPC_MSG_REQUEST);

        /* If this is a re-transmit, we're required to have disengaged
         * cleanly from the previous attempt */
        LASSERT (!request->rq_receiving_reply);

        connection = request->rq_import->imp_connection;

        if (request->rq_bulk != NULL) {
                rc = ptlrpc_register_bulk (request);
                if (rc != 0)
                        RETURN(rc);
        }

        request->rq_reqmsg->handle = request->rq_import->imp_remote_handle;
        request->rq_reqmsg->type = PTL_RPC_MSG_REQUEST;
        request->rq_reqmsg->conn_cnt = request->rq_import->imp_conn_cnt;

        /* wrap_request might need to refresh gss cred, if this is called
         * in ptlrpcd then the whole daemon thread will be waiting on
         * gss negotiate rpc. FIXME
         */
        rc = ptlrpcs_cli_wrap_request(request);
        if (rc)
                GOTO(cleanup_bulk, rc);

        LASSERT (request->rq_replen != 0);
        if (request->rq_repbuf == NULL) {
                rc = ptlrpcs_cli_alloc_repbuf(request, request->rq_replen);
                if (rc)
                        GOTO(cleanup_bulk, rc);
        }

        rc = PtlMEAttach(connection->c_peer.peer_ni->pni_ni_h,
                         request->rq_reply_portal, /* XXX FIXME bug 249 */
                         connection->c_peer.peer_id, request->rq_xid, 0,
                         PTL_UNLINK, PTL_INS_AFTER, &reply_me_h);
        if (rc != PTL_OK) {
                CERROR("PtlMEAttach failed: %d\n", rc);
                LASSERT (rc == PTL_NO_SPACE);
                GOTO(cleanup_repmsg, rc = -ENOMEM);
        }

        spin_lock_irqsave (&request->rq_lock, flags);
        /* If the MD attach succeeds, there _will_ be a reply_in callback */
        request->rq_receiving_reply = 1;
        /* Clear any flags that may be present from previous sends. */
        request->rq_replied = 0;
        request->rq_err = 0;
        request->rq_timedout = 0;
        request->rq_net_err = 0;
        request->rq_resend = 0;
        request->rq_restart = 0;
        request->rq_ptlrpcs_restart = 0;
        request->rq_ptlrpcs_err = 0;
        spin_unlock_irqrestore (&request->rq_lock, flags);

        reply_md.start     = request->rq_repbuf;
        reply_md.length    = request->rq_repbuf_len;
        reply_md.threshold = 1;
        reply_md.options   = PTLRPC_MD_OPTIONS | PTL_MD_OP_PUT;
        reply_md.user_ptr  = &request->rq_reply_cbid;
        reply_md.eq_handle    = connection->c_peer.peer_ni->pni_eq_h;

        rc = PtlMDAttach(reply_me_h, reply_md, PTL_UNLINK, 
                         &request->rq_reply_md_h);
        if (rc != PTL_OK) {
                CERROR("PtlMDAttach failed: %d\n", rc);
                LASSERT (rc == PTL_NO_SPACE);
                GOTO(cleanup_me, rc -ENOMEM);
        }

        CDEBUG(D_NET, "Setup reply buffer: %u bytes, xid "LPU64
               ", portal %u on %s\n",
               request->rq_replen, request->rq_xid,
               request->rq_reply_portal,
               connection->c_peer.peer_ni->pni_name);

        ptlrpc_request_addref(request);        /* +1 ref for the SENT callback */

        request->rq_sent = LTIME_S(CURRENT_TIME);
        rc = ptl_send_buf(&request->rq_req_md_h, 
                          request->rq_reqbuf, request->rq_reqdata_len,
                          PTL_NOACK_REQ, &request->rq_req_cbid, 
                          connection,
                          request->rq_request_portal,
                          request->rq_xid);
        if (rc == 0) {
                ptlrpc_lprocfs_rpc_sent(request);
                RETURN(rc);
        }

        ptlrpc_req_finished (request);          /* drop callback ref */

 cleanup_me:
        /* MEUnlink is safe; the PUT didn't even get off the ground, and
         * nobody apart from the PUT's target has the right nid+XID to
         * access the reply buffer. */
        rc2 = PtlMEUnlink(reply_me_h);
        LASSERT (rc2 == PTL_OK);
        /* UNLINKED callback called synchronously */
        LASSERT (!request->rq_receiving_reply);

 cleanup_repmsg:
        ptlrpcs_cli_free_repbuf(request);

 cleanup_bulk:
        if (request->rq_bulk != NULL)
                ptlrpc_unregister_bulk(request);

        return rc;
}

int ptlrpc_register_rqbd (struct ptlrpc_request_buffer_desc *rqbd)
{
        struct ptlrpc_srv_ni    *srv_ni = rqbd->rqbd_srv_ni;
        struct ptlrpc_service   *service = srv_ni->sni_service;
        static ptl_process_id_t  match_id = {PTL_NID_ANY, PTL_PID_ANY};
        int                      rc;
        ptl_md_t                 md;
        ptl_handle_me_t          me_h;

        CDEBUG(D_NET, "PtlMEAttach: portal %d on %s\n",
               service->srv_req_portal, srv_ni->sni_ni->pni_name);

        if (OBD_FAIL_CHECK_ONCE(OBD_FAIL_PTLRPC_RQBD))
                return (-ENOMEM);

        rc = PtlMEAttach(srv_ni->sni_ni->pni_ni_h, service->srv_req_portal,
                         match_id, 0, ~0, PTL_UNLINK, PTL_INS_AFTER, &me_h);
        if (rc != PTL_OK) {
                CERROR("PtlMEAttach failed: %d\n", rc);
                return (-ENOMEM);
        }

        LASSERT(rqbd->rqbd_refcount == 0);
        rqbd->rqbd_refcount = 1;

        md.start     = rqbd->rqbd_buffer;
        md.length    = service->srv_buf_size;
        md.max_size  = service->srv_max_req_size;
        md.threshold = PTL_MD_THRESH_INF;
        md.options   = PTLRPC_MD_OPTIONS | PTL_MD_OP_PUT | PTL_MD_MAX_SIZE;
        md.user_ptr  = &rqbd->rqbd_cbid;
        md.eq_handle = srv_ni->sni_ni->pni_eq_h;
        
        rc = PtlMDAttach(me_h, md, PTL_UNLINK, &rqbd->rqbd_md_h);
        if (rc == PTL_OK)
                return (0);

        CERROR("PtlMDAttach failed: %d; \n", rc);
        LASSERT (rc == PTL_NO_SPACE);
        rc = PtlMEUnlink (me_h);
        LASSERT (rc == PTL_OK);
        rqbd->rqbd_refcount = 0;
        
        return (-ENOMEM);
}


/********************************************
 * rawrpc stuff, currently only used by gss *
 ********************************************/

static int rawrpc_timedout(void *data)
{
        struct ptlrpc_request *req = (struct ptlrpc_request *) data;
        unsigned long flags;

        spin_lock_irqsave(&req->rq_lock, flags);
        if (!req->rq_replied)
                req->rq_timedout = 1;
        spin_unlock_irqrestore(&req->rq_lock, flags);

        return 1;
}

static int rawrpc_timedout_wait(void *data)
{
        return 0;
}

/* to make things as simple as possible */
static int rawrpc_check_reply(struct ptlrpc_request *req)
{
        unsigned long flags;
        int rc;

        spin_lock_irqsave (&req->rq_lock, flags);
        rc = req->rq_replied || req->rq_net_err || req->rq_err ||
             req->rq_resend || req->rq_restart;
        spin_unlock_irqrestore(&req->rq_lock, flags);
        return rc;
}

void rawrpc_req_finished(struct ptlrpc_request *req)
{
        struct obd_import *imp;
        unsigned long irq_flags;

        if (!req)
                return;

        if (!atomic_dec_and_test(&req->rq_refcount))
                return;

        LASSERT(req->rq_import);
        imp = req->rq_import;

        spin_lock_irqsave(&imp->imp_lock, irq_flags);
        list_del_init(&req->rq_list);
        spin_unlock_irqrestore(&imp->imp_lock, irq_flags);

        class_import_put(imp);

        if (req->rq_reqbuf) {
                LASSERT(req->rq_reqbuf_len);
                OBD_FREE(req->rq_reqbuf, req->rq_reqbuf_len);
        }
        if (req->rq_repbuf) {
                LASSERT(req->rq_repbuf_len);
                OBD_FREE(req->rq_repbuf, req->rq_repbuf_len);
        }
        OBD_FREE(req, sizeof(*req));
}

/*
 * if returned non-NULL, reqbuf and repbuf will be take over by the
 * request: the caller can't release them directly, instead should
 * call rawrpc_req_finished().
 */
struct ptlrpc_request *
ptl_do_rawrpc(struct obd_import *imp,
              char *reqbuf, int reqbuf_len, int reqlen,
              char *repbuf, int repbuf_len, int *replenp,
              int timeout, int *res)
{
        struct ptlrpc_connection *conn;
        struct ptlrpc_request *request;
        ptl_handle_me_t reply_me_h;
        ptl_md_t reply_md, req_md;
        struct l_wait_info lwi;
        unsigned long irq_flags;
        int rc;
        ENTRY;

        LASSERT(imp);
        LASSERT(reqbuf && reqbuf_len);
        LASSERT(repbuf && repbuf_len);
        LASSERT(reqlen && reqlen <= reqbuf_len);

        OBD_ALLOC(request, sizeof(*request));
        if (!request) {
                *res = -ENOMEM;
                RETURN(NULL);
        }

        imp = request->rq_import = class_import_get(imp);
        conn = imp->imp_connection;

        if (imp->imp_state == LUSTRE_IMP_CLOSED) {
                CDEBUG(D_SEC, "raw rpc on closed imp(=>%s)\n",
                       imp->imp_target_uuid.uuid);
        }

        /* initialize request */
        request->rq_req_cbid.cbid_fn = rawrpc_request_out_callback;
        request->rq_req_cbid.cbid_arg = request;
        request->rq_reply_cbid.cbid_fn  = reply_in_callback;
        request->rq_reply_cbid.cbid_arg = request;
        request->rq_reqbuf = reqbuf;
        request->rq_reqbuf_len = reqbuf_len;
        request->rq_repbuf = repbuf;
        request->rq_repbuf_len = repbuf_len;
        request->rq_set = NULL;
        spin_lock_init(&request->rq_lock);
        init_waitqueue_head(&request->rq_reply_waitq);
        atomic_set(&request->rq_refcount, 1);
        request->rq_xid = ptlrpc_next_xid();

        /* add into sending list */
        spin_lock_irqsave(&imp->imp_lock, irq_flags);
        list_add_tail(&request->rq_list, &imp->imp_rawrpc_list);
        spin_unlock_irqrestore(&imp->imp_lock, irq_flags);

        /* prepare reply buffer */
        rc = PtlMEAttach(conn->c_peer.peer_ni->pni_ni_h,
                         imp->imp_client->cli_reply_portal,
                         conn->c_peer.peer_id, request->rq_xid, 0, PTL_UNLINK,
                         PTL_INS_AFTER, &reply_me_h);
        if (rc != PTL_OK) {
                CERROR("PtlMEAttach failed: %d\n", rc);
                LASSERT (rc == PTL_NO_SPACE);
                GOTO(out, rc = -ENOMEM);
        }

        spin_lock_irqsave(&request->rq_lock, irq_flags);
        request->rq_receiving_reply = 1;
        spin_unlock_irqrestore(&request->rq_lock, irq_flags);

        reply_md.start          = repbuf;
        reply_md.length         = repbuf_len;
        reply_md.threshold      = 1;
        reply_md.options        = PTLRPC_MD_OPTIONS | PTL_MD_OP_PUT;
        reply_md.user_ptr       = &request->rq_reply_cbid;
        reply_md.eq_handle      = conn->c_peer.peer_ni->pni_eq_h;

        rc = PtlMDAttach(reply_me_h, reply_md, PTL_UNLINK,
                          &request->rq_reply_md_h);
        if (rc != PTL_OK) {
                CERROR("PtlMDAttach failed: %d\n", rc);
                LASSERT (rc == PTL_NO_SPACE);
                GOTO(cleanup_me, rc = -ENOMEM);
        }

        /*
         * the extra 2 refcount will be balanced by out_callback
         */
        atomic_set(&request->rq_refcount, 3);

        /* prepare request buffer */
        req_md.start            = reqbuf;
        req_md.length           = reqlen;
        req_md.threshold        = 1;
        req_md.options          = PTLRPC_MD_OPTIONS;
        req_md.user_ptr         = &request->rq_req_cbid;
        req_md.eq_handle        = conn->c_peer.peer_ni->pni_eq_h;

        rc = PtlMDBind(conn->c_peer.peer_ni->pni_ni_h,
                       req_md, PTL_UNLINK, &request->rq_req_md_h);
        if (rc != PTL_OK) {
                CERROR("PtlMDBind failed %d\n", rc);
                LASSERT (rc == PTL_NO_SPACE);
                atomic_set(&request->rq_refcount, 1);
                GOTO(cleanup_me, rc = -ENOMEM);
        }

        rc = PtlPut(request->rq_req_md_h, PTL_NOACK_REQ, conn->c_peer.peer_id,
                    imp->imp_client->cli_request_portal, 0,
                    request->rq_xid, 0, 0);
        if (rc != PTL_OK) {
                CERROR("PtlPut failed %d\n", rc);
                GOTO(cleanup_md, rc);
        }

        if (timeout)
                lwi = LWI_TIMEOUT(timeout * HZ, rawrpc_timedout, request);
        else
                lwi = LWI_TIMEOUT(100 * HZ, rawrpc_timedout_wait, request);

        l_wait_event(request->rq_reply_waitq,
                     rawrpc_check_reply(request), &lwi);

        ptlrpc_unregister_reply(request);

        if (request->rq_replied) {
                *replenp = request->rq_nob_received;
                rc = 0;
        } else {
                CERROR("rawrpc error: err %d, neterr %d, int %d, timedout %d\n",
                        request->rq_err, request->rq_net_err,
                        request->rq_intr, request->rq_timedout);

                /* give timedout higher priority */
                rc = request->rq_timedout ? -ETIMEDOUT : -EIO;
        }

out:
        *res = rc;
        RETURN(request);

cleanup_md:
        PtlMDUnlink(request->rq_req_md_h);
cleanup_me:
        PtlMEUnlink(reply_me_h);
        goto out;
}

/*
 * caller will take care the reqbuf & repbuf, and only return
 * when the rpc really finished on wire.
 */
int ptl_do_rawrpc_simple(struct obd_import *imp,
                         char *reqbuf, int reqlen,
                         char *repbuf, int *replenp)
{
        struct ptlrpc_request *req;
        int res;

        req = ptl_do_rawrpc(imp, reqbuf, reqlen, reqlen,
                            repbuf, *replenp, replenp, 0, &res);

        if (req == NULL)
                return res;

        req->rq_reqbuf = req->rq_repbuf = NULL;
        req->rq_reqbuf_len = req->rq_repbuf_len = 0;

        rawrpc_req_finished(req);
        return res;
}
