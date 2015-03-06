#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for libpq subsystem (common library for replication server)
#
#-------------------------------------------------------------------------

subdir = src/pgcluster/libpgc
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = dllist.o sem.o show.o signal.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

distclean: clean

clean: 
	rm -f SUBSYS.o $(OBJS) 

ifeq (depend,$(wildcard depend))
include depend
endif
