/****************************************************************
 *								*
 *	Copyright 2001, 2007 Fidelity Information Services, Inc	*
 *								*
 *	This source code contains the intellectual property	*
 *	of its copyright holder(s), and is made available	*
 *	under a license.  If you do not know the terms of	*
 *	the license, please stop and do not read further.	*
 *								*
 ****************************************************************/

#include "mdef.h"

#ifdef VMS
#include <ssdef.h>
#include <psldef.h>
#include <descrip.h>
#endif

#include "gtm_inet.h"
#include "gtm_string.h"

#include "gdsroot.h"
#include "gtm_facility.h"
#include "fileinfo.h"
#include "gdsbt.h"
#include "gdsblk.h"
#include "gdsfhead.h"
#include "filestruct.h"
#include "gdscc.h"
#include "min_max.h"
#include "gdsblkops.h"
#include "gdsbml.h"
#include "gdskill.h"
#include "copy.h"
#ifdef VMS
#include "lockconst.h"
#endif
#include "interlock.h"
#include "jnl.h"
#include "probe.h"
#include "buddy_list.h"		/* needed for tp.h */
#include "hashtab_int4.h"	/* needed for tp.h */
#include "tp.h"
#include "io.h"
#include "gtmsecshr.h"
#include "repl_msg.h"
#include "gtmsource.h"
#include "is_proc_alive.h"
#include "aswp.h"
#include "util.h"
#include "compswap.h"
#ifdef UNIX
#include "mutex.h"
#include "repl_instance.h"	/* needed for JNLDATA_BASE_OFF macro */
#endif
#include "sec_shr_blk_build.h"
#include "sec_shr_map_build.h"
#include "add_inter.h"
#include "send_msg.h"	/* for send_msg prototype */
#include "secshr_db_clnup.h"
#include "gdsbgtr.h"
#include "memcoherency.h"
#include "shmpool.h"

/* This section documents DOs and DONTs about code used by GTMSECSHR on Alpha VMS. Any module linked into GTMSECSHR (see
 * secshrlink.axp for the current list) must follow certain rules as GTMSECSHR provides user-defined system services
 * (privileged image that runs in kernel mode). See "Creating User Written System Sevice" chapter of the "Programming Concepts"
 * OpenVMS manual and the "Shareable Images Cookbook" available from the OpenVMS Wizard's page. SYS$EXAMPLES:uwss*.* is also a
 * good reference.
 *
 ** DO NOT use modulo (%) operation. If % is used, GTMSECSHR links with LIBOTS.EXE - an external shared image. This will result
 *  in "-SYSTEM-F-NOSHRIMG, privileged shareable image cannot have outbound calls" errors when GTMSECSHR is invoked. We might as
 *  well avoid division too.
 *
 ** The only library/system calls allowed are SYS$ calls.
 *
 ** No I/O allowed - any device, including operator console.
 *
 ** Always PROBE memory before accessing it. If not, should SECSHR access invalid memory (out of bounds for instance) the machine
 *  will crash (BUGCHECK in VMS parlance). Remember, SECSHR is running in kernel mode!
 *
 ** Both secshr_db_clnup.c and sec_shr_blk_build.c are compiled with /prefix=except=memmove. If any of the other modules used
 *  memmove, they would need special treatment as well.
 */

#define FLUSH 1

#define	WCBLOCKED_WBUF_DQD_LIT	"wcb_secshr_db_clnup_wbuf_dqd"
#define	WCBLOCKED_NOW_CRIT_LIT	"wcb_secshr_db_clnup_now_crit"

/* IMPORTANT : SECSHR_PROBE_REGION sets csa */
#define	SECSHR_PROBE_REGION(reg)									\
	if (!GTM_PROBE(sizeof(gd_region), (reg), READ))							\
		continue; /* would be nice to notify the world of a problem but where and how?? */	\
	if (!reg->open || reg->was_open)								\
		continue;										\
	if (!GTM_PROBE(sizeof(gd_segment), (reg)->dyn.addr, READ))					\
		continue; /* would be nice to notify the world of a problem but where and how? */	\
	if ((dba_bg != (reg)->dyn.addr->acc_meth) && (dba_mm != (reg)->dyn.addr->acc_meth))		\
		continue;										\
	if (!GTM_PROBE(sizeof(file_control), (reg)->dyn.addr->file_cntl, READ))				\
		continue; /* would be nice to notify the world of a problem but where and how? */	\
	if (!GTM_PROBE(sizeof(GDS_INFO), (reg)->dyn.addr->file_cntl->file_info, READ))			\
		continue; /* would be nice to notify the world of a problem but where and how? */	\
	csa = &(FILE_INFO((reg)))->s_addrs;								\
	if (!GTM_PROBE(sizeof(sgmnt_addrs), csa, WRITE))						\
		continue; /* would be nice to notify the world of a problem but where and how? */	\
	assert(reg->read_only && !csa->read_write || !reg->read_only && csa->read_write);

#ifdef DEBUG_CHECK_LATCH
#  define DEBUG_LATCH(x) x
#else
#  define DEBUG_LATCH(x)
#endif

#ifdef VMS
/* Use compswap_secshr instead of compswap in our expansions */
#  define compswap compswap_secshr
#  define CHECK_UNIX_LATCH(X, is_exiting)
#else
#  define CHECK_UNIX_LATCH(X, is_exiting) 	CHECK_LATCH(X, is_exiting)
GBLREF pid_t	process_id;	/* Used in xxx_SWAPLOCK macros .. has same value as rundown_process_id on UNIX */
#endif

#define CHECK_LATCH(X, is_exiting)								\
{												\
	uint4 pid;										\
									                        \
	if ((pid = (X)->u.parts.latch_pid) == rundown_process_id)				\
	{											\
		if (is_exiting)									\
		{										\
			SET_LATCH_GLOBAL(X, LOCK_AVAILABLE);					\
			DEBUG_LATCH(util_out_print("Latch cleaned up", FLUSH));			\
		}										\
	} else if (0 != pid && FALSE == is_proc_alive(pid, UNIX_ONLY(0) VMS_ONLY((X)->u.parts.latch_image_count)))	\
	{											\
		  DEBUG_LATCH(util_out_print("Orphaned latch cleaned up", TRUE));		\
		  COMPSWAP_UNLOCK((X), pid, (X)->u.parts.latch_image_count, LOCK_AVAILABLE, 0);	\
	}											\
}

GBLDEF gd_addr		*(*get_next_gdr_addrs)();
GBLDEF cw_set_element	*cw_set_addrs;
GBLDEF sgm_info		**first_sgm_info_addrs;
GBLDEF sgm_info		**first_tp_si_by_ftok_addrs;
GBLDEF unsigned char	*cw_depth_addrs;
GBLDEF uint4		rundown_process_id;
GBLDEF uint4		rundown_image_count;
GBLDEF int4		rundown_os_page_size;
GBLDEF gd_region	**jnlpool_reg_addrs;
GBLDEF inctn_opcode_t	*inctn_opcode_addrs;
GBLDEF inctn_detail_t	*inctn_detail_addrs;
GBLDEF short		*dollar_tlevel_addrs;
GBLDEF int4		*update_trans_addrs;
GBLDEF sgmnt_addrs	**cs_addrs_addrs;
GBLDEF boolean_t 	*kip_incremented_addrs;
GBLDEF boolean_t	*need_kip_incr_addrs;

#ifdef UNIX
GBLREF	short			crash_count;
GBLREF	node_local_ptr_t	locknl;
GBLREF	inctn_opcode_t		inctn_opcode;
GBLREF	boolean_t		dse_running;
#endif

typedef enum
{
	REG_COMMIT_UNSTARTED = 0,/* indicates that GT.M has not committed even one cse in this region */
	REG_COMMIT_PARTIAL,	 /* indicates that GT.M has committed at least one but not all cses for this region */
	REG_COMMIT_COMPLETE	 /* indicates that GT.M has already committed all cw-set-elements for this region */
} commit_type;

bool secshr_tp_get_cw(cw_set_element *cs, int depth, cw_set_element **cs1);

void secshr_db_clnup(enum secshr_db_state secshr_state)
{
	unsigned char		*chain_ptr;
	char			*wcblocked_ptr;
	boolean_t		is_bg, jnlpool_reg, do_accounting, first_time = TRUE, is_exiting;
	boolean_t		dlr_tlevel, kipincremented_usable, needkipincr;
	int4			upd_trans; /* a copy of the global variable "update_trans" which is needed for VMS STOP/ID case */
	boolean_t		tp_update_underway = FALSE;	/* set to TRUE if TP commit was in progress or complete */
	boolean_t		non_tp_update_underway = FALSE;	/* set to TRUE if non-TP commit was in progress or complete */
	boolean_t		update_underway = FALSE;	/* set to TRUE if either TP or non-TP commit was underway */
	int			max_bts;
	unsigned int		lcnt;
	cache_rec_ptr_t		clru, cr, cr_alt, cr_top, start_cr;
	cw_set_element		*cs, *cs_ptr, *cs_top, *first_cw_set, *nxt, *orig_cs;
	gd_addr			*gd_header;
	gd_region		*reg, *reg_top;
	jnl_buffer_ptr_t	jbp;
	off_chain		chain;
	sgm_info		*si;
	sgmnt_addrs		*csa, *csaddrs;
	sgmnt_data_ptr_t	csd;
	sm_uc_ptr_t		blk_ptr;
	jnlpool_ctl_ptr_t	jpl;
	jnldata_hdr_ptr_t	jh;
	uint4			cumul_jnl_rec_len, jsize, new_write, imgcnt;
	pid_t			pid;
	sm_uc_ptr_t		bufstart;
	int4			bufindx;	/* should be the same type as "csd->bt_buckets" */
	commit_type		this_reg_commit_type;	/* indicate the type of commit of a given region in a TP transaction */

	error_def(ERR_WCBLOCKED);

	if (NULL == get_next_gdr_addrs)
		return;
	/*
	 * secshr_db_clnup can be called with one of the following three values for "secshr_state"
	 *
	 * 	a) NORMAL_TERMINATION   --> We are called from the exit-handler for precautionary cleanup.
	 * 				    We should NEVER be in the midst of a database update in this case.
	 * 	b) COMMIT_INCOMPLETE    --> We are called from t_commit_cleanup.
	 * 				    We should ALWAYS be in the midst of a database update in this case.
	 * 	c) ABNORMAL_TERMINATION --> This is currently VMS ONLY. This process received a STOP/ID.
	 * 				    We can POSSIBLY be in the midst of a database update in this case.
	 * 				    When UNIX boxes allow kernel extensions, this can be made to handle "kill -9" too.
	 *
	 * If we are in the midst of a database update, then depending on the stage of the commit we are in,
	 * 	we need to ROLL-BACK (undo the partial commit) or ROLL-FORWARD (complete the partial commit) the database update.
	 *
	 * t_commit_cleanup handles the ROLL-BACK and secshr_db_clnup handles the ROLL-FORWARD
	 *
	 * For all error conditions in the database commit logic, t_commit_cleanup gets control first.
	 * If then determines whether to do a ROLL-BACK or a ROLL-FORWARD.
	 * If a ROLL-BACK needs to be done, then t_commit_cleanup handles it all by itself and we will not come here.
	 * If a ROLL-FORWARD needs to be done, then t_commit_cleanup invokes secshr_db_clnup.
	 * 	In this case, secshr_db_clnup will be called with a "secshr_state" value of "COMMIT_INCOMPLETE".
	 *
	 * In case of a STOP/ID in VMS, secshr_db_clnup is directly invoked with a "secshr_state" value of "ABNORMAL_TERMINATION".
	 * Irrespective of whether we are in the midst of a database commit or not, t_commit_cleanup does not get control.
	 * Since the process can POSSIBLY be in the midst of a database update while it was STOP/IDed,
	 * 	the logic for determining whether it is a ROLL-BACK or a ROLL-FORWARD needs to also be in secshr_db_clnup.
	 * If it is determined that a ROLL-FORWARD needs to be done, secshr_db_clnup takes care of it by itself.
	 * But if a ROLL-BACK needs to be done, then secshr_db_clnup DOES NOT invoke t_commit_cleanup.
	 * Instead it sets csd->wc_blocked to TRUE thereby ensuring the next process that gets CRIT does a cache recovery
	 * 	which will take care of doing more than the ROLL-BACK that t_commit_cleanup would have otherwise done.
	 *
	 * The logic for determining if it is a ROLL-BACK or ROLL-FORWARD is explained below.
	 * The commit logic flow in tp_tend and t_end can be captured as follows. Note that in t_end there is only one region.
	 *
	 *  1) Get crit on all regions
	 *  2) Get crit on jnlpool
	 *  3) jnlpool_ctl->early_write_addr += delta;
	 *       For each participating region being UPDATED
	 *       {
	 *  4)     csd->trans_hist.early_tn++;
	 *         Write journal records
	 *  5)     csa->hdr->reg_seqno = jnlpool_ctl->jnl_seqno + 1;
	 *       }
	 *       For each participating region being UPDATED
	 *       {
	 *  6)	    csa->t_commit_crit = TRUE;
	 *             For every cw-set-element of this region
	 *             {
	 *               Commit this particular block.
	 *               cs->mode = gds_t_committed;
	 *             }
	 *  7)       csa->t_commit_crit = FALSE;
	 *  8)     csd->trans_hist.curr_tn++;
	 *       }
	 *  9) jnlpool_ctl->write_addr = jnlpool_ctl->early_write_addr;
	 * 10) jnlpool_ctl->jnl_seqno++;
	 * 11) Release crit on all db regions
	 * 12) Release crit on jnlpool
	 *
	 * If a TP transaction has proceeded to step (6) for at least one region, then "tp_update_underway" is set to TRUE
	 * and the transaction cannot be rolled back but has to be committed. Otherwise the transaction is rolled back.
	 *
	 * If a non-TP transaction has proceeded to step (6), then "non_tp_update_underway" is set to TRUE
	 * and the transaction cannot be rolled back but has to be committed. Otherwise the transaction is rolled back.
	 */
	is_exiting = (ABNORMAL_TERMINATION == secshr_state) || (NORMAL_TERMINATION == secshr_state);
	dlr_tlevel = FALSE;
	if (GTM_PROBE(sizeof(*dollar_tlevel_addrs), dollar_tlevel_addrs, READ))
		dlr_tlevel = *dollar_tlevel_addrs;
	if (dlr_tlevel && GTM_PROBE(sizeof(*first_tp_si_by_ftok_addrs), first_tp_si_by_ftok_addrs, READ))
	{	/* Determine update_underway for TP transaction. A similar check is done in t_commit_cleanup as well.
		 * Regions are committed in the ftok order using "first_tp_si_by_ftok". Also crit is released on each region
		 * as the commit completes. Take that into account while determining if update is underway.
		 */
		for (si = *first_tp_si_by_ftok_addrs; NULL != si; si = si->next_sgm_info)
		{
			if (GTM_PROBE(sizeof(sgm_info), si, READ))
			{
				assert(GTM_PROBE(sizeof(cw_set_element), si->first_cw_set, READ) || (NULL == si->first_cw_set));
				if (T_COMMIT_STARTED == si->update_trans)
				{	/* Two possibilities.
					 *	(a) case of duplicate set not creating any cw-sets but updating db curr_tn++.
					 *	(b) Have completed commit for this region and have released crit on this region.
					 *		(in a potentially multi-region TP transaction).
					 * In either case, update is underway and the transaction cannot be rolled back.
					 */
					tp_update_underway = TRUE;
					update_underway = TRUE;
					break;
				}
				if (GTM_PROBE(sizeof(cw_set_element), si->first_cw_set, READ))
				{	/* Note that SECSHR_PROBE_REGION does a "continue" if any probes fail. */
					SECSHR_PROBE_REGION(si->gv_cur_region);	/* sets csa */
					/* Assert that if we are in the midst of commit in a region, we better hold crit */
					assert(!csa->t_commit_crit || csa->now_crit);
					/* Just to be safe, set update_underway to TRUE only if we have crit on this region. */
					if (csa->now_crit && csa->t_commit_crit)
					{
						tp_update_underway = TRUE;
						update_underway = TRUE;
						break;
					}
				}
			} else
			{
				assert(FALSE);
				break;
			}
		}
	}
	if (!dlr_tlevel)
	{	/* determine update_underway for non-TP transaction */
		upd_trans = FALSE;
		if (GTM_PROBE(sizeof(*update_trans_addrs), update_trans_addrs, READ))
			upd_trans = *update_trans_addrs;
		csaddrs = NULL;
		if (GTM_PROBE(sizeof(*cs_addrs_addrs), cs_addrs_addrs, READ))
			csaddrs = *cs_addrs_addrs;
		if (GTM_PROBE(sizeof(sgmnt_addrs), csaddrs, READ))
		{
			if (csaddrs->now_crit && (csaddrs->t_commit_crit || (T_COMMIT_STARTED == upd_trans)))
			{
				non_tp_update_underway = TRUE;	/* non-tp update was underway */
				update_underway = TRUE;
			}
		}
	}
	/* Assert that if we had been called from t_commit_cleanup, we independently concluded that update is underway
	 * (as otherwise t_commit_cleanup would not have called us)
	 */
	assert((COMMIT_INCOMPLETE != secshr_state) || update_underway);
	for (gd_header = (*get_next_gdr_addrs)(NULL);  NULL != gd_header;  gd_header = (*get_next_gdr_addrs)(gd_header))
	{
		if (!GTM_PROBE(sizeof(gd_addr), gd_header, READ))
			break;	/* if gd_header is accessible */
		for (reg = gd_header->regions, reg_top = reg + gd_header->n_regions;  reg < reg_top;  reg++)
		{
			SECSHR_PROBE_REGION(reg);	/* SECSHR_PROBE_REGION sets csa */
			csd = csa->hdr;
			if (!GTM_PROBE(sizeof(sgmnt_data), csd, WRITE))
			{
				assert(FALSE);
				continue; /* would be nice to notify the world of a problem but where and how? */
			}
			if (!GTM_PROBE(NODE_LOCAL_SIZE_DBS, csa->nl, WRITE))
			{
				assert(FALSE);
				continue; /* would be nice to notify the world of a problem but where and how? */
			}
			is_bg = (csd->acc_meth == dba_bg);
			do_accounting = FALSE;	/* used by SECSHR_ACCOUNTING macro */
			/* do SECSHR_ACCOUNTING only if holding crit (to avoid another process' normal termination call
			 * to secshr_db_clnup from overwriting whatever important information we wrote. if we are in
			 * crit, for the next process to overwrite us it needs to get crit which in turn will invoke
			 * wcs_recover which in turn will send whatever we wrote to the operator log).
			 * also cannot update csd if MM and read-only. take care of that too. */
			if (csa->now_crit && (csa->read_write || is_bg))
			{	/* start accounting */
				csa->nl->secshr_ops_index = 0;
				do_accounting = TRUE;	/* used by SECSHR_ACCOUNTING macro */
			}
			assert(rundown_process_id);
			SECSHR_ACCOUNTING(4);	/* 4 is the number of arguments following including self */
			SECSHR_ACCOUNTING(__LINE__);
			SECSHR_ACCOUNTING(rundown_process_id);
			SECSHR_ACCOUNTING(secshr_state);
			if (csa->ti != &csd->trans_hist)
			{
				SECSHR_ACCOUNTING(4);
				SECSHR_ACCOUNTING(__LINE__);
				SECSHR_ACCOUNTING(csa->ti);
				SECSHR_ACCOUNTING(&csd->trans_hist);
				csa->ti = &csd->trans_hist;	/* better to correct and proceed than to stop */
			}
			SECSHR_ACCOUNTING(3);	/* 3 is the number of arguments following including self */
			SECSHR_ACCOUNTING(__LINE__);
			SECSHR_ACCOUNTING(csd->trans_hist.curr_tn);
			if (GTM_PROBE(NODE_LOCAL_SIZE_DBS, csa->nl, WRITE) && is_exiting)
			{
				/* If we hold any latches in the node_local area, release them. Note we do not check
				   db_latch here because it is never used by the compare and swap logic but rather
				   the aswp logic. Since it is only used for the 3 state cache record lock and
				   separate recovery exists for it, we do not do anything with it here.
				*/
				CHECK_UNIX_LATCH(&csa->nl->wc_var_lock, is_exiting);
				if (ABNORMAL_TERMINATION == secshr_state)
				{
					if (csa->timer)
					{
						if (-1 < csa->nl->wcs_timers) /* private flag is optimistic: dont overdo */
							CAREFUL_DECR_CNT(&csa->nl->wcs_timers, &csa->nl->wc_var_lock);
						csa->timer = FALSE;
					}
					if (csa->read_write && csa->ref_cnt)
					{
						assert(0 < csa->nl->ref_cnt);
						csa->ref_cnt--;
						assert(!csa->ref_cnt);
						CAREFUL_DECR_CNT(&csa->nl->ref_cnt, &csa->nl->wc_var_lock);
					}
				}
				if ((csa->in_wtstart) && (0 < csa->nl->in_wtstart))
					CAREFUL_DECR_CNT(&csa->nl->in_wtstart, &csa->nl->wc_var_lock);
				csa->in_wtstart = FALSE;	/* Let wcs_wtstart run for exit processing */
				if (csa->nl->wcsflu_pid == rundown_process_id)
					csa->nl->wcsflu_pid = 0;
			}
			if (is_bg)
			{
				if ((0 == reg->sec_size) || !GTM_PROBE(reg->sec_size VMS_ONLY(* OS_PAGELET_SIZE), csa->nl, WRITE))
				{
					SECSHR_ACCOUNTING(3);
					SECSHR_ACCOUNTING(__LINE__);
					SECSHR_ACCOUNTING(reg->sec_size VMS_ONLY(* OS_PAGELET_SIZE));
					assert(FALSE);
					continue;
				}
				CHECK_UNIX_LATCH(&csa->acc_meth.bg.cache_state->cacheq_active.latch, is_exiting);
				start_cr = csa->acc_meth.bg.cache_state->cache_array + csd->bt_buckets;
				max_bts = csd->n_bts;
				if (!GTM_PROBE((uint4)(max_bts * sizeof(cache_rec)), start_cr, WRITE))
				{
					SECSHR_ACCOUNTING(3);
					SECSHR_ACCOUNTING(__LINE__);
					SECSHR_ACCOUNTING(start_cr);
					assert(FALSE);
					continue;
				}
				cr_top = start_cr + max_bts;
				if (is_exiting)
				{
					for (cr = start_cr;  cr < cr_top;  cr++)
					{	/* walk the cache looking for incomplete writes and reads issued by self */
						VMS_ONLY(
							if ((0 == cr->iosb.cond) && (cr->epid == rundown_process_id))
						        {
								cr->shmpool_blk_off = 0;	/* Cut link to reformat blk */
								cr->wip_stopped = TRUE;
							}
						)
						CHECK_UNIX_LATCH(&cr->rip_latch, is_exiting);
						if ((cr->r_epid == rundown_process_id) && (0 == cr->dirty)
								&& (FALSE == cr->in_cw_set))
						{	/* increment cycle for blk number changes (for tp_hist) */
							cr->cycle++;
							cr->blk = CR_BLKEMPTY;
							/* ensure no bt points to this cr for empty blk */
							assert(0 == cr->bt_index);
							/* don't mess with ownership the I/O may not yet be cancelled;
							 * ownership will be cleared by whoever gets stuck waiting
							 * for the buffer */
						}
					}
				}
			}
			first_cw_set = cs = NULL;
			/* If tp_update_underway has been determined to be TRUE, then we are guaranteed we have a well formed
			 * ftok ordered linked list ("first_tp_si_by_ftok") so we can safely use this.
			 */
			if (tp_update_underway)
			{	/* this is constructed to deal with the issue of reg != si->gv_cur_region
				 * due to the possibility of multiple global directories pointing to regions
				 * that resolve to the same physical file; was_open prevents processing the segment
				 * more than once, so this code matches on the file rather than the region to make sure
				 * that it gets processed at least once */
				for (si = *first_tp_si_by_ftok_addrs; NULL != si; si = si->next_tp_si_by_ftok)
				{
					if (!GTM_PROBE(sizeof(sgm_info), si, READ))
					{
						SECSHR_ACCOUNTING(3);
						SECSHR_ACCOUNTING(__LINE__);
						SECSHR_ACCOUNTING(si);
						assert(FALSE);
						break;
					} else if (!GTM_PROBE(sizeof(gd_region), si->gv_cur_region, READ))
					{
						SECSHR_ACCOUNTING(3);
						SECSHR_ACCOUNTING(__LINE__);
						SECSHR_ACCOUNTING(si->gv_cur_region);
						assert(FALSE);
						continue;
					} else if (!GTM_PROBE(sizeof(gd_segment), si->gv_cur_region->dyn.addr, READ))
					{
						SECSHR_ACCOUNTING(3);
						SECSHR_ACCOUNTING(__LINE__);
						SECSHR_ACCOUNTING(si->gv_cur_region->dyn.addr);
						assert(FALSE);
						continue;
					} else if (si->gv_cur_region->dyn.addr->file_cntl == reg->dyn.addr->file_cntl)
					{
						cs = si->first_cw_set;
						if (cs && GTM_PROBE(sizeof(cw_set_element), cs, READ))
						{
							while (cs->high_tlevel)
							{
								if (GTM_PROBE(sizeof(cw_set_element),
											cs->high_tlevel, READ))
									cs = cs->high_tlevel;
								else
								{
									SECSHR_ACCOUNTING(3);
									SECSHR_ACCOUNTING(__LINE__);
									SECSHR_ACCOUNTING(cs->high_tlevel);
									assert(FALSE);
									first_cw_set = cs = NULL;
									break;
								}
							}
						}
						first_cw_set = cs;
						break;
					}
				}
			} else if (!dlr_tlevel && csa->t_commit_crit)
			{	/* We better have held crit on this region. GTMASSERT only in Unix as this module runs
				 * in kernel mode in VMS and no IO is allowed in that mode.
				 */
				if (!csa->now_crit)
				{
					UNIX_ONLY(GTMASSERT;)
					VMS_ONLY(assert(FALSE);)
				}
				if (!GTM_PROBE(sizeof(unsigned char), cw_depth_addrs, READ))
				{
					SECSHR_ACCOUNTING(3);
					SECSHR_ACCOUNTING(__LINE__);
					SECSHR_ACCOUNTING(cw_depth_addrs);
					assert(FALSE);
				} else
				{	/* csa->t_commit_crit being TRUE is a clear cut indication that we have
					 * reached stage (6). ROLL-FORWARD the commit unconditionally.
					 */
					if (0 != *cw_depth_addrs)
					{
						first_cw_set = cs = cw_set_addrs;
						cs_top = cs + *cw_depth_addrs;
					}
					/* else is the case where we had a duplicate set that did not update any cw-set */
					assert(!tp_update_underway);
					assert(non_tp_update_underway);	/* should have already determined update is underway */
					if (!non_tp_update_underway)
					{	/* This is a situation where we are in non-TP and have a region that we hold
						 * crit in and are in the midst of commit but this region was not the current
						 * region when we entered secshr_db_clnup. This is an out-of-design situation
						 * that we want to catch in Unix (not VMS because it runs in kernel mode).
						 */
						UNIX_ONLY(GTMASSERT;)	/* in Unix we want to catch this situation even in pro */
					}
					non_tp_update_underway = TRUE;	/* just in case */
					update_underway = TRUE;		/* just in case */
				}
			}
			assert(!tp_update_underway || (NULL == first_cw_set) || (NULL != si));
			/* It is possible that we were in the midst of a non-TP commit for this region at or past stage (7),
			 * with csa->t_commit_crit set to FALSE. It is a case of duplicate SET with zero cw_set_depth.
			 * In this case, dont have any cw-set-elements to commit. The only thing remaining to do is
			 * steps (9) through (12) which are done later in this function.
			 *
			 * Note that it is possible at this point we dont have CRIT on this region if in TP
			 * (e.g. if the TP involves two regions and we have committed and released crit on one region
			 * and encounter an error while committing the second region). In this case, skip processing on this region.
			 */
			assert((NULL == first_cw_set) || csa->now_crit || tp_update_underway);
			if (csa->now_crit && (NULL != first_cw_set))
			{
				assert(non_tp_update_underway || tp_update_underway);
				assert(!non_tp_update_underway || !tp_update_underway);
				if (is_bg)
				{
					clru = (cache_rec_ptr_t)GDS_ANY_REL2ABS(csa, csa->nl->cur_lru_cache_rec_off);
					lcnt = 0;
				}
				if (csa->t_commit_crit)
				{
					assert(NORMAL_TERMINATION != secshr_state); /* for normal termination we should not
										     * have been in the midst of commit */
					csd->trans_hist.free_blocks = csa->prev_free_blks;
				}
				SECSHR_ACCOUNTING(tp_update_underway ? 6 : 7);
				SECSHR_ACCOUNTING(__LINE__);
				SECSHR_ACCOUNTING(first_cw_set);
				SECSHR_ACCOUNTING(tp_update_underway);
				SECSHR_ACCOUNTING(non_tp_update_underway);
				if (!tp_update_underway)
				{
					SECSHR_ACCOUNTING(cs_top);
					SECSHR_ACCOUNTING(*cw_depth_addrs);
				} else
				{
					SECSHR_ACCOUNTING(si->cw_set_depth);
					this_reg_commit_type = REG_COMMIT_UNSTARTED; /* assume GT.M did no commits in this region */
					/* Note that "this_reg_commit_type" is uninitialized if "tp_update_underway" is not TRUE
					 * so should always be used within an "if (tp_update_underway)" */
				}
				for (; (tp_update_underway  &&  NULL != cs) || (!tp_update_underway  &&  cs < cs_top);
					cs = tp_update_underway ? orig_cs->next_cw_set : (cs + 1))
				{
					if (tp_update_underway)
					{
						orig_cs = cs;
						if (cs && GTM_PROBE(sizeof(cw_set_element), cs, READ))
						{
							while (cs->high_tlevel)
							{
								if (GTM_PROBE(sizeof(cw_set_element),
											cs->high_tlevel, READ))
									cs = cs->high_tlevel;
								else
								{
									SECSHR_ACCOUNTING(3);
									SECSHR_ACCOUNTING(__LINE__);
									SECSHR_ACCOUNTING(cs->high_tlevel);
									assert(FALSE);
									cs = NULL;
									break;
								}
							}
						}
					}
					if (!GTM_PROBE(sizeof(cw_set_element), cs, READ))
					{
						SECSHR_ACCOUNTING(3);
						SECSHR_ACCOUNTING(__LINE__);
						SECSHR_ACCOUNTING(cs);
						assert(FALSE);
						break;
					}
					if ((gds_t_committed < cs->mode) && (n_gds_t_op > cs->mode))
					{	/* Currently there are only two possibilities and both are only possible in NON-TP.
						 * In either case, no need to do any block update so simulate commit.
						 */
						assert(!tp_update_underway);
						assert((gds_t_write_root == cs->mode) || (gds_t_busy2free == cs->mode));
						cs->old_mode = cs->mode;
						cs->mode = gds_t_committed;
						continue;
					}
					if (gds_t_committed == cs->mode)
					{	/* already processed */
						if (csa->t_commit_crit)
							csd->trans_hist.free_blocks -= cs->reference_cnt;
						if (tp_update_underway)
						{	/* We have seen at least one already-committed cse. Assume GT.M
							 * has committed ALL cses. This will be later overridden if we see
							 * an uncommitted cse in this region.
							 */
							assert(REG_COMMIT_PARTIAL != this_reg_commit_type);
							this_reg_commit_type = REG_COMMIT_COMPLETE;
						}
						cr = cs->cr;
						assert(!dlr_tlevel || (gds_t_write_root != cs->old_mode));
						assert(gds_t_committed != cs->old_mode);
						if (gds_t_committed > cs->old_mode)
						{
							if (!GTM_PROBE(sizeof(cache_rec), cr, WRITE))
							{
								SECSHR_ACCOUNTING(4);
								SECSHR_ACCOUNTING(__LINE__);
								SECSHR_ACCOUNTING(cs);
								SECSHR_ACCOUNTING(cr);
								assert(FALSE);
							} else if (cr->in_tend)
							{	/* We should have been shot (STOP/ID) in the window of two
								 * statements in bg_update immediately after cs->mode got set to
								 * gds_t_committed but just before cr->in_tend got reset to FALSE.
								 * Complete the cr->in_tend reset and skip processing this cs
								 */
								UNIX_ONLY(assert(FALSE);)
								assert(ABNORMAL_TERMINATION == secshr_state);
								cr->in_tend = FALSE;
							}
						} else
						{	/* For the kill_t_* case, cs->cr will be NULL as bg_update was not invoked
							 * and the cw-set-elements were memset to 0 in TP. But for gds_t_write_root
							 * and gds_t_busy2free, they are non-TP ONLY modes and cses are not
							 * initialized so cant check for NULL cr. Thankfully "n_gds_t_op" demarcates
							 * the boundaries between non-TP only and TP only modes. So use that.
							 */
							assert((n_gds_t_op > cs->old_mode) || (NULL == cr));
						}
						continue;
					}
					assert(NORMAL_TERMINATION != secshr_state); /* for normal termination we should not
										     * have been in the midst of commit */
					if (tp_update_underway && (REG_COMMIT_COMPLETE == this_reg_commit_type))
					{	/* We have seen at least one committed cse in this region. Since the current
						 * cse has not been committed, this is a partial GT.M commit in this region.
						 */
						this_reg_commit_type = REG_COMMIT_PARTIAL;
					}
					if (is_bg)
					{
						for ( ; lcnt++ < max_bts; )
						{	/* find any available cr */
							if (clru++ >= cr_top)
								clru = start_cr;
							assert(!clru->stopped);
							if (!clru->stopped && (0 == clru->dirty) && (FALSE == clru->in_cw_set)
								&& (-1 == clru->read_in_progress)
								&& GTM_PROBE(csd->blk_size,
									GDS_ANY_REL2ABS(csa, clru->buffaddr), WRITE))
								break;
						}
						if (lcnt >= max_bts)
						{
							SECSHR_ACCOUNTING(9);
							SECSHR_ACCOUNTING(__LINE__);
							SECSHR_ACCOUNTING(cs);
							SECSHR_ACCOUNTING(cs->blk);
							SECSHR_ACCOUNTING(cs->tn);
							SECSHR_ACCOUNTING(cs->level);
							SECSHR_ACCOUNTING(cs->done);
							SECSHR_ACCOUNTING(cs->forward_process);
							SECSHR_ACCOUNTING(cs->first_copy);
							assert(FALSE);
							continue;
						}
						cr = clru;
						cr->cycle++;	/* increment cycle for blk number changes (for tp_hist) */
						cr->blk = cs->blk;
						cr->jnl_addr = cs->jnl_freeaddr;
						cr->stopped = TRUE;
						/* Check if online backup is in progress and if there is a before-image to write.
						 * If so need to store link to it so wcs_recover can back it up later. Cannot
						 * rely on precomputed value csa->backup_in_prog since it is not initialized
						 * if (cw_depth == 0) (see t_end.c). Hence using csa->nl->nbb expicitly in check.
						 */
						if ((BACKUP_NOT_IN_PROGRESS != csa->nl->nbb) && (NULL != cs->old_block))
						{	/* Set "cr->twin" to point to "cs->old_block". This is not normal usage
							 * since "twin" usually points to a cache-record. But this is a special
							 * case where we want to record the before-image somewhere for wcs_recover
							 * to see and we are not allowed division operations in secshr_db_clnup
							 * (which is required to find out the corresponding cache-record). Hence
							 * we store the relative offset of "cs->old_block". This is a special
							 * case where "cr->twin" can be non-zero even in Unix. wcs_recover will
							 * recognize this special usage of "twin" (since cr->stopped is non-zero
							 * as well) and fix it. Note that in VMS, it is possible to have two other
							 * crs for the same block cr1, cr2 which are each twinned so we could end
							 * up with the following twin configuration.
							 *	cr1 <---> cr2 <--- cr
							 * Note cr->twin = cr2 is a one way link and stores "cs->old_block", while
							 * "cr1->twin" and "cr2->twin" store each other's cacherecord pointers.
							 */
							UNIX_ONLY(
								bufstart = (sm_uc_ptr_t)GDS_ANY_REL2ABS(csa, start_cr->buffaddr);
								bufindx = (cs->old_block - bufstart) / csd->blk_size;
								assert(0 <= bufindx);
								assert(bufindx < csd->n_bts);
								cr_alt = &start_cr[bufindx];
								assert(cr_alt != cr);
								assert(cs->blk == cr_alt->blk);
								assert(cr_alt->in_cw_set);
							)
							cr->twin = GDS_ANY_ABS2REL(csa, cs->old_block);
						}
						/* the following code is very similar to that in bg_update */
						if (gds_t_acquired == cs->mode)
						{
							cr->ondsk_blkver = csd->desired_db_format;
							if (GDSV4 == csd->desired_db_format)
							{
								INCR_BLKS_TO_UPGRD(csa, csd, 1);
							}
						} else
						{
							cr->ondsk_blkver = cs->ondsk_blkver;
							if (cr->ondsk_blkver != csd->desired_db_format)
							{
								cr->ondsk_blkver = csd->desired_db_format;
								if (GDSV4 == csd->desired_db_format)
								{
									if (gds_t_write_recycled != cs->mode)
										INCR_BLKS_TO_UPGRD(csa, csd, 1);
								} else
								{
									if (gds_t_write_recycled != cs->mode)
										DECR_BLKS_TO_UPGRD(csa, csd, 1);
								}
							}
						}
						blk_ptr = (sm_uc_ptr_t)GDS_ANY_REL2ABS(csa, cr->buffaddr);
					} else
					{	/* access method is MM */
						blk_ptr = (sm_uc_ptr_t)csa->acc_meth.mm.base_addr + csd->blk_size * cs->blk;
						if (!GTM_PROBE(csd->blk_size, blk_ptr, WRITE))
						{
							SECSHR_ACCOUNTING(7);
							SECSHR_ACCOUNTING(__LINE__);
							SECSHR_ACCOUNTING(cs);
							SECSHR_ACCOUNTING(cs->blk);
							SECSHR_ACCOUNTING(blk_ptr);
							SECSHR_ACCOUNTING(csd->blk_size);
							SECSHR_ACCOUNTING(csa->acc_meth.mm.base_addr);
							assert(FALSE);
							continue;
						}
					}
					if (cs->mode == gds_t_writemap)
					{
						if (!GTM_PROBE(csd->blk_size, cs->old_block, READ))
						{
							SECSHR_ACCOUNTING(11);
							SECSHR_ACCOUNTING(__LINE__);
							SECSHR_ACCOUNTING(cs);
							SECSHR_ACCOUNTING(cs->blk);
							SECSHR_ACCOUNTING(cs->tn);
							SECSHR_ACCOUNTING(cs->level);
							SECSHR_ACCOUNTING(cs->done);
							SECSHR_ACCOUNTING(cs->forward_process);
							SECSHR_ACCOUNTING(cs->first_copy);
							SECSHR_ACCOUNTING(cs->old_block);
							SECSHR_ACCOUNTING(csd->blk_size);
							assert(FALSE);
							continue;
						}
						memmove(blk_ptr, cs->old_block, csd->blk_size);
						/* Note that "sec_shr_map_build" modifies cs->reference_cnt. It is only after
						 * this that csd->trans_hist.free_blocks needs to be updated.
						 */
						if (FALSE == sec_shr_map_build(csa, (uint4*)cs->upd_addr, blk_ptr, cs,
							csd->trans_hist.curr_tn, BM_SIZE(csd->bplmap)))
						{
							SECSHR_ACCOUNTING(11);
							SECSHR_ACCOUNTING(__LINE__);
							SECSHR_ACCOUNTING(cs);
							SECSHR_ACCOUNTING(cs->blk);
							SECSHR_ACCOUNTING(cs->tn);
							SECSHR_ACCOUNTING(cs->level);
							SECSHR_ACCOUNTING(cs->done);
							SECSHR_ACCOUNTING(cs->forward_process);
							SECSHR_ACCOUNTING(cs->first_copy);
							SECSHR_ACCOUNTING(cs->upd_addr);
							SECSHR_ACCOUNTING(blk_ptr);
							assert(FALSE);
						}
					} else
					{
						if (!tp_update_underway)
						{
							if (FALSE == sec_shr_blk_build(csa, csd, is_bg,
										cs, blk_ptr, csd->trans_hist.curr_tn))
							{
								SECSHR_ACCOUNTING(10);
								SECSHR_ACCOUNTING(__LINE__);
								SECSHR_ACCOUNTING(cs);
								SECSHR_ACCOUNTING(cs->blk);
								SECSHR_ACCOUNTING(cs->level);
								SECSHR_ACCOUNTING(cs->done);
								SECSHR_ACCOUNTING(cs->forward_process);
								SECSHR_ACCOUNTING(cs->first_copy);
								SECSHR_ACCOUNTING(cs->upd_addr);
								SECSHR_ACCOUNTING(blk_ptr);
								assert(FALSE);
								continue;
							} else if (cs->ins_off)
							{
								if ((cs->ins_off >
									((blk_hdr *)blk_ptr)->bsiz - sizeof(block_id))
									|| (cs->ins_off < (sizeof(blk_hdr)
										+ sizeof(rec_hdr)))
									|| (0 > (short)cs->index)
									|| ((cs - cw_set_addrs) <= cs->index))
								{
									SECSHR_ACCOUNTING(7);
									SECSHR_ACCOUNTING(__LINE__);
									SECSHR_ACCOUNTING(cs);
									SECSHR_ACCOUNTING(cs->blk);
									SECSHR_ACCOUNTING(cs->index);
									SECSHR_ACCOUNTING(cs->ins_off);
									SECSHR_ACCOUNTING(((blk_hdr *)blk_ptr)->bsiz);
									assert(FALSE);
									continue;
								}
								PUT_LONG((blk_ptr + cs->ins_off),
								 ((cw_set_element *)(cw_set_addrs + cs->index))->blk);
								if (((nxt = cs + 1) < cs_top)
									&& (gds_t_write_root == nxt->mode))
								{
									if ((nxt->ins_off >
									     ((blk_hdr *)blk_ptr)->bsiz - sizeof(block_id))
										|| (nxt->ins_off < (sizeof(blk_hdr)
											 + sizeof(rec_hdr)))
										|| (0 > (short)nxt->index)
										|| ((cs - cw_set_addrs) <= nxt->index))
									{
										SECSHR_ACCOUNTING(7);
										SECSHR_ACCOUNTING(__LINE__);
										SECSHR_ACCOUNTING(nxt);
										SECSHR_ACCOUNTING(cs->blk);
										SECSHR_ACCOUNTING(nxt->index);
										SECSHR_ACCOUNTING(nxt->ins_off);
										SECSHR_ACCOUNTING(
											((blk_hdr *)blk_ptr)->bsiz);
										assert(FALSE);
										continue;
									}
									PUT_LONG((blk_ptr + nxt->ins_off),
										 ((cw_set_element *)
										 (cw_set_addrs + nxt->index))->blk);
								}
							}
						} else
						{	/* TP */
							if (cs->done == 0)
							{
								if (FALSE == sec_shr_blk_build(csa, csd, is_bg, cs, blk_ptr,
												csd->trans_hist.curr_tn))
								{
									SECSHR_ACCOUNTING(10);
									SECSHR_ACCOUNTING(__LINE__);
									SECSHR_ACCOUNTING(cs);
									SECSHR_ACCOUNTING(cs->blk);
									SECSHR_ACCOUNTING(cs->level);
									SECSHR_ACCOUNTING(cs->done);
									SECSHR_ACCOUNTING(cs->forward_process);
									SECSHR_ACCOUNTING(cs->first_copy);
									SECSHR_ACCOUNTING(cs->upd_addr);
									SECSHR_ACCOUNTING(blk_ptr);
									assert(FALSE);
									continue;
								}
								if (cs->ins_off != 0)
								{
									if ((cs->ins_off
										> ((blk_hdr *)blk_ptr)->bsiz
											- sizeof(block_id))
										|| (cs->ins_off
										 < (sizeof(blk_hdr) + sizeof(rec_hdr))))
									{
										SECSHR_ACCOUNTING(7);
										SECSHR_ACCOUNTING(__LINE__);
										SECSHR_ACCOUNTING(cs);
										SECSHR_ACCOUNTING(cs->blk);
										SECSHR_ACCOUNTING(cs->index);
										SECSHR_ACCOUNTING(cs->ins_off);
										SECSHR_ACCOUNTING(
											((blk_hdr *)blk_ptr)->bsiz);
										assert(FALSE);
										continue;
									}
									if (cs->first_off == 0)
										cs->first_off = cs->ins_off;
									chain_ptr = blk_ptr + cs->ins_off;
									chain.flag = 1;
									chain.cw_index = cs->index;
									/* note: currently no verification of cs->index */
									chain.next_off = cs->next_off;
									GET_LONGP(chain_ptr, &chain);
									cs->ins_off = cs->next_off = 0;
								}
							} else
							{
								memmove(blk_ptr, cs->new_buff,
									((blk_hdr *)cs->new_buff)->bsiz);
								((blk_hdr *)blk_ptr)->tn = csd->trans_hist.curr_tn;
							}
							if (cs->first_off)
							{
								for (chain_ptr = blk_ptr + cs->first_off; ;
									chain_ptr += chain.next_off)
								{
									GET_LONGP(&chain, chain_ptr);
									if ((1 == chain.flag)
									   && ((chain_ptr - blk_ptr + sizeof(block_id))
										  <= ((blk_hdr *)blk_ptr)->bsiz)
									   && (chain.cw_index < si->cw_set_depth)
									   && (TRUE == secshr_tp_get_cw(
									      first_cw_set, chain.cw_index, &cs_ptr)))
									{
										PUT_LONG(chain_ptr, cs_ptr->blk);
										if (0 == chain.next_off)
											break;
									} else
									{
										SECSHR_ACCOUNTING(11);
										SECSHR_ACCOUNTING(__LINE__);
										SECSHR_ACCOUNTING(cs);
										SECSHR_ACCOUNTING(cs->blk);
										SECSHR_ACCOUNTING(cs->index);
										SECSHR_ACCOUNTING(blk_ptr);
										SECSHR_ACCOUNTING(chain_ptr);
										SECSHR_ACCOUNTING(chain.next_off);
										SECSHR_ACCOUNTING(chain.cw_index);
										SECSHR_ACCOUNTING(si->cw_set_depth);
										SECSHR_ACCOUNTING(
											((blk_hdr *)blk_ptr)->bsiz);
										assert(FALSE);
										break;
									}
								}
							}
						}	/* TP */
					}	/* non-map processing */
					if (0 > cs->reference_cnt)
					{	/* blocks were freed up */
						assert(non_tp_update_underway);
						UNIX_ONLY(
							assert((inctn_opcode == *inctn_opcode_addrs)
								&& ((inctn_bmp_mark_free_gtm == inctn_opcode)
									|| (inctn_bmp_mark_free_mu_reorg == inctn_opcode)
									|| (inctn_blkmarkfree == inctn_opcode)
									|| dse_running));
						)
						/* Check if we are freeing a V4 format block and if so decrement the
						 * blks_to_upgrd counter. Do not do this in case MUPIP REORG UPGRADE/DOWNGRADE
						 * is marking a recycled block as free (inctn_opcode is inctn_blkmarkfree).
						 */
						if ((NULL != inctn_opcode_addrs)
							&& (GTM_PROBE(sizeof(*inctn_opcode_addrs), inctn_opcode_addrs, READ))
							&& ((inctn_bmp_mark_free_gtm == *inctn_opcode_addrs)
								|| (inctn_bmp_mark_free_mu_reorg == *inctn_opcode_addrs))
							&& (NULL != inctn_detail_addrs)
							&& (GTM_PROBE(sizeof(*inctn_detail_addrs), inctn_detail_addrs, READ))
							&& (0 != inctn_detail_addrs->blknum))
						{
							DECR_BLKS_TO_UPGRD(csa, csd, 1);
						}
					}
					csd->trans_hist.free_blocks -= cs->reference_cnt;
					cs->old_mode = cs->mode;
					cs->mode = gds_t_committed;
				}	/* for all cw_set entries */
				/* Check if kill_in_prog flag in file header has to be incremented. */
				if (tp_update_underway)
				{	/* TP : Do this only if GT.M has not already completed the commit on this region. */
					assert((REG_COMMIT_COMPLETE == this_reg_commit_type)
						|| (REG_COMMIT_PARTIAL == this_reg_commit_type)
						|| (REG_COMMIT_UNSTARTED == this_reg_commit_type));
					/* We have already checked that "si" is READABLE. Check that it is WRITABLE since
					 * we might need to set "si->kip_incremented" in the CAREFUL_INCR_KIP macro.
					 */
					if (GTM_PROBE(sizeof(sgm_info), si, WRITE))
						kipincremented_usable = TRUE;
					else
					{
						kipincremented_usable = FALSE;
						assert(FALSE);
					}
					if (REG_COMMIT_COMPLETE != this_reg_commit_type)
					{
						UNIX_ONLY(assert(!si->kip_incremented););
						if ((NULL != si->kill_set_head) && kipincremented_usable && !si->kip_incremented)
							CAREFUL_INCR_KIP(csd, csa, si->kip_incremented);
					} else
						assert((NULL == si->kill_set_head) || si->kip_incremented);
					assert((NULL == si->kill_set_head) || si->kip_incremented);
				} else
				{	/* Non-TP. Check need_kip_incr and kip_incremented flags. */
					assert(non_tp_update_underway);
					if (GTM_PROBE(sizeof(*kip_incremented_addrs), kip_incremented_addrs, WRITE))
					{
						kipincremented_usable = TRUE;
						/* Note that *kip_incremented_addrs could be FALSE if we are in the
						 * 1st phase of the M-kill and TRUE if we are in the 2nd phase of the kill.
						 * Only if it is FALSE, should we increment the kill_in_prog flag.
						 */
					} else
					{
						kipincremented_usable = FALSE;
						assert(FALSE);
					}
					if (GTM_PROBE(sizeof(*need_kip_incr_addrs), need_kip_incr_addrs, WRITE))
						needkipincr = *need_kip_incr_addrs;
					else
					{
						needkipincr = FALSE;
						assert(FALSE);
					}
					if (needkipincr && kipincremented_usable && !*kip_incremented_addrs)
					{
						CAREFUL_INCR_KIP(csd, csa, *kip_incremented_addrs);
						*need_kip_incr_addrs = FALSE;
					}
				}
			}	/* if (NULL != first_cw_set) */
			if (JNL_ENABLED(csd))
			{
				if (GTM_PROBE(sizeof(jnl_private_control), csa->jnl, WRITE))
				{
					jbp = csa->jnl->jnl_buff;
					if (GTM_PROBE(sizeof(jnl_buffer), jbp, WRITE) && is_exiting)
					{
						CHECK_UNIX_LATCH(&jbp->fsync_in_prog_latch, is_exiting);
						if (VMS_ONLY(csa->jnl->qio_active)
							UNIX_ONLY(jbp->io_in_prog_latch.u.parts.latch_pid \
								  == rundown_process_id))
						{
							if (csa->jnl->dsk_update_inprog)
							{
								jbp->dsk = csa->jnl->new_dsk;
								jbp->dskaddr = csa->jnl->new_dskaddr;
							}
							VMS_ONLY(
								bci(&jbp->io_in_prog);
								csa->jnl->qio_active = FALSE;
							)
							UNIX_ONLY(RELEASE_SWAPLOCK(&jbp->io_in_prog_latch));
						}
						if (jbp->free_update_pid == rundown_process_id)
						{
							assert(csa->now_crit);
							jbp->free = csa->jnl->temp_free;
							jbp->freeaddr = csa->jnl->new_freeaddr;
							jbp->free_update_pid = 0;
						}
						if (jbp->blocked == rundown_process_id)
						{
							assert(csa->now_crit);
							jbp->blocked = 0;
						}
					}
				} else
				{
					SECSHR_ACCOUNTING(4);
					SECSHR_ACCOUNTING(__LINE__);
					SECSHR_ACCOUNTING(csa->jnl);
					SECSHR_ACCOUNTING(sizeof(jnl_private_control));
					assert(FALSE);
				}
			}
			if (is_exiting && csa->freeze && csd->freeze == rundown_process_id && !csa->persistent_freeze)
			{
				csd->image_count = 0;
				csd->freeze = 0;
			}
			if (is_bg && (csa->wbuf_dqd || csa->now_crit))
			{	/* if csa->wbuf_dqd == TRUE, most likely failed during REMQHI in wcs_wtstart
				 * or db_csh_get.  cache corruption is suspected so set wc_blocked.
				 * if csa->now_crit is TRUE, someone else should clean the cache, so set wc_blocked.
				 */
				SET_TRACEABLE_VAR(csd->wc_blocked, TRUE);
				if (csa->now_crit)
				{
					wcblocked_ptr = WCBLOCKED_NOW_CRIT_LIT;
					BG_TRACE_PRO_ANY(csa, wcb_secshr_db_clnup_now_crit);
				} else
				{
					wcblocked_ptr = WCBLOCKED_WBUF_DQD_LIT;
					BG_TRACE_PRO_ANY(csa, wcb_secshr_db_clnup_wbuf_dqd);
				}
				UNIX_ONLY(
					/* cannot send oplog message in VMS as privileged routines cannot do I/O */
					send_msg(VARLSTCNT(8) ERR_WCBLOCKED, 6, LEN_AND_STR(wcblocked_ptr),
						rundown_process_id, &csd->trans_hist.curr_tn, DB_LEN_STR(reg));
				)
			}
			csa->wbuf_dqd = 0;	/* We can clear the flag now */
			if (csa->now_crit)
			{
				if (csd->trans_hist.curr_tn == csd->trans_hist.early_tn - 1)
				{	/* there can be at most one region in non-TP with different curr_tn and early_tn */
					assert(!non_tp_update_underway || first_time);
					assert(NORMAL_TERMINATION != secshr_state); /* for normal termination we should not
										     * have been in the midst of commit */
					DEBUG_ONLY(first_time = FALSE;)
					if (update_underway)
					{
						INCREMENT_CURR_TN(csd);	/* roll forward step (8) */
					} else
						csd->trans_hist.early_tn = csd->trans_hist.curr_tn;
				}
				assert(csd->trans_hist.early_tn == csd->trans_hist.curr_tn);
				csa->t_commit_crit = FALSE;	/* ensure we don't process this region again */
				if ((GTM_PROBE(NODE_LOCAL_SIZE_DBS, csa->nl, WRITE)) &&
					(GTM_PROBE(CRIT_SPACE, csa->critical, WRITE)))
				{
					if (csa->nl->in_crit == rundown_process_id)
						csa->nl->in_crit = 0;
					UNIX_ONLY(
						DEBUG_ONLY(locknl = csa->nl;)	/* for DEBUG_ONLY LOCK_HIST macro */
						mutex_unlockw(reg, crash_count);/* roll forward step (11) */
						DEBUG_ONLY(locknl = NULL;)	/* restore "locknl" to default value */
					)
					VMS_ONLY(
						mutex_stoprelw(csa->critical);	/* roll forward step (11) */
					)
					csa->now_crit = FALSE;
					UNSUPPORTED_PLATFORM_CHECK;
				} else
				{
					SECSHR_ACCOUNTING(6);
					SECSHR_ACCOUNTING(__LINE__);
					SECSHR_ACCOUNTING(csa->nl);
					SECSHR_ACCOUNTING(NODE_LOCAL_SIZE_DBS);
					SECSHR_ACCOUNTING(csa->critical);
					SECSHR_ACCOUNTING(CRIT_SPACE);
					assert(FALSE);
				}
			} else  if (csa->read_lock)
			{
				if (GTM_PROBE(CRIT_SPACE, csa->critical, WRITE))
				{
					VMS_ONLY(mutex_stoprelr(csa->critical);)
					csa->read_lock = FALSE;
				} else
				{
					SECSHR_ACCOUNTING(4);
					SECSHR_ACCOUNTING(__LINE__);
					SECSHR_ACCOUNTING(csa->critical);
					SECSHR_ACCOUNTING(CRIT_SPACE);
					assert(FALSE);
				}
			}
			if ((NORMAL_TERMINATION == secshr_state || ABNORMAL_TERMINATION == secshr_state)
			    && GTM_PROBE(SHMPOOL_BUFFER_SIZE, csa->shmpool_buffer, WRITE))
			{
				if ((pid = csa->shmpool_buffer->shmpool_crit_latch.u.parts.latch_pid)
				    == rundown_process_id VMS_ONLY(&&)
				    VMS_ONLY((imgcnt = csa->shmpool_buffer->shmpool_crit_latch.u.parts.latch_image_count) \
					     == rundown_image_count))
				{
					if (is_exiting)
					{	/* Tiz our lock. Force recovery to run and release */
						csa->shmpool_buffer->shmpool_blocked = TRUE;
						BG_TRACE_PRO_ANY(csa, shmpool_blkd_by_sdc);
						SET_LATCH_GLOBAL(&csa->shmpool_buffer->shmpool_crit_latch, LOCK_AVAILABLE);
						DEBUG_LATCH(util_out_print("Latch cleaned up", FLUSH));
					}
				} else if (0 != pid && FALSE == is_proc_alive(pid, 0))
				{
					/* Attempt to make it our lock so we can set blocked */
					if (COMPSWAP_LOCK(&csa->shmpool_buffer->shmpool_crit_latch, pid, imgcnt,
							  rundown_process_id, rundown_image_count))
					{	/* Now our lock .. set blocked and release.  */
						csa->shmpool_buffer->shmpool_blocked = TRUE;
						BG_TRACE_PRO_ANY(csa, shmpool_blkd_by_sdc);
						DEBUG_LATCH(util_out_print("Orphaned latch cleaned up", TRUE));
						COMPSWAP_UNLOCK(&csa->shmpool_buffer->shmpool_crit_latch, rundown_process_id,
								rundown_image_count, LOCK_AVAILABLE, 0);
					} /* Else someone else took care of it */
				}
			}
#ifdef UNIX
			/* All releases done now. Double check latch is really cleared */
			if ((GTM_PROBE(NODE_LOCAL_SIZE_DBS, csa->nl, WRITE)) &&
			    (GTM_PROBE(CRIT_SPACE, csa->critical, WRITE)))
			{
				CHECK_UNIX_LATCH(&csa->critical->semaphore, is_exiting);
				CHECK_UNIX_LATCH(&csa->critical->crashcnt_latch, is_exiting);
				CHECK_UNIX_LATCH(&csa->critical->prochead.latch, is_exiting);
				CHECK_UNIX_LATCH(&csa->critical->freehead.latch, is_exiting);
			}
#endif
		}	/* For all regions */
	}	/* For all glds */
	if (jnlpool_reg_addrs && (GTM_PROBE(sizeof(*jnlpool_reg_addrs), jnlpool_reg_addrs, READ)))
	{	/* although there is only one jnlpool reg, SECSHR_PROBE_REGION macro might do a "continue" and hence the for loop */
		for (reg = *jnlpool_reg_addrs, jnlpool_reg = TRUE; jnlpool_reg && reg; jnlpool_reg = FALSE) /* only jnlpool reg */
		{
			SECSHR_PROBE_REGION(reg);	/* SECSHR_PROBE_REGION sets csa */
			if (csa->now_crit)
			{
				assert(NORMAL_TERMINATION != secshr_state); /* for normal termination we should not
									     * have been holding the journal pool crit lock */
				jpl = (jnlpool_ctl_ptr_t)((sm_uc_ptr_t)csa->critical - JNLPOOL_CTL_SIZE); /* see jnlpool_init() for
													   * relationship between
													   * critical and jpl */
				if (GTM_PROBE(sizeof(jnlpool_ctl_struct), jpl, WRITE))
				{
					if ((jpl->early_write_addr > jpl->write_addr) && (update_underway))
					{	/* we need to update journal pool to reflect the increase in jnl-seqno */
						cumul_jnl_rec_len = (uint4)(jpl->early_write_addr - jpl->write_addr);
						jh = (jnldata_hdr_ptr_t)((sm_uc_ptr_t)jpl + JNLDATA_BASE_OFF + jpl->write);
						if (GTM_PROBE(sizeof(*jh), jh, WRITE) && 0 != (jsize = jpl->jnlpool_size))
						{	/* Below chunk of code mirrors  what is done in t_end/tp_tend */
							/* Begin atomic stmnts */
							jh->jnldata_len = cumul_jnl_rec_len;
							jh->prev_jnldata_len = jpl->lastwrite_len;
							jpl->lastwrite_len = cumul_jnl_rec_len;
							SECSHR_SHM_WRITE_MEMORY_BARRIER;
							/* Emulate
							 * jpl->write = (jpl->write + cumul_jnl_rec_len) % jsize;
							 * See note in DOs and DONTs about using % operator
							 */
							for (new_write = jpl->write + cumul_jnl_rec_len;
								new_write >= jsize;
								new_write -= jsize)
								;
							jpl->write = new_write;
							jpl->write_addr += cumul_jnl_rec_len;
							jpl->jnl_seqno++;
							/* End atomic stmts */
							/* the above takes care of rolling forward steps (9) and (10) of the
							 * commit flow */
						}
					}
				}
				if ((GTM_PROBE(NODE_LOCAL_SIZE_DBS, csa->nl, WRITE)) &&
					(GTM_PROBE(CRIT_SPACE, csa->critical, WRITE)))
				{
					if (csa->nl->in_crit == rundown_process_id)
						csa->nl->in_crit = 0;
					UNIX_ONLY(
						DEBUG_ONLY(locknl = csa->nl;)	/* for DEBUG_ONLY LOCK_HIST macro */
						mutex_unlockw(reg, 0);		/* roll forward step (12) */
						DEBUG_ONLY(locknl = NULL;)	/* restore "locknl" to default value */
					)
					VMS_ONLY(
						mutex_stoprelw(csa->critical);	/* roll forward step (12) */
						csa->now_crit = FALSE;
					)
					/* the above takes care of rolling forward step (12) of the commit flow */
				}
			}
		}
	}
	return;
}

bool secshr_tp_get_cw(cw_set_element *cs, int depth, cw_set_element **cs1)
{
	int	iter;

	*cs1 = cs;
	for (iter = 0; iter < depth; iter++)
	{
		if (!(GTM_PROBE(sizeof(cw_set_element), *cs1, READ)))
		{
			*cs1 = NULL;
			return FALSE;
		}
		*cs1 = (*cs1)->next_cw_set;
	}
	if (*cs1 && GTM_PROBE(sizeof(cw_set_element), *cs1, READ))
	{
		while ((*cs1)->high_tlevel)
		{
			if (GTM_PROBE(sizeof(cw_set_element), (*cs1)->high_tlevel, READ))
				*cs1 = (*cs1)->high_tlevel;
			else
			{
				*cs1 = NULL;
				return FALSE;
			}
		}
	}
	return TRUE;
}
