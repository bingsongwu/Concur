#
# Concur 2.0 Library Makefile
#

## process this file with automake to produce Makefile.in

EXTRA_DIST = README LICENSE

AUTOMAKE_OPTIONS = 

INCLUDES = $(LIBINCLUDES)

lib_LTLIBRARIES = libconcur.la libccmalloc.la 

libconcur_la_SOURCES = concur.c concur_plat.c

libccmalloc_la_SOURCES = concur_malloc.c

include_HEADERS = concur.h concur_internal.h concur_plat.h concur_queue.h

libraries: $(LIBRARIES) $(HEADERS)
	$(MAKE) install-libLTLIBRARIES install-includeHEADERS

