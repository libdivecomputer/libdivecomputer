MANPAGES = \
	dc_buffer_append.3 \
	dc_buffer_free.3 \
	dc_buffer_get_data.3 \
	dc_buffer_get_size.3 \
	dc_buffer_new.3 \
	dc_buffer_prepend.3 \
	dc_context_free.3 \
	dc_context_new.3 \
	dc_context_set_logfunc.3 \
	dc_context_set_loglevel.3 \
	dc_datetime_gmtime.3 \
	dc_datetime_localtime.3 \
	dc_datetime_mktime.3 \
	dc_datetime_now.3 \
	dc_descriptor_free.3 \
	dc_descriptor_get_model.3 \
	dc_descriptor_get_product.3 \
	dc_descriptor_get_vendor.3 \
	dc_descriptor_iterator.3 \
	dc_device_close.3 \
	dc_device_foreach.3 \
	dc_device_open.3 \
	dc_device_set_cancel.3 \
	dc_device_set_events.3 \
	dc_device_set_fingerprint.3 \
	dc_iterator_free.3 \
	dc_iterator_next.3 \
	dc_parser_destroy.3 \
	dc_parser_get_datetime.3 \
	dc_parser_get_field.3 \
	dc_parser_new.3 \
	dc_parser_samples_foreach.3 \
	dc_parser_set_data.3 \
	libdivecomputer.3

HTMLPAGES = $(MANPAGES:%=%.html)

dist_man_MANS = $(MANPAGES)

if HAVE_MANDOC
doc_DATA = $(HTMLPAGES)
endif

SUFFIXES = .3 .3.html

.3.3.html:
	$(AM_V_GEN) $(MANDOC) -Thtml -Ostyle=mandoc.css,man=%N.%S.html $< > $@

CLEANFILES = $(HTMLPAGES)
