// Stanko Novakovic
// All-software implementation of RMC

#include "soft_rmc.h"

#include <stdbool.h>
#include <sys/ioctl.h>
#include <sys/shm.h>
#include <unistd.h>
#include <assert.h>

#define DEBUG_RMC

//server information
static server_info_t *sinfo;

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

char *local_mem_region;

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
    //fd = rmc_open((char *)"/root/node");
    
    //first memory map local memory
    /*
    *mem = (char *)mmap(NULL, page_cnt * PAGE_SIZE,
			PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, 0);
    */
    *mem = local_mem_region; //snovakov: this is allocation upon connect
    
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

static int net_init(int node_cnt, int this_nid, char *filename) {
    FILE * fp;
    char * line = NULL;
    size_t len = 0;
    ssize_t read;
    int i = 0;

    printf("[network] net_init <- \n");
    
    sinfo = (server_info_t *)malloc(node_cnt * sizeof(server_info_t));

    //retreive ID, IP, DOMID
    fp = fopen(filename, "r");
    if (fp == NULL)
	exit(EXIT_FAILURE);

    // get server information
    while ((read = getline(&line, &len, fp)) != -1) {
      //printf("%s", line);
	char * pch = strtok (line,":");
	sinfo[i].nid = atoi(pch);
	printf("ID: %d ", sinfo[i].nid);
	pch = strtok(NULL, ":");
	strcpy(sinfo[i].ip, pch);
	printf("IP: %s ", sinfo[i].ip);
	pch = strtok(NULL, ":");
	sinfo[i].domid = atoi(pch);
	printf("DOMID: %d\n", sinfo[i].domid);
	i++;
    }

    printf("[network] net_init -> \n");
    
    return 0;
}

int soft_rmc_connect(int node_cnt, int this_nid) {
  int i, srv_idx;
  int listen_fd; // errsv, errno;
  struct sockaddr_in servaddr; //listen
  struct sockaddr_in raddr; //connect, accept
  int optval = 1;
  unsigned n;
  
  printf("[soft_rmc] soft_rmc_connect <- \n");

  //allocate the pointer array for PGAS
  fd = rmc_open((char *)"/root/node");

  //snovakov: first allocate memory
  unsigned long *ctxl;
  unsigned long dom_region_size = MAX_REGION_PAGES * PAGE_SIZE;

  /*
  int fd_mem = open("/dev/zero", O_RDWR);
  local_mem_region = (char *)mmap(0, dom_region_size*sizeof(char), PROT_READ | PROT_WRITE,
			MAP_SHARED, fd_mem, 0);
  */
  
  //key_t key = 25;
  int shmid = shmget(IPC_PRIVATE, dom_region_size*sizeof(char), SHM_R | SHM_W);
  if(-1 != shmid) {
    printf("[soft_rmc] shmget okay, shmid = %d\n", shmid);
    local_mem_region = (char *)shmat(shmid, NULL, 0);
  } else {
    printf("[soft_rmc] shmget failed\n");
  }

  if(local_mem_region != NULL) {
    printf("[soft_rmc] memory for the context allocated\n");
    memset(local_mem_region, 0, dom_region_size);
    mlock(local_mem_region, dom_region_size);
  }
  
  printf("[soft_rmc] managed to lock pages in memory\n");
  
  ctxl = (unsigned long *)local_mem_region;

  //snovakov:need to do this to fault the pages into memory
  for(i=0; i<(dom_region_size*sizeof(char))/8; i++) {
    ctxl[i] = 0;
  }

  ioctl_info_t info;
  info.op = MR_ALLOC;
  info.ctx = (unsigned long)local_mem_region;
  
  if(ioctl(fd, 0, &info) == -1) {
    perror("kal ioctl failed");
    return -1;
  }
  //
  
  net_init(node_cnt, this_nid, (char *)"/root/servers.txt");
  
  //listen
  listen_fd = socket(AF_INET, SOCK_STREAM, 0);

  bzero(&servaddr,sizeof(servaddr));
  servaddr.sin_family = AF_INET;
  servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
  servaddr.sin_port=htons(PORT);
    
  if((setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval))) == -1) {
    printf("Error on setsockopt\n");
    exit(EXIT_FAILURE);
  }

  if(bind(listen_fd, (struct sockaddr *)&servaddr, sizeof(servaddr)) == -1) {
    fprintf(stderr, "Address binding error\n");
    exit(EXIT_FAILURE);
  }

  if(listen(listen_fd, 1024) == -1) {
    //fprintf(stderr, "Listen call error: %s\n", strerror(errno));
    printf("[soft_rmc] Listen call error\n");
    exit(EXIT_FAILURE);
  }

  //ioctl_info_t info; // = (ioctl_info_t *)malloc(sizeof(ioctl_info_t));

  for(i=0; i<node_cnt; i++) {
    if(i != this_nid) {
      //first connect to it
      if(i > this_nid) {
	printf("[soft_rmc] server accept..\n");
	char *remote_ip;
       
	socklen_t slen = sizeof(raddr);

       	sinfo[i].fd = accept(listen_fd, (struct sockaddr*)&raddr, &slen);

	//retrieve nid of the remote node
	remote_ip = inet_ntoa(raddr.sin_addr); //retreive remote address
	
	printf("[soft_rmc] Connect received from %s\n", remote_ip);
	printf("[soft_rmc] Connect received on port %d\n", raddr.sin_port);
	//receive the reference to the remote memory
	while(1) {
	  n = recv(sinfo[i].fd, (char *)&srv_idx, sizeof(int), 0);
	  if(n == sizeof(int)) {
	    printf("[soft_rmc] received the node_id\n");
	    break;
	  }
	}

	printf("[soft_rmc] server ID is %u\n", srv_idx);
	//strcpy(ip_tmp, remote_ip);
	/*
	sprintf(ip_tmp, "%s", remote_ip);
	printf("[soft_rmc] Connect received from %s\n", ip_tmp);
	srv_idx = return_nid(ip_tmp);
	if(srv_idx < 0) {
	  printf("[soft_rmc] couldn't find server's IP\n");
	  return -1;
	}
	*/
      } else {
	printf("[soft_rmc] server connect..\n");

	memset(&raddr, 0, sizeof(raddr));
	raddr.sin_family = AF_INET;
	inet_aton(sinfo[i].ip, &raddr.sin_addr);
	raddr.sin_port = htons(PORT);

	sinfo[i].fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	while(1) {
	  if(connect(sinfo[i].fd, (struct sockaddr *)&raddr, sizeof(raddr)) == 0) {
	    printf("[soft_rmc] Connected to %s\n", sinfo[i].ip);
	    break;
	  }
	}
	unsigned n = send(sinfo[i].fd, (char *)&this_nid, sizeof(int), 0); //MSG_DONTWAIT
	if(n < sizeof(int)) {
	  printf("[soft_rmc] ERROR: couldn't send the node_id\n");
	  //return -1;
	}

	srv_idx = i;
      }

      //first get the reference for this domain
      info.op = GETREF;
      info.node_id = srv_idx;
      info.domid = sinfo[srv_idx].domid;
      if(ioctl(fd, 0, (void *)&info) == -1) {
	printf("[soft_rmc] failed to unmap a remote region\n");
	return -1;
      }

      //send the reference to the local memory
      unsigned n = send(sinfo[srv_idx].fd, (char *)&info.desc_gref, sizeof(int), 0); //MSG_DONTWAIT
      if(n < sizeof(int)) {
	printf("[soft_rmc] ERROR: couldn't send the grant reference\n");
	return -1;
      }
      
      printf("[soft_rmc] grant reference sent: %u\n", info.desc_gref);

      //receive the reference to the remote memory
      while(1) {
	n = recv(sinfo[srv_idx].fd, (char *)&info.desc_gref, sizeof(int), 0);
	if(n == sizeof(int)) {
	  printf("[soft_rmc] received the grant reference\n");
	  break;
	}
      }
	
      printf("[soft_rmc] grant reference received: %u\n", info.desc_gref);
    
      //put the ref for this domain
      info.op = PUTREF;
      if(ioctl(fd, 0, (void *)&info) == -1) {
	printf("[soft_rmc] failed to unmap a remote region\n");
	return -1;
      }
    }    
  } //for

  return 0;
}

void *core_rmc_fun(void *arg) {
    qp_info_t * qp_info = (qp_info_t *)arg;

    int i;
    
    //WQ ptrs
    uint8_t local_wq_tail = 0;
    uint8_t local_wq_SR = 1;

    //CQ ptrs
    uint8_t local_cq_head = 0;
    uint8_t local_cq_SR = 1;
    
    uint8_t compl_idx;
    
    nam_obj_header *header_src;
    nam_obj_header *header_dst;
    
    printf("[soft_rmc] this node ID %d, number of nodes %d\n",
	   qp_info->this_nid, qp_info->node_cnt);
    
    mynid = qp_info->this_nid;
    node_cnt = qp_info->node_cnt;
	
    while(!rmc_active)
	;

    printf("[soft_rmc] RMC activated\n");

    volatile wq_entry_t *curr;
    unsigned object_offset;

    cpu_set_t cpuset;
    pthread_t thread;
    int s, abort_cnt;
    
    thread = pthread_self();

    /* Set affinity mask to include CPUs 0 to 7 */

    CPU_ZERO(&cpuset);

    CPU_SET(1, &cpuset);

    s = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
    if (s != 0)
      printf("[soft_rmc] RMC not pinned\n");
    
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

	    //HW OCC implementation
#ifdef HW_OCC
	    for(i = 0; i < OBJ_COUNT; i++) {
	      abort_cnt = 0;
	      do {
		if(abort_cnt > 0)
		  printf("[abort detected; abort_cnt = %u\n", abort_cnt);
		object_offset = i * (curr->length/OBJ_COUNT);
		memcpy((uint8_t *)(curr->buf_addr + object_offset),
		       ctx[curr->nid] + curr->offset + object_offset,
		       curr->length/OBJ_COUNT);
		header_src = (nam_obj_header *)(ctx[curr->nid] + curr->offset + object_offset);
		header_dst = (nam_obj_header *)(curr->buf_addr + object_offset);
		abort_cnt++;
	      } while(header_src->version != header_dst->version);
	    }
#else
	    //old version w/o HW OCC
	    memcpy((uint8_t *)curr->buf_addr,
		   ctx[curr->nid] + curr->offset,
		   curr->length);
#endif
	    
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
