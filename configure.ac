# Initialize autoconf.
AC_PREREQ([2.60])
AC_INIT([libdivecomputer], [0.1])
AC_CONFIG_SRCDIR([config.h.in])
AC_CONFIG_HEADERS([config.h])

# Initialize automake.
AM_INIT_AUTOMAKE([-Wall -Werror foreign])

# Initialize libtool.
LT_PREREQ([2.2.0])
LT_INIT([win32-dll])

# Checks for programs.
AC_PROG_CC
AC_PROG_CC_C99

# Enable automake silent build rules.
m4_ifdef([AM_SILENT_RULES],[AM_SILENT_RULES([yes])])

# Checks for native Windows.
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

# Checks for IrDA support.
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

# Checks for library functions.
AC_CHECK_FUNCS([localtime_r gmtime_r])

AC_CONFIG_FILES([
   libdivecomputer.pc
   Makefile
   src/Makefile
   examples/Makefile
])
AC_OUTPUT
