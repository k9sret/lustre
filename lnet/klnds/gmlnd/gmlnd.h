/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (c) 2003 Los Alamos National Laboratory (LANL)
 *
 *   This file is part of Lustre, http://www.lustre.org/
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
 */


/*
 *      Portals GM kernel NAL header file
 *      This file makes all declaration and prototypes 
 *      for the API side and CB side of the NAL
 */
#ifndef __INCLUDE_GMNAL_H__
#define __INCLUDE_GMNAL_H__

/* XXX Lustre as of V1.2.2 drop defines VERSION, which causes problems
 * when including <GM>/include/gm_lanai.h which defines a structure field
 * with the name VERSION XXX */
#ifdef VERSION
# undef VERSION
#endif

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif

#include "linux/config.h"
#include "linux/module.h"
#include "linux/tty.h"
#include "linux/kernel.h"
#include "linux/mm.h"
#include "linux/string.h"
#include "linux/stat.h"
#include "linux/errno.h"
#include "linux/version.h"
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
#include "linux/buffer_head.h"
#include "linux/fs.h"
#else
#include "linux/locks.h"
#endif
#include "linux/unistd.h"
#include "linux/init.h"
#include "linux/sem.h"
#include "linux/vmalloc.h"
#include "linux/sysctl.h"

#define DEBUG_SUBSYSTEM S_NAL

#include "libcfs/kp30.h"
#include "lnet/lnet.h"
#include "lnet/lib-lnet.h"

/* undefine these before including the GM headers which clash */
#undef PACKAGE_BUGREPORT
#undef PACKAGE_NAME
#undef PACKAGE_STRING
#undef PACKAGE_TARNAME
#undef PACKAGE_VERSION

#define GM_STRONG_TYPES 1
#ifdef VERSION
#undef VERSION
#endif
#include "gm.h"
#include "gm_internal.h"

/* Default Tunable Values */
#define GMNAL_PORT                 4            /* which port to use */
#define GMNAL_NTX                  32           /* # tx descs */
#define GMNAL_NTX_NBLK             256          /* # reserved tx descs */
#define GMNAL_NRX_SMALL            128          /* # small receives to post */
#define GMNAL_NRX_LARGE            64           /* # large receives to post */
#define GMNAL_NLARGE_TX_BUFS       32           /* # large tx buffers */

/* Fixed tunables */
#define GMNAL_RESCHED              100          /* # busy loops to force scheduler to yield */
#define GMNAL_NETADDR_BASE         0x10000000   /* where we start in network VM */
#define GMNAL_LARGE_PRIORITY       GM_LOW_PRIORITY /* large message GM priority */
#define GMNAL_SMALL_PRIORITY       GM_LOW_PRIORITY /* small message GM priority */

/* Wire protocol */
typedef struct {
        ptl_hdr_t       gmim_hdr;               /* portals header */
        char            gmim_payload[0];        /* payload */
} gmnal_immediate_msg_t;

typedef struct {
        /* First 2 fields fixed FOR ALL TIME */
        __u32           gmm_magic;              /* I'm a GM message */
        __u16           gmm_version;            /* this is my version number */

        __u16           gmm_type;               /* msg type */
        __u64           gmm_srcnid;             /* sender's NID */
        __u64           gmm_dstnid;             /* destination's NID */
        union {
                gmnal_immediate_msg_t   immediate;
        }               gmm_u;
} WIRE_ATTR gmnal_msg_t;

#define GMNAL_MSG_MAGIC                 0x6d797269 /* 'myri'! */
#define GMNAL_MSG_VERSION               1
#define GMNAL_MSG_IMMEDIATE             1

typedef struct netbuf {
        __u64                    nb_netaddr;    /* network VM address */
        struct page             *nb_pages[1];   /* the pages (at least 1) */
} gmnal_netbuf_t;

#define GMNAL_NETBUF_MSG(nb)            ((gmnal_msg_t *)page_address((nb)->nb_pages[0]))
#define GMNAL_NETBUF_LOCAL_NETADDR(nb)  ((void *)((unsigned long)(nb)->nb_netaddr))

typedef struct gmnal_txbuf {
        struct list_head         txb_list;      /* queue on gmni_idle_ltxbs */
        struct gmnal_txbuf      *txb_next;      /* stash on gmni_ltxs */
        gmnal_netbuf_t           txb_buf;       /* space */
} gmnal_txbuf_t;

typedef struct gmnal_tx {
        struct list_head         tx_list;       /* queue */
        int                      tx_isnblk:1;   /* reserved for non-blocking? */
        int                      tx_credit:1;   /* consumed a credit? */
        int                      tx_large_iskiov:1; /* large is in kiovs? */
        struct gmnal_ni         *tx_gmni;       /* owning NI */
        lnet_nid_t               tx_nid;        /* destination NID */
        int                      tx_gmlid;      /* destination GM local ID */
        ptl_msg_t               *tx_ptlmsg;     /* ptlmsg to finalize on completion */

        gmnal_netbuf_t           tx_buf;        /* small tx buffer */
        gmnal_txbuf_t           *tx_ltxb;       /* large buffer (to free on completion) */
        int                      tx_msgnob;     /* message size (so far) */

        int                      tx_large_nob;  /* # bytes large buffer payload */
        int                      tx_large_offset; /* offset within frags */
        int                      tx_large_niov; /* # VM frags */
        union {
                struct iovec    *iov;           /* mapped frags */
                lnet_kiov_t     *kiov;          /* page frags */
        }                        tx_large_frags;
        struct gmnal_tx         *tx_next;       /* stash on gmni_txs */
} gmnal_tx_t;

typedef struct gmnal_rx {
        struct list_head         rx_list;	/* enqueue on gmni_rxq for handling */
        int                      rx_islarge:1;  /* large receive buffer? */
        unsigned int             rx_recv_nob;	/* bytes received */
        __u16                    rx_recv_gmid;	/* sender */
        __u8                     rx_recv_port;	/* sender's port */
        __u8                     rx_recv_type;	/* ?? */
        struct gmnal_rx         *rx_next;	/* stash on gmni_rxs */
        gmnal_netbuf_t           rx_buf;        /* the buffer */
} gmnal_rx_t;

typedef struct gmnal_ni {
        ptl_ni_t         *gmni_ni;              /* generic NI */
        struct gm_port   *gmni_port;            /* GM port */
        spinlock_t        gmni_gm_lock;         /* serialise GM calls */
        int               gmni_large_pages;     /* # pages in a large message buffer */
        int               gmni_large_msgsize;   /* nob in large message buffers */
        int               gmni_large_gmsize;    /* large message GM bucket */
        int               gmni_small_msgsize;   /* nob in small message buffers */
        int               gmni_small_gmsize;    /* small message GM bucket */
        __u64             gmni_netaddr_base;    /* base of mapped network VM */
        int               gmni_netaddr_size;    /* # bytes of mapped network VM */

        gmnal_tx_t       *gmni_txs;             /* all txs */
        gmnal_rx_t       *gmni_rxs;		/* all rx descs */
        gmnal_txbuf_t    *gmni_ltxbs;           /* all large tx bufs */
        
        atomic_t          gmni_nthreads;        /* total # threads */
        gm_alarm_t        gmni_alarm;           /* alarm to wake caretaker */
        int               gmni_shutdown;	/* tell all threads to exit */

        struct list_head  gmni_idle_txs;        /* idle tx's */
        struct list_head  gmni_nblk_idle_txs;   /* reserved for non-blocking callers */
        wait_queue_head_t gmni_idle_tx_wait;    /* block here for idle tx */
        int               gmni_tx_credits;      /* # transmits still possible */
        struct list_head  gmni_idle_ltxbs;      /* idle large tx buffers */
        struct list_head  gmni_buf_txq;         /* tx's waiting for buffers */
        struct list_head  gmni_cred_txq;        /* tx's waiting for credits */
        spinlock_t        gmni_tx_lock;         /* serialise */

        struct gm_hash   *gmni_rx_hash;		/* buffer->rx lookup */
        struct semaphore  gmni_rx_mutex;        /* serialise blocking on GM */
} gmnal_ni_t;

typedef struct {
        int              *gm_port;
        int              *gm_ntx;
        int              *gm_ntx_nblk;
        int              *gm_nlarge_tx_bufs;
        int              *gm_nrx_small;
        int              *gm_nrx_large;

#if CONFIG_SYSCTL && !CFS_SYSFS_MODULE_PARM
        struct ctl_table_header *gm_sysctl;    /* sysctl interface */
#endif
} gmnal_tunables_t;


/* gmnal_api.c */
int gmnal_init(void);
void gmnal_fini(void);
int gmnal_ctl(ptl_ni_t *ni, unsigned int cmd, void *arg);
int gmnal_startup(ptl_ni_t *ni);
void gmnal_shutdown(ptl_ni_t *ni);

/* gmnal_cb.c */
int gmnal_recv(ptl_ni_t *ni, void *private, ptl_msg_t *ptlmsg,
               unsigned int niov, struct iovec *iov, lnet_kiov_t *kiov,
               unsigned int offset, unsigned int mlen, unsigned int rlen);
int gmnal_send(ptl_ni_t *ni, void *private, ptl_msg_t *ptlmsg, 
               ptl_hdr_t *hdr, int type, lnet_process_id_t tgt, 
               int target_is_router, int routing,
               unsigned int niov, struct iovec *iov, lnet_kiov_t *kiov,
               unsigned int offset, unsigned int len);

/* gmnal_util.c */
void gmnal_free_ltxbufs(gmnal_ni_t *gmni);
int gmnal_alloc_ltxbufs(gmnal_ni_t *gmni);
void gmnal_free_txs(gmnal_ni_t *gmni);
int gmnal_alloc_txs(gmnal_ni_t *gmni);
void gmnal_free_rxs(gmnal_ni_t *gmni);
int gmnal_alloc_rxs(gmnal_ni_t *gmni);
char *gmnal_gmstatus2str(gm_status_t status);
char *gmnal_rxevent2str(gm_recv_event_t *ev);
void gmnal_yield(int delay);

void gmnal_copy_tofrom_netbuf(int niov, struct iovec *iov, lnet_kiov_t *kiov, int offset, 
                              int nb_pages, gmnal_netbuf_t *nb, int nb_offset,
                              int nob, int from_nb);

static inline void
gmnal_copy_from_netbuf(int niov, struct iovec *iov, lnet_kiov_t *kiov, int offset, 
                       int nb_pages, gmnal_netbuf_t *nb, int nb_offset, int nob)
{
        gmnal_copy_tofrom_netbuf(niov, iov, kiov, offset,
                                 nb_pages, nb, nb_offset, nob, 1);
}

static inline void
gmnal_copy_to_netbuf(int nb_pages, gmnal_netbuf_t *nb, int nb_offset,
                     int niov, struct iovec *iov, lnet_kiov_t *kiov, int offset, 
                     int nob)
{
        gmnal_copy_tofrom_netbuf(niov, iov, kiov, offset,
                                 nb_pages, nb, nb_offset, nob, 0);
}

/* gmnal_comm.c */
void gmnal_post_rx(gmnal_ni_t *gmni, gmnal_rx_t *rx);
gmnal_tx_t *gmnal_get_tx(gmnal_ni_t *gmni, int may_block);
void gmnal_tx_done(gmnal_tx_t *tx, int rc);
void gmnal_pack_msg(gmnal_ni_t *gmni, gmnal_msg_t *msg,
                    lnet_nid_t dstnid, int type);
void gmnal_stop_threads(gmnal_ni_t *gmni);
int gmnal_start_threads(gmnal_ni_t *gmni);
void gmnal_check_txqueues_locked (gmnal_ni_t *gmni);

/* Module Parameters */
extern gmnal_tunables_t gmnal_tunables;

#endif /*__INCLUDE_GMNAL_H__*/
