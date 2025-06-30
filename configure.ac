# Versioning.
m4_define([dc_version_major],[0])
m4_define([dc_version_minor],[9])
m4_define([dc_version_micro],[0])
m4_define([dc_version_suffix],[])
m4_define([dc_version],dc_version_major.dc_version_minor.dc_version_micro[]m4_ifset([dc_version_suffix],-[dc_version_suffix]))

# Libtool versioning.
m4_define([dc_version_lt_current],[0])
m4_define([dc_version_lt_revision],[0])
m4_define([dc_version_lt_age],[0])

# Initialize autoconf.
AC_PREREQ([2.60])
AC_INIT([libdivecomputer],[dc_version])
AC_CONFIG_SRCDIR([config.h.in])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])

# Initialize automake.
AM_INIT_AUTOMAKE([-Wall -Werror foreign])

# Fix for automake >= 1.12
m4_ifdef([AM_PROG_AR], [AM_PROG_AR])

# Initialize libtool.
LT_PREREQ([2.2.0])
LT_INIT([win32-dll])
LT_PROG_RC

# Logging support.
AC_ARG_ENABLE([logging],
	[AS_HELP_STRING([--enable-logging=@<:@yes/no@:>@],
		[Enable logging @<:@default=yes@:>@])],
	[], [enable_logging=yes])
AS_IF([test "x$enable_logging" = "xyes"], [
	AC_DEFINE(ENABLE_LOGGING, [1], [Enable logging.])
])

# Pseudo terminal support.
AC_ARG_ENABLE([pty],
	[AS_HELP_STRING([--enable-pty=@<:@yes/no@:>@],
		[Enable pseudo terminal support @<:@default=no@:>@])],
	[], [enable_pty=no])
AS_IF([test "x$enable_pty" = "xyes"], [
	AC_DEFINE(ENABLE_PTY, [1], [Enable pseudo terminal support.])
])

# Example applications.
AC_ARG_ENABLE([examples],
	[AS_HELP_STRING([--enable-examples=@<:@yes/no@:>@],
		[Build example applications @<:@default=yes@:>@])],
	[], [enable_examples=yes])
AM_CONDITIONAL([ENABLE_EXAMPLES], [test "x$enable_examples" = "xyes"])

# Documentation.
AC_ARG_ENABLE([doc],
	[AS_HELP_STRING([--enable-doc=@<:@yes/no@:>@],
		[Build documentation @<:@default=yes@:>@])],
	[], [enable_doc=yes])
AM_CONDITIONAL([ENABLE_DOC], [test "x$enable_doc" = "xyes"])

# Checks for programs.
AC_PROG_CC
AC_PROG_CC_C99
AC_CHECK_PROGS([DOXYGEN], [doxygen])
AC_CHECK_PROGS([MANDOC], [mandoc])

AM_CONDITIONAL([HAVE_DOXYGEN],[test -n "$DOXYGEN"])
AM_CONDITIONAL([HAVE_MANDOC],[test -n "$MANDOC"])

# Enable automake silent build rules.
m4_ifdef([AM_SILENT_RULES],[AM_SILENT_RULES([yes])])

# Checks for operating system.
AC_MSG_CHECKING([for operating system])
case "$host" in
  *-*-mingw*)
    platform=windows
    ;;
  *-*-darwin*)
    platform=mac
    ;;
  *)
    platform=default
    ;;
esac
AC_MSG_RESULT([$platform])
AM_CONDITIONAL([OS_WIN32], [test "$platform" = "windows"])

DEPENDENCIES=""

# Checks for USB support.
AC_ARG_WITH([libusb],
	[AS_HELP_STRING([--without-libusb],
		[Build without the libusb library])],
	[], [with_libusb=auto])
AS_IF([test "x$with_libusb" != "xno"], [
	PKG_CHECK_MODULES([LIBUSB], [libusb-1.0], [have_libusb=yes], [have_libusb=no])
	AS_IF([test "x$have_libusb" = "xyes"], [
		AC_DEFINE([HAVE_LIBUSB], [1], [libusb library])
		DEPENDENCIES="$DEPENDENCIES libusb-1.0"
	])
])

# Checks for HIDAPI support.
AC_ARG_WITH([hidapi],
	[AS_HELP_STRING([--without-hidapi],
		[Build without the hidapi library])],
	[], [with_hidapi=hidapi])
AS_IF([test "x$with_hidapi" != "xno"], [
	PKG_CHECK_MODULES([HIDAPI], [$with_hidapi], [have_hidapi=yes], [have_hidapi=no])
	AS_IF([test "x$have_hidapi" = "xyes"], [
		AC_DEFINE([HAVE_HIDAPI], [1], [hidapi library])
		DEPENDENCIES="$DEPENDENCIES $with_hidapi"
	])
])

# Checks for BlueZ (bluetooth) support.
AC_ARG_WITH([bluez],
	[AS_HELP_STRING([--without-bluez],
		[Build without the BlueZ library])],
	[], [with_bluez=auto])
AS_IF([test "x$with_bluez" != "xno"], [
	PKG_CHECK_MODULES([BLUEZ], [bluez], [have_bluez=yes], [have_bluez=no])
	AS_IF([test "x$have_bluez" = "xyes"], [
		AC_DEFINE([HAVE_BLUEZ], [1], [BlueZ library])
		DEPENDENCIES="$DEPENDENCIES bluez"
	])
])

AC_SUBST([DEPENDENCIES])

# Checks for Windows bluetooth support.
AC_CHECK_HEADERS([winsock2.h ws2bth.h], , , [
#if HAVE_WINSOCK2_H
#   include <winsock2.h>
# endif
])

# Checks for IrDA support.
AC_CHECK_HEADERS([winsock2.h af_irda.h], , , [
#if HAVE_WINSOCK2_H
#   include <winsock2.h>
# endif
])

AC_CHECK_HEADERS([sys/socket.h linux/types.h linux/irda.h], , , [
#if HAVE_SYS_SOCKET_H
#   include <sys/socket.h>
# endif
#if HAVE_LINUX_TYPES_H
#   include <linux/types.h>
# endif
])

# Checks for header files.
AC_CHECK_HEADERS([linux/serial.h])
AC_CHECK_HEADERS([IOKit/serial/ioss.h])
AC_CHECK_HEADERS([unistd.h getopt.h])
AC_CHECK_HEADERS([sys/param.h])
AC_CHECK_HEADERS([pthread.h])
AC_CHECK_HEADERS([mach/mach_time.h])

# Checks for global variable declarations.
AC_CHECK_DECLS([optreset])

# Checks for structures.
AC_CHECK_MEMBERS([struct tm.tm_gmtoff],,,[
#include <time.h>
])

# Checks for library functions.
AC_FUNC_STRERROR_R
AC_CHECK_FUNCS([localtime_r gmtime_r timegm _mkgmtime])
AC_CHECK_FUNCS([clock_gettime mach_absolute_time])
AC_CHECK_FUNCS([getopt_long])

# Checks for supported compiler options.
AX_APPEND_COMPILE_FLAGS([-Werror=unknown-warning-option],[ERROR_CFLAGS])
AX_APPEND_COMPILE_FLAGS([ \
	-pedantic \
	-Wall \
	-Wextra \
	-Wshadow \
	-Wrestrict \
	-Wformat=2 \
	-Wwrite-strings \
	-Wcast-qual \
	-Wpointer-arith \
	-Wstrict-prototypes \
	-Wmissing-prototypes \
	-Wmissing-declarations \
	-Wno-unused-parameter \
	-fmacro-prefix-map='$(top_srcdir)/'= \
],,[$ERROR_CFLAGS])

# Windows specific compiler options.
AS_IF([test "$platform" = "windows"], [
	AX_APPEND_COMPILE_FLAGS([-Wno-pedantic-ms-format])
])

# Versioning.
AC_SUBST([DC_VERSION],[dc_version])
AC_SUBST([DC_VERSION_MAJOR],[dc_version_major])
AC_SUBST([DC_VERSION_MINOR],[dc_version_minor])
AC_SUBST([DC_VERSION_MICRO],[dc_version_micro])
AC_SUBST([DC_VERSION_LIBTOOL],[dc_version_lt_current:dc_version_lt_revision:dc_version_lt_age])

# Version suffix.
m4_ifset([dc_version_suffix],[
	AC_DEFINE(HAVE_VERSION_SUFFIX, [1], [Define if a version suffix is present.])
])

# Supported transports
transport_serial="yes"
transport_usb="${have_libusb-no}"
if test "$have_hidapi" = "yes"; then
	transport_usbhid="yes"
elif test "$have_libusb" = "yes" && test "$platform" != "mac"; then
	transport_usbhid="yes"
else
	transport_usbhid="no"
fi
if test "$platform" = "windows"; then
	transport_irda="$ac_cv_header_af_irda_h"
	transport_bluetooth="$ac_cv_header_ws2bth_h"
else
	transport_irda="$ac_cv_header_linux_irda_h"
	transport_bluetooth="${have_bluez-no}"
fi
transport_ble="no"

AC_CONFIG_FILES([
   libdivecomputer.pc
   Makefile
   include/Makefile
   include/libdivecomputer/Makefile
   include/libdivecomputer/version.h
   src/Makefile
   doc/Makefile
   doc/doxygen.cfg
   doc/man/Makefile
   examples/Makefile
])
AC_OUTPUT
AC_MSG_NOTICE([
  $PACKAGE $VERSION
  ===============

  Compiler:

    CC      : $CC
    CFLAGS  : $CFLAGS
    LDFLAGS : $LDFLAGS

  Features:

    Logging              : $enable_logging
    Pseudo terminal      : $enable_pty
    Example applications : $enable_examples
    Documentation        : $enable_doc

  Transports:

    Serial    : $transport_serial
    USB       : $transport_usb
    USBHID    : $transport_usbhid
    IrDA      : $transport_irda
    Bluetooth : $transport_bluetooth
    BLE       : $transport_ble

  Building:

    Type 'make' to compile $PACKAGE.

    Type 'make install' to install $PACKAGE.
])
