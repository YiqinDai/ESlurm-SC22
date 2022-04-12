##*****************************************************************************
# AUTHOR:
#	Written by Alejandro Sanchez - alex@schedmd.com
#
# SYNOPSIS:
#	X_AC_SYSTEMD
#
# DESCRIPTION:
#	Determine systemd presence
#	Substitute SYSTEMD_TASKSMAX_OPTION output var if systemd version >= 227
##*****************************************************************************

AC_DEFUN([X_AC_SYSTEMD],
[

	AC_CACHE_CHECK([for systemd presence],
			[_cv_systemd_presence],
			[PKG_CHECK_EXISTS([systemd],
					  [_cv_systemd_presence=yes],
					  [_cv_systemd_presence=no])])

	if [test "x$_cv_systemd_presence" != "xno"]; then
	    AC_DEFINE([HAVE_SYSTEMD],
		      [1],
		      [Define systemd presence])

	    SYSTEMD_TASKSMAX_OPTION=""
	    $PKG_CONFIG --atleast-version 227 systemd
	    if [test "$?" -eq 0]; then
		    SYSTEMD_TASKSMAX_OPTION="TasksMax=infinity"
	    fi
	    AC_SUBST(SYSTEMD_TASKSMAX_OPTION)

	    # In the future we might want to enable the configure option
	    #  --with-systemdsystemunitdir=DIR, so that users can specify
	    # at configure time the directory to install the .service files.
	    # https://www.freedesktop.org/software/systemd/man/daemon.html#Installing%20Systemd%20Service%20Files

	    #AC_CACHE_CHECK([for systemd system unit dir],
	    #		[_cv_systemd_systemunitdir],
	    #		[PKG_CHECK_VAR([SYSTEMD_SYSTEM_UNIT_DIR],
	    #				[systemd],
	    #				[systemdsystemunitdir],
	    #				[_cv_systemd_systemunitdir=$SYSTEMD_SYSTEM_UNIT_DIR],
	    #				[_cv_systemd_systemunitdir=no])])
	fi

])
