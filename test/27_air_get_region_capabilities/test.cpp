
#include <cstdio>
#include <cassert>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>

#include <xaiengine.h>

#include "air_host.h"
#include "acdc_queue.h"
#include "hsa_defs.h"

#define SHMEM_BASE 0x020100000000LL
#define CAPABILITIES_SCRATCH_BASE (SHMEM_BASE+0x1000LL)

#define XAIE_NUM_ROWS            8
#define XAIE_NUM_COLS           50
#define XAIE_ADDR_ARRAY_OFF     0x800

#define HIGH_ADDR(addr)	((addr & 0xffffffff00000000) >> 32)
#define LOW_ADDR(addr)	(addr & 0x00000000ffffffff)


int main(int argc, char *argv[])
{

  // create the queue
  queue_t *q = nullptr;
  auto ret = air_queue_create(MB_QUEUE_SIZE, HSA_QUEUE_TYPE_SINGLE, &q, SHMEM_BASE);
  assert(ret == 0 && "failed to create queue!");

  // reserve a packet in the queue
  uint64_t wr_idx = queue_add_write_index(q, 1);
  uint64_t packet_id = wr_idx % q->size;

  // capabilities packet
  dispatch_packet_t *herd_pkt = (dispatch_packet_t*)(q->base_address_vaddr) + packet_id;
  initialize_packet(herd_pkt);
  herd_pkt->type = HSA_PACKET_TYPE_AGENT_DISPATCH;

  // Set up a 2x4 herd starting 7,2
  herd_pkt->arg[0]  = AIR_PKT_TYPE_GET_CAPABILITIES;
  herd_pkt->arg[1] = CAPABILITIES_SCRATCH_BASE;

  // dispatch packet
  signal_create(1, 0, NULL, (signal_t*)&herd_pkt->completion_signal);
  signal_create(0, 0, NULL, (signal_t*)&q->doorbell);
  signal_store_release((signal_t*)&q->doorbell, wr_idx);

  // wait for packet completion
  while (signal_wait_aquire((signal_t*)&herd_pkt->completion_signal, HSA_SIGNAL_CONDITION_EQ, 0, 0x80000, HSA_WAIT_STATE_ACTIVE) != 0) {
    printf("packet completion signal timeout on getting capabilities!\n");
    printf("%x\n", herd_pkt->header);
    printf("%x\n", herd_pkt->type);
    printf("%x\n", (unsigned)herd_pkt->completion_signal);
  }

  // Check the packet we got back
  uint32_t errors = 0;

  int fd = open("/dev/mem", O_RDWR | O_SYNC);
  if (fd == -1)
    return HSA_STATUS_ERROR_INVALID_QUEUE_CREATION;

  uint64_t *capabilities = (uint64_t *)mmap(NULL, 0x8000, PROT_READ|PROT_WRITE, MAP_SHARED, fd, CAPABILITIES_SCRATCH_BASE);
  uint64_t expected[8] = {0L, 1L, 0x10001L, 16L, 32768L, 8L, 16384L, 0L};
  for (int i=0;i<8;i++) {
    if (capabilities[i] != expected[i]) {
      printf("Register %X: expected 0x%016lX: read 0x%016lX\n", i, expected[i], capabilities[i]);
      errors++;
    }
  }
  printf("MB %d of %d total\n", capabilities[0], capabilities[1]);

  if (errors == 0x0)
    printf("PASS!\n");
  else
    printf("fail.\n");
}