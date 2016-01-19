AM_CFLAGS = \
	$(TDM_SPRD_CFLAGS) \
	-I$(top_srcdir)/src

libtdm_sprd_la_LTLIBRARIES = libtdm-sprd.la
libtdm_sprd_ladir = $(TDM_MODULE_PATH)
libtdm_sprd_la_LDFLAGS = -module -avoid-version
libtdm_sprd_la_LIBADD = $(TDM_SPRD_LIBS) -ldl -ldrm

libtdm_sprd_la_SOURCES = \
	tdm_sprd_format.c \
	tdm_sprd_display.c \
	tdm_sprd_pp.c \
	tdm_sprd.c
