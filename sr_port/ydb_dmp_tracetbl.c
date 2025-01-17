/****************************************************************
 *								*
 * Copyright (c) 2018-2019 YottaDB LLC and/or its subsidiaries.	*
 * All rights reserved.						*
 *								*
 *	This source code contains the intellectual property	*
 *	of its copyright holder(s), and is made available	*
 *	under a license.  If you do not know the terms of	*
 *	the license, please stop and do not read further.	*
 *								*
 ****************************************************************/
#include "mdef.h"

#include "gtm_stdio.h"
#include "gtmio.h"

#include "trace_table.h"
#include "libyottadb_int.h"

/* Routine to dump the current (circular queue) trace table contents */
void ydb_dmp_tracetbl(void)
{
#	ifdef DEBUG
	trctbl_entry	*cur, *start, *end;
	boolean_t	first = TRUE;
	const char	*funcname;
	DCL_THREADGBL_ACCESS;

	SETUP_THREADGBL_ACCESS;
	start = cur = TREF(gtm_trctbl_cur);			/* The last entry that was filled in */
	if ((NULL != start) && (start >= TREF(gtm_trctbl_start)))
		fprintf(stderr, "Trace table entries (newest to oldest order):\n");
	else
		return;
	/* At least one entry exists - format entries from cur backwards until we hit start again or we run into
	 * 0 type entries which means table was not full.
	 */
	for (; first || (cur != start); cur--)
	{
		first = FALSE;
		if (cur < TREF(gtm_trctbl_start))
			cur = TREF(gtm_trctbl_end) - 1;		/* Point to actual last entry in physical table */
		if (0 == cur->type)
			break;					/* Hit unused entries - time to stop */
		switch(cur->type)
		{
			case SOCKRFL_ENTRY:
				fprintf(stderr, "   Entry: SOCKRFL_ENTRY,  Width: %d,  iod: %p,  intrpt_cnt: %d\n",
					cur->intfld, cur->addrfld1, (int)(intptr_t)cur->addrfld2);
				break;
			case SOCKRFL_MVS_ZINTR:
				fprintf(stderr, "   Entry: SOCKRFL_MVS_ZINTR,  bytes_read: %d, buffer_start: %p,  "
					"stp_free: %p\n", cur->intfld, cur->addrfld1, cur->addrfld2);
				break;
			case SOCKRFL_RESTARTED:
				fprintf(stderr, "   Entry: SOCKRFL_RESTARTED, chars_read: %d,  max_bufflen: %d,  stp_need: %d,  "
					"buffer_start: %p\n", cur->intfld,(int)(intptr_t) cur->addrfld1,
					(int)(intptr_t)cur->addrfld2, cur->addrfld3);
				break;
			case SOCKRFL_RSTGC:
				fprintf(stderr, "   Entry: SOCKRFL_RSTGC,  buffer_start: %p,  stp_free: %p\n",
					cur->addrfld1, cur->addrfld2);
				break;
			case SOCKRFL_BEGIN:
				fprintf(stderr, "   Entry: SOCKRFL_BEGIN,  chars_read: %d,  buffer_start: %p,  stp_free: %p\n",
					cur->intfld, cur->addrfld1, cur->addrfld2);
				break;
			case SOCKRFL_OUTOFBAND:
				fprintf(stderr, "   Entry: SOCKRFL_OUTOFBAND,  bytes_read: %d,  chars_read: %d,  "
					"buffer_start: %p\n", cur->intfld, (int)(intptr_t)cur->addrfld1, cur->addrfld2);
				break;
			case SOCKRFL_EXPBUFGC:
				fprintf(stderr, "   Entry: SOCKRFL_EXPBUFGC,  bytes_read: %d,  stp_free: %p,  "
					"old_stp_free: %p,  max_bufflen: %d\n", cur->intfld, cur->addrfld1,
					cur->addrfld2, (int)(intptr_t)cur->addrfld3);
				break;
			case SOCKRFL_RDSTATUS:
				fprintf(stderr, "   Entry: SOCKRFL_RDSTATUS,  read_status: %d,  out_of_band: %d, out_of_time: %d\n",
					cur->intfld, (int)(intptr_t)cur->addrfld1, (int)(intptr_t)cur->addrfld2);
				break;
			case CONDHNDLR_INVOKED:
				fprintf(stderr, "   Entry: CONDHNDLR_INVOKED,  PID: %d,  TID: 0x"lvaddr",  PC: %p\n",
					(int)(intptr_t)cur->addrfld1, (unsigned long)cur->addrfld2, cur->addrfld3);
				break;
			default:
				assertpro(FALSE);
		}
	}
	fprintf(stderr, "\n");
	FFLUSH(stderr);
#	endif
}
