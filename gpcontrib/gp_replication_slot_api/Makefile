MODULES = gp_replication_slot_api

EXTENSION = gp_replication_slot_api
DATA = gp_replication_slot_api--1.0.sql

REGRESS = gp_replication_slot_api_test

PG_CPPFLAGS = -I$(libpq_srcdir)

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = gpcontrib/gp_replication_slot_api
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
