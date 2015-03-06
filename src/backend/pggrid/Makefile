#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for pggrid
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/executor/Makefile,v 1.26 2007/06/11 01:16:22 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/pggrid
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = allocation.o dquery.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS) 
