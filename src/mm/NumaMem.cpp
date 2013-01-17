/** Memory allocator implementation -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2012, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 *
 * @section Description
 *
 * @author Andrew Lenharth <andrewl@lenharth.org>
 */
#include "Galois/Runtime/mm/Mem.h"

#ifdef GALOIS_USE_NUMA
#include <numa.h>
#endif

void* Galois::Runtime::MM::largeAlloc(size_t len) {
  return malloc(len);
}

void Galois::Runtime::MM::largeFree(void* m, size_t len) {
  free(m);
}

void* Galois::Runtime::MM::largeInterleavedAlloc(size_t len) {
  void* data = 0;
#if defined GALOIS_USE_NUMA_OLD
  nodemask_t nm = numa_no_nodes;
  unsigned int num = activeThreads;
  for (unsigned y = 0; y < num; ++y)
    nodemask_set(&nm, y/4);
  data = numa_alloc_interleaved_subset(len, &nm);
#elif defined GALOIS_USE_NUMA
  bitmask* nm = numa_allocate_nodemask();
  unsigned int num = activeThreads;
  for (unsigned y = 0; y < num; ++y)
    numa_bitmask_setbit(nm, y/4);
  data = numa_alloc_interleaved_subset(len, nm);
  numa_free_nodemask(nm);
#else
  data = malloc(len);
#endif
  if (!data)
    abort();
  return data;
}

void Galois::Runtime::MM::largeInterleavedFree(void* m, size_t len) {
#ifdef GALOIS_USE_NUMA
  numa_free(m, len);
#else
  free(m);
#endif
}
