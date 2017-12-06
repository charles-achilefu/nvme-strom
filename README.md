# nvme-strom
NVMe-Strom is a Linux kernel module and related tools to intermediate peer-to-peer data transfer from NVMe-SSD to GPU's device memory.
This software is designed for Red Hat Enterprise Linux 7.3 or later, on top of NVIDIA GPUDirect RDMA feature.
Once ```nvme_strom.ko``` module gets installed onto the kernel, userspace application can (1) maps GPU device memory acquired using ```cuMemAlloc()``` on PCIe BAR1 region, then (2) issues P2P DMA request from a file on NVMe-SSD device to the mapped GPU device memory region.

## Prerequisites
- Red Hat Enterprise Linux 7.3 or later
- Tesla or Quadro GPU device
    - High-end Tesla GPU is recommended, because it has more than GB class PCIe BAR1 region. Other model provides just 256MB for PCIe BAR1 area.
    - http://docs.nvidia.com/cuda/gpudirect-rdma/index.html#supported-systems
- NVMe-SSD device
    - Due to the restriction of NVIDIA GPUDirect RDMA, GPU and SSD have to be installed a single root complex; typically, both of devices must be managed by same CPU.
    - Development team had evaluated the following devices. (Note that we never guarantee them.)
       - Intel SSD 750 (400GB; HHHL)
       - Samsung 960 PRO (512GB; M.2)
       - HGST SN260 (7.6TB; HHHL)
       - Intel SSD DC P4600 (2.0TB; HHHL)

## Contents
This repository contains the following software modules.
```
nvme-strom/
  +- kmod/              ... Linux kernel module of NVMe-Strom
  +- pgsql/             ... Test extension for PostgreSQL; using SSD2RAM transfer
  +- utils/
       +- nvme_stat     ... A tool to print statistics of the nvme_strom kernel module
       +- ssd2gpu_test  ... A simple throughput measurement tool for SSD2GPU DMA
       +- ssd2ram_test  ... A simple throughput measurement tool for SSD2RAM DMA
```
