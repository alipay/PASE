EXTENSION = pase
OBJS = $(patsubst %.c,%.o,$(wildcard type/*.c hnsw/*.c *.c ivfflat/*.c utils/*.c))

MODULE_big = $(EXTENSION)
PG_LDFLAGS = -fopenmp -msse4 -mpopcnt
PG_CPPFLAGS = -fopenmp -msse4 -mpopcnt -I/usr/lib64/openmpi/share/vampirtrace
PG_LIBS = -fopenmp
DATA = pase--0.0.1.sql
PGFILEDESC = "pase - ant ai similarity search"
REGRESS = $(EXTENSION)

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = external/pase
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

perl-check:
	$(prove_check)
