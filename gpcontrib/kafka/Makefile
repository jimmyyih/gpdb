PG_CONFIG = pg_config

MODULE_big = gp_kafka_thing
OBJS = gp_kafka_thing.o
CFLAGS_SL += -lrdkafka
LDFLAGS_SL += -lrdkafka

ifdef USE_PGXS
  PGXS := $(shell pg_config --pgxs)
  include $(PGXS)
else
  top_builddir = ../..
  include $(top_builddir)/src/Makefile.global
  include $(top_srcdir)/contrib/contrib-global.mk
endif


# Install into cdb_init.d, so that the catalog changes performed by initdb,
# and the compressor is available in all databases.
.PHONY: install-data
install-data:
	$(INSTALL_DATA) gp_kafka_thing.sql '$(DESTDIR)$(datadir)/cdb_init.d/gp_kafka_thing.sql'

install: install-data

.PHONY: uninstall-data

uninstall-data:
	rm -f '$(DESTDIR)$(datadir)/cdb_init.d/gp_kafka_thing.sql'

uninstall: uninstall-data

