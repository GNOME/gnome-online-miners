AC_INIT([GNOME Online Miners],
        [3.34.0],
        [https://gitlab.gnome.org/GNOME/gnome-online-miners/-/issues],
        [gnome-online-miners],
        [https://wiki.gnome.org/Projects/GnomeOnlineMiners])
AC_PREREQ(2.63)

AC_CONFIG_AUX_DIR([config])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_SRCDIR([src])

m4_ifdef([AM_SILENT_RULES],[AM_SILENT_RULES([yes])])

AM_INIT_AUTOMAKE([dist-xz no-dist-gzip tar-ustar])
AM_MAINTAINER_MODE([enable])

AC_PROG_CC
AC_PROG_CC_C_O
AC_HEADER_STDC

GDATA_MIN_VERSION=0.15.2
GFBGRAPH_MIN_VERSION=0.2.2
GLIB_MIN_VERSION=2.35.1
GOA_MIN_VERSION=3.13.3
GRILO_MIN_VERSION=0.3.0
ZAPOJIT_MIN_VERSION=0.0.2

AX_CHECK_ENABLE_DEBUG([yes],[GNOME_ENABLE_DEBUG])

LT_INIT

PKG_CHECK_MODULES(GLIB, [glib-2.0 >= $GLIB_MIN_VERSION])
PKG_CHECK_MODULES(GIO, [gio-2.0 gio-unix-2.0])

PKG_CHECK_MODULES(GOA, [goa-1.0 >= $GOA_MIN_VERSION])
AC_DEFINE([GOA_API_IS_SUBJECT_TO_CHANGE], [], [We are aware that GOA's API can change])

PKG_CHECK_MODULES(TRACKER, [tracker-miner-2.0 tracker-sparql-2.0])

# Facebook
AC_ARG_ENABLE([facebook], [AS_HELP_STRING([--enable-facebook], [Enable Facebook miner])], [], [enable_facebook=yes])
if test "$enable_facebook" != "no"; then
  PKG_CHECK_MODULES(GFBGRAPH, [libgfbgraph-0.2 >= $GFBGRAPH_MIN_VERSION])
fi
AM_CONDITIONAL(BUILD_FACEBOOK, [test x$enable_facebook != xno])

# Flickr
AC_ARG_ENABLE([flickr], [AS_HELP_STRING([--enable-flickr], [Enable Flickr miner])], [], [enable_flickr=yes])
if test "$enable_flickr" != "no"; then
  PKG_CHECK_MODULES(GRILO, [grilo-0.3 >= $GRILO_MIN_VERSION])
fi
AM_CONDITIONAL(BUILD_FLICKR, [test x$enable_flickr != xno])

# Google
AC_ARG_ENABLE([google], [AS_HELP_STRING([--enable-google], [Enable Google miner])], [], [enable_google=yes])
if test "$enable_google" != "no"; then
  PKG_CHECK_MODULES(GDATA, [libgdata >= $GDATA_MIN_VERSION])
fi
AM_CONDITIONAL(BUILD_GOOGLE, [test x$enable_google != xno])

# Media Server
AC_ARG_ENABLE([media-server], [AS_HELP_STRING([--enable-media-server],
                                              [Enable media server miner])],
                                              [],
                                              [enable_media_server=yes])
AM_CONDITIONAL(BUILD_MEDIA_SERVER, [test x$enable_media_server != xno])

# ownCloud
AC_ARG_ENABLE([owncloud], [AS_HELP_STRING([--enable-owncloud], [Enable ownCloud miner])], [], [enable_owncloud=yes])
AM_CONDITIONAL(BUILD_OWNCLOUD, [test x$enable_owncloud != xno])

# Windows Live
AC_ARG_ENABLE([windows-live], [AS_HELP_STRING([--enable-windows-live],
                                              [Enable Windows Live miner])],
                                              [],
                                              [enable_windows_live=yes])
if test "$enable_windows_live" != "no"; then
  PKG_CHECK_MODULES(ZAPOJIT, [zapojit-0.0 >= $ZAPOJIT_MIN_VERSION])
fi
AM_CONDITIONAL(BUILD_WINDOWS_LIVE, [test x$enable_windows_live != xno])

AC_CONFIG_FILES([
Makefile
data/Makefile
src/Makefile
])
AC_OUTPUT

echo "
               gnome-online-miners $VERSION
             ==============================

            Facebook miner:              ${enable_facebook}
            Flickr miner:                ${enable_flickr}
            Google miner:                ${enable_google}
            Media server miner:          ${enable_media_server}
            ownCloud miner:              ${enable_owncloud}
            Windows Live miner:          ${enable_windows_live}
"
