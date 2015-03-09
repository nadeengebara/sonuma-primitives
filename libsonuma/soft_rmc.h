/**
 * soNUMA core rmc implementation.
 *
 * Copyright (C) EPFL. All rights reserved.
 * @authors novakovic
 */

#ifndef SOFT_RMC_H
#define SOFT_RMC_H

#include <inttypes.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/mman.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>

#include "RMCdefines.h"

#define MAX_NODE_CNT 24

#define RMAP 1
#define RUNMAP 0

typedef struct ioctl_info {
    int op;
    int node_id;
} ioctl_info_t;

//#define DEBUG_RMC

//RMC thread
void *core_rmc_fun(void *arg);
void deactivate_rmc();

int soft_rmc_wq_reg(rmc_wq_t *qp_wq);
int soft_rmc_cq_reg(rmc_cq_t *qp_cq);
int soft_rmc_ctx_alloc(char **mem, unsigned page_cnt);

#endif
