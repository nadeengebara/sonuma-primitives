#include "soft_rmc.h"
#include "RMCdefines.h"

#include <stdbool.h>

static bool rmc_active;
static int fd;

int rmc_open(char *shm_name) {
    printf("[soft_rmc] open called in VM mode\n");
    
    int fd;
    
    if ((fd=open(shm_name, O_RDWR|O_SYNC)) < 0) {
        return -1;
    }
    
    return fd;
}

int setup_pgas(void *pgas) {
    fd = rmc_open("/root/node");

    if(ioctl(fd, 1, (void *)0) == -1) {
	return -1;
    }

    pgas = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pgas == MAP_FAILED) {
	close(fd);
	perror("Error mmapping the file");
	exit(EXIT_FAILURE);
    }
    
    printf("[soft_rmc] remote region successfully mapped\n");
}

int destroy_pgas(void *pgas) {
    if(ioctl(fd, 1, (void *)pgas) == -1) {
	return -1;
    }

    munmap(pgas, PAGE_SIZE);
    return 0;
}

void *core_rmc_fun(void *arg) {
    qp_info_t * qp_info = (qp_info_t *)arg;
    rmc_wq_t * wq = qp_info->wq;
    rmc_cq_t * cq = qp_info->cq;

    //WQ ptrs
    uint8_t local_wq_tail = 0;
    uint8_t local_wq_SR = 1;

    //CQ ptrs
    uint8_t local_cq_head = 0;
    uint8_t local_cq_SR = 1;
    
    uint8_t compl_idx;

    void * pgas[2]; //this should be an array

    //TODO: initialize remote domain references
    //for now, just use one page in remote domain
    setup_pgas(pgas);

    rmc_active = true;

    while(rmc_active) {
	while (wq->q[local_wq_tail].SR == local_wq_SR) {
	    //printf("[core_rmc] reading remote memory\n");
	    //perform remote read
	    memcpy((uint8_t *)wq->q[local_wq_tail].buf_addr, 
		   pgas + (wq->q[local_wq_tail].offset),
		   wq->q[local_wq_tail].length);

	    compl_idx = local_wq_tail;

	    local_wq_tail += 1;
	    if (local_wq_tail >= MAX_NUM_WQ) {
		local_wq_tail = 0;
		local_wq_SR ^= 1;
	    }
	    
	    //notify the application
	    cq->q[local_cq_head].tid = compl_idx;
	    cq->q[local_cq_head].SR = local_cq_SR;

	    local_cq_head += 1;
	    if(local_cq_head >= MAX_NUM_WQ) {
		local_cq_head = 0;
		local_cq_SR ^= 1;
	    }
	}
    }
    
    destroy_pgas(pgas);

    printf("[soft_rmc] RMC deactivated\n");
}

void deactivate_rmc() {
    rmc_active = false;
}
