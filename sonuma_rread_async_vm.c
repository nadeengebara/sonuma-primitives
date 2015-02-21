#define __STDC_FORMAT_MACROS
#include "libsonuma/sonuma.h"

#define BILLION 1000000000L
#define ITERS 10000000
#define SLOT_SIZE 4096

rmc_wq_t *wq;
rmc_cq_t *cq;

static uint64_t op_count_issued;
static uint64_t op_count_completed;

void handler(uint8_t tid, wq_entry_t head, void *owner) {
    // do nothing
    //printf("[rread_async] completed read\n");
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
    
    uint8_t * lbuff, * ctx;
    uint8_t * lbuff_slot;
    uint64_t ctx_offset;

    struct timespec start_time, stop_time;

    //local buffer
    lbuff = (uint8_t *)malloc(PAGE_SIZE * sizeof(uint8_t)); //memalign(PAGE_SIZE, buf_size*sizeof(uint8_t));
    if (lbuff == NULL) {
        fprintf(stdout, "Local buffer could not be allocated.\n");
        return 1;
    }

    //uint32_t num_pages =  buf_size * sizeof(uint8_t) / PAGE_SIZE;
    //fprintf(stdout, "Local buffer was allocated by address %p, number of pages is %d\n", lbuff, num_pages);

    //kal_reg_lbuff(0, &lbuff, num_pages);
    //fprintf(stdout, "Local buffer was registered.\n");

    //ctx = memalign(PAGE_SIZE, ctx_size*sizeof(uint8_t));
    //if (ctx == NULL) {
    //  fprintf(stdout, "Context buffer could not be allocated.\n");
    //  return 1;
    //}
    //kal_reg_ctx(0, &ctx, ctx_size*sizeof(uint8_t) / PAGE_SIZE);
    //fprintf(stdout, "Ctx buffer was registered.\n");

    kal_reg_wq(0, &wq);
    fprintf(stdout, "WQ was registered.\n");

    kal_reg_cq(0, &cq);
    fprintf(stdout, "CQ was registered.\n");

    //initialize and activate RMC
    rmc_init(node_count, this_nid, wq, cq, ctx);

    //uB kernel
    op_count_completed = 0;
    op_count_issued = 0;

    sleep(2);

    printf("[rread_async] starting experiment\n");

    clock_gettime(CLOCK_MONOTONIC, &start_time);
    while(op_count_completed < num_iter) {
	//sleep(1);
        rmc_check_cq(wq, cq, &handler, NULL);

	lbuff_slot = (void *)(lbuff + ((op_count_issued * SLOT_SIZE) % PAGE_SIZE));
	ctx_offset = (op_count_issued * SLOT_SIZE) % PAGE_SIZE;

        rmc_rread_async(wq, (uint64_t)lbuff_slot, snid, 0, ctx_offset, SLOT_SIZE);
	//printf("[rread_async] issued read\n");
	op_count_issued++;
    }
    clock_gettime(CLOCK_MONOTONIC, &stop_time);

    unsigned long long ns_acm = ((BILLION * stop_time.tv_sec + stop_time.tv_nsec) - (BILLION * start_time.tv_sec + start_time.tv_nsec));
    
    double exec_time = (double)ns_acm/BILLION; //in secs
    printf("[rread_async] exec. time = %fs, IOPS = %f, bw = %f \n", exec_time, num_iter/exec_time, (((num_iter/exec_time) * SLOT_SIZE)/1024)/1024);

    rmc_deinit();

free(lbuff);
//free(ctx);
free(wq);
free(cq);

return 0;
}
