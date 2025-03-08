AC_PREREQ(2.59)

AC_INIT(NetworkManager-fortisslvpn,
        1.4.1,
        https://gitlab.gnome.org/GNOME/NetworkManager-fortisslvpn/issues,
        NetworkManager-fortisslvpn)
AM_INIT_AUTOMAKE([1.9 no-dist-gzip dist-xz -Wno-portability])
AM_MAINTAINER_MODE
AM_SILENT_RULES([yes])

AC_USE_SYSTEM_EXTENSIONS
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_HEADERS([config.h])

AC_USE_SYSTEM_EXTENSIONS

dnl
dnl Require programs
dnl
AC_PROG_CC
AM_PROG_CC_C_O
AC_PROG_INSTALL
AC_PROG_CPP
AC_PROG_EGREP
LT_INIT([disable-static])
AC_PATH_PROG(GLIB_COMPILE_RESOURCES, glib-compile-resources)
PKG_PROG_PKG_CONFIG()


AC_CHECK_PROG([has_file], file, yes, no)
if test x$has_file = xno ; then
    AC_MSG_ERROR(["file" utility not found.])
fi

AC_CHECK_PROG([has_find], find, yes, no)
if test x$has_find = xno ; then
    AC_MSG_ERROR(["find" utility not found.])
fi

dnl
dnl Required headers
dnl
AC_CHECK_HEADERS(fcntl.h paths.h stdarg.h stdbool.h sys/ioctl.h sys/time.h syslog.h unistd.h)

AC_CHECK_HEADERS(pppd/pppd.h,,
  AC_MSG_ERROR(couldn't find pppd.h. pppd development headers are required.))

dnl
dnl Check the presense of other pppd/*.h files
AC_CHECK_HEADERS(pppd/chap.h pppd/chap-new.h pppd/chap_ms.h)

dnl
dnl Versions >= 2.5.0 will have pkg-config support
PKG_CHECK_EXISTS([pppd],
    [AS_VAR_SET([pppd_pkgconfig_support],[yes])])

dnl
dnl Get the version of pppd using pkg-config, assume 2.4.9 if not present
PPPD_VERSION=2.4.5
if test x"$pppd_pkgconfig_support" = xyes; then
    PPPD_VERSION=`$PKG_CONFIG --modversion pppd`
fi


AC_ARG_WITH([pppd-plugin-dir], AS_HELP_STRING([--with-pppd-plugin-dir=DIR], [path to the pppd plugins directory]))

if test -n "$with_pppd_plugin_dir" ; then
	PPPD_PLUGIN_DIR="$with_pppd_plugin_dir"
else
	PPPD_PLUGIN_DIR="${libdir}/pppd/$PPPD_VERSION"
fi
AC_SUBST(PPPD_PLUGIN_DIR)

dnl The version of pppd dictates what code can be included, i.e. enable use of
dnl   #if WITH_PPP_VERSION >= PPP_VERSION(2,5,0) in the code
AC_DEFINE_UNQUOTED([PPP_VERSION(x,y,z)],
    [((x & 0xFF) << 16 | (y & 0xFF) << 8 | (z & 0xFF) << 0)],
    [Macro to help determine the particular version of pppd])
PPP_VERSION=$(echo $PPPD_VERSION | sed -e "s/\./\,/g")
AC_DEFINE_UNQUOTED(WITH_PPP_VERSION, PPP_VERSION($PPP_VERSION),
    [The real version of pppd represented as an int])

dnl
dnl Checks for typedefs, structures, and compiler characteristics.
dnl
AC_TYPE_MODE_T
AC_TYPE_PID_T

dnl
dnl Checks for library functions.
dnl
AC_PROG_GCC_TRADITIONAL
AC_FUNC_MEMCMP
AC_CHECK_FUNCS(select socket uname)

GIT_SHA_RECORD(NM_GIT_SHA)

AC_SEARCH_LIBS([dlopen], [dl dld], [], [ac_cv_search_dlopen=])
AC_SUBST([DL_LIBS], "$ac_cv_search_dlopen")

dnl
dnl GNOME support
dnl
AC_ARG_WITH(gnome, AS_HELP_STRING([--without-gnome], [Build NetworkManager-fortisslvpn without GNOME support, e.g. vpn service only]), [], [with_gnome_specified=no])
AC_ARG_WITH(gtk4, AS_HELP_STRING([--with-gtk4], [Build NetworkManager-fortisslvpn with libnma-gtk4 support]), [], [with_gtk4_specified=no])
if test "$with_gnome" != no; then
	with_gnome=yes
fi
if test "$with_gtk4_specified" == no; then
	with_gtk4=no
fi
if test "$with_gtk4" != yes; then
	with_gtk4=no
fi
AM_CONDITIONAL(WITH_GNOME, test "$with_gnome" != no)
AM_CONDITIONAL(WITH_GTK4, test "$with_gtk4" != no)

AC_ARG_ENABLE(absolute-paths, AS_HELP_STRING([--enable-absolute-paths], [Use absolute paths to in .name files. Useful for development. (default is no)]))

GETTEXT_PACKAGE=NetworkManager-fortisslvpn
AC_SUBST(GETTEXT_PACKAGE)
AC_DEFINE_UNQUOTED(GETTEXT_PACKAGE,"$GETTEXT_PACKAGE", [Gettext package])

AM_GNU_GETTEXT([external])
AM_GNU_GETTEXT_VERSION([0.19])

# gio-unix will require glib and gio
PKG_CHECK_MODULES(GLIB, gio-unix-2.0 >= 2.32)
AC_SUBST(GLIB_CFLAGS)
AC_SUBST(GLIB_LIBS)
GLIB_CFLAGS="$GLIB_CFLAGS -DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_32"
GLIB_CFLAGS="$GLIB_CFLAGS -DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_32"

if test x"$with_gnome" != xno || test x"$with_gtk4" != xno; then
	PKG_CHECK_MODULES(LIBSECRET, libsecret-1 >= 0.18, [], [
		dnl We use the secret service API that went stable in 0.18
		PKG_CHECK_MODULES(LIBSECRET, libsecret-unstable)
		LIBSECRET_CFLAGS="$LIBSECRET_CFLAGS -DSECRET_API_SUBJECT_TO_CHANGE"
	])
fi

if test x"$with_gnome" != xno; then
	PKG_CHECK_MODULES(GTK, gtk+-3.0 >= 3.4)
	GTK_CFLAGS="$GTK_CFLAGS -DGDK_VERSION_MIN_REQUIRED=GDK_VERSION_3_4"
	GTK_CFLAGS="$GTK_CFLAGS -DGDK_VERSION_MAX_ALLOWED=GDK_VERSION_3_4"
	PKG_CHECK_MODULES(LIBNMA, libnma >= 1.8.33)
fi

if test x"$with_gtk4" != xno; then
	PKG_CHECK_MODULES(GTK4, gtk4 >= 4.0)
	GTK4_CFLAGS="$GTK4_CFLAGS -DGDK_VERSION_MIN_REQUIRED=GDK_VERSION_4_0"
	GTK4_CFLAGS="$GTK4_CFLAGS -DGDK_VERSION_MAX_ALLOWED=GDK_VERSION_4_0"
	PKG_CHECK_MODULES(LIBNMA_GTK4, libnma-gtk4 >= 1.8.33)
fi

PKG_CHECK_MODULES(LIBNM, libnm >= 1.2.0)
LIBNM_CFLAGS="$LIBNM_CFLAGS -DNM_VERSION_MIN_REQUIRED=NM_VERSION_1_2"
LIBNM_CFLAGS="$LIBNM_CFLAGS -DNM_VERSION_MAX_ALLOWED=NM_VERSION_1_2"

NM_VPN_SERVICE_DIR=`$PKG_CONFIG --define-variable prefix='\${prefix}' --variable vpnservicedir libnm`
AC_SUBST(NM_VPN_SERVICE_DIR)

NM_COMPILER_WARNINGS([yes])

NM_PLUGIN_DIR="$libdir/NetworkManager"
AC_SUBST(NM_PLUGIN_DIR)
if test x"$enable_absolute_paths" == x"yes"; then
    NM_PLUGIN_DIR_NAME_FILE="$NM_PLUGIN_DIR/"
else
    enable_absolute_paths=no
    NM_PLUGIN_DIR_NAME_FILE=""
fi
AC_SUBST(NM_PLUGIN_DIR_NAME_FILE)

dnl
dnl Distribution version string
dnl
AC_ARG_WITH(dist-version, AS_HELP_STRING([--with-dist-version=<dist-version>], [Define the custom version (like distribution package name and revision)]), ac_distver=$withval, ac_distver="")
if ! test x"$ac_distver" = x""; then
  AC_DEFINE_UNQUOTED(DIST_VERSION, "$ac_distver", [Define the distribution version string])
fi

AC_CONFIG_FILES([
Makefile
po/Makefile.in
])
AC_OUTPUT

echo ""
echo "Build configuration: "
echo "  --with-dist-version=$ac_distver"
echo "  --with-gnome=$with_gnome"
echo "  --with-gtk4=$with_gtk4"
echo "  --with-pppd-plugin-dir=$PPPD_PLUGIN_DIR"
echo "  --enable-absolute-paths=$enable_absolute_paths"
echo "  --enable-more-warnings=$set_more_warnings"
