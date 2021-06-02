// (c) Copyright 2020 Xilinx Inc. All Rights Reserved.

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <xaiengine.h>

#include "air_host.h"
#include "test_library.h"

#include "acdc_queue.h"
#include "hsa_defs.h"

#define SHMEM_BASE 0x020100000000LL

namespace {

// global libxaie state
air_libxaie1_ctx_t *xaie;

#define TileInst (xaie->TileInst)
#define TileDMAInst (xaie->TileDMAInst)
#include "aie.inc"
#undef TileInst
#undef TileDMAInst

}

int
main(int argc, char *argv[])
{
  uint64_t col = 7;
  uint64_t row = 0;

  xaie = air_init_libxaie1();

  ACDC_print_dma_status(xaie->TileInst[7][2]);


  mlir_configure_cores();
  mlir_configure_switchboxes();
  mlir_initialize_locks();
  mlir_configure_dmas();
  mlir_start_cores();

  XAieDma_Shim ShimDmaInst1;
  uint32_t *bram_ptr;

  #define BRAM_ADDR 0x020100000000LL
  #define DMA_COUNT 512

  // Ascending plus 2 sequence in the tile memory, and toggle the associated lock
  for (int i=0; i<DMA_COUNT; i++)
    mlir_write_buffer_a(i, i+2);
  XAieTile_LockRelease(&(xaie->TileInst[7][2]), 0, 0x1, 0);
  //XAieTile_LockRelease(&(xaie->TileInst[7][2]), 1, 0x1, 0);


  // create the queue
  queue_t *q = nullptr;
  auto ret = air_queue_create(MB_QUEUE_SIZE, HSA_QUEUE_TYPE_SINGLE, &q, 0x020100000000LL);
  assert(ret == 0 && "failed to create queue!");


  // Let's make a buffer that we can transfer in the same BRAM, after the queue of HSA packets
  int fd = open("/dev/mem", O_RDWR | O_SYNC);
  if (fd == -1)
    return HSA_STATUS_ERROR_INVALID_QUEUE_CREATION;

  bram_ptr = (uint32_t *)mmap(NULL, 0x8000, PROT_READ|PROT_WRITE, MAP_SHARED, fd, BRAM_ADDR+(MB_QUEUE_SIZE*64));
  // Lets stomp over it!
  for (int i=0;i<DMA_COUNT;i++) {
    bram_ptr[i] = 0xdeadbeef;
  }

  uint64_t wr_idx = queue_add_write_index(q, 1);
  uint64_t packet_id = wr_idx % q->size;

  dispatch_packet_t *herd_pkt = (dispatch_packet_t*)(q->base_address_vaddr) + packet_id;
  air_packet_herd_init(herd_pkt, 0, col, 1, row, 3);
  air_queue_dispatch_and_wait(q, wr_idx, herd_pkt);

  wr_idx = queue_add_write_index(q, 1);
  packet_id = wr_idx % q->size;

  dispatch_packet_t *shim_pkt = (dispatch_packet_t*)(q->base_address_vaddr) + packet_id;
  initialize_packet(shim_pkt);
  shim_pkt->type = HSA_PACKET_TYPE_AGENT_DISPATCH;
  shim_pkt->arg[0]  = AIR_PKT_TYPE_DEVICE_INITIALIZE;
  shim_pkt->arg[0] |= (AIR_ADDRESS_ABSOLUTE_RANGE << 48);
  shim_pkt->arg[0] |= ((uint64_t)XAIE_NUM_COLS << 40);

  air_queue_dispatch_and_wait(q, wr_idx, shim_pkt);

  wr_idx = queue_add_write_index(q, 1);
  packet_id = wr_idx % q->size;

  dispatch_packet_t *pkt = (dispatch_packet_t*)(q->base_address_vaddr) + packet_id;
  air_packet_nd_memcpy(pkt, 0, col, 0, 0, 4, 2, BRAM_ADDR+(MB_QUEUE_SIZE*64), DMA_COUNT*sizeof(float), 1, 0, 1, 0, 1, 0);
  air_queue_dispatch_and_wait(q, wr_idx, pkt);

  ACDC_print_dma_status(xaie->TileInst[7][2]);

  uint32_t errs = 0;
  // Let go check the tile memory
  for (int i=0; i<DMA_COUNT; i++) {
    uint32_t d = mlir_read_buffer_a(i);
    if (d != i+2) {
      printf("ERROR: Tile Memory id %d Expected %08X, got %08X\n", i, i+2, d);
      errs++;
    }
  }
  for (int i=0; i<DMA_COUNT; i++) {
    if (bram_ptr[i] != 2+i) {
      printf("ERROR: L2 Memory id %d Expected %08X, got %08X\n", i, i+2, bram_ptr[i]);
      errs++;
    }
  }

  if (errs == 0)
    printf("PASS!\n");
  else
    printf("fail.\n");


  for (int bd=0;bd<16;bd++) {
    // Take no prisoners.  No regerts
    // Overwrites the DMA_BDX_Control registers
    XAieGbl_Write32(xaie->TileInst[7][0].TileAddr + 0x0001D008+(bd*0x14), 0x0);
    XAieGbl_Write32(xaie->TileInst[7][2].TileAddr + 0x0001D018+(bd*0x20), 0x0);
  }
}
