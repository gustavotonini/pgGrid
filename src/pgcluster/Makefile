#-------------------------------------------------------------------------
#
# Makefile for src/pgcluster (server programs)
#
#-------------------------------------------------------------------------

subdir = src/pgcluster
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

DIRS := libpgc pgrp pglb tool

all install installdirs uninstall depend distprep:
	@for dir in $(DIRS); do $(MAKE) -C $$dir $@ || exit; done

clean distclean maintainer-clean:
	-@for dir in $(DIRS); do $(MAKE) -C $$dir $@; done
