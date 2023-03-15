LOCAL_PATH := $(call my-dir)/../..

include $(CLEAR_VARS)
LOCAL_MODULE := libdivecomputer
LOCAL_CFLAGS := -DENABLE_LOGGING -DHAVE_VERSION_SUFFIX -DHAVE_PTHREAD_H -DHAVE_STRERROR_R -DHAVE_CLOCK_GETTIME -DHAVE_LOCALTIME_R -DHAVE_GMTIME_R -DHAVE_TIMEGM -DHAVE_STRUCT_TM_TM_GMTOFF
LOCAL_C_INCLUDES := $(LOCAL_PATH)/include
LOCAL_SRC_FILES := \
	src/aes.c \
	src/array.c \
	src/atomics_cobalt.c \
	src/atomics_cobalt_parser.c \
	src/bluetooth.c \
	src/buffer.c \
	src/checksum.c \
	src/citizen_aqualand.c \
	src/citizen_aqualand_parser.c \
	src/cochran_commander.c \
	src/cochran_commander_parser.c \
	src/common.c \
	src/context.c \
	src/cressi_edy.c \
	src/cressi_edy_parser.c \
	src/cressi_goa.c \
	src/cressi_goa_parser.c \
	src/cressi_leonardo.c \
	src/cressi_leonardo_parser.c \
	src/custom.c \
	src/datetime.c \
	src/deepblu_cosmiq.c \
	src/deepblu_cosmiq_parser.c \
	src/deepsix_excursion.c \
	src/deepsix_excursion_parser.c \
	src/descriptor.c \
	src/device.c \
	src/diverite_nitekq.c \
	src/diverite_nitekq_parser.c \
	src/divesoft_freedom.c \
	src/divesoft_freedom_parser.c \
	src/divesystem_idive.c \
	src/divesystem_idive_parser.c \
	src/hdlc.c \
	src/hw_frog.c \
	src/hw_ostc3.c \
	src/hw_ostc.c \
	src/hw_ostc_parser.c \
	src/ihex.c \
	src/iostream.c \
	src/irda.c \
	src/iterator.c \
	src/liquivision_lynx.c \
	src/liquivision_lynx_parser.c \
	src/mares_common.c \
	src/mares_darwin.c \
	src/mares_darwin_parser.c \
	src/mares_iconhd.c \
	src/mares_iconhd_parser.c \
	src/mares_nemo.c \
	src/mares_nemo_parser.c \
	src/mares_puck.c \
	src/mclean_extreme.c \
	src/mclean_extreme_parser.c \
	src/oceanic_atom2.c \
	src/oceanic_atom2_parser.c \
	src/oceanic_common.c \
	src/oceanic_veo250.c \
	src/oceanic_veo250_parser.c \
	src/oceanic_vtpro.c \
	src/oceanic_vtpro_parser.c \
	src/oceans_s1.c \
	src/oceans_s1_common.c \
	src/oceans_s1_parser.c \
	src/packet.c \
	src/parser.c \
	src/platform.c \
	src/rbstream.c \
	src/reefnet_sensus.c \
	src/reefnet_sensus_parser.c \
	src/reefnet_sensuspro.c \
	src/reefnet_sensuspro_parser.c \
	src/reefnet_sensusultra.c \
	src/reefnet_sensusultra_parser.c \
	src/ringbuffer.c \
	src/seac_screen.c \
	src/seac_screen_parser.c \
	src/serial_posix.c \
	src/shearwater_common.c \
	src/shearwater_petrel.c \
	src/shearwater_predator.c \
	src/shearwater_predator_parser.c \
	src/socket.c \
	src/sporasub_sp2.c \
	src/sporasub_sp2_parser.c \
	src/suunto_common2.c \
	src/suunto_common.c \
	src/suunto_d9.c \
	src/suunto_d9_parser.c \
	src/suunto_eon.c \
	src/suunto_eon_parser.c \
	src/suunto_eonsteel.c \
	src/suunto_eonsteel_parser.c \
	src/suunto_solution.c \
	src/suunto_solution_parser.c \
	src/suunto_vyper2.c \
	src/suunto_vyper.c \
	src/suunto_vyper_parser.c \
	src/tecdiving_divecomputereu.c \
	src/tecdiving_divecomputereu_parser.c \
	src/timer.c \
	src/usb.c \
	src/usbhid.c \
	src/uwatec_aladin.c \
	src/uwatec_memomouse.c \
	src/uwatec_memomouse_parser.c \
	src/uwatec_smart.c \
	src/uwatec_smart_parser.c \
	src/version.c \
	src/zeagle_n2ition3.c
include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := dctool
LOCAL_SHARED_LIBRARIES := libdivecomputer
LOCAL_CFLAGS := -DHAVE_UNISTD_H -DHAVE_GETOPT_H -DHAVE_GETOPT_LONG -DHAVE_DECL_OPTRESET=1
LOCAL_C_INCLUDES := $(LOCAL_PATH)/include
LOCAL_SRC_FILES := \
	examples/common.c \
	examples/dctool.c \
	examples/dctool_download.c \
	examples/dctool_dump.c \
	examples/dctool_fwupdate.c \
	examples/dctool_help.c \
	examples/dctool_list.c \
	examples/dctool_parse.c \
	examples/dctool_read.c \
	examples/dctool_scan.c \
	examples/dctool_timesync.c \
	examples/dctool_version.c \
	examples/dctool_write.c \
	examples/output.c \
	examples/output_raw.c \
	examples/output_xml.c \
	examples/utils.c
include $(BUILD_EXECUTABLE)
