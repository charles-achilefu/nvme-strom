/*
 * nvme_stat.c
 *
 * A utility command to collect run-time statistics of NVMe-Strom
 * --------
 * Copyright 2017 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2017 (C) The PG-Strom Development Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2,
 * as published by the Free Software Foundation.
 */
#include <fcntl.h>
#include <libgen.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include "utils_common.h"

static int		verbose = 0;

static void
show_avg8(uint64_t N, uint64_t clocks, double clock_per_sec)
{
	if (N == 0)
		printf("    ---- ");
	else
	{
		double		value = (double)(clocks / (double)N) / clock_per_sec;

		if (value >= 2.0)			/* 2.0s */
			printf(" %7.2fs", value);
		else if (value >= 1.000)	/* 1000ms; xxxx.xms */
			printf(" %6.1fms", value * 1000.0);
		else if (value >= 0.005)	/*    5ms; xxx.xxms */
			printf(" %6.2fms", value * 1000.0);
		else if (value >= 0.001)	/* 1000us; xxxx.xus */
			printf(" %6.1fus", value * 1000000.0);
		else if (value >= 0.000005)	/*    5us; xxx.xxus */
			printf(" %6.2fus", value * 1000000.0);
		else
			printf(" %6.0fns", value * 1000000000.0);
	}
}

static void
print_stat_verbose(int loop, StromCmd__StatInfo *p, StromCmd__StatInfo *c,
				   struct timeval *tv1, struct timeval *tv2)
{
#define DECL_DIFF(C,P,FIELD)	uint64_t FIELD = (C)->FIELD - (P)->FIELD;
	DECL_DIFF(c,p,nr_ioctl_memcpy_submit);
	DECL_DIFF(c,p,clk_ioctl_memcpy_submit);
	DECL_DIFF(c,p,nr_ioctl_memcpy_wait);
	DECL_DIFF(c,p,clk_ioctl_memcpy_wait);
	DECL_DIFF(c,p,nr_ssd2gpu);
	DECL_DIFF(c,p,clk_ssd2gpu);
	DECL_DIFF(c,p,nr_setup_prps);
	DECL_DIFF(c,p,clk_setup_prps);
	DECL_DIFF(c,p,nr_submit_dma);
	DECL_DIFF(c,p,clk_submit_dma);
	DECL_DIFF(c,p,nr_wait_dtask);
	DECL_DIFF(c,p,clk_wait_dtask);
	DECL_DIFF(c,p,nr_wrong_wakeup);
	DECL_DIFF(c,p,total_dma_length);
	DECL_DIFF(c,p,nr_debug1);
	DECL_DIFF(c,p,nr_debug2);
	DECL_DIFF(c,p,nr_debug3);
	DECL_DIFF(c,p,nr_debug4);
	DECL_DIFF(c,p,clk_debug1);
	DECL_DIFF(c,p,clk_debug2);
	DECL_DIFF(c,p,clk_debug3);
	DECL_DIFF(c,p,clk_debug4);
#undef DECL_DIFF
	double		interval;
	double		clocks_per_sec;

	interval = ((double)((tv2->tv_sec - tv1->tv_sec) * 1000000 +
						 (tv2->tv_usec - tv1->tv_usec))) / 1000000.0;
	clocks_per_sec = (double)(c->tsc - p->tsc) / interval;

	if (loop % 20 == 0)
	{
		puts("   ioctl-   ioctl-              avg-                   avg-size   wrong-");
		puts("   submit     wait avg-prps   submit  avg-dma avg-wait     (KB)   wakeup DMA(cur) DMA(max)"
			 "   debug1   debug2   debug3   debug4");
	}
	show_avg8(nr_ioctl_memcpy_submit,
			  clk_ioctl_memcpy_submit, clocks_per_sec);
	show_avg8(nr_ioctl_memcpy_wait,
			  clk_ioctl_memcpy_wait, clocks_per_sec);
	show_avg8(nr_setup_prps, clk_setup_prps, clocks_per_sec);
	show_avg8(nr_submit_dma, clk_submit_dma, clocks_per_sec);
	show_avg8(nr_ssd2gpu, clk_ssd2gpu, clocks_per_sec);
	show_avg8(nr_wait_dtask, clk_wait_dtask, clocks_per_sec);
	if (nr_submit_dma == 0)
		printf("    ---- ");
	else
		printf(" %6lukB", total_dma_length / (1024 * nr_submit_dma));
	printf(" %8lu %8lu %8lu",
		   nr_wrong_wakeup,
		   c->cur_dma_count,
		   c->max_dma_count);
	show_avg8(nr_debug1, clk_debug1, clocks_per_sec);
	show_avg8(nr_debug2, clk_debug2, clocks_per_sec);
	show_avg8(nr_debug3, clk_debug3, clocks_per_sec);
	show_avg8(nr_debug4, clk_debug4, clocks_per_sec);
	putchar('\n');
}

static void
print_stat_normal(int loop, StromCmd__StatInfo *p, StromCmd__StatInfo *c,
				  struct timeval *tv1, struct timeval *tv2)
{
#define DECL_DIFF(C,P,FIELD)	uint64_t FIELD = (C)->FIELD - (P)->FIELD;
	DECL_DIFF(c,p,nr_ioctl_memcpy_submit);
	DECL_DIFF(c,p,clk_ioctl_memcpy_submit);
	DECL_DIFF(c,p,nr_ioctl_memcpy_wait);
	DECL_DIFF(c,p,clk_ioctl_memcpy_wait);
	DECL_DIFF(c,p,nr_ssd2gpu);
	DECL_DIFF(c,p,clk_ssd2gpu);
	DECL_DIFF(c,p,nr_submit_dma);
	DECL_DIFF(c,p,nr_wait_dtask);
	DECL_DIFF(c,p,clk_wait_dtask);
	DECL_DIFF(c,p,nr_wrong_wakeup);
	DECL_DIFF(c,p,total_dma_length);
#undef DECL_DIFF
	double		interval;
	double		clocks_per_sec;

	interval = ((double)((tv2->tv_sec - tv1->tv_sec) * 1000000 +
						 (tv2->tv_usec - tv1->tv_usec))) / 1000000.0;
	clocks_per_sec = (double)(c->tsc - p->tsc) / interval;

	if (loop % 20 == 0)
	{
		puts("   ioctl-   ioctl-                   avg-size   wrong-");
		puts("   submit     wait  avg-dma avg-wait     (KB)   wakeup DMA(cur) DMA(max)");
	}
	show_avg8(nr_ioctl_memcpy_submit,
			  clk_ioctl_memcpy_submit, clocks_per_sec);
	show_avg8(nr_ioctl_memcpy_wait,
			  clk_ioctl_memcpy_wait, clocks_per_sec);
	show_avg8(nr_ssd2gpu, clk_ssd2gpu, clocks_per_sec);
	show_avg8(nr_wait_dtask, clk_wait_dtask, clocks_per_sec);
	if (nr_submit_dma == 0)
		printf("    ---- ");
	else
	{
		double	avg_size = ((double)total_dma_length /
							(double)(nr_submit_dma << 10));
		if (avg_size >= 100.0)
			printf(" %6.1fkB", avg_size);
		else
			printf(" %6.2fkB", avg_size);
	}
	printf(" %8lu %8lu %8lu\n",
		   nr_wrong_wakeup,
		   c->cur_dma_count,
		   c->max_dma_count);
}

static void
usage(const char *command_name)
{
	fprintf(stderr,
			"usage: %s [-v] [<interval>]\n",
			basename(strdup(command_name)));
	exit(1);
}

int
main(int argc, char *argv[])
{
	int		loop;
	int		interval;
	int		c;
	StromCmd__StatInfo	curr_stat;
	StromCmd__StatInfo	prev_stat;
	struct timeval		tv1, tv2;

	while ((c = getopt(argc, argv, "hv")) >= 0)
	{
		switch (c)
		{
			case 'v':
				verbose = 1;
				break;
			case 'h':
			default:
				usage(argv[0]);
				break;
		}
	}
	if (optind == argc)
		interval = -1;
	else if (optind + 1 == argc)
		interval = atoi(argv[optind]);
	else
		usage(argv[0]);

	if (interval > 0)
	{
		for (loop=-1; ; loop++)
		{
			memset(&curr_stat, 0, sizeof(StromCmd__StatInfo));
			curr_stat.version = 1;
			if (verbose)
				curr_stat.flags = NVME_STROM_STATFLAGS__DEBUG;
			if (nvme_strom_ioctl(STROM_IOCTL__STAT_INFO, &curr_stat))
				ELOG(errno, "failed on ioctl(STROM_IOCTL__STAT_INFO)");

			gettimeofday(&tv2, NULL);
			if (loop >= 0)
			{
				if (!verbose)
					print_stat_normal(loop, &prev_stat, &curr_stat,
									  &tv1, &tv2);
				else
					print_stat_verbose(loop, &prev_stat, &curr_stat,
									   &tv1, &tv2);
			}
			sleep(interval);
			memcpy(&prev_stat, &curr_stat, sizeof(StromCmd__StatInfo));
			tv1 = tv2;
		}
	}
	else
	{
		memset(&curr_stat, 0, sizeof(StromCmd__StatInfo));
		curr_stat.version = 1;
		if (verbose)
			curr_stat.flags = NVME_STROM_STATFLAGS__DEBUG;
		if (nvme_strom_ioctl(STROM_IOCTL__STAT_INFO, &curr_stat))
			ELOG(errno, "failed on ioctl(STROM_IOCTL__STAT_INFO)");

		printf("tsc:               %lu\n"
			   "ioctl(nr_submit)   %lu\n"
			   "ioctl(clk_submit)  %lu\n"
			   "ioctl(nr_wait)     %lu\n"
			   "ioctl(clk_wait)    %lu\n"
			   "nr_ssd2gpu:        %lu\n"
			   "clk_ssd2gpu:       %lu\n"
			   "nr_setup_prps:     %lu\n"
			   "clk_setup_prps:    %lu\n"
			   "nr_submit_dma:     %lu\n"
			   "clk_submit_dma:    %lu\n"
			   "nr_wait_dtask:     %lu\n"
			   "clk_wait_dtask:    %lu\n"
			   "nr_wrong_wakeup:   %lu\n"
			   "total_dma_length:  %lu\n"
			   "cur_dma_count:     %lu\n"
			   "max_dma_count:     %lu\n",
			   (unsigned long)curr_stat.tsc,
			   (unsigned long)curr_stat.nr_ioctl_memcpy_submit,
			   (unsigned long)curr_stat.clk_ioctl_memcpy_submit,
			   (unsigned long)curr_stat.nr_ioctl_memcpy_wait,
			   (unsigned long)curr_stat.clk_ioctl_memcpy_wait,
			   (unsigned long)curr_stat.nr_ssd2gpu,
			   (unsigned long)curr_stat.clk_ssd2gpu,
			   (unsigned long)curr_stat.nr_setup_prps,
			   (unsigned long)curr_stat.clk_setup_prps,
			   (unsigned long)curr_stat.nr_submit_dma,
			   (unsigned long)curr_stat.clk_submit_dma,
			   (unsigned long)curr_stat.nr_wait_dtask,
			   (unsigned long)curr_stat.clk_wait_dtask,
			   (unsigned long)curr_stat.nr_wrong_wakeup,
			   (unsigned long)curr_stat.total_dma_length,
			   (unsigned long)curr_stat.cur_dma_count,
			   (unsigned long)curr_stat.max_dma_count);
		if (verbose)
			printf("nr_debug1:        %lu\n"
				   "clk_debug1:       %lu\n"
				   "nr_debug2:        %lu\n"
				   "clk_debug2:       %lu\n"
				   "nr_debug3:        %lu\n"
				   "clk_debug3:       %lu\n"
				   "nr_debug4:        %lu\n"
				   "clk_debug4:       %lu\n",
				   (unsigned long)curr_stat.nr_debug1,
				   (unsigned long)curr_stat.clk_debug1,
				   (unsigned long)curr_stat.nr_debug2,
				   (unsigned long)curr_stat.clk_debug2,
				   (unsigned long)curr_stat.nr_debug3,
				   (unsigned long)curr_stat.clk_debug3,
				   (unsigned long)curr_stat.nr_debug4,
				   (unsigned long)curr_stat.clk_debug4);
	}
	return 0;
}
