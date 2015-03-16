// Stanko Novakovic
// All-software implementation of RMC

#include "soft_rmc.h"

#include <stdbool.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <assert.h>

static volatile bool rmc_active;
static int fd;

//pgas - one context supported
static char *ctx[MAX_NODE_CNT];

//node info
static int mynid, node_cnt;
static int dom_region_size;

//queue pair info
static volatile rmc_wq_t *wq = NULL;
static volatile rmc_cq_t *cq = NULL;

int soft_rmc_wq_reg(rmc_wq_t *qp_wq) {
    if((qp_wq == NULL) || (wq != NULL))
	return -1;
    
    wq = qp_wq;
    printf("[soft_rmc] work queue registered\n");
    return 0;
}

int soft_rmc_cq_reg(rmc_cq_t *qp_cq) {
    if((qp_cq == NULL) || (cq != NULL))
	return -1;
    
    cq = qp_cq;
    printf("[soft_rmc] completion queue registered\n");
    return 0;
}

static int rmc_open(char *shm_name) {
    printf("[soft_rmc] open called in VM mode\n");
    
    int fd;
    
    if ((fd=open(shm_name, O_RDWR|O_SYNC)) < 0) {
        return -1;
    }
    
    return fd;
}

//allocates local memory and maps remote memory 
int soft_rmc_ctx_alloc(char **mem, unsigned page_cnt) {
    ioctl_info_t info; 
    int i;

    printf("[soft_rmc] soft_rmc_alloc_ctx ->\n");
    dom_region_size = page_cnt * PAGE_SIZE;
    
    //allocate the pointer array for PGAS
    fd = rmc_open((char *)"/root/node");
    
    //first memory map local memory
    *mem = (char *)mmap(NULL, page_cnt * PAGE_SIZE,
			PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, 0);
    
    ctx[mynid] = *mem;

    printf("[soft_rmc] registered local memory\n");
    printf("[soft_rmc] registering remote memory, number of remote nodes %d\n", node_cnt-1);

    info.op = RMAP;
    //map the rest of pgas
    for(i=0; i<node_cnt; i++) {
	if(i != mynid) {
	    info.node_id = i;
	    if(ioctl(fd, 0, (void *)&info) == -1) {
		printf("[soft_rmc] ioctl failed\n");
		return -1;
	    }

	    printf("[soft_rmc] mapping memory of node %d\n", i);
	    
	    ctx[i] = (char *)mmap(NULL, page_cnt * PAGE_SIZE,
			    PROT_READ | PROT_WRITE,
			    MAP_SHARED, fd, 0);
	    if(ctx[i] == MAP_FAILED) {
		close(fd);
		perror("[soft_rmc] error mmapping the file");
		exit(EXIT_FAILURE);
	    }

#ifdef DEBUG_RMC
	    //for testing purposes
	    for(j=0; j<(dom_region_size)/sizeof(unsigned long); j++)
		printf("%lu\n", *((unsigned long *)ctx[i]+j));
#endif
	}
    }
    
    printf("[soft_rmc] context successfully created, %lu bytes\n",
	   (unsigned long)page_cnt * PAGE_SIZE * node_cnt);

    //activate the RMC
    rmc_active = true;
    
    return 0;
}

static int soft_rmc_ctx_destroy() {
    int i;

    ioctl_info_t info;

    info.op = RUNMAP;
    for(i=0; i<node_cnt; i++) {
	if(i != mynid) {
	    info.node_id = i;
	    if(ioctl(fd, 0, (void *)&info) == -1) {
		printf("[soft_rmc] failed to unmap a remote region\n");
		return -1;
	    }
	    //munmap(ctx[i], dom_region_size);
	}
    }

    //close(fd);
    
    return 0;
}

void *core_rmc_fun(void *arg) {
    qp_info_t * qp_info = (qp_info_t *)arg;

    //WQ ptrs
    uint8_t local_wq_tail = 0;
    uint8_t local_wq_SR = 1;

    //CQ ptrs
    uint8_t local_cq_head = 0;
    uint8_t local_cq_SR = 1;
    
    uint8_t compl_idx;

    printf("[soft_rmc] this node ID %d, number of nodes %d\n",
	   qp_info->this_nid, qp_info->node_cnt);
    
    mynid = qp_info->this_nid;
    node_cnt = qp_info->node_cnt;
	
    while(!rmc_active)
	;

    printf("[soft_rmc] RMC activated\n");

    volatile wq_entry_t *curr;
    
    while(rmc_active) {
	while (wq->q[local_wq_tail].SR == local_wq_SR) {
#ifdef DEBUG_RMC
	    printf("[soft_rmc] reading remote memory, offset = %lu\n",
	    	   wq->q[local_wq_tail].offset);

	    printf("[soft_rmc] buffer address %lu\n",
	    	   wq->q[local_wq_tail].buf_addr);

	    printf("[soft_rmc] nid = %d; offset = %d, len = %d\n", wq->q[local_wq_tail].nid, wq->q[local_wq_tail].offset, wq->q[local_wq_tail].length);
#endif
	    curr = &(wq->q[local_wq_tail]);

	    memcpy((uint8_t *)curr->buf_addr,
		   ctx[curr->nid] + curr->offset,
		   curr->length);
	    
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
    
    soft_rmc_ctx_destroy();
    
    printf("[soft_rmc] RMC deactivated\n");
    
    return NULL;
}

void deactivate_rmc() {
    rmc_active = false;
}
