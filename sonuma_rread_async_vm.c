#define __STDC_FORMAT_MACROS
#include "libsonuma/sonuma.h"

#define BILLION 1000000000L
#define ITERS 10000
#define SLOT_SIZE 64

#define ASYNC

rmc_wq_t *wq;
rmc_cq_t *cq;

static uint64_t op_count_issued;
static uint64_t op_count_completed;

void handler(uint8_t tid, wq_entry_t *head, void *owner) {
    // do nothing
    //printf("[rread_async] completion handler ->\n");
    printf("[rread_async] completed read.. value = %lu\n", *((unsigned long *)head->buf_addr));
    //printf("[rread_async] completed %d operations\n", op_count_completed);
    op_count_completed++;    
}

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stdout,"Usage: ./uBench <target_nid> <context_size> <buffer_size>\n");
        return 1;
    }

    int node_count = atoi(argv[1]);
    int this_nid = atoi(argv[2]);
    int snid = atoi(argv[3]);
    uint64_t ctx_size = atoi(argv[4]);
    uint64_t buf_size = atoi(argv[5]);

    int num_iter = (int)ITERS;
    
    uint8_t *lbuff = NULL;
    uint8_t *ctx = NULL;
    uint8_t *lbuff_slot;
    uint64_t ctx_offset;
    uint64_t pgas_size;
    
    struct timespec start_time, stop_time;

    //initialize and activate RMC
    rmc_init(node_count, this_nid);

    //register the queue pair
    kal_reg_wq(0, &wq);
    fprintf(stdout, "WQ was registered.\n");

    kal_reg_cq(0, &cq);
    fprintf(stdout, "CQ was registered.\n");

    //register the context and a local buffer
    uint32_t num_pages =  buf_size * sizeof(uint8_t) / PAGE_SIZE;

    kal_reg_lbuff(0, &lbuff, num_pages);
    fprintf(stdout, "Local buffer was registered.\n");

    uint32_t ctx_num_pages = ctx_size*sizeof(uint8_t) / PAGE_SIZE;
    if(kal_reg_ctx(0, &ctx, ctx_num_pages) < 0) {
	printf("[rread_async] couldn't create the context\n");
	return -1;
    }
    
    fprintf(stdout, "Ctx buffer was registered.\n");
    
    //uB kernel
    op_count_completed = 0;
    op_count_issued = 0;

    sleep(2);

    printf("[rread_async] starting experiment\n");

    pgas_size = node_count * ctx_size;
    printf("[rread_async] pgas size in KB: %lu\n", pgas_size/1024);
    
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    while(op_count_completed < num_iter) {
	//sleep(1);
#ifdef ASYNC
        rmc_check_cq(wq, cq, &handler, NULL);
#endif
	lbuff_slot = (void *)(lbuff + ((op_count_issued * SLOT_SIZE)
			       % PAGE_SIZE));
	//ctx_offset = ctx_size + ((op_count_issued * SLOT_SIZE) % ctx_size); //only remote, two nodes
	ctx_offset = ctx_size + ((op_count_issued * SLOT_SIZE) % (pgas_size-ctx_size)); //stream through the global AS	
#ifdef ASYNC
        rmc_rread_async(wq, (uint64_t)lbuff_slot, snid, 0, ctx_offset, SLOT_SIZE);
#else
	rmc_rread_sync(wq, cq, (uint64_t)lbuff_slot, snid, 0, ctx_offset, SLOT_SIZE);
	op_count_completed++;
	//printf("[rread_sync] completed one remote read\n");
#endif
	//printf("[rread_async] remote read scheduled\n");
	op_count_issued++;
    }
    clock_gettime(CLOCK_MONOTONIC, &stop_time);

    unsigned long long ns_acm = ((BILLION * stop_time.tv_sec + stop_time.tv_nsec) -
				 (BILLION * start_time.tv_sec + start_time.tv_nsec));
    
    double exec_time = (double)ns_acm/BILLION; //in secs
    printf("[rread_async] exec. time = %fs, IOPS = %f, bw = %f \n",
	   exec_time, num_iter/exec_time, (((num_iter/exec_time) * SLOT_SIZE)/1024)/1024);
#ifndef ASYNC
    printf("[rread_sync] remote read latency %f us\n", (exec_time/num_iter)*1000000);
#endif

    rmc_deinit();

free(lbuff);
//free(ctx);
free(wq);
free(cq);

return 0;
}
