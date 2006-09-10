/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  linux/mdt/mdt_recovery.c
 *  Lustre Metadata Target (mdt) recovery-related methods
 *
 *  Copyright (C) 2002-2006 Cluster File Systems, Inc.
 *   Author: Huang Hua <huanghua@clusterfs.com>
 *   Author; Pershin Mike <tappro@clusterfs.com>
 *
 *   This file is part of the Lustre file system, http://www.lustre.org
 *   Lustre is a trademark of Cluster File Systems, Inc.
 *
 *   You may have signed or agreed to another license before downloading
 *   this software.  If so, you are bound by the terms and conditions
 *   of that agreement, and the following does not apply to you.  See the
 *   LICENSE file included with this distribution for more information.
 *
 *   If you did not agree to a different license, then this copy of Lustre
 *   is open source software; you can redistribute it and/or modify it
 *   under the terms of version 2 of the GNU General Public License as
 *   published by the Free Software Foundation.
 *
 *   In either case, Lustre is distributed in the hope that it will be
 *   useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   license text for more details.
 */
#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_MDS

#include "mdt_internal.h"

static int mdt_update_server_data(const struct lu_context *ctx,
                                  struct mdt_device *mdt);

/* TODO: maybe this pair should be defined in dt_object.c */
static /*inline*/ ssize_t mdt_read_record(const struct lu_context *ctx,
                                      struct dt_object *dt,
                                      void *buf,
                                      size_t count,
                                      loff_t *pos)
{
        int rc;

        LASSERTF(dt != NULL, "dt is NULL when we want to read record\n");

        rc = dt->do_body_ops->dbo_read(ctx, dt, buf, count, pos);

        if (rc == count)
                rc = 0;
        else if (rc >= 0)
                rc = -EFAULT;
        return rc;
}

static /*inline*/ ssize_t mdt_write_record(const struct lu_context *ctx,
                                       struct dt_object *dt,
                                       const void *buf,
                                       size_t count,
                                       loff_t *pos,
                                       struct thandle *th)
{
        int rc;

        LASSERTF(dt != NULL, "dt is NULL when we want to write record\n");

        rc = dt->do_body_ops->dbo_write(ctx, dt, buf, count, pos, th);
        if (rc == count)
                rc = 0;
        else if (rc >= 0)
                rc = -EFAULT;
        return rc;
}

/* last_rcvd handling */
static int mdt_read_last_rcvd_header(const struct lu_context *ctx,
                                     struct mdt_device *mdt,
                                     struct mdt_server_data *msd)
{
        loff_t off = 0;
        return mdt_read_record(ctx, mdt->mdt_last_rcvd, 
                               msd, sizeof(*msd), &off);
}

static int mdt_write_last_rcvd_header(const struct lu_context *ctx,
                                      struct mdt_device *mdt,
                                      struct mdt_server_data *msd,
                                      struct thandle *th)
{
        int rc;
        loff_t off = 0;

        rc = mdt_write_record(ctx, mdt->mdt_last_rcvd, 
                              msd, sizeof(*msd), &off, th);
        
        CDEBUG(D_INFO, "write last_rcvd header rc = %d:\n"
                       "uuid = %s\n"
                       "last_transno = "LPU64"\n",
                        rc,
                        msd->msd_uuid,
                        msd->msd_last_transno);
        return rc;
}

static int mdt_read_last_rcvd(const struct lu_context *ctx,
                              struct mdt_device *mdt,
                              struct mdt_client_data *mcd, loff_t *off)
{
        return mdt_read_record(ctx, mdt->mdt_last_rcvd, 
                               mcd, sizeof(*mcd), off);
}

static int mdt_write_last_rcvd(const struct lu_context *ctx,
                               struct mdt_device *mdt,
                               struct mdt_client_data *mcd,
                               loff_t *off, struct thandle *th)
{
        int rc;
        rc = mdt_write_record(ctx, mdt->mdt_last_rcvd, 
                              mcd, sizeof(*mcd), off, th);

        CDEBUG(D_INFO, "write mcd rc = %d:\n"
                       "uuid = %s\n"
                       "last_transno = "LPU64"\n"
                       "last_xid = "LPU64"\n"
                       "last_result = %d\n"
                       "last_data = %d\n"
                       "last_close_transno = "LPU64"\n"
                       "last_close_xid = "LPU64"\n"
                       "last_close_result = %d\n",
                        rc,
                        mcd->mcd_uuid,
                        mcd->mcd_last_transno,
                        mcd->mcd_last_xid,
                        mcd->mcd_last_result,
                        mcd->mcd_last_data,
                        mcd->mcd_last_close_transno,
                        mcd->mcd_last_close_xid,
                        mcd->mcd_last_close_result);
        return rc;
}

static int mdt_init_clients_data(const struct lu_context *ctx,
                                struct mdt_device *mdt,
                                unsigned long last_size)
{
        struct mdt_server_data *msd = &mdt->mdt_msd;
        struct mdt_client_data *mcd = NULL;
        struct obd_device      *obd = mdt->mdt_md_dev.md_lu_dev.ld_obd;
        loff_t off = 0;
        int cl_idx;
        int rc = 0;
        ENTRY;

        /* When we do a clean MDS shutdown, we save the last_transno into
         * the header.  If we find clients with higher last_transno values
         * then those clients may need recovery done. */

        for (cl_idx = 0, off = le32_to_cpu(msd->msd_client_start);
             off < last_size; cl_idx++) {
                __u64 last_transno;
                struct obd_export *exp;
                struct mdt_export_data *med;
                
                if (!mcd) {
                        OBD_ALLOC_WAIT(mcd, sizeof(*mcd));
                        if (!mcd)
                                RETURN(-ENOMEM);
                }

                off = le32_to_cpu(msd->msd_client_start) +
                        cl_idx * le16_to_cpu(msd->msd_client_size);

                rc = mdt_read_last_rcvd(ctx, mdt, mcd, &off);
                if (rc) {
                        CERROR("error reading MDS %s idx %d, off %llu: rc %d\n",
                               LAST_RCVD, cl_idx, off, rc);
                        rc = 0;
                        break; /* read error shouldn't cause startup to fail */
                }

                if (mcd->mcd_uuid[0] == '\0') {
                        CDEBUG(D_INFO, "skipping zeroed client at offset %d\n",
                               cl_idx);
                        continue;
                }

                last_transno = mcd_last_transno(mcd);

                /* These exports are cleaned up by mdt_obd_disconnect(), so
                 * they need to be set up like real exports as
                 * mdt_obd_connect() does.
                 */
                CDEBUG(D_HA, "RCVRNG CLIENT uuid: %s idx: %d lr: "LPU64
                       " srv lr: "LPU64" lx: "LPU64"\n", mcd->mcd_uuid, cl_idx,
                       last_transno, le64_to_cpu(msd->msd_last_transno),
                       mcd_last_xid(mcd));

                exp = class_new_export(obd, (struct obd_uuid *)mcd->mcd_uuid);
                if (IS_ERR(exp)) {
                        rc = 0;
                        continue;
                        /* FIXME: Do we really want to return error?
                        GOTO(err_client, rc = PTR_ERR(exp));
                        */
                }

                med = &exp->exp_mdt_data;
                med->med_mcd = mcd;
                rc = mdt_client_add(ctx, mdt, med, cl_idx);
                LASSERTF(rc == 0, "rc = %d\n", rc); /* can't fail existing */
                mcd = NULL;
                exp->exp_replay_needed = 1;
                exp->exp_connecting = 0;
                obd->obd_recoverable_clients++;
                obd->obd_max_recoverable_clients++;
                class_export_put(exp);

                CDEBUG(D_OTHER, "client at idx %d has last_transno = "LPU64"\n",
                       cl_idx, last_transno);

                spin_lock(&mdt->mdt_transno_lock);
                if (last_transno > mdt->mdt_last_transno)
                        mdt->mdt_last_transno = last_transno;
                spin_unlock(&mdt->mdt_transno_lock);
        }

        if (mcd)
                OBD_FREE_PTR(mcd);
        RETURN(rc);
}

static int mdt_init_server_data(const struct lu_context *ctx,
                                struct mdt_device *mdt)
{
        struct mdt_server_data *msd = &mdt->mdt_msd;
        struct mdt_client_data *mcd = NULL;
        struct obd_device      *obd = mdt->mdt_md_dev.md_lu_dev.ld_obd;
        struct mdt_thread_info *mti;
        struct dt_object       *obj;
        struct lu_attr         *la;
        unsigned long last_rcvd_size = 0;
        __u64 mount_count;
        int rc = 0;
        ENTRY;

        /* ensure padding in the struct is the correct size */
        LASSERT(offsetof(struct mdt_server_data, msd_padding) +
                sizeof(msd->msd_padding) == LR_SERVER_SIZE);
        LASSERT(offsetof(struct mdt_client_data, mcd_padding) +
                sizeof(mcd->mcd_padding) == LR_CLIENT_SIZE);

        mti = lu_context_key_get(ctx, &mdt_thread_key);
        LASSERT(mti != NULL);
        la = &mti->mti_attr.ma_attr;

        obj = mdt->mdt_last_rcvd;
        obj->do_ops->do_read_lock(ctx, obj);
        rc = obj->do_ops->do_attr_get(ctx, mdt->mdt_last_rcvd, la);
        obj->do_ops->do_read_unlock(ctx, obj);
        if (rc)
                RETURN(rc);

        last_rcvd_size = la->la_size;
        
        if (last_rcvd_size == 0) {
                LCONSOLE_WARN("%s: new disk, initializing\n", obd->obd_name);

                memcpy(msd->msd_uuid, obd->obd_uuid.uuid,
                       sizeof(msd->msd_uuid));
                msd->msd_last_transno = 0;
                msd->msd_mount_count = 0;
                msd->msd_server_size = cpu_to_le32(LR_SERVER_SIZE);
                msd->msd_client_start = cpu_to_le32(LR_CLIENT_START);
                msd->msd_client_size = cpu_to_le16(LR_CLIENT_SIZE);
                msd->msd_feature_rocompat = cpu_to_le32(OBD_ROCOMPAT_LOVOBJID);
                msd->msd_feature_incompat = cpu_to_le32(OBD_INCOMPAT_MDT |
                                                       OBD_INCOMPAT_COMMON_LR);
        } else {
                rc = mdt_read_last_rcvd_header(ctx, mdt, msd);
                if (rc) {
                        CERROR("error reading MDS %s: rc %d\n", LAST_RCVD, rc);
                        GOTO(out, rc);
                }
                if (strcmp(msd->msd_uuid, obd->obd_uuid.uuid) != 0) {
                        LCONSOLE_ERROR("Trying to start OBD %s using the wrong"
                                       " disk %s. Were the /dev/ assignments "
                                       "rearranged?\n",
                                       obd->obd_uuid.uuid, msd->msd_uuid);
                        GOTO(out, rc = -EINVAL);
                }
        }
        mount_count = le64_to_cpu(msd->msd_mount_count);

        if (msd->msd_feature_incompat & ~cpu_to_le32(MDT_INCOMPAT_SUPP)) {
                CERROR("%s: unsupported incompat filesystem feature(s) %x\n",
                       obd->obd_name, le32_to_cpu(msd->msd_feature_incompat) &
                       ~MDT_INCOMPAT_SUPP);
                GOTO(out, rc = -EINVAL);
        }
        if (msd->msd_feature_rocompat & ~cpu_to_le32(MDT_ROCOMPAT_SUPP)) {
                CERROR("%s: unsupported read-only filesystem feature(s) %x\n",
                       obd->obd_name, le32_to_cpu(msd->msd_feature_rocompat) &
                       ~MDT_ROCOMPAT_SUPP);
                /* Do something like remount filesystem read-only */
                GOTO(out, rc = -EINVAL);
        }
        if (!(msd->msd_feature_incompat & cpu_to_le32(OBD_INCOMPAT_COMMON_LR))){
                CDEBUG(D_WARNING, "using old last_rcvd format\n");
                msd->msd_mount_count = msd->msd_last_transno;
                msd->msd_last_transno = msd->msd_unused;
                /* If we update the last_rcvd, we can never go back to
                   an old install, so leave this in the old format for now.
                msd->msd_feature_incompat |= cpu_to_le32(LR_INCOMPAT_COMMON_LR);
                */
        }
        msd->msd_feature_compat = cpu_to_le32(OBD_COMPAT_MDT);

        spin_lock(&mdt->mdt_transno_lock);
        mdt->mdt_last_transno = le64_to_cpu(msd->msd_last_transno);
        spin_unlock(&mdt->mdt_transno_lock);
        CDEBUG(D_INODE, "========BEGIN DUMPING LAST_RCVD========\n");
        CDEBUG(D_INODE, "%s: server last_transno: "LPU64"\n",
               obd->obd_name, mdt->mdt_last_transno);
        CDEBUG(D_INODE, "%s: server mount_count: "LPU64"\n",
               obd->obd_name, mount_count + 1);
        CDEBUG(D_INODE, "%s: server data size: %u\n",
               obd->obd_name, le32_to_cpu(msd->msd_server_size));
        CDEBUG(D_INODE, "%s: per-client data start: %u\n",
               obd->obd_name, le32_to_cpu(msd->msd_client_start));
        CDEBUG(D_INODE, "%s: per-client data size: %u\n",
               obd->obd_name, le32_to_cpu(msd->msd_client_size));
        CDEBUG(D_INODE, "%s: last_rcvd size: %lu\n",
               obd->obd_name, last_rcvd_size);
        CDEBUG(D_INODE, "%s: last_rcvd clients: %lu\n", obd->obd_name,
               last_rcvd_size <= le32_to_cpu(msd->msd_client_start) ? 0 :
               (last_rcvd_size - le32_to_cpu(msd->msd_client_start)) /
                le16_to_cpu(msd->msd_client_size));
        CDEBUG(D_INODE, "========END DUMPING LAST_RCVD========\n");

        if (!msd->msd_server_size || !msd->msd_client_start ||
            !msd->msd_client_size) {
                CERROR("Bad last_rcvd contents!\n");
                GOTO(out, rc = -EINVAL);
        }

        rc = mdt_init_clients_data(ctx, mdt, last_rcvd_size);
        if (rc)
                GOTO(err_client, rc);

        spin_lock(&mdt->mdt_transno_lock);
        /* obd_last_committed is used for compatibility
         * with other lustre recovery code */
        obd->obd_last_committed = mdt->mdt_last_transno;
        spin_unlock(&mdt->mdt_transno_lock);

        if (obd->obd_recoverable_clients) {
                CWARN("RECOVERY: service %s, %d recoverable clients, "
                      "last_transno "LPU64"\n", obd->obd_name,
                      obd->obd_recoverable_clients, mdt->mdt_last_transno);
                obd->obd_next_recovery_transno = obd->obd_last_committed + 1;
                obd->obd_recovering = 1;
                obd->obd_recovery_start = CURRENT_SECONDS;
                /* Only used for lprocfs_status */
                obd->obd_recovery_end = obd->obd_recovery_start +
                        OBD_RECOVERY_TIMEOUT;
        }

        mdt->mdt_mount_count++;
        msd->msd_mount_count = cpu_to_le64(mdt->mdt_mount_count);

        /* save it, so mount count and last_transno is current */
        rc = mdt_update_server_data(ctx, mdt);
        if (rc)
                GOTO(err_client, rc);

        RETURN(0);

err_client:
        class_disconnect_exports(obd);
out:
        return rc;
}

static int mdt_update_server_data(const struct lu_context *ctx,
                                  struct mdt_device *mdt)
{
        struct mdt_server_data *msd = &mdt->mdt_msd;
        int rc = 0;
        ENTRY;

        CDEBUG(D_SUPER, "MDS mount_count is "LPU64", last_transno is "LPU64"\n",
                mdt->mdt_mount_count, mdt->mdt_last_transno);

        spin_lock(&mdt->mdt_transno_lock);
        msd->msd_last_transno = cpu_to_le64(mdt->mdt_last_transno);
        spin_unlock(&mdt->mdt_transno_lock);

        rc = mdt_write_last_rcvd_header(ctx, mdt, msd, NULL);
        RETURN(rc);
}

/* Add client data to the MDS.  We use a bitmap to locate a free space
 * in the last_rcvd file if cl_off is -1 (i.e. a new client).
 * Otherwise, we just have to read the data from the last_rcvd file and
 * we know its offset.
 *
 * It should not be possible to fail adding an existing client - otherwise
 * mdt_init_server_data() callsite needs to be fixed.
 */
int mdt_client_add(const struct lu_context *ctx,
                   struct mdt_device *mdt,
                   struct mdt_export_data *med, int cl_idx)
{
        unsigned long *bitmap = mdt->mdt_client_bitmap;
        struct mdt_client_data *mcd = med->med_mcd;
        struct mdt_server_data *msd = &mdt->mdt_msd;
        int new_client = (cl_idx == -1);
        int rc = 0;
        ENTRY;

        LASSERT(bitmap != NULL);
        LASSERTF(cl_idx > -2, "%d\n", cl_idx);

        /* the bitmap operations can handle cl_idx > sizeof(long) * 8, so
         * there's no need for extra complication here
         */
        if (new_client) {
                cl_idx = find_first_zero_bit(bitmap, LR_MAX_CLIENTS);
        repeat:
                if (cl_idx >= LR_MAX_CLIENTS ||
                    MDT_FAIL_CHECK_ONCE(OBD_FAIL_MDS_CLIENT_ADD)) {
                        CERROR("no room for clients - fix LR_MAX_CLIENTS\n");
                        return -EOVERFLOW;
                }
                if (test_and_set_bit(cl_idx, bitmap)) {
                        cl_idx = find_next_zero_bit(bitmap, LR_MAX_CLIENTS,
                                                    cl_idx);
                        goto repeat;
                }
        } else {
                if (test_and_set_bit(cl_idx, bitmap)) {
                        CERROR("MDS client %d: bit already set in bitmap!!\n",
                               cl_idx);
                        LBUG();
                }
        }

        CDEBUG(D_INFO, "client at idx %d with UUID '%s' added\n",
               cl_idx, med->med_mcd->mcd_uuid);

        med->med_lr_idx = cl_idx;
        med->med_lr_off = le32_to_cpu(msd->msd_client_start) +
                          (cl_idx * le16_to_cpu(msd->msd_client_size));
        init_mutex(&med->med_mcd_lock);

        LASSERTF(med->med_lr_off > 0, "med_lr_off = %llu\n", med->med_lr_off);

        if (new_client) {
                loff_t off = med->med_lr_off;
                rc = mdt_write_last_rcvd(ctx, mdt, mcd, &off, NULL);
                CDEBUG(D_INFO, 
                       "wrote client mcd at idx %u off %llu (len %u)\n",
                       cl_idx, med->med_lr_off, sizeof(*mcd));
        }
        RETURN(rc);
}

int mdt_client_free(const struct lu_context *ctx,
                    struct mdt_device *mdt,
                    struct mdt_export_data *med)
{
        struct mdt_client_data *mcd  = med->med_mcd;
        struct obd_device      *obd = mdt->mdt_md_dev.md_lu_dev.ld_obd;
        loff_t off;
        int rc = 0;
        ENTRY;

        if (!mcd)
                RETURN(0);

        /* XXX if mcd_uuid were a real obd_uuid, I could use obd_uuid_equals */
        if (!strcmp(med->med_mcd->mcd_uuid, obd->obd_uuid.uuid))
                GOTO(free, 0);

        CDEBUG(D_INFO, "freeing client at idx %u, offset %lld\n",
               med->med_lr_idx, med->med_lr_off);

        off = med->med_lr_off;

        /* Don't clear med_lr_idx here as it is likely also unset.  At worst
         * we leak a client slot that will be cleaned on the next recovery. */
        if (off <= 0) {
                CERROR("client idx %d has offset %lld\n",
                        med->med_lr_idx, off);
                GOTO(free, rc = -EINVAL);
        }

        /* Clear the bit _after_ zeroing out the client so we don't
           race with mdt_client_add and zero out new clients.*/
        if (!test_bit(med->med_lr_idx, mdt->mdt_client_bitmap)) {
                CERROR("MDT client %u: bit already clear in bitmap!!\n",
                       med->med_lr_idx);
                LBUG();
        }

        mutex_down(&med->med_mcd_lock);
        memset(mcd, 0, sizeof *mcd);
        rc = mdt_write_last_rcvd(ctx, mdt, mcd, &off, NULL);
        mutex_up(&med->med_mcd_lock);
        
        CDEBUG(rc == 0 ? D_INFO : D_ERROR,
                        "zeroing out client idx %u in %s rc %d\n",
                        med->med_lr_idx, LAST_RCVD, rc);
        
        if (!test_and_clear_bit(med->med_lr_idx, mdt->mdt_client_bitmap)) {
                CERROR("MDS client %u: bit already clear in bitmap!!\n",
                       med->med_lr_idx);
                LBUG();
        }

        /* Make sure the server's last_transno is up to date. Do this
         * after the client is freed so we know all the client's
         * transactions have been committed. */
        mdt_update_server_data(ctx, mdt);

        EXIT;
free:
        OBD_FREE_PTR(mcd);
        med->med_mcd = NULL;
        return 0;
}

/*
 * last_rcvd & last_committed update callbacks
 */
static int mdt_update_last_rcvd(struct mdt_thread_info *mti,
                                struct thandle *th)
{
        struct mdt_device *mdt = mti->mti_mdt;
        struct ptlrpc_request *req = mdt_info_req(mti);
        struct mdt_export_data *med;
        struct mdt_client_data *mcd;
        loff_t off;
        int err;
        __s32 rc = th->th_result;

        ENTRY;
        LASSERT(req);
        LASSERT(req->rq_export);
        LASSERT(mdt);
        med = &req->rq_export->exp_mdt_data;
        LASSERT(med);
        mcd = med->med_mcd;
        /* if the export has already been failed, we have no last_rcvd slot */
        if (req->rq_export->exp_failed) {
                CWARN("commit transaction for disconnected client %s: rc %d\n",
                      req->rq_export->exp_client_uuid.uuid, rc);
                if (rc == 0)
                        rc = -ENOTCONN;
                RETURN(rc);
        }

        off = med->med_lr_off;
        mutex_down(&med->med_mcd_lock);
        if(lustre_msg_get_opc(req->rq_reqmsg) == MDS_CLOSE) {
                mcd->mcd_last_close_transno = cpu_to_le64(mti->mti_transno);
                mcd->mcd_last_close_xid = cpu_to_le64(req->rq_xid);
                mcd->mcd_last_close_result = cpu_to_le32(rc);
        } else {
                mcd->mcd_last_transno = cpu_to_le64(mti->mti_transno);
                mcd->mcd_last_xid = cpu_to_le64(req->rq_xid);
                mcd->mcd_last_result = cpu_to_le32(rc);
                /*XXX: save intent_disposition in mdt_thread_info?
                 * also there is bug - intent_dispostion is __u64,
                 * see struct ldlm_reply->lock_policy_res1; */
                 mcd->mcd_last_data = cpu_to_le32(mti->mti_opdata);
        }
        if (off <= 0) {
                CERROR("client idx %d has offset %lld\n", med->med_lr_idx, off);
                err = -EINVAL;
        } else {
                err = mdt_write_last_rcvd(mti->mti_ctxt, mdt, mcd, &off, th);
        }
        mutex_up(&med->med_mcd_lock);
        RETURN(err);
}

extern struct lu_context_key mdt_txn_key;
extern struct lu_context_key mdt_thread_key;

enum {
        MDT_TXN_LAST_RCVD_CREDITS = 3
};

/* add credits for last_rcvd update */
static int mdt_txn_start_cb(const struct lu_context *ctx,
                            struct txn_param *param, void *cookie)
{
        param->tp_credits += MDT_TXN_LAST_RCVD_CREDITS;
        return 0;
}

static inline __u64 req_exp_last_xid(struct ptlrpc_request *req)
{
        return le64_to_cpu(req->rq_export->exp_mdt_data.med_mcd->mcd_last_xid);
}

/* Update last_rcvd records with latests transaction data */
static int mdt_txn_stop_cb(const struct lu_context *ctx,
                           struct thandle *txn, void *cookie)
{
        struct mdt_device *mdt = cookie;
        struct mdt_txn_info *txi;
        struct mdt_thread_info *mti;
        struct ptlrpc_request *req;
        
        /* transno in two contexts - for commit_cb and for thread */
        txi = lu_context_key_get(&txn->th_ctx, &mdt_txn_key);
        mti = lu_context_key_get(ctx, &mdt_thread_key);

        /* FIXME: don't handle requests from SEQ/FLD,
         * should be fixed
         */
        if (mti->mti_mdt == NULL) {
                txi->txi_transno = 0;
                return 0;
        }
        /*TODO: checks for recovery cases, see mds_finish_transno */
        spin_lock(&mdt->mdt_transno_lock);
        if (txn->th_result != 0) {
                if (mti->mti_transno != 0) {
                        CERROR("Replay transno "LPU64" failed: rc %i\n",
                               mti->mti_transno, txn->th_result);
                        mti->mti_transno = 0;
                }
        } else if (mti->mti_transno == 0) {
                mti->mti_transno = ++ mdt->mdt_last_transno;
        } else {
                /* replay */
                if (mti->mti_transno > mdt->mdt_last_transno)
                        mdt->mdt_last_transno = mti->mti_transno;
        }
        spin_unlock(&mdt->mdt_transno_lock);

        /* filling reply data */
        req = mdt_info_req(mti);

        /* sometimes the reply message has not been successfully packed */
        LASSERT(req != NULL && req->rq_repmsg != NULL);

        CDEBUG(D_INODE, "transno = %llu, last_committed = %llu\n",
               mti->mti_transno, req->rq_export->exp_obd->obd_last_committed);

        spin_lock(&mdt->mdt_transno_lock);
        req->rq_transno = mti->mti_transno;
        lustre_msg_set_transno(req->rq_repmsg, mti->mti_transno);
        target_committed_to_req(req);
        lustre_msg_set_last_xid(req->rq_repmsg, req_exp_last_xid(req));
        /* save transno for the commit callback */
        txi->txi_transno = mti->mti_transno;
        spin_unlock(&mdt->mdt_transno_lock);

        return mdt_update_last_rcvd(mti, txn);
}

/* commit callback, need to update last_commited value */
static int mdt_txn_commit_cb(const struct lu_context *ctxt, 
                             struct thandle *txn, void *cookie)
{
        struct mdt_device *mdt = cookie;
        struct obd_device *obd = md2lu_dev(&mdt->mdt_md_dev)->ld_obd;
        struct mdt_txn_info *txi;

        txi = lu_context_key_get(&txn->th_ctx, &mdt_txn_key);

        /* copy of obd_transno_commit_cb() but with locking */
        spin_lock(&mdt->mdt_transno_lock);
        if (txi->txi_transno > obd->obd_last_committed) {
                obd->obd_last_committed = txi->txi_transno;
                spin_unlock(&mdt->mdt_transno_lock);
                ptlrpc_commit_replies(obd);
        } else
                spin_unlock(&mdt->mdt_transno_lock);

        CDEBUG(D_HA, "%s: transno "LPD64" committed\n",
               obd->obd_name, txi->txi_transno);

        return 0;
}

int mdt_fs_setup(const struct lu_context *ctx, struct mdt_device *mdt)
{
        struct lu_fid last_fid;
        struct dt_object *last;
        int rc = 0;
        ENTRY;

        /* prepare transactions callbacks */
        mdt->mdt_txn_cb.dtc_txn_start = mdt_txn_start_cb;
        mdt->mdt_txn_cb.dtc_txn_stop = mdt_txn_stop_cb;
        mdt->mdt_txn_cb.dtc_txn_commit = mdt_txn_commit_cb;
        mdt->mdt_txn_cb.dtc_cookie = mdt;

        dt_txn_callback_add(mdt->mdt_bottom, &mdt->mdt_txn_cb);

        last = dt_store_open(ctx, mdt->mdt_bottom,
                             LAST_RCVD, &last_fid);
        if(!IS_ERR(last)) {
                mdt->mdt_last_rcvd = last;
                rc = mdt_init_server_data(ctx, mdt);
                if (rc) {
                        lu_object_put(ctx, &last->do_lu);
                        mdt->mdt_last_rcvd = NULL;
                }
        } else {
                rc = PTR_ERR(last);
                CERROR("cannot open %s: rc = %d\n", LAST_RCVD, rc);
        }

        RETURN (rc);
}


void mdt_fs_cleanup(const struct lu_context *ctx, struct mdt_device *mdt)
{
        struct obd_device *obd = mdt->mdt_md_dev.md_lu_dev.ld_obd;

        /* remove transaction callback */
        dt_txn_callback_del(mdt->mdt_bottom, &mdt->mdt_txn_cb);

        class_disconnect_exports(obd); /* cleans up client info too */
        if (mdt->mdt_last_rcvd)
                lu_object_put(ctx, &mdt->mdt_last_rcvd->do_lu);
        mdt->mdt_last_rcvd = NULL;
}

/* reconstruction code */
void mdt_req_from_mcd(struct ptlrpc_request *req,
                      struct mdt_client_data *mcd)
{
        DEBUG_REQ(D_HA, req, "restoring transno "LPD64"/status %d",
                  mcd->mcd_last_transno, mcd->mcd_last_result);
        req->rq_transno = mcd->mcd_last_transno;
        req->rq_status = mcd->mcd_last_result;
        lustre_msg_set_transno(req->rq_repmsg, req->rq_transno);
        lustre_msg_set_status(req->rq_repmsg, req->rq_status);
        //mds_steal_ack_locks(req);
}

static void mdt_reconstruct_generic(struct mdt_thread_info *mti)
{
        struct ptlrpc_request *req = mdt_info_req(mti);
        struct mdt_export_data *med = &req->rq_export->exp_mdt_data;

        return mdt_req_from_mcd(req, med->med_mcd);
}

static void mdt_reconstruct_create(struct mdt_thread_info *mti)
{
        struct ptlrpc_request  *req = mdt_info_req(mti);
        struct mdt_export_data *med = &req->rq_export->exp_mdt_data;
        struct mdt_device *mdt = mti->mti_mdt;
        struct mdt_object *child;
        struct mdt_body *body;
        int rc;

        mdt_req_from_mcd(req, med->med_mcd);
        if (req->rq_status)
                return;

        /* if no error, so child was created with requested fid */
        child = mdt_object_find(mti->mti_ctxt, mdt, mti->mti_rr.rr_fid2);
        LASSERT(!IS_ERR(child));

        body = req_capsule_server_get(&mti->mti_pill, &RMF_MDT_BODY);
        rc = mo_attr_get(mti->mti_ctxt, mdt_object_child(child),
                         &mti->mti_attr);
        if (rc == -EREMOTE) {
                /* object was created on remote server */
                req->rq_status = rc;
                body->valid |= OBD_MD_MDS;
        }
        mdt_pack_attr2body(body, &mti->mti_attr.ma_attr,
                           mti->mti_rr.rr_fid2);
        mdt_object_put(mti->mti_ctxt, child);
}

static void mdt_reconstruct_setattr(struct mdt_thread_info *mti)
{
        struct ptlrpc_request  *req = mdt_info_req(mti);
        struct mdt_export_data *med = &req->rq_export->exp_mdt_data;
        struct mdt_device *mdt = mti->mti_mdt;
        struct mdt_object *obj;
        struct mdt_body *body;

        mdt_req_from_mcd(req, med->med_mcd);
        if (req->rq_status)
                return;

        body = req_capsule_server_get(&mti->mti_pill, &RMF_MDT_BODY);
        obj = mdt_object_find(mti->mti_ctxt, mdt, mti->mti_rr.rr_fid1);
        LASSERT(!IS_ERR(obj));
        mo_attr_get(mti->mti_ctxt, mdt_object_child(obj), &mti->mti_attr);
        mdt_pack_attr2body(body, &mti->mti_attr.ma_attr,
                           mti->mti_rr.rr_fid1);

        /* Don't return OST-specific attributes if we didn't just set them */
/*
        if (rec->ur_iattr.ia_valid & ATTR_SIZE)
                body->valid |= OBD_MD_FLSIZE | OBD_MD_FLBLOCKS;
        if (rec->ur_iattr.ia_valid & (ATTR_MTIME | ATTR_MTIME_SET))
                body->valid |= OBD_MD_FLMTIME;
        if (rec->ur_iattr.ia_valid & (ATTR_ATIME | ATTR_ATIME_SET))
                body->valid |= OBD_MD_FLATIME;
*/
        mdt_object_put(mti->mti_ctxt, obj);
}

typedef void (*mdt_reconstructor)(struct mdt_thread_info *mti);

static mdt_reconstructor reconstructors[REINT_MAX] = {
        [REINT_SETATTR]  = mdt_reconstruct_setattr,
        [REINT_CREATE] = mdt_reconstruct_create,
        [REINT_LINK] = mdt_reconstruct_generic,
        [REINT_UNLINK] = mdt_reconstruct_generic,
        [REINT_RENAME] = mdt_reconstruct_generic,
        [REINT_OPEN] = mdt_reconstruct_open
};

void mdt_reconstruct(struct mdt_thread_info *mti)
{
        ENTRY;
        reconstructors[mti->mti_rr.rr_opcode](mti);
        EXIT;
}

