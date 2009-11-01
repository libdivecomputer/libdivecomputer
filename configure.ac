AC_INIT([libdivecomputer], [0.1])
AM_INIT_AUTOMAKE([-Wall -Werror foreign])
AC_PROG_CC
AC_PROG_CC_C99
AC_LIBTOOL_WIN32_DLL
AC_PROG_LIBTOOL

m4_ifdef([AM_SILENT_RULES],[AM_SILENT_RULES([yes])])

AC_MSG_CHECKING([for native Win32])
case "$host" in
  *-*-mingw*)
    os_win32=yes
    ;;
  *)
    os_win32=no
    ;;
esac
AC_MSG_RESULT([$os_win32])
AM_CONDITIONAL([OS_WIN32], [test "$os_win32" = "yes"])

AC_CHECK_HEADERS([winsock2.h af_irda.h], [irda_win32=yes], [irda_win32=no], [
#if HAVE_WINSOCK2_H
#   include <winsock2.h>
# endif
])

AC_CHECK_HEADERS([sys/socket.h linux/types.h linux/irda.h], [irda_linux=yes], [irda_linux=no], [
#if HAVE_SYS_SOCKET_H
#   include <sys/socket.h>
# endif
#if HAVE_LINUX_TYPES_H
#   include <linux/types.h>
# endif
])

AM_CONDITIONAL([IRDA], [test "$irda_win32" = "yes" || test "$irda_linux" = "yes"])
AM_CONDITIONAL([IRDA_WIN32], [test "$irda_win32" = "yes"])
AM_CONDITIONAL([IRDA_LINUX], [test "$irda_linux" = "yes"])

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([
   libdivecomputer.pc
   Makefile
   src/Makefile
   examples/Makefile
])
AC_OUTPUT
