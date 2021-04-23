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
	dc_descriptor_get_transports.3 \
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
	dc_bluetooth_open.3 \
	dc_bluetooth_iterator_new.3 \
	dc_bluetooth_device_get_address.3 \
	dc_bluetooth_device_get_name.3 \
	dc_bluetooth_addr2str.3 \
	dc_bluetooth_str2addr.3 \
	dc_bluetooth_device_free.3 \
	dc_usbhid_open.3 \
	dc_usbhid_device_get_pid.3 \
	dc_usbhid_device_get_vid.3 \
	dc_usbhid_iterator_new.3 \
	dc_usbhid_device_free.3 \
	dc_serial_open.3 \
	dc_serial_device_get_name.3 \
	dc_serial_iterator_new.3 \
	dc_serial_device_free.3 \
	dc_irda_open.3 \
	dc_irda_device_get_name.3 \
	dc_irda_device_get_address.3 \
	dc_irda_iterator_new.3 \
	dc_irda_device_free.3 \
	dc_iostream_close.3 \
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
