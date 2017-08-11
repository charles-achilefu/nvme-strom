UTILS = nvme_stat ssd2ram_test
CC_FLAGS = -O2 -g -Wall

CUDA_PATH_LIST := /usr/local/cuda /usr/local/cuda-*
CUDA_PATH := $(shell for x in $(CUDA_PATH_LIST);    \
	do test -e "$$x/include/cuda.h" && echo $$x; done | head -1)
ifdef CUDA_PATH
UTILS += ssd2gpu_test
CC_FLAGS += -I$(CUDA_PATH)/include -L$(CUDA_PATH)/lib64
endif

all: $(UTILS)

nvme_stat: nvme_strom.h nvme_stat.c
	$(CC) nvme_stat.c -o $@ $(CC_FLAGS)

ssd2ram_test: nvme_strom.h ssd2ram_test.c
	$(CC) ssd2ram_test.c -o $@ $(CC_FLAGS) -lpthread

ssd2gpu_test: nvme_strom.h ssd2gpu_test.c
	$(CC) ssd2gpu_test.c -o $@ $(CC_FLAGS) -lcuda -lpthread

clean:
	rm -f $(UTILS)
