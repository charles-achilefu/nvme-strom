PG_CONFIG := pg_config

MODULE_big = nvme_strom
OBJS = nvme_strom.o
EXTENSION = nvme_strom
#PG_CPPFLAGS := -O0 -g
SHLIB_LINK := -lnuma

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
