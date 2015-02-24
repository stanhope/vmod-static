AM_CPPFLAGS = @VMOD_INCLUDES@

vmoddir = @VMOD_DIR@
vmod_LTLIBRARIES = libvmod_static.la

libvmod_static_la_LDFLAGS = -module -export-dynamic -avoid-version -shared -lmagic -lJudy

libvmod_static_la_SOURCES = \
	vcc_if.c \
	vcc_if.h \
	vmod_static.c

vcc_if.c vcc_if.h: @VMODTOOL@ $(top_srcdir)/src/vmod_static.vcc
	@VMODTOOL@ $(top_srcdir)/src/vmod_static.vcc

EXTRA_DIST = \
	vmod_static.vcc

CLEANFILES = $(builddir)/vcc_if.c $(builddir)/vcc_if.h
