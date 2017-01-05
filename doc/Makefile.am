SUBDIRS = man

doxygen: doxygen.cfg
if HAVE_DOXYGEN
	$(DOXYGEN) $(DOXYFLAGS) $<
	touch $@
endif

all-local: doxygen

clean-local:
	-$(RM) -rf html latex

CLEANFILES = doxygen
