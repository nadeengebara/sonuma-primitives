#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <signal.h>

#include "../libsonuma/RMCdefines.h"

#define DATA_SIZE 1024	//in bytes
//#define DEBUG

static inline __attribute__ ((always_inline))
    uint8_t acquire_lock(uint8_t *address) {
        uint8_t ret_value;
        __asm__ __volatile__ (
                "ldstub [%1], %%l0\n\t"
                "mov %%l0, %0\n\t"  
                : "=r"(ret_value)    //output
                : "r"(address)    //input registers
            	: "%l0", "memory" //clobbered registers
                );
         return ret_value;
    }


typedef struct data_object {
  uint64_t version;
  uint8_t lock;
  uint32_t key;
  uint8_t value[DATA_SIZE];
  uint8_t padding[64-(DATA_SIZE+16)%64]; //All objects need to be cache-line-aligned
} data_object_t;

typedef struct {
	int id;
} parm;

data_object_t myObject;
int iters;

void par_phase(void *arg) {
  int i,j;
  parm *p=(parm *)arg;
  for (i = 0; i<iters; i++) {

  	uint8_t prevLockVal;
	do {
		prevLockVal = acquire_lock(&(myObject.lock));	//Test-and-set
		#ifdef DEBUG
		if (prevLockVal) {
			printf("thread %d failed to grab lock! (lock value = %"PRIu8")\n", p->id, prevLockVal);
		}
		#endif
	} while (prevLockVal);
	for (j=0; j<DATA_SIZE; j++) {
		myObject.value[j] ^= 1;
	}
	#ifdef DEBUG
  	printf("version = %" PRIu64 ",\t\
	  lock = %" PRIu8 ",\t\
	  key = %" PRIu32 "\n", myObject.version, myObject.lock, myObject.key);
	#endif
	myObject.key ^= 7;
	myObject.version++;
	#ifdef DEBUG
  	printf("Thread %d: \tprevious lock value = %" PRIu8 ",\t\
	  	new lock value = %" PRIu8 ",\t\
		new version = %" PRIu64 "\n", p->id, prevLockVal, myObject.lock, myObject.version);
	#endif
	myObject.lock = 0;	//unlock
  }
}

int main(int argc, char* argv[]) {
  pthread_t *threads;
  pthread_attr_t pthread_custom_attr;
  parm *p;  

  myObject.version = 0; 
  myObject.lock = 0;
  myObject.key = 1031;

  if (argc != 3){
	printf ("Usage: %s n i\n  where n is no. of threads, i is the number of iterations per thread\n",argv[0]);
	exit(1);
  }

  int n=atoi(argv[1]);
  iters=atoi(argv[2]);

#ifdef DEBUG
  printf("Size of data object is %d\n", sizeof(data_object_t));
  printf("Addresses of object fields:\n\
	myObject.version = %d\t\
	myObject.lock = %d\t\
	myObject.key = %d\t\n", &(myObject.version), &(myObject.lock), &(myObject.key));
  printf("Running with %d threads\n", n);
#endif

  threads=(pthread_t *)malloc(n*sizeof(*threads));
  pthread_attr_init(&pthread_custom_attr);

  p=(parm *)malloc(sizeof(parm)*n);
	/* Start up thread */
  int i;
  for (i=0; i<n; i++) {
	p[i].id=i;
	pthread_create(&threads[i], &pthread_custom_attr, par_phase, (void *)(p+i));
  }

 /* Synchronize the completion of each thread. */
  for (i=0; i<n; i++) {
	pthread_join(threads[i],NULL);
  }
  printf("Expected object version is %d. Final object version is %d \n", n*iters, myObject.version);
  free(p);

  return 0;
}
