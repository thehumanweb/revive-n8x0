#
# Copyright(c) 2008 Imagination Technologies Ltd. All rights reserved.
# 
# This program is free software; you can redistribute it and/or modify it
# under the terms and conditions of the GNU General Public License,
# version 2, as published by the Free Software Foundation.
# 
# This program is distributed in the hope it will be useful but, except 
# as otherwise stated in writing, without any warranty; without even the 
# implied warranty of merchantability or fitness for a particular purpose. 
# See the GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License along with
# this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
# 
# The full GNU General Public License is included in this distribution in
# the file called "COPYING".
#
# Contact Information:
# Imagination Technologies Ltd. <gpl-support@imgtec.com>
# Home Park Estate, Kings Langley, Herts, WD4 8LZ, UK 
# 
#

include(../scripts/common.m4)



###############################################################################
##
## Diversion discipline
##
## 50    generic functions
## 51    function header of install_pvr function
## 52    body of install_pvr function
## 53    function trailer of install_pvr function
## 54    function header of install_X function
## 55    body of install_X function
## 56    function trailer of install_X function
## 80    script body
## 200   main case statement for decoding arguments.
##
###############################################################################

###############################################################################
##
## Diversion #50 - Some generic functions
##
###############################################################################
pushdivert(50)dnl
# PVR Consumer services version number
#
[[PVRVERSION]]=PVRVERSION
popdivert


ifelse(KBUILD,TRUE, [[pushdivert(50)dnl
[[# Where we record what we did so we can undo it.
#
DDK_INSTALL_LOG=/etc/powervr_ddk_install_km.log
]]popdivert]],
[[pushdivert(50)dnl
[[# Where we record what we did so we can undo it.
#
DDK_INSTALL_LOG=/etc/powervr_ddk_install.log
]]popdivert]])


pushdivert(50)dnl
[[# basic installation function
# $1=blurb
#
bail()
{
    echo "$1" >&2
    echo "" >&2
    echo "Installation failed" >&2
    exit 1
}

# basic installation function
# $1=fromfile, $2=destfilename, $3=blurb $4=chmod-flags
#
install_file()
{
	DESTFILE=${DISCIMAGE}$2
	DESTDIR=`dirname $DESTFILE`

	# Destination directory - make sure it's there and writable
	#
	if [ -d "${DESTDIR}" ]; then
		if [ ! -w "${DESTDIR}" ]; then
			bail "${DESTDIR} is not writable."
		fi
	else
		$DOIT mkdir -p ${DESTDIR} || bail "Couldn't mkdir -p ${DESTDIR}"
		[ -n "$VERBOSE" ] && echo "Created directory `dirname $2`"
	fi

	# If it already exists then back it up if there isn't a back-up already.
	#
	#if [ -f $DESTFILE ]; then
	#	if [ ! -f ${DESTFILE}.old ]; then
	#		$DOIT mv $DESTFILE ${DESTFILE}.old ||
	#				bail "Couldn't back up $DESTFILE";
	#		[ -n "$VERBOSE" ] && echo "Backed up $DESTFILE";
	#	fi
	#fi

	# Delete the original so that permissions don't persist.
	#
	$DOIT rm -f $DESTFILE

	$DOIT cp -f $1 $DESTFILE || bail "Couldn't copy $1 to $DESTFILE"
	$DOIT chmod 644 ${DESTFILE}
	if [ "$4" ]; then
		$DOIT chmod $4 ${DESTFILE};
	fi
	echo "$3 `basename $1` -> $2"
	$DOIT echo "file $2" >>${DISCIMAGE}${DDK_INSTALL_LOG}
}

# Install a symbolic link
# $1=fromfile, $2=destfilename
#
install_link()
{
	DESTFILE=${DISCIMAGE}$2
	DESTDIR=`dirname $DESTFILE`

	# Destination directory - make sure it's there and writable
	#
	if [ -d "${DESTDIR}" ]; then
		if [ ! -w "${DESTDIR}" ]; then
			bail "${DESTDIR} is not writable."
		fi
	else
		$DOIT mkdir -p ${DESTDIR} || bail "Couldn't mkdir -p ${DESTDIR}"
		[ -n "$VERBOSE" ] && echo "Created directory `dirname $2`"
	fi

	# Delete the original so that permissions don't persist.
	#
	$DOIT rm -f $DESTFILE

	$DOIT ln -s $1 $DESTFILE || bail "Couldn't link $1 to $DESTFILE"
	$DOIT echo "link $2" >>${DISCIMAGE}${DDK_INSTALL_LOG}
	[ -n "$VERBOSE" ] && echo " linked `basename $1` -> $2"
}

# Tree-based installation function
# $1 = fromdir $2=destdir $3=blurb
#
install_tree()
{
	# Make the destination directory if it's not there
	#
	if [ ! -d ${DISCIMAGE}$2 ]; then
		$DOIT mkdir -p ${DISCIMAGE}$2 || bail "Couldn't mkdir -p ${DISCIMAGE}$2"
	fi
	if [ "$DONTDOIT" ]; then
		echo "### tar -C $1 -cf - . | tar -C ${DISCIMAGE}$2 -x${VERBOSE}f -" 
	else
		tar -C $1 -cf - . | tar -C ${DISCIMAGE}$2 -x${VERBOSE}f -
	fi
	if [ $? = 0 ]; then
		echo "Installed $3 in ${DISCIMAGE}$2"
		$DOIT echo "tree $2" >>${DISCIMAGE}${DDK_INSTALL_LOG}
	else
		echo "Failed copying $3 from $1 to ${DISCIMAGE}$2"
	fi
}

# Uninstall something.
#
uninstall()
{
	if [ ! -f ${DISCIMAGE}${DDK_INSTALL_LOG} ]; then
		echo "Nothing to un-install."
		return;
	fi

	BAD=0
	VERSION=""
	while read type data; do
		case $type in
		version)	# do nothing
			echo "Uninstalling existing version $data"
			VERSION="$data"
			;;
		link|file) 
			if [ -z "$VERSION" ]; then
				BAD=1;
				echo "No version record at head of ${DISCIMAGE}${DDK_INSTALL_LOG}"
			elif ! $DOIT rm -f ${DISCIMAGE}${data}; then
				BAD=1;
			else
				[ -n "$VERBOSE" ] && echo "Deleted $type $data"
			fi
			;;
		tree)		# so far, do nothing
			;;
		esac
	done < ${DISCIMAGE}${DDK_INSTALL_LOG};

	if [ $BAD = 0 ]; then
		echo "Uninstallation completed."
		$DOIT rm -f ${DISCIMAGE}${DDK_INSTALL_LOG}
	else
		echo "Uninstallation failed!!!"
	fi
}

# Help on how to invoke
#
usage()
{
	echo "usage: $0 [options...]"
	echo ""
	echo "Options: -v            verbose mode"
	echo "         -n            dry-run mode"
	echo "         -u            uninstall-only mode"
	echo "         --no-pvr      don't install PowerVR driver components"
	echo "         --no-x        don't install X window system"
	echo "         --no-display  don't install integrated PowerVR display module"
	echo "         --no-bcdevice don't install buffer class device module"
	echo "         --root path   use path as the root of the install file system"
	exit 1
}

]]popdivert

###############################################################################
##
## Diversion 51 - the start of the install_pvr() function
##
###############################################################################
pushdivert(51)dnl
install_pvr()
{
	$DOIT echo "version PVRVERSION" >${DISCIMAGE}${DDK_INSTALL_LOG}
popdivert

pushdivert(53)dnl
}

popdivert

pushdivert(80)[[
# Work out if there are any special instructions.
#
while [ "$1" ]; do
	case "$1" in
	-v|--verbose)
		VERBOSE=v;
		;;
	-r|--root)
		DISCIMAGE=$2;
		shift; shift;
		;;
	-u|--uninstall)
		UNINSTALL=y
		;;
	-n)	DOIT=echo
		;;
	--no-pvr)
		NO_PVR=y
		;;
	--no-x)
		NO_X=y
		;;
	--no-display)
		NO_DISPLAYMOD=y
		;;
	--no-bcdevice)
		NO_BCDEVICE=y
		;;
	-h | --help | *)	
		usage
		;;
	esac
	shift
done

# Find out where we are?  On the target?  On the host?
#
case `uname -m` in
arm*)	host=0;
		from=target;
		DISCIMAGE=/;
		;;
sh*)	host=0;
		from=target;
		DISCIMAGE=/;
		;;
i?86)	host=1;
		from=host;
		if [ -z "$DISCIMAGE" ]; then	
			echo "DISCIMAGE must be set for installation to be possible." >&2
			exit 1
		fi
		;;
*)		echo "Don't know host to perform on machine type `uname -m`" >&2;
		exit 1
		;;
esac

if [ ! -d "$DISCIMAGE" ]; then
	echo "$0: $DISCIMAGE does not exist." >&2
	exit 1
fi

echo
echo "Installing PowerVR Consumer/Embedded DDK ]]PVRVERSION[[ on $from"
echo
echo "File system installation root is $DISCIMAGE"
echo

]]popdivert


###############################################################################
##
## Main Diversion - call the install_pvr function unless NO_PVR is non-null
##
###############################################################################
pushmain()dnl
# Uninstall whatever's there already.
#
uninstall
[ -n "$UNINSTALL" ] && exit

#  Now start installing things we want.
#
[ -z "$NO_PVR" ] && install_pvr
popdivert


###############################################################################
##
## Diversion 200 -
## Divert some text to appear when the final input stream is closed.
##
###############################################################################
pushdivert(200)dnl

# All done...
#
echo 
echo "Installation complete!"
if [ "$host" = 0 ]; then
   echo "You may now reboot your target."
fi
echo
popdivert

###############################################################################
##
## DISPLAY_CONTROLLER_MODULE -
## install a display controller device module in the correct place in the file system,
## updating the module dependencies file(s) if necessary.
##
## Parameters:
## $1 - name of display controller module file without leading path components
## $2 - name of any other kernel module upon which $1 is dependent.
##
###############################################################################
define([[DISPLAY_CONTROLLER_MODULE]], [[pushdivert(52)dnl
ifdef([[_DISPLAY_CONTROLLER_]],
	if [ -z "$NO_DISPLAYMOD" ]; then
	INSTALL_KERNEL_MODULE($1, $2)
fi)
popdivert()]])dnl


###############################################################################
##
## BUFFERCLASS_DEVICE_MODULE -
## install a buffer class device loadable module in the correct place in the file system,
## updating the module dependencies file(s) if necessary.
##
## Parameters:
## $1 - name of bufferclass device module file without leading path components
## $2 - name of any other kernel module upon which $1 is dependent.
##
###############################################################################
define([[BUFFERCLASS_DEVICE_MODULE]], [[pushdivert(52)dnl
ifdef([[_BUFFER_CLASS_DEVICE_]],
	if [ -z "$NO_BCDEVICE" ]; then
	INSTALL_KERNEL_MODULE($1, $2)
fi)
popdivert()]])dnl


###############################################################################
##
## INSTALL_KERNEL_MODULE -
## install a kernel loadable module in the correct place in the file system,
## updating the module dependencies file(s) if necessary.
##
## Parameters:
## $1 - name of kernel module file without leading path components
## $2 - name of any other kernel module upon which $1 is dependent.
##
###############################################################################
define([[INSTALL_KERNEL_MODULE]], [[pushdivert(52)dnl
	install_file $1 MOD_DESTDIR/$1 "kernel module"
ifelse(TARGET_RUNS_DEPMOD_AT_BOOT,TRUE,,TARGET_HAS_DEPMOD,FALSE,[[dnl
FIXUP_MODULES_DEP($1, $2)dnl
]],[[dnl
	if [ "$host" = 1 ]; then
		FIXUP_MODULES_DEP($1, $2)dnl
	fi
]])dnl
popdivert()]])


###############################################################################
##
## FIXUP_MODULES_DEP -
## Fix up the modules dependency file.
##
## Parameters:
## $1 - name of kernel module file without leading path components
## $2 - name of any other kernel module upon which $1 is dependent.
##
###############################################################################
define([[FIXUP_MODULES_DEP]], [[pushdivert(52)dnl
	grep -v -e "drivers/char/$1" MODULES_DEP >MODULES_TMP
ifelse([[$2]], [[]],dnl
	echo "MOD_DESTDIR/$1:" >>MODULES_TMP,
	echo "MOD_DESTDIR/$1: MOD_DESTDIR/$2" >>MODULES_TMP)
	cp MODULES_TMP MODULES_DEP
popdivert()]])


###############################################################################
##
## INSTALL_SHARED_LIBRARY -
## Install a shared library (in /usr/lib) with the correct version number and
## links.
##
## Parameters:
## $1 - name of shared library to install
## $2 - optional version suffix.
##
###############################################################################
define([[INSTALL_SHARED_LIBRARY]], [[pushdivert(52)dnl
	ifelse($2,,install_file $1 SHLIB_DESTDIR/$1 "shared library",install_file $1 SHLIB_DESTDIR/$1.$2 "shared library"
	install_link $1.$2 SHLIB_DESTDIR/$1)
popdivert()]])


###############################################################################
##
## INSTALL_BINARY -
## Install a binary file.  These always go in /usr/local/bin presently.
##
## Parameters:
## $1 - name of file in local directory which is to be copied to /usr/local/bin
##
###############################################################################
define([[INSTALL_BINARY]], [[pushdivert(52)dnl
	install_file $1 DEMO_DESTDIR/$1 "binary" a+x
popdivert()]])

###############################################################################
##
## INSTALL_GLESBINARY -
## Install a gles binary file.  These always go in /usr/local/bin presently.
##
## Parameters:
## $1 - name of file in local directory which is to be copied to /usr/local/bin
##
###############################################################################
define([[INSTALL_GLESBINARY]], [[pushdivert(52)dnl
	ifelse(PROFILE_COMMONLITE,1,[[install_file $1_cl DEMO_DESTDIR/$1_cl "binary" a+x]],
								[[install_file $1 DEMO_DESTDIR/$1 "binary" a+x]])
popdivert()]])



###############################################################################
##
## INSTALL_INITRC -
## Install a run-time configuration file.  This goes in /etc/init.d unless
## RC_DESTDIR has been updated.
##
## No parameters.
##
###############################################################################
define([[INSTALL_INITRC]], [[pushdivert(52)dnl
	install_file $1 RC_DESTDIR/$1 "boot script" a+x
popdivert()]])


###############################################################################
##
## Some defines for general expansion
##
## No parameters.
##
###############################################################################
define([[SHARED_LIBRARY]],			[[INSTALL_SHARED_LIBRARY($1,$2)]])
define([[UNITTEST]],				[[INSTALL_BINARY($1)]])
define([[GLESUNITTEST]],			[[INSTALL_GLESBINARY($1)]])
define([[KERNEL_MODULE]],			[[INSTALL_KERNEL_MODULE($1, $2)]])


###############################################################################
##
## STANDARD_SCRIPTS -
## Install all standard script parts of a DDK distribution.
##
## No parameters.
##
###############################################################################
define([[STANDARD_SCRIPTS]], [[pushdivert(52)dnl
	# Install the standard scripts
	#

INSTALL_INITRC([[PVR_KERNEL_MODULE]])dnl

popdivert()]])


###############################################################################
##
## STANDARD_LIBRARIES
## Install all standard parts of a DDK distribution.
##
## No parameters.
##
###############################################################################
define([[STANDARD_LIBRARIES]], [[pushdivert(52)dnl
	# Install the standard libraries
	#
ifelse(PROFILE_COMMONLITE,1,[[SHARED_LIBRARY([[libGLES_CL.LIB_SUFFIX]],[[SOLIB_VERSION]])]],
							[[SHARED_LIBRARY([[libGLES_CM.LIB_SUFFIX]],[[SOLIB_VERSION]])]])dnl
ifelse(SUPPORT_OPENVG,1,[[SHARED_LIBRARY([[libOpenVG.LIB_SUFFIX]],[[SOLIB_VERSION]])]])dnl
ifelse(SUPPORT_OPENVG,1,[[SHARED_LIBRARY([[libOpenVGU.LIB_SUFFIX]],[[SOLIB_VERSION]])]])dnl
SHARED_LIBRARY([[libIMGegl.LIB_SUFFIX]],[[SOLIB_VERSION]])dnl
ifelse(EGL1_2,1,[[SHARED_LIBRARY([[libEGL.LIB_SUFFIX]],[[SOLIB_VERSION]])]])dnl
SHARED_LIBRARY([[libpvr2d.LIB_SUFFIX]],[[SOLIB_VERSION]])dnl
SHARED_LIBRARY([[libsrv_um_[[]]SOLIB_VERSION.so]])dnl
SHARED_LIBRARY([[libpvrNULLWSEGL.LIB_SUFFIX]],[[SOLIB_VERSION]])dnl
ifelse(MBX_GPL,1,[[SHARED_LIBRARY([[libmbxservicesdaemon_[[]]SOLIB_VERSION.so]])]])dnl
ifelse(SUPPORT_XWS,1,[[SHARED_LIBRARY([[libpvrX11WSEGL.LIB_SUFFIX]],[[SOLIB_VERSION]])]])dnl

ifdef([[_DISPLAY_CONTROLLER_]],
	if [ -z "$NO_DISPLAYMOD" ]; then
	SHARED_LIBRARY([[lib]]_DISPLAY_CONTROLLER_[[.]]LIB_SUFFIX,[[SOLIB_VERSION]])
fi)

ifdef([[_BUFFER_CLASS_DEVICE_]], 
	if [ -z "$NO_BCDEVICE" ]; then
	SHARED_LIBRARY([[lib]]_BUFFER_CLASS_DEVICE_[[.]]LIB_SUFFIX,[[SOLIB_VERSION]])
fi)

popdivert()]])


###############################################################################
##
## STANDARD_UNITTESTS
## Install all standard unitests built as part of a DDK distribution.
##
## No parameters.
##
###############################################################################
define([[STANDARD_UNITTESTS]], [[pushdivert(52)dnl
	# Install the standard unittests
	#
ifelse(MBX_GPL,1,[[UNITTEST(mbxdaemon)]])dnl
UNITTEST(services_test)dnl
UNITTEST(pvr2d_test)dnl
UNITTEST(power_test)dnl

ifelse(PDUMP2,1,
 [[UNITTEST(pdump)]])dnl

GLESUNITTEST(egl_test)dnl
GLESUNITTEST(glinfo)dnl
ifelse(SUPPORT_OPENVG,1,[[UNITTEST(ovg_demos)]])dnl
ifelse(SUPPORT_OPENVG,1,[[UNITTEST(ovg_test)]])dnl
ifelse(SUPPORT_XWS,1,[[
GLESUNITTEST(xegltest)dnl
UNITTEST(xtest)dnl
ifelse(SUPPORT_OPENVG,1,[[UNITTEST(xovg_test)]])dnl
]])dnl
ifdef([[_BUFFER_CLASS_DEVICE_]],
	if [ -z "$NO_BCDEVICE" ]; then
	UNITTEST(sw_camera_ctrl)dnl
	GLESUNITTEST(ogles_camera_ext)dnl
fi)

popdivert()]])


###############################################################################
##
## STANDARD_KERNEL_MODULES
## Install all regular kernel modules - the PVR serviecs module (includes the
## MBX driver) and the display driver, if one is defined.  We also do a run of
## depmod(8) if the mode requires.
##
## No parameters.
##
###############################################################################
define([[STANDARD_KERNEL_MODULES]], [[pushdivert(52)dnl
	# Check the kernel module directory is there
	#
	if [ ! -d "${DISCIMAGE}MOD_ROOTDIR" ]; then
		echo ""
		echo "Can't find MOD_ROOTDIR on file system installation root"
		echo -n "There is no kernel module area setup yet. "
		if [ "$from" = target ]; then
			echo "On your build machine you should invoke:"
			echo
			echo " $ cd \$KERNELDIR"
			echo " $ make INSTALL_MOD_PATH=\$DISCIMAGE modules_install"
		else
			echo "You should invoke:"
			echo
			echo " $ cd $KERNELDIR"
			echo " $ make INSTALL_MOD_PATH=$DISCIMAGE modules_install"
		fi
		echo
		exit 1;
	fi

	# Install the standard kernel modules
	# Touch some files that might not exist so that busybox/modprobe don't complain
	#
	ifelse(TARGET_HAS_DEPMOD,FALSE,[[cp MODULES_DEP MODULES_DEP[[.old]]
]])dnl

ifelse(MBX_GPL,1,
	[[KERNEL_MODULE(mbxaccess.KM_SUFFIX)]],
	[[KERNEL_MODULE(pvr.KM_SUFFIX)]]
)dnl

ifelse(PDUMP2,1,
 [[KERNEL_MODULE(dbgdrv.KM_SUFFIX)]])dnl

ifelse(MBX_GPL,1,
	[[DISPLAY_CONTROLLER_MODULE(_DISPLAY_CONTROLLER_[[.]]KM_SUFFIX, [[mbxaccess.KM_SUFFIX]])]],
	[[DISPLAY_CONTROLLER_MODULE(_DISPLAY_CONTROLLER_[[.]]KM_SUFFIX, [[pvr.KM_SUFFIX]])]]
)dnl

ifelse(MBX_GPL,1,
	[[BUFFERCLASS_DEVICE_MODULE(_BUFFER_CLASS_DEVICE_[[.]]KM_SUFFIX, [[mbxaccess.KM_SUFFIX]])]],
	[[BUFFERCLASS_DEVICE_MODULE(_BUFFER_CLASS_DEVICE_[[.]]KM_SUFFIX, [[pvr.KM_SUFFIX]])]]
)dnl


ifelse(TARGET_RUNS_DEPMOD_AT_BOOT,TRUE,,TARGET_HAS_DEPMOD,TRUE,[[dnl
	if [ "$host" = 0 ]; then
		PATH_DEPMOD -a -b ${DISCIMAGE} KERNEL_ID
	fi
]])dnl
	$DOIT touch LIBMODPROBE_CONF
	$DOIT touch MODULES_CONF
	$DOIT rm -f MODULES_TMP

popdivert()]])


###############################################################################
##
## FREEDESKTOP -
## Record where the freedesktop is installed and place the function header
## and trailer into the install script.
##
## Parameters:
## $1 - the base directory for X11 installation - normally /usr/X11R6.
##
###############################################################################
define([[FREEDESKTOP]], [[define([[_FREEDESKTOPROOT]], $1)
pushdivert(54)dnl
install_X()
{
popdivert

pushdivert(56)dnl
}

popdivert
pushmain()dnl
[ -z "$NO_X" ] && install_X
popdivert
]])


###############################################################################
##
## FREEDESKTOP_XSERVERS -
## Install the Xmbx and Xfbdev (or whatever is required) X servers.
##
## No parameters.
##
###############################################################################
define([[FREEDESKTOP_XSERVERS]], [[pushdivert(55)dnl
	# Install X11 server binaries
	#
	for file in ./freedesktop/bin/X*; do
		install_file $file _FREEDESKTOPROOT/bin/`basename $file` "X11 server" a+x
	done
	$DOIT rm -f ${DISCIMAGE}_FREEDESKTOPROOT/bin/X && $DOIT ln -s ./Xmbx ${DISCIMAGE}_FREEDESKTOPROOT/bin/X

popdivert()]])


###############################################################################
##
## FREEDESKTOP_LIBRARIES -
## Install the shared libraries required for the X environment.
##
## No parameters.
##
###############################################################################
define([[FREEDESKTOP_LIBRARIES]], [[pushdivert(55)dnl
	# Install libraries for run-time and developer support
	#
	install_tree ./freedesktop/lib _FREEDESKTOPROOT/lib "X11 libraries"

popdivert()]])


###############################################################################
##
## FREEDESKTOP_XSERVERS_LIBS -
## Install the shared libraries required only for the X servers.
##
## No parameters.
##
###############################################################################
define([[FREEDESKTOP_XSERVERS_LIBS]], [[pushdivert(55)dnl
	# Install libraries for X server run-time
	#
	for file in libXdmcp.so.0 libXau.so.0 libXfont.so.1 libfontcache.so.0; do
		install_file ./freedesktop/lib/$file _FREEDESKTOPROOT/lib/$file
	done

popdivert()]])


###############################################################################
##
## FREEDESKTOP_HEADERS -
## Install the header files for developers.
##
## No parameters.
##
###############################################################################
define([[FREEDESKTOP_HEADERS]], [[pushdivert(55)dnl
	# Install headers for developers
	#
	install_tree ./freedesktop/include _FREEDESKTOPROOT/include "X11 header files"

popdivert()]])


###############################################################################
##
## FREEDESKTOP_MISC -
## Install any miscellaneous support/doc files. (/usr/X11R6/share etc.)
##
## No parameters.
##
###############################################################################
define([[FREEDESKTOP_MISC]], [[pushdivert(55)dnl
	# Install miscellaneous X bits
	#
	install_tree ./freedesktop/man _FREEDESKTOPROOT/man "X11 manual pages"
	install_tree ./freedesktop/share _FREEDESKTOPROOT/share "X11 miscellaneous files"

popdivert()]])


###############################################################################
##
## FREEDESKTOP_APPS -
## Install any X11 applications in /usr/X11R6/bin.
##
## No parameters.
##
###############################################################################
define([[FREEDESKTOP_APPS]], [[pushdivert(55)dnl
	# Install X11 application binaries
	#
	for file in ./freedesktop/bin/[^X]*; do
		install_file $file _FREEDESKTOPROOT/bin/$file "X11 applications" a+x
	done

popdivert()]])


###############################################################################
##
## FREEDESKTOP_SUPPORT -
## Install any non-X stuff that X needs - this is zlib and such like.
##
## No parameters.
##
###############################################################################
define([[FREEDESKTOP_SUPPORT]], [[pushdivert(55)dnl
	# Install support packages
	#
	[ -d ./packages/include ] &&
		install_tree ./packages/include /usr/include "support package header files"
	[ -d ./packages/lib ] &&
		install_tree ./packages/lib /usr/lib "support package libraries"
	[ -d ./packages/bin ] &&
		install_tree ./packages/bin /usr/bin "support package binaries"
	[ -d ./packages/share ] &&
		install_tree ./packages/share /usr/share "support package miscellania"

popdivert()]])


###############################################################################
##
## FREEDESTOP_ALL -
## This is a short cut to install freedesktop stuff. We don't copy the
## applications because there aren't any at the moment.
##
## No parameters.
##
###############################################################################
define([[FREEDESKTOP_ALL]], [[pushdivert(55)dnl
FREEDESKTOP_XSERVERS()dnl
FREEDESKTOP_LIBRARIES()dnl
FREEDESKTOP_HEADERS()dnl
FREEDESKTOP_MISC()dnl
FREEDESKTOP_SUPPORT()dnl
popdivert()]])
