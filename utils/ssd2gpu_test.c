/* ----------------------------------------------------------------
 *
 * ssd2gpu_test
 *
 * Test program for SSD-to-GPU Direct Loading
 * --------
 * Copyright 2016-2017 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2016-2017 (C) The PG-Strom Development Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2,
 * as published by the Free Software Foundation.
 * ----------------------------------------------------------------
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <cuda.h>
#include "nvme_strom.h"

#define offsetof(type, field)   ((long) &((type *)0)->field)
#define Max(a,b)				((a) > (b) ? (a) : (b))
#define Min(a,b)				((a) < (b) ? (a) : (b))
#define BLCKSZ					(8192)
#define RELSEG_SIZE				(131072)

/* command line options */
static int		device_index = -1;
static int		nr_segments = 6;
static size_t	segment_sz = 32UL << 20;
static int		enable_checks = 0;
static int		print_mapping = 0;
static int		test_by_vfs = 0;
static size_t	vfs_io_size = 0;

/* static variables */
static CUdevice			cuda_device;
static CUcontext		cuda_context;
static unsigned long	curr_fpos = 0;	/* to be updated by atomic add */
static int				file_desc = -1;
static size_t			filesize = 0;
static const char	   *filename = NULL;

typedef struct
{
	pthread_t		thread;
	unsigned long	mgmem_handle;
	size_t			mgmem_offset;
	char		   *src_buffer;
	char		   *dst_buffer;
	CUdeviceptr		dev_buffer;
	long			nr_ram2gpu;
	long			nr_ssd2gpu;
	long			nr_dma_submit;
	long			nr_dma_blocks;
	uint32_t		chunk_ids[1];
} worker_context;

/*
 * nvme_strom_ioctl - entrypoint of NVME-Strom
 */
static int
nvme_strom_ioctl(int cmd, const void *arg)
{
	static __thread int fdesc_nvme_strom = -1;

	if (fdesc_nvme_strom < 0)
	{
		fdesc_nvme_strom = open(NVME_STROM_IOCTL_PATHNAME, O_RDONLY);
		if (fdesc_nvme_strom < 0)
		{
			fprintf(stderr, "failed to open \"%s\" : %m\n",
					NVME_STROM_IOCTL_PATHNAME);
			return -1;
		}
	}
	return ioctl(fdesc_nvme_strom, cmd, arg);
}

#define cuda_exit_on_error(__RC, __API_NAME)							\
	do {																\
		if ((__RC) != CUDA_SUCCESS)										\
		{																\
			const char *error_name;										\
																		\
			if (cuGetErrorName((__RC), &error_name) != CUDA_SUCCESS)	\
				error_name = "unknown error";							\
			fprintf(stderr, "%d: failed on %s: %s\n",					\
					__LINE__, __API_NAME, error_name);					\
			exit(1);													\
		}																\
	} while(0)

#define system_exit_on_error(__RC, __API_NAME)							\
	do {																\
		if ((__RC))														\
		{																\
			fprintf(stderr, "%d: failed on %s: %m\n",					\
					__LINE__, __API_NAME);								\
			exit(1);													\
		}																\
	} while(0)

static void
ioctl_check_file(const char *filename, int fdesc)
{
	StromCmd__CheckFile uarg;
	int		rc;

	memset(&uarg, 0, sizeof(uarg));
	uarg.fdesc = fdesc;

	rc = nvme_strom_ioctl(STROM_IOCTL__CHECK_FILE, &uarg);
	if (rc)
	{
		fprintf(stderr, "STROM_IOCTL__CHECK_FILE('%s') --> %d: %m\n",
				filename, rc);
		exit(1);
	}
}

static void
ioctl_wait_memcpy(unsigned long dma_task_id)
{
	
	StromCmd__MemCopyWait uarg;
	int		rv;

	memset(&uarg, 0, sizeof(StromCmd__MemCopyWait));
	uarg.dma_task_id = dma_task_id;
	rv = nvme_strom_ioctl(STROM_IOCTL__MEMCPY_WAIT, &uarg);
	if (uarg.status)
		printf("DMA status (id=%lu, status=%ld\n",
			   dma_task_id, uarg.status);
	system_exit_on_error(rv, "STROM_IOCTL__MEMCPY_SSD2GPU_WAIT");
}

static unsigned long
ioctl_map_gpu_memory(CUdeviceptr cuda_devptr, size_t buffer_size)
{
	StromCmd__MapGpuMemory uarg;
	int			retval;

	memset(&uarg, 0, sizeof(StromCmd__MapGpuMemory));
	uarg.vaddress = cuda_devptr;
	uarg.length = buffer_size;

	retval = nvme_strom_ioctl(STROM_IOCTL__MAP_GPU_MEMORY, &uarg);
	if (retval)
	{
		fprintf(stderr, "STROM_IOCTL__MAP_GPU_MEMORY(%p, %lu) --> %d: %m",
			   (void *)cuda_devptr, buffer_size, retval);
		exit(1);
	}
	return uarg.handle;
}

static void
memdump_on_corruption(const char *src_buffer,
					  const char *dst_buffer,
					  loff_t fpos, size_t total_length)
{
	long	unitsz = 16;
	long	pos;
	int		enable_dump = 0;
	int		i;

	for (pos=0; pos < total_length; pos += unitsz)
	{
		const char *src_ptr = src_buffer + pos;
		const char *dst_ptr = dst_buffer + pos;

		if (memcmp(src_ptr, dst_ptr, unitsz) != 0)
		{
			if (!enable_dump)
			{
				enable_dump = 1;
				total_length = Min(total_length, pos + 8 * unitsz);
				pos = Max(pos - 4 * unitsz, -unitsz);
				continue;
			}
			printf("- 0x%08lx ", (long)(fpos + pos));
			for (i=0; i < unitsz; i++)
			{
				if (i == unitsz / 2)
					putchar(' ');
				printf(" %02x", (int)(src_ptr[i] & 0xff));
			}
			putchar('\n');
			printf("+ 0x%08lx ", (long)(fpos + pos));
			for (i=0; i < unitsz; i++)
			{
				if (i == unitsz / 2)
					putchar(' ');
				printf(" %02x", (int)(dst_ptr[i] & 0xff));
			}
			putchar('\n');
		}
		else if (enable_dump)
		{
			printf("  0x%08lx ", (long)(fpos + pos));
			for (i=0; i < unitsz; i++)
			{
				if (i == unitsz / 2)
					putchar(' ');
				printf(" %02x", (int)(src_ptr[i] & 0xff));
			}
			putchar('\n');
		}
	}
	fprintf(stderr, "memory corruption detected\n");
	abort();
	exit(1);
}

static void
show_throughput(const char *filename, size_t file_size,
				struct timeval tv1, struct timeval tv2,
				long nr_ram2gpu, long nr_ssd2gpu,
				long nr_dma_submit, long nr_dma_blocks)
{
	long		time_ms;
	double		throughput;

	time_ms = ((tv2.tv_sec * 1000 + tv2.tv_usec / 1000) -
			   (tv1.tv_sec * 1000 + tv1.tv_usec / 1000));
	throughput = (double)file_size / ((double)time_ms / 1000.0);

	if (file_size < (4UL << 10))
		printf("read: %zuBytes", file_size);
	else if (file_size < (4UL << 20))
		printf("read: %.2fKB", (double)file_size / (double)(1UL << 10));
	else if (file_size < (4UL << 30))
		printf("read: %.2fMB", (double)file_size / (double)(1UL << 20));
	else
		printf("read: %.2fGB", (double)file_size / (double)(1UL << 30));

	if (time_ms < 4000UL)
		printf(", time: %lums", time_ms);
	else
		printf(", time: %.2fsec", (double)time_ms / 1000.0);

	if (throughput < (double)(4UL << 10))
		printf(", throughput: %zuB/s\n", (size_t)throughput);
	else if (throughput < (double)(4UL << 20))
		printf(", throughput: %.2fKB/s\n", throughput / (double)(1UL << 10));
	else if (throughput < (double)(4UL << 30))
		printf(", throughput: %.2fMB/s\n", throughput / (double)(1UL << 20));
	else
		printf(", throughput: %.2fGB/s\n", throughput / (double)(1UL << 30));

	if (nr_ram2gpu > 0 || nr_ssd2gpu > 0)
	{
		printf("nr_ram2gpu: %ld, nr_ssd2gpu: %ld",
			   nr_ram2gpu, nr_ssd2gpu);
	}
	if (nr_dma_submit > 0)
	{
		double	avg_dma_sz = ((double)(nr_dma_blocks << 9) /
							  (double)(nr_dma_submit));
		if (avg_dma_sz > 4194304.0)
			printf(", average DMA size: %.1fMB", avg_dma_sz / 1048576.0);
		else if (avg_dma_sz > 4096.0)
			printf(", average DMA size: %.1fKB", avg_dma_sz / 1024);
		else
			printf(", average DMA size: %.0fb", avg_dma_sz);
	}
	putchar('\n');
}

static void *
exec_test_by_strom(void *private)
{
	worker_context *wcontext = private;
	unsigned long	next_fpos;
	unsigned int	nr_chunks = segment_sz / BLCKSZ;
	CUresult		rc;
	StromCmd__MemCopySsdToGpu uarg;
	ssize_t			i, j, nbytes;
	uint32_t		chunk_base;
	int				rv;

	rc = cuCtxSetCurrent(cuda_context);
	cuda_exit_on_error(rc, "cuCtxSetCurrent");

	for (;;)
	{
		next_fpos = __atomic_fetch_add(&curr_fpos,
									   segment_sz,
									   __ATOMIC_SEQ_CST);
		if (next_fpos >= filesize)
			break;	/* end of the source file */

		uarg.handle		= wcontext->mgmem_handle;
		uarg.offset		= wcontext->mgmem_offset;
		uarg.file_desc	= file_desc;
		uarg.nr_chunks	= nr_chunks;
		uarg.chunk_sz	= BLCKSZ;
		uarg.relseg_sz	= 0;
		uarg.chunk_ids	= wcontext->chunk_ids;
		uarg.wb_buffer	= wcontext->src_buffer;
		chunk_base		= next_fpos / BLCKSZ;
		for (i=0; i < nr_chunks; i++)
			uarg.chunk_ids[nr_chunks - (i+1)] = chunk_base + i;

		rv = nvme_strom_ioctl(STROM_IOCTL__MEMCPY_SSD2GPU, &uarg);
		system_exit_on_error(rv, "STROM_IOCTL__MEMCPY_SSD2GPU");

		wcontext->nr_ram2gpu	+= uarg.nr_ram2gpu;
		wcontext->nr_ssd2gpu	+= uarg.nr_ssd2gpu;
		wcontext->nr_dma_submit	+= uarg.nr_dma_submit;
		wcontext->nr_dma_blocks	+= uarg.nr_dma_blocks;

		/* kick RAM-to-GPU DMA, if written back */
		if (uarg.nr_ram2gpu > 0)
		{
			rc = cuMemcpyHtoD(wcontext->dev_buffer +
							  BLCKSZ * (uarg.nr_chunks -
										uarg.nr_ram2gpu),
							  wcontext->src_buffer +
							  BLCKSZ * (uarg.nr_chunks -
										uarg.nr_ram2gpu),
							  BLCKSZ * (uarg.nr_ram2gpu));
			cuda_exit_on_error(rc, "cuMemcpyHtoD");

			rc = cuStreamSynchronize(NULL);
			cuda_exit_on_error(rc, "cuStreamSynchronize");
		}
		ioctl_wait_memcpy(uarg.dma_task_id);

		/* corruption checks? */
		if (enable_checks)
		{
			rc = cuMemcpyDtoH(wcontext->dst_buffer,
							  wcontext->dev_buffer,
							  segment_sz);
			cuda_exit_on_error(rc, "cuMemcpyDtoH");

			/* read file via VFS */
			nbytes = pread(file_desc,
						   wcontext->src_buffer,
						   segment_sz,
						   next_fpos);
			system_exit_on_error(nbytes < segment_sz, "pread");

			for (i=0; i < nr_chunks; i++)
			{
				j = uarg.chunk_ids[i] - chunk_base;
				assert(j >=0 && j < nr_chunks);
				if (memcmp(wcontext->dst_buffer + i * BLCKSZ,
						   wcontext->src_buffer + j * BLCKSZ,
						   BLCKSZ) != 0)
				{
					fprintf(stderr, "i=%zu j=%zu\n", i, j);
					memdump_on_corruption(wcontext->src_buffer + i * BLCKSZ,
										  wcontext->dst_buffer + j * BLCKSZ,
										  next_fpos + i * BLCKSZ,
										  BLCKSZ);
				}
			}
		}
	}
	return NULL;
}

static void *
exec_test_by_vfs(void *private)
{
	worker_context *wcontext = private;
	unsigned long	next_fpos;
	CUresult		rc;
	ssize_t			nbytes;

	rc = cuCtxSetCurrent(cuda_context);
	cuda_exit_on_error(rc, "cuCtxSetCurrent");

	for (;;)
	{
		next_fpos = __atomic_fetch_add(&curr_fpos,
									   segment_sz,
									   __ATOMIC_SEQ_CST);
		if (next_fpos >= filesize)
			break;		/* end of the source file */

		/* Load SSD-to-RAM */
		nbytes = pread(file_desc,
					   wcontext->src_buffer,
					   segment_sz,
					   next_fpos);
		system_exit_on_error(nbytes != segment_sz, "pread");

		/* Kick RAM-to-GPU DMA */
		rc = cuMemcpyHtoD(wcontext->dev_buffer,
						  wcontext->src_buffer,
						  segment_sz);
		cuda_exit_on_error(rc, "cuMemcpyHtoD");

		/* Kick GPU-to-RAM DMA */
		if (enable_checks)
		{
			rc = cuMemcpyDtoH(wcontext->dst_buffer,
							  wcontext->dev_buffer,
							  segment_sz);
			cuda_exit_on_error(rc, "cuMemcpyDtoH");

			if (memcmp(wcontext->src_buffer,
					   wcontext->dst_buffer,
					   segment_sz) != 0)
			{
				memdump_on_corruption(wcontext->src_buffer,
									  wcontext->dst_buffer,
									  next_fpos,
									  segment_sz);
			}
		}
	}
	return NULL;
}

/*
 * ioctl_print_gpu_memory
 */
static int ioctl_print_gpu_memory(void)
{
	StromCmd__ListGpuMemory *cmd_list;
	StromCmd__InfoGpuMemory	*cmd_info;
	uint32_t		nrooms = 2000;
	int				i, j;

	/* get list of mapped memory handles */
	do {
		cmd_list = malloc(offsetof(StromCmd__ListGpuMemory,
								   handles[nrooms]));
		system_exit_on_error(!cmd_list, "malloc");
		cmd_list->nrooms = nrooms;
		cmd_list->nitems = 0;
		if (nvme_strom_ioctl(STROM_IOCTL__LIST_GPU_MEMORY, cmd_list))
		{
			if (errno != ENOBUFS)
				system_exit_on_error(errno, "STROM_IOCTL__LIST_GPU_MEMORY");
			assert(cmd_list->nitems > cmd_list->nrooms);
			nrooms = cmd_list->nitems + 100;	/* with some margin */
			free(cmd_list);
		}
	} while (errno != 0);

	/* get property for each mapped device memory */
	cmd_info = malloc(offsetof(StromCmd__InfoGpuMemory,
							   paddrs[nrooms]));
	system_exit_on_error(!cmd_info, "malloc");
	i = 0;
	while (i < cmd_list->nitems)
	{
		cmd_info->handle = cmd_list->handles[i];
		cmd_info->nrooms = nrooms;

		if (nvme_strom_ioctl(STROM_IOCTL__INFO_GPU_MEMORY, cmd_info))
		{
			if (errno == ENOENT)
			{
				i++;
				continue;
			}
			else if (errno != ENOBUFS)
				system_exit_on_error(errno, "STROM_IOCTL__INFO_GPU_MEMORY");
			assert(cmd_info->nitems > nrooms);
			nrooms = cmd_info->nitems + 100;
			free(cmd_info);
			cmd_info = malloc(offsetof(StromCmd__InfoGpuMemory,
									   paddrs[nrooms]));
			system_exit_on_error(!cmd_info, "malloc");
			continue;
		}
		else
		{
			printf("%s"
				   "Mapped GPU Memory (handle: 0x%016lx) %p - %p\n"
				   "GPU Page: version=%u, size=%u, n_entries=%u\n"
				   "Owner: uid=%u\n",
				   (i == 0 ? "" : "\n"),
				   cmd_info->handle,
				   (void *)(cmd_info->paddrs[0] +
							cmd_info->map_offset),
				   (void *)(cmd_info->paddrs[0] +
							cmd_info->map_offset + cmd_info->map_length),
				   cmd_info->version,
				   cmd_info->gpu_page_sz,
				   cmd_info->nitems,
				   cmd_info->owner);

			for (j=0; j < cmd_info->nitems; j++)
			{
				printf("+%08lx: %p - %p\n",
					   j * (size_t)cmd_info->gpu_page_sz,
					   (void *)(cmd_info->paddrs[j]),
					   (void *)(cmd_info->paddrs[j] + cmd_info->gpu_page_sz));
			}
		}
		i++;
	}
	return 0;
}

/*
 * usage
 */
static void usage(const char *cmdname)
{
	fprintf(stderr,
			"usage: %s [OPTIONS] <filename>\n"
			"    -d <device index>:        (default 0)\n"
			"    -n <num of segments>:     (default 6)\n"
			"    -s <segment size in MB>:  (default 32MB)\n"
			"    -c : Enables corruption check (default off)\n"
			"    -h : Print this message   (default off)\n"
			"    -f([<i/o size in KB>]): Test by VFS access (default off)\n"
			"    -p (<map handle>): Print property of mapped device memory\n",
			basename(strdup(cmdname)));
	exit(1);
}

/*
 * entrypoint of driver_test
 */
int main(int argc, char * const argv[])
{
	struct stat		stbuf;
	size_t			buffer_size;
	CUresult		rc;
	CUdeviceptr		dev_buffer;
	void		   *src_buffer;
	void		   *dst_buffer;
	unsigned long	mgmem_handle;
	char			devname[256];
	worker_context **wcontext;
	int				i, code;
	long			nr_ram2gpu = 0;
	long			nr_ssd2gpu = 0;
	long			nr_dma_submit = 0;
	long			nr_dma_blocks = 0;
	struct timeval	tv1, tv2;

	while ((code = getopt(argc, argv, "d:n:s:cpf::gh")) >= 0)
	{
		switch (code)
		{
			case 'd':
				device_index = atoi(optarg);
				break;
			case 'n':		/* number of chunks */
				nr_segments = atoi(optarg);
				break;
			case 's':		/* size of chunks in MB */
				segment_sz = (size_t)atoi(optarg) << 20;
				break;
			case 'c':
				enable_checks = 1;
				break;
			case 'p':
				print_mapping = 1;
				break;
			case 'f':
				test_by_vfs = 1;
				if (optarg)
					vfs_io_size = (size_t)atoi(optarg) << 10;
				break;
			case 'h':
			default:
				usage(argv[0]);
				break;
		}
	}
	buffer_size = (size_t)segment_sz * nr_segments;

	/* dump the current device memory mapping */
	if (print_mapping)
		return ioctl_print_gpu_memory();

	if (optind + 1 == argc)
		filename = argv[optind];
	else
		usage(argv[0]);

	if (vfs_io_size == 0)
		vfs_io_size = segment_sz;
	else if (segment_sz % vfs_io_size != 0)
	{
		fprintf(stderr, "VFS I/O size (%zuKB) mismatch to ChunkSize (%zuMB)\n",
				vfs_io_size >> 10, segment_sz >> 20);
		return 1;
	}

	/* open the target file */
	file_desc = open(filename, O_RDONLY);
	if (file_desc < 0)
	{
		fprintf(stderr, "failed to open \"%s\": %m\n", filename);
		return 1;
	}

	if (fstat(file_desc, &stbuf) != 0)
	{
		fprintf(stderr, "failed on fstat(\"%s\"): %m\n", filename);
		return 1;
	}
	filesize = (stbuf.st_size / segment_sz) * segment_sz;
	if (filesize == 0)
	{
		fprintf(stderr, "file size (%zu) is smaller than segment size %zu",
				filesize, segment_sz);
		return 1;
	}

	/* is this file supported? */
	ioctl_check_file(filename, file_desc);

	/* allocate and map device memory */
	rc = cuInit(0);
	cuda_exit_on_error(rc, "cuInit");

	if (device_index < 0)
	{
		int		count;

		rc = cuDeviceGetCount(&count);
		cuda_exit_on_error(rc, "cuDeviceGetCount");

		for (device_index = 0; device_index < count; device_index++)
		{
			rc = cuDeviceGet(&cuda_device, device_index);
			cuda_exit_on_error(rc, "cuDeviceGet");

			rc = cuDeviceGetName(devname, sizeof(devname), cuda_device);
			cuda_exit_on_error(rc, "cuDeviceGetName");

			if (strstr(devname, "Tesla") != NULL ||
				strstr(devname, "Quadro") != NULL)
				break;
		}
		if (device_index == count)
		{
			fprintf(stderr, "No Tesla or Quadro GPUs are installed\n");
			return 1;
		}
	}
	else
	{
		rc = cuDeviceGet(&cuda_device, device_index);
		cuda_exit_on_error(rc, "cuDeviceGet");

		rc = cuDeviceGetName(devname, sizeof(devname), cuda_device);
		cuda_exit_on_error(rc, "cuDeviceGetName");
	}

	/* print test scenario */
	printf("GPU[%d] %s - file: %s", device_index, devname, filename);
	if (filesize < (4UL << 10))
		printf(", i/o size: %zuB", filesize);
	else if (filesize < (4UL << 20))
		printf(", i/o size: %.2fKB", (double)filesize / (double)(1UL << 10));
	else if (filesize < (4UL << 30))
		printf(", i/o size: %.2fMB", (double)filesize / (double)(1UL << 20));
	else
		printf(", i/o size: %.2fGB", (double)filesize / (double)(1UL << 30));
	if (test_by_vfs)
		printf(" by VFS (i/o unitsz: %zuKB)", vfs_io_size >> 10);

	printf(", buffer %zuMB x %d\n",
		   segment_sz >> 20, nr_segments);

	/* set up CUDA resources */
	rc = cuCtxCreate(&cuda_context, CU_CTX_SCHED_AUTO, cuda_device);
	cuda_exit_on_error(rc, "cuCtxCreate");

	rc = cuMemAlloc(&dev_buffer, buffer_size);
	cuda_exit_on_error(rc, "cuMemAlloc");

	rc = cuMemHostAlloc(&src_buffer, buffer_size,
						CU_MEMHOSTALLOC_PORTABLE);
	cuda_exit_on_error(rc, "cuMemHostAlloc");

	rc = cuMemHostAlloc(&dst_buffer, buffer_size,
						CU_MEMHOSTALLOC_PORTABLE);
	cuda_exit_on_error(rc, "cuMemHostAlloc");

	mgmem_handle = ioctl_map_gpu_memory(dev_buffer, buffer_size);

	/* set up worker's context */
	wcontext = calloc(nr_segments, sizeof(worker_context *));
	system_exit_on_error(!wcontext, "calloc");

	wcontext = calloc(nr_segments, offsetof(worker_context,
											chunk_ids[segment_sz / BLCKSZ]));
	gettimeofday(&tv1, NULL);
	for (i=0; i < nr_segments; i++)
	{
		size_t	offset = i * segment_sz;

		wcontext[i] = calloc(1, offsetof(worker_context,
										 chunk_ids[segment_sz / BLCKSZ]));
		wcontext[i]->mgmem_handle	= mgmem_handle;
		wcontext[i]->mgmem_offset	= offset;
		wcontext[i]->src_buffer		= (char *)src_buffer + offset;
		wcontext[i]->dst_buffer		= (char *)dst_buffer + offset;
		wcontext[i]->dev_buffer		= dev_buffer + offset;

		errno = pthread_create(&wcontext[i]->thread, NULL,
							   test_by_vfs
							   ? exec_test_by_vfs
							   : exec_test_by_strom,
							   wcontext[i]);
	}

	/* wait for threads completion */
	for (i=0; i < nr_segments; i++)
	{
		pthread_join(wcontext[i]->thread, NULL);
		nr_ram2gpu += wcontext[i]->nr_ram2gpu;
		nr_ssd2gpu += wcontext[i]->nr_ssd2gpu;
		nr_dma_submit += wcontext[i]->nr_dma_submit;
		nr_dma_blocks += wcontext[i]->nr_dma_blocks;
	}
	gettimeofday(&tv2, NULL);

	show_throughput(filename, filesize,
					tv1, tv2,
					nr_ram2gpu, nr_ssd2gpu,
					nr_dma_submit, nr_dma_blocks);
	return 0;
}
