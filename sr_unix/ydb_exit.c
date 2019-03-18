/****************************************************************
 *								*
 * Copyright (c) 2001-2018 Fidelity National Information	*
 * Services, Inc. and/or its subsidiaries. All rights reserved.	*
 *								*
 * Copyright (c) 2017-2019 YottaDB LLC and/or its subsidiaries. *
 * All rights reserved.						*
 *								*
 *	This source code contains the intellectual property	*
 *	of its copyright holder(s), and is made available	*
 *	under a license.  If you do not know the terms of	*
 *	the license, please stop and do not read further.	*
 *								*
 ****************************************************************/

#include "mdef.h"

#include "libyottadb_int.h"
#include "invocation_mode.h"
#include "gdsroot.h"
#include "gtm_facility.h"
#include "fileinfo.h"
#include "filestruct.h"
#include "gdscc.h"
#include "gdskill.h"
#include "jnl.h"
#include "tp_frame.h"
#include "buddy_list.h"		/* needed for tp.h */
#include "hashtab_int4.h"	/* needed for tp.h and cws_insert.h */
#include "tp.h"
#ifdef GTM_TRIGGER
#include "gv_trigger.h"
#include "gtm_trigger.h"
#endif
#include "op.h"
#include "gtmci.h"
#include "gtm_exit_handler.h"
#include "dlopen_handle_array.h"

GBLREF	stm_workq		*stmWorkQueue[];
GBLREF	int			mumps_status;
GBLREF	struct sigaction	orig_sig_action[];

/* Routine exposed to call-in user to exit from active YottaDB environment */
int ydb_exit()
{
	int			status, sig;
	pthread_t		thisThread, threadid;
	libyottadb_routines	save_active_stapi_rtn;
	ydb_buffer_t		*save_errstr;
	boolean_t		get_lock;
	boolean_t		error_encountered;
        DCL_THREADGBL_ACCESS;

        SETUP_THREADGBL_ACCESS;
	if (!ydb_init_complete)
		return 0;		/* If we aren't initialized, we don't have things to take down so just return */
	THREADED_API_YDB_ENGINE_LOCK(YDB_NOTTP, NULL, LYDB_RTN_NONE, save_active_stapi_rtn, save_errstr, get_lock, status);
	if (0 != status)
		return status;		/* Lock failed - no condition handler yet so just return the error code */
	if (!ydb_init_complete)
	{	/* "ydb_init_complete" was TRUE before we got the "THREADED_API_YDB_ENGINE_LOCK" lock but became FALSE
		 * afterwards. This implies some other concurrent thread did the "ydb_exit" so we can return from this
		 * "ydb_exit" call without doing anything more.
		 */
	} else
	{
		/* If this is a SimpleThreadAPI environment and we hold the YottaDB engine multi-thread mutex lock (obtained
		 * above in the THREADED_API_YDB_ENGINE_LOCK call). So we can proceed with exit handling. We are also
		 * guaranteed this thread is not the MAIN worker thread (asserted below).
		 */
		assert(!simpleThreadAPI_active || !IS_STAPI_WORKER_THREAD);
		ESTABLISH_NORET(gtmci_ch, error_encountered);
		if (error_encountered)
		{	/* "gtmci_ch" encountered an error and transferred control back here. Return after mutex lock cleanup. */
			THREADED_API_YDB_ENGINE_UNLOCK(YDB_NOTTP, NULL, save_active_stapi_rtn, save_errstr, get_lock);
			return mumps_status;
		}
		assert(NULL != frame_pointer);
		/* If process_exiting is set (and the YottaDB environment is still active since "ydb_init_complete" is TRUE
		 * here), shortcut some of the checks and cleanups we are making in this routine as they are not
		 * particularly useful. If the environment is not active though, that's still an error.
		 */
		if (!process_exiting)
		{	/* Do not allow ydb_exit() to be invoked from external calls (unless process_exiting) */
			if (!(SFT_CI & frame_pointer->type) || !(MUMPS_CALLIN & invocation_mode)
					|| (1 < TREF(gtmci_nested_level)))
				rts_error_csa(CSA_ARG(NULL) VARLSTCNT(1) ERR_INVYDBEXIT);
			/* Now get rid of the whole M stack - end of YottaDB environment */
			while (NULL != frame_pointer)
			{
				while ((NULL != frame_pointer) && !(frame_pointer->type & SFT_CI))
				{
					if (SFT_TRIGR & frame_pointer->type)
						gtm_trigger_fini(TRUE, FALSE);
					else
						op_unwind();
				}
				if (NULL != frame_pointer)
				{	/* unwind the current invocation of call-in environment */
					assert(frame_pointer->type & SFT_CI);
					ci_ret_code_quit();
				}
			}
		}
		gtm_exit_handler(); /* rundown all open database resource */
		/* If libyottadb was loaded via (or on account of) dlopen() and is later unloaded via dlclose()
		 * the exit handler on AIX and HPUX still tries to call the registered atexit() handler causing
		 * 'problems'. AIX 5.2 and later have the below unatexit() call to unregister the function if
		 * our exit handler has already been called. Linux and Solaris don't need this, looking at the
		 * other platforms we support to see if resolutions can be found. SE 05/2007
		 */
		REVERT;
		/* Restore the signal handlers that were saved and overridden during ydb_init()->gtm_startup()->sig_init() */
		for (sig = 1; sig <= NSIG; sig++)
			sigaction(sig, &orig_sig_action[sig], NULL);
		/* We might have opened one or more shlib handles using "dlopen". Do a "dlclose" of them now. */
		dlopen_handle_array_close();
		ydb_init_complete = FALSE;
	}
	THREADED_API_YDB_ENGINE_UNLOCK(YDB_NOTTP, NULL, save_active_stapi_rtn, save_errstr, get_lock);
	return 0;
}