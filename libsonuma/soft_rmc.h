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

void *core_rmc_fun(void *arg);

#endif
