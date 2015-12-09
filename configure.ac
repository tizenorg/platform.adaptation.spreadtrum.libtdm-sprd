AC_PREREQ([2.60])
AC_INIT([libtdm-sprd],
        [1.0.0],
        [https://www.tizen.org],
        [libtdm-sprd])

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_SRCDIR([Makefile.am])
AM_INIT_AUTOMAKE([1.10 foreign dist-bzip2])
AM_MAINTAINER_MODE([enable])

# Check for programs
AC_PROG_CC

AC_USE_SYSTEM_EXTENSIONS
AC_SYS_LARGEFILE
AC_FUNC_ALLOCA

# Initialize libtool
LT_PREREQ([2.2])
LT_INIT([disable-static])

# Enable quiet compiles on automake 1.11.
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

PKG_CHECK_MODULES(TDM_SPRD, libtdm libtbm libdrm libdrm_sprd)
PKG_CHECK_MODULES(UDEV, libudev, [udev=yes], [udev=no])
if test x"$udev" = xyes; then
	AC_DEFINE(HAVE_UDEV,1,[Enable udev-based monitor hotplug detection])
	TDM_SPRD_CFLAGS="$TDM_SPRD_CFLAGS $UDEV_CFLAGS"
	TDM_SPRD_LIBS="$TDM_SPRD_LIBS $UDEV_LIBS"
fi

AC_SUBST(TDM_SPRD_CFLAGS)
AC_SUBST(TDM_SPRD_LIBS)

# set the dir for the tbm module
DEFAULT_TDM_MODULE_PATH="${libdir}/tdm"
AC_ARG_WITH(tdm-module-path, AS_HELP_STRING([--with-tdm-module-path=PATH], [tdm module dir]),
				[ TDM_MODULE_PATH="$withval" ],
				[ TDM_MODULE_PATH="${DEFAULT_TDM_MODULE_PATH}" ])
AC_SUBST(TDM_MODULE_PATH)

# For enumerating devices in test case
AC_OUTPUT([
	Makefile
	src/Makefile])

echo ""
echo "$PACKAGE_STRING will be compiled with:"
echo ""
echo "TDM_SPRD_CFLAGS : $TDM_SPRD_CFLAGS"
echo "TDM_SPRD_LIBS   : $TDM_SPRD_LIBS"
echo "TDM_MODULE_DIR  : $TDM_MODULE_PATH"
echo ""
