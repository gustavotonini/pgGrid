#-------------------------------------------------------------------------
#
# Makefile for src/pgcluster/pgrp
#
#-------------------------------------------------------------------------

subdir = src/pgcluster/tool
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

# EXTRA_OBJS = $(top_builddir)/src/backend/libpq/replicate_com.o

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS) -DBINDIR=\"$(bindir)\"

all: pgcbench bwmeasure

pgcbench: pgcbench.o $(libpq_builddir)/libpq.a 
	$(CC) $(CFLAGS) pgcbench.o $(EXTRA_OBJS) $(libpq) $(LDFLAGS) $(LIBS) -o $@

bwmeasure: bwmeasure.o 
	$(CC) -g bwmeasure.o  -o $@

install: all installdirs
	$(INSTALL_PROGRAM) pgcbench$(X) $(DESTDIR)$(bindir)/pgcbench$(X)
	$(INSTALL_PROGRAM) bwmeasure$(X) $(DESTDIR)$(bindir)/bwmeasure$(X)

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir)

uninstall:
	rm -f $(addprefix $(DESTDIR)$(bindir)/, pgcbench$(X))
	rm -f $(addprefix $(DESTDIR)$(bindir)/, bwmeasure$(X))

clean distclean maintainer-clean:
	rm -f pgcbench$(X) pgcbench.o
	rm -f bwmeasure$(X) bwmeasure.o
