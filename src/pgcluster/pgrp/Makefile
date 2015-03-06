#-------------------------------------------------------------------------
#
# Makefile for src/pgcluster/pgrp
#
#-------------------------------------------------------------------------

subdir = src/pgcluster/pgrp
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

# this setup is for V2 protocol
#OBJS= cascade.o conf.o main.o recovery.o replicate.o rlog.o 
# this setup is for V3 protocol
OBJS= pqformat.o cascade.o conf.o main.o recovery.o replicate.o rlog.o lifecheck.o

EXTRA_OBJS = $(top_builddir)/src/backend/libpq/replicate_com.o ../libpgc/SUBSYS.o

CFLAGS += -DPRINT_DEBUG -DPG_GRID
override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS) -DBINDIR=\"$(bindir)\"
all: pgreplicate

pgreplicate: $(OBJS) $(libpq_builddir)/libpq.a 
	$(CC) $(CFLAGS) $(OBJS) $(EXTRA_OBJS) $(libpq) $(libpq_builddir)/libpq.a $(LDFLAGS) $(LIBS) -o $@

install: all installdirs
	$(INSTALL_PROGRAM) pgreplicate$(X) $(DESTDIR)$(bindir)/pgreplicate$(X)
	$(INSTALL_DATA) pgreplicate.conf.sample  $(DESTDIR)$(datadir)/pgreplicate.conf.sample

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir)
	$(mkinstalldirs) $(DESTDIR)$(datadir)

uninstall:
	rm -f $(addprefix $(DESTDIR)$(bindir)/, pgreplicate$(X))
	rm -f $(DESTDIR)$(datadir)/pgreplicate.conf.sample

clean distclean maintainer-clean:
	rm -f pgreplicate$(X) $(OBJS) 

clean_obj:
	rm -f $(OBJS)
