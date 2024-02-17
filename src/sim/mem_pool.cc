/*
 * Copyright (c) 2011-2021 Advanced Micro Devices, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "sim/mem_pool.hh"

#include "base/addr_range.hh"
#include "base/logging.hh"

namespace gem5
{
  
MemPool::MemPool(Addr page_shift, Addr ptr, Addr limit)
        : pageShift(page_shift), startPageNum(ptr >> page_shift),
        freePageNum(ptr >> page_shift),
        _totalPages((limit - ptr) >> page_shift)
{
    gem5_assert(_totalPages > 0);

    // buddy init
    for(int i = 0; i <= 9; i++) {
      // nothing is connected to the list for orders 0 to 9
      list_heads[i] = NULL;
    }

    // initialize the 10th order list
    const int n_buddies_init = _totalPages / 1024;
    struct buddy* next_buddy = NULL;
    for(int i = n_buddies_init; i > 0; i--) {    
      struct buddy* b = (struct buddy*)malloc(sizeof(struct buddy));
      b->size = 1024;
      b->start = (i - 1) * 1024;
      b->next = next_buddy;
      next_buddy = b;
    }
    list_heads[10] = next_buddy;
    
    // set the prev members of the initialized buddies
    struct buddy* prev_buddy = NULL;
    for(struct buddy* p = list_heads[10]; p != NULL; p = p->next) {
      p->prev = prev_buddy;
      prev_buddy = p;
    }
}

int power2(int n) {
  int ret = 1;
  for(int i = 0; i<n; i++) {
    ret *= 2;
  }
  return ret;
}

Counter
MemPool::startPage() const
{
    return startPageNum;
}

Counter
MemPool::freePage() const
{
    return freePageNum;
}

void
MemPool::setFreePage(Counter value)
{
    freePageNum = value;
}

Addr
MemPool::freePageAddr() const
{
    return freePageNum << pageShift;
}

Counter
MemPool::totalPages() const
{
    return _totalPages;
}

Counter
MemPool::allocatedPages() const
{
    return freePageNum - startPageNum;
}

Counter
MemPool::freePages() const
{
    return _totalPages - allocatedPages();
}

Addr
MemPool::startAddr() const
{
    return startPage() << pageShift;
}

Addr
MemPool::allocatedBytes() const
{
    return allocatedPages() << pageShift;
}

Addr
MemPool::freeBytes() const
{
    return freePages() << pageShift;
}

Addr
MemPool::totalBytes() const
{
    return totalPages() << pageShift;
}

struct buddy* MemPool::find_buddy(int order) {
  assert(order <= 10);
  if(list_heads[order] != NULL) {
    struct buddy* b = list_heads[order];

    // disconnect b from the list
    list_heads[order] = b->next;
    if (list_heads[order] != NULL)
      list_heads[order]->prev = NULL;

    // clean up metadata in b before returning it
    b->next = NULL;
    b->prev = NULL;

    return b;
  }
  else {
    struct buddy* bigger = find_buddy(order + 1);
    struct buddy* smaller_1 = (struct buddy*)malloc(sizeof(struct buddy));
    struct buddy* smaller_2 = (struct buddy*)malloc(sizeof(struct buddy));

    // to be returned
    smaller_1->size = bigger->size / 2;
    smaller_1->start = bigger->start;
    smaller_1->next = NULL;
    smaller_1->prev = NULL;

    // to be connected to a list
    smaller_2->size = bigger->size / 2;
    smaller_2->start = bigger->start + smaller_2->size;
    smaller_2->next = NULL;
    smaller_2->prev = NULL;
    list_heads[order] = smaller_2;

    // the bigger buddy vanishes
    free(bigger);
    
    return smaller_1;
  }
}

void MemPool::insert_buddy(struct buddy* b, int order) {
  if(list_heads[order] == NULL) {
    list_heads[order]= b;
    return;
  }

  bool inserted = false;
  for(struct buddy* p = list_heads[order]; !inserted; p = p->next) {
    Addr prev_start = (p->prev == NULL ? 0 : p->prev->start);

    if (prev_start <= b->start && p->start > b->start) {
      b->next = p;

      // insert the buddy before p
      if (p->prev == NULL) {
	list_heads[order] = b;
      }
      else {
	p->prev->next = b;
      }

      inserted = true;
    }

    // nothing beyond and b is still not inserted
    if (p->next == NULL && !inserted) {
      p->next = b;
      b->next = NULL;
      b->prev = p;
      inserted = true;
    }
  }

  // maximum order: no merging happens
  if (order == 10)
    return;
  
  // merging: b is the left buddy
  if (b->start % power2(order+1) == 0) {
    if (b->next != NULL && (b->next->start - b->start) == b->size) {
      // disconnect b and its paired buddy (b->next->next could be NULL)
      if (b->prev == NULL) {
	list_heads[order] = b->next->next; 
      }
      else {
	b->prev->next = b->next->next;
      }

      // create a bigger buddy and insert it into a higer order list
      struct buddy* bigger = (struct buddy*)malloc(sizeof(struct buddy));
      bigger->start = b->start;
      bigger->size = b->size * 2;
      bigger->next = NULL;
      bigger->prev = NULL;
      insert_buddy(bigger, order + 1);

      free(b->next);
      free(b);
    }
  }
  // merging: b is the right buddy
  else {
    if (b->prev != NULL && (b->start - b->prev->start) == b->size) {
      // disconnect b and its paired buddy (b->next->next could be NULL)
      if (b->prev->prev == NULL) {
	list_heads[order] = b->next;
      }
      else {
	b->prev->prev->next = b->next;
      }

      // create a bigger buddy and insert it into a higer order list
      struct buddy* bigger = (struct buddy*)malloc(sizeof(struct buddy));
      bigger->start = b->prev->start;
      bigger->size = b->prev->size * 2;
      bigger->next = NULL;
      bigger->prev = NULL;
      insert_buddy(bigger, order + 1);

      free(b->prev);
      free(b);
    }
  }
}

Addr MemPool::allocate(Addr npages) {
  assert(npages >= Addr(1));
  assert(npages <= Addr(1024));

  int order;
  for(order = 0; power2(order) < npages && order <= 10; order++) {
    ;
  }

  struct buddy* b = find_buddy(order);
  Addr ret = b->start << pageShift;
  free(b);

  return ret;
}

void MemPool::deallocate(Addr start, Addr npages) {
  assert(npages == Addr(1));
  assert(((start >> pageShift) << pageShift) == start);

  struct buddy* b = (struct buddy*)malloc(sizeof(struct buddy));
  b->size = 1;
  b->start = (start >> pageShift);

  insert_buddy(b, 0);
}

void
MemPool::serialize(CheckpointOut &cp) const
{
    paramOut(cp, "page_shift", pageShift);
    paramOut(cp, "start_page", startPageNum);
    paramOut(cp, "free_page_num", freePageNum);
    paramOut(cp, "total_pages", _totalPages);
}

void
MemPool::unserialize(CheckpointIn &cp)
{
    paramIn(cp, "page_shift", pageShift);
    paramIn(cp, "start_page", startPageNum);
    paramIn(cp, "free_page_num", freePageNum);
    paramIn(cp, "total_pages", _totalPages);
}

void
MemPools::populate(const AddrRangeList &memories)
{
    for (const auto &mem : memories)
        pools.emplace_back(pageShift, mem.start(), mem.end());
}

Addr
MemPools::allocPhysPages(int npages, int pool_id)
{
    return pools[pool_id].allocate(npages);
}

void
MemPools::deallocPhysPages(Addr start, int npages, int pool_id)
{
    pools[pool_id].deallocate(start, npages);
}
  
Addr
MemPools::memSize(int pool_id) const
{
    return pools[pool_id].totalBytes();
}

Addr
MemPools::freeMemSize(int pool_id) const
{

    return pools[pool_id].freeBytes();
}

void
MemPools::serialize(CheckpointOut &cp) const
{
    ScopedCheckpointSection sec(cp, "mempools");
    int num_pools = pools.size();
    SERIALIZE_SCALAR(num_pools);

    for (int i = 0; i < num_pools; i++)
        pools[i].serializeSection(cp, csprintf("pool%d", i));
}

void
MemPools::unserialize(CheckpointIn &cp)
{
    // Delete previous mem_pools
    pools.clear();

    ScopedCheckpointSection sec(cp, "mempools");
    int num_pools = 0;
    UNSERIALIZE_SCALAR(num_pools);

    for (int i = 0; i < num_pools; i++) {
        MemPool pool;
        pool.unserializeSection(cp, csprintf("pool%d", i));
        pools.push_back(pool);
    }
}

} // namespace gem5
