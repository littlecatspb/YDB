#! /usr/local/bin/tcsh
#################################################################
#								#
#	Copyright 2001, 2007 Fidelity Information Services, Inc	#
#								#
#	This source code contains the intellectual property	#
#	of its copyright holder(s), and is made available	#
#	under a license.  If you do not know the terms of	#
#	the license, please stop and do not read further.	#
#								#
#################################################################

if ("$1" == "-m") then
        set mods_only=1
        shift
else
        set mods_only=0
endif

if( $# != 2 ) then
        echo ""
        echo "  Usage: $0 [-m] <cms-directory> <release-directory>"
        echo ""
        exit 5
endif

set cms_dir = $1
set dst_dir = $2

set dst_top_dir = $dst_dir:h
set dst_ver = $dst_dir:t
set cms_ver = $cms_dir:t

unalias cp chmod mv ls grep
alias ls 'echo "ls \!:* 2>/dev/null" | sh'	# we want to redirect only stderr to /dev/null; can't do that in tcsh.
alias cp 'cp -f \!:* >& /dev/null'		# some copies may be null copies. we dont want error messages coming out.
alias mv 'mv -f \!:* >& /dev/null'		# some moves may be null moves. we dont want error messages coming out.
alias chmod 'chmod \!:* >& /dev/null'		# some chmods may be null chmods. we dont want error messages coming out.

set platform_name = `uname | sed 's/-//g' | tr '[A-Z]' '[a-z]'`
if ($platform_name == "sunos") then
	alias grep /usr/xpg4/bin/grep		# for -E option to work on sparky
endif

set build_types        = "pro dbg bta"
set build_dirs         = "map obj"
set dir_structure      = "inc pct src tools log $build_types"

############# Define mapping between file-types and directory-name ##################

set gtm_src_types = "c m64 s msg"
set gtm_inc_types = "h max mac si"
set gtm_pct_types = "mpt m hlp"
set gtm_tools_types = "gtc sed awk sh csh list txt exp mk"

#####################################################################################

if !(-e $cms_dir) then
	echo "$cms_dir doesn't exist. Exiting..."
	exit 1
endif

if !(-e $dst_top_dir) then
	echo "$dst_top_dir doesn't exist. Exiting..."
	exit 1
endif

set preserve_time = "-p"	# while doing the copy, let us preserve time by default
if ($platform_name == "hpux") then
	# We want to preserve time (by default) in the cp but there is an issue.
	# In HPUX, cp -pf does not work with differing source and destination owner usernames. Avoid -p in that platform.
	set srcuid = `ls -ld $cms_dir | awk '{print $3}'`
	set dstuid = `whoami`
	if ($srcuid != $dstuid) then
		set preserve_time = ""
	endif
endif

set machine_name = `hostname`
if ($machine_name == "hanuman" || $machine_name == "saktiman" || $machine_name == "spiderman" || $machine_name == "batman") then
	set s7_env = true
else
	set s7_env = false
endif

cd $dst_top_dir
if (-e $dst_ver) then
	foreach image (pro bta dbg)
		if (-e $gtm_root/$dst_ver/$image/gtmsecshr) then
			$gtm_com/IGS $gtm_root/$dst_ver/$image/gtmsecshr 1	/* stop gtmsecshr in case it is running */
			$gtm_com/IGS $gtm_root/$dst_ver/$image/gtmsecshr 2	/* reset gtmsecshr to be suid and root owned */
		endif
	end
	# Verify if anybody is using this version before deleting
	if ($platform_name == "linux") then
		set psopt = "-ef --width 300"		# to get more screen output have a 300 column screen
	else
		set psopt = "-ef"
	endif
	/bin/ps $psopt | grep "$dst_top_dir/$dst_ver/" | grep -vE "grep|$0" >& /dev/null
	if ($status == 0) then
		# This check does not cover all cases of usage. There is still a window where new processes might start.
		# But, this is better than not checking at all.
		echo "Following processes are still using $dst_ver; not deleting $dst_top_dir/$dst_ver"
		/bin/ps $psopt | grep "$dst_top_dir/$dst_ver/" | grep -vE "grep|$0"
		if ($s7_env == "false") then
			exit 1
		endif
	endif
	if ($dst_ver =~ V3* || $dst_ver =~ V4* || $dst_ver =~ V5* || $dst_ver == "V990") then
		if ($s7_env == "false") then
			set move_args = "compulsory"
		endif
	endif
	if ($?move_args)  then
		set save_ver = `ls -ld ${gtm_root}/$dst_ver | awk '{if (length($7)==1) $7="0"_$7; time=$6"_"$7"_"$8; print toupper(time)}' | sed 's/://g'`
		echo "Renaming ${gtm_root}/${dst_ver} to ${gtm_root}/${dst_ver}_${save_ver}"
		mv ${gtm_root}/$dst_ver ${gtm_root}/${dst_ver}_${save_ver}
	else if (! $mods_only) then
		if ($s7_env == "false") then
			echo "Deleting existing $dst_dir directory structure"
			rm -rf $dst_ver
			if ($status != 0) then
				exit $status
			endif
		endif
	else
		echo "Updating $dst_dir directory structure"
	endif
endif

############## Create $dst_dir and subdirectories ##################

if ($s7_env == "true" || ! -e $dst_ver) then
	echo "Creating -------> $dst_dir Directory Structure ..."
	mkdir -p $dst_ver
	if ($status != 0) then
		exit $status
	endif
	cd $dst_ver
	set gtm_ver = `pwd`
	if ($status != 0) then
		exit $status
	endif
	if ($s7_env == "true") then
        	rm -rf $dir_structure
	endif
	mkdir $dir_structure {`echo $build_types | sed 's/ /,/g'`}/{`echo $build_dirs | sed 's/ /,/g'`}
	if ($status != 0) then
		exit $status
	endif
else
	set gtm_ver = ${dst_dir}
endif
cd $gtm_ver
cp $preserve_time $cms_dir/*/gtmsrc.csh .

############### Define platform-specific libraries ##################################

set gtm_s_aix     = "sr_port sr_port_cm sr_unix             sr_unix_cm sr_unix_gnp sr_rs6000 sr_aix  "
set gtm_s_osf1    = "sr_port sr_port_cm sr_unix             sr_unix_cm sr_unix_gnp sr_alpha  sr_dux  "
set gtm_s_hpux    = "sr_port sr_port_cm sr_unix             sr_unix_cm sr_unix_gnp sr_hppa   sr_hpux "
set gtm_s_linux   = "sr_port sr_port_cm sr_unix sr_unix_nsb sr_unix_cm sr_unix_gnp sr_i386   sr_linux"
set gtm_s_sunos   = "sr_port sr_port_cm sr_unix sr_unix_nsb sr_unix_cm sr_unix_gnp sr_sparc  sr_sun  "
set gtm_s_os390   = "sr_port sr_port_cm sr_unix sr_unix_nsb sr_unix_cm sr_unix_gnp sr_s390   sr_os390"
set gtm_s_l390    = "sr_port sr_port_cm sr_unix sr_unix_nsb sr_unix_cm sr_unix_gnp sr_l390   sr_linux"
set gtm_s_hpia    = "sr_port sr_port_cm sr_unix             sr_unix_cm sr_unix_gnp sr_hpux   sr_ia64 "
set gtm_s_linuxia = "sr_port sr_port_cm sr_unix             sr_unix_cm sr_unix_gnp sr_linux  sr_ia64 "

set platform_library = "$platform_name"
if ( "s390" == $MACHTYPE && "linux" == $platform_library ) then
	set platform_library = "l390"
endif
if ( "z/OS" == $MACHTYPE ) then
	set platform_library = "os390"
endif

set mach_type = `uname -m`
if ( "ia64" == $mach_type && "hpux" == $platform_library ) then
	set platform_library = "hpia"
endif
if ( "ia64" == $mach_type && "linux" == $platform_library ) then
	set platform_library = "linuxia"
endif

########### Copy sources from platform-specific directories into appropriate version-subdirectories ############

cd $cms_dir
set ref_libs = `set | grep "^gtm_s_${platform_library}[ 	]" | sed 's/^gtm_s_'${platform_library}'[ 	][ 	]*//g'`
foreach ref_library ( $ref_libs )
    if ( -d $ref_library ) then
	set override_libs=`set | grep "^gtm_s_${platform_library}" | sed "s/.*$ref_library //"`
	# echo "Override_libs for $ref_library are $override_libs"
	cd $ref_library
	foreach dir (src inc pct tools)
		foreach ftype (`set | grep "^gtm_${dir}_types[ 	]" | sed 's/^gtm_'$dir'_types[ 	][ 	]*//g'`)
			set nfiles = `\ls -1 | grep "\.$ftype"'$' | wc -l | sed 's/^[ ]*//g'`
			if ($nfiles != 0) then
				if ($mods_only == 0) then
					echo "Copying $nfiles files of type .$ftype from $ref_library to ${gtm_ver}/${dir}"
					if ($s7_env == "false") then
						\ls -1 | grep "\.$ftype"'$' | xargs -i cp -f $preserve_time {} $gtm_ver/${dir}
					else
						\ls -1 | grep "\.$ftype"'$' | xargs -i ln -f -s "$PWD/{}" $gtm_ver/${dir}
					endif
				else
					# @ n_modfiles=0
					foreach srcfile (*.$ftype)
						if ("" != "${override_libs}") then
							set override_exists=0
							foreach override_lib ($override_libs)
								if (-f ../${override_lib}/$srcfile) then
								# echo "Override for ${ref_library}/${srcfile} found in
								# ${override_lib}"
									set override_exists=1
									break
								endif
							end
							if ($override_exists) then
								continue	# on to the next file
							endif
						endif
						set dstfile="${srcfile}"
						if ($srcfile:e == "mpt") then
							set dstfile="_$srcfile:r.m"
							# echo "$srcfile is mpt, comparing with $dstfile"
						endif
						if ($srcfile == "release_name.h") then
							# echo "Skipping release_name.h"
							continue	# assume up-to-date release_name.h
						endif
						if (! { cmp -s ${srcfile} ${gtm_ver}/${dir}/${dstfile} } ) then
							echo "Copying differing $srcfile from $ref_library to $gtm_ver/${dir}"
							if ($s7_env == "false") then
								cp -f $preserve_time $srcfile $gtm_ver/${dir}
							else
								ln -s -f ${PWD}/$srcfile $gtm_ver/${dir}
							endif
							# @ n_modfiles++
						endif
					end
					# echo "Copied $n_modfiles out of $nfiles files of type .$ftype from $ref_library to
					# ${gtm_ver}/${dir}"
					endif
				endif
			endif
			set nfiles=`ls -1 *.${ftype}nix | wc -l | sed 's/^[ ]*//g'`
			if ($nfiles != 0) then
				echo "Restoring $nfiles NIXed files of type .$ftype in directory ${gtm_ver}/${dir}"
				if ($s7_env == "false") then
				    ls -1 *.${ftype}nix |\
						awk '{printf "cp -f $preserve_time %s %s/%s\n", $1, '\"${gtm_ver}/${dir}\"', $1}' |\
						sed 's/nix$//g' | sh
				else
				    ls -1 *.${ftype}nix |\
						awk '{printf "ln -s -f ${PWD}/%s %s/%s\n", $1, '\"${gtm_ver}/${dir}\"', $1}' |\
						sed 's/nix$//g' | sh
				endif
			endif
		end
	end
	cd ..
    else
	echo "Skipping missing library $ref_library"
    endif
end
cp sr_unix_cm/makefile* $gtm_ver/tools

######################## Rename .mpt files to _*.m files #######################

if ($mods_only == 0) then
	echo "Renaming .mpt files to _*.m in $gtm_ver/pct"
	cd $gtm_ver/pct
	ls -1 *.mpt | awk '{printf "mv %s _%s\n", $1, $1}' | sed 's/mpt$/m/g' | sh

######################## Edit release_name.h ####################################

	if ($cms_ver != $dst_ver) then
		echo "Modifying release_name.h"
		$cms_tools/edrelnam.csh $dst_ver # Do we care if this fails?
	endif
endif

############## Set appropriate permissions on the files. For comments see $gtm_tools/comlist.csh ##############

set gtm_verno = $gtm_ver:t
switch ($gtm_verno)
	case "V990":
		set chmod_protect = 1
		breaksw
	case "V9*":
		set chmod_protect = 0
		breaksw
	default:
		set chmod_protect = 1
		breaksw
endsw
if ($chmod_protect == 1) then
        set chmod_conf = 755
        set chmod_src = 444
else
        set chmod_conf = 775
        set chmod_src = 664
endif
chmod 775 $gtm_ver
cd $gtm_ver
chmod $chmod_conf bta dbg pro inc pct src tools gtmsrc.csh
chmod 775 log
cd $gtm_ver/inc
chmod $chmod_src *
cd $gtm_ver/pct
chmod $chmod_src *
cd $gtm_ver/src
/bin/ls | xargs -n25 chmod $chmod_src
cd $gtm_ver/tools
chmod $chmod_src *
if ($chmod_protect} == 1 ) then
        chmod 555 *sh
else
        chmod 775 *sh
endif
echo ""
echo "Done"
echo ""
exit 0
