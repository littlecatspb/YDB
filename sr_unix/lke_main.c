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
#include "main_pragma.h"

#include "gtm_inet.h"
#include "gtm_fcntl.h"
#include "gtm_unistd.h"
#include "gtm_signal.h"

#include "mlkdef.h"
#include "gtm_stdlib.h"
#include "gtm_stdio.h"
#include "gtm_string.h"
#include "stp_parms.h"
#include "iosp.h"
#include "error.h"
#include "cli.h"
#include "stringpool.h"
#include "interlock.h"
#include "gtmimagename.h"
#include "gdsroot.h"
#include "gtm_facility.h"
#include "fileinfo.h"
#include "gdsbt.h"
#include "gdsfhead.h"
#include "gdskill.h"
#include "gdscc.h"
#include "filestruct.h"
#include "jnl.h"
#include "buddy_list.h"
#include "hashtab_int4.h"	/* needed for tp.h */
#include "tp.h"
#include "repl_msg.h"
#include "gtmsource.h"
#include "util.h"
#include "gt_timer.h"
#include "lke.h"
#include "lke_fileio.h"
#include "ydb_chk_dist.h"
#include "generic_signal_handler.h"
#include "cli_parse.h"
#include "getzdir.h"
#include "getjobname.h"
#include "sig_init.h"
#include "gtmmsg.h"
#include "suspsigs_handler.h"
#include "patcode.h"
#include "common_startup_init.h"
#include "gtm_threadgbl_init.h"
#include "gtmio.h"
#include "have_crit.h"
#include "gt_timers_add_safe_hndlrs.h"
#include "continue_handler.h"

#ifdef UTF8_SUPPORTED
# include "gtm_icu_api.h"
# include "gtm_utf8.h"
# include "gtm_conv.h"
GBLREF	u_casemap_t 		gtm_strToTitle_ptr;		/* Function pointer for gtm_strToTitle */
#endif

GBLREF VSIG_ATOMIC_T		util_interrupt;
GBLREF bool			licensed;
GBLREF void			(*func)(void);
GBLREF spdesc			rts_stringpool, stringpool;
GBLREF global_latch_t		defer_latch;
GBLREF char			cli_err_str[];
GBLREF CLI_ENTRY		lke_cmd_ary[];
GBLREF ch_ret_type		(*stpgc_ch)();			/* Function pointer to stp_gcol_ch */

static bool lke_process(int argc);
static void display_prompt(void);

error_def(ERR_CTRLC);

int lke_main(int argc, char *argv[], char **envp)
{
	DCL_THREADGBL_ACCESS;

	GTM_THREADGBL_INIT;
	common_startup_init(LKE_IMAGE, &lke_cmd_ary[0]);
	licensed = TRUE;
	err_init(util_base_ch);
	DEFINE_EXIT_HANDLER(util_exit_handler, TRUE);	/* Must be defined only AFTER err_init() has setup condition handling */
	UTF8_ONLY(gtm_strToTitle_ptr = &gtm_strToTitle);
	sig_init(generic_signal_handler, lke_ctrlc_handler, suspsigs_handler, continue_handler);
	SET_LATCH_GLOBAL(&defer_latch, LOCK_AVAILABLE);
	stp_init(STP_INITSIZE);
	stpgc_ch = &stp_gcol_ch;
	rts_stringpool = stringpool;
	getjobname();
	getzdir();
	ydb_chk_dist(argv[0]);
	prealloc_gt_timers();
	gt_timers_add_safe_hndlrs();
	initialize_pattern_table();
	gvinit();
	region_init(FALSE);	/* Was TRUE, but that doesn't actually work if there are GTCM regions in the GLD,
				 * at least in DEBUG, so leave it off for now to allow LKE to work in this situation.
				 */
	cli_lex_setup(argc, argv);
	/*      this should be after cli_lex_setup() due to S390 A/E conversion    */
	OPERATOR_LOG_MSG;
	while (1)
	{
		if (!lke_process(argc) || 2 <= argc)
			break;
	}
	lke_exit();
	return 0;
}

static bool lke_process(int argc)
{
	bool		flag = FALSE;
	int		res;
	static int	save_stderr = SYS_STDERR;
	DCL_THREADGBL_ACCESS;

	SETUP_THREADGBL_ACCESS;
	ESTABLISH_RET(util_ch, TRUE);
	if (util_interrupt)
		rts_error_csa(CSA_ARG(NULL) VARLSTCNT(1) ERR_CTRLC);
	if (SYS_STDERR != save_stderr)  /* necesary in case of rts_error */
		close_fileio(&save_stderr);
	assert(SYS_STDERR == save_stderr);

	func = 0;
	util_interrupt = 0;
 	if (argc < 2)
		display_prompt();
	if ( EOF == (res = parse_cmd()))
	{
		if (util_interrupt)
		{
			rts_error_csa(CSA_ARG(NULL) VARLSTCNT(1) ERR_CTRLC);
			REVERT;
			return TRUE;
		}
		else
		{
			REVERT;
			return FALSE;
		}
	} else if (res)
	{
		if (1 < argc)
		{
			REVERT;
			rts_error_csa(CSA_ARG(NULL) VARLSTCNT(4) res, 2, LEN_AND_STR(cli_err_str));
		} else
			gtm_putmsg_csa(CSA_ARG(NULL) VARLSTCNT(4) res, 2, LEN_AND_STR(cli_err_str));
	}
	if (func)
	{
		flag = open_fileio(&save_stderr); /* save_stderr = SYS_STDERR if -output option not present */
		func();
		if (flag)
			close_fileio(&save_stderr);
		assert(SYS_STDERR == save_stderr);
	}
	REVERT;
	return(1 >= argc);
}

static void display_prompt(void)
{
	PRINTF("LKE> ");
	FFLUSH(stdout);
}

