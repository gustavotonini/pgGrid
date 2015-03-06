#-------------------------------------------------------------------------
#
# Makefile for src/pgcluster/pgrp
#
#-------------------------------------------------------------------------

subdir = src/pgcluster/pglb
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS= child.o cluster_table.o load_balance.o main.o pool_auth.o \
	pool_connection_pool.o pool_ip.o pool_process_query.o pool_stream.o \
	pool_params.o recovery.o socket.o lifecheck.o ps_status.o

EXTRA_OBJS = $(top_builddir)/src/backend/libpq/replicate_com.o ../libpgc/SUBSYS.o 

CFLAGS += -DPRINT_DEBUG
override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS) -DBINDIR=\"$(bindir)\"

all: pglb

pglb: $(OBJS) $(libpq_builddir)/libpq.a 
	$(CC) $(CFLAGS) $(OBJS) $(EXTRA_OBJS) $(libpq) $(LDFLAGS) $(LIBS) -o $@

install: all installdirs
	$(INSTALL_PROGRAM) pglb$(X) $(DESTDIR)$(bindir)/pglb$(X)
	$(INSTALL_DATA) pglb.conf.sample  $(DESTDIR)$(datadir)/pglb.conf.sample

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir)
	$(mkinstalldirs) $(DESTDIR)$(datadir)

uninstall:
	rm -f $(addprefix $(DESTDIR)$(bindir)/, pglb$(X))
	rm -f $(DESTDIR)$(datadir)/pglb.conf.sample

clean distclean maintainer-clean:
	rm -f pglb$(X) $(OBJS) 
