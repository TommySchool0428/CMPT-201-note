#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct header {
  uint64_t size;
  struct header *next;
  int id;
};

void initialize_block(struct header *block, uint64_t size, struct header *next,
                      int id) {
  block->size = size;
  block->next = next;
  block->id = id;
}

int find_first_fit(struct header *free_list_ptr, uint64_t size) {
  struct header *current = free_list_ptr;
  while (current != NULL) {
    if (current->size >= size) {
      return current->id;
    }
    current = current->next;
  }
  return -1;
}

int find_best_fit(struct header *free_list_ptr, uint64_t size) {
  int best_fit_id = -1;
  uint64_t best_fit_size = UINT64_MAX;
  struct header *current = free_list_ptr;
  while (current != NULL) {
    if (current->size >= size && current->size < best_fit_size) {
      best_fit_size = current->size;
      best_fit_id = current->id;
    }
    current = current->next;
  }
  return best_fit_id;
}

int find_worst_fit(struct header *free_list_ptr, uint64_t size) {
  int worst_fit_id = -1;
  uint64_t worst_fit_size = 0;
  struct header *current = free_list_ptr;
  while (current != NULL) {
    if (current->size >= size && current->size > worst_fit_size) {
      worst_fit_size = current->size;
      worst_fit_id = current->id;
    }
    current = current->next;
  }
  return worst_fit_id;
}

int main(void) {

  struct header *free_block1 = (struct header*) malloc(sizeof(struct header));
  struct header *free_block2 = (struct header*) malloc(sizeof(struct header));
  struct header *free_block3 = (struct header*) malloc(sizeof(struct header));
  struct header *free_block4 = (struct header*) malloc(sizeof(struct header));
  struct header *free_block5 = (struct header*) malloc(sizeof(struct header));

  initialize_block(free_block1, 6, free_block2, 1);
  initialize_block(free_block2, 12, free_block3, 2);
  initialize_block(free_block3, 24, free_block4, 3);
  initialize_block(free_block4, 8, free_block5, 4);
  initialize_block(free_block5, 4, NULL, 5);

  struct header *free_list_ptr = free_block1;

  int first_fit_id = find_first_fit(free_list_ptr, 7);
  int best_fit_id = find_best_fit(free_list_ptr, 7);
  int worst_fit_id = find_worst_fit(free_list_ptr, 7);

  printf("First fit block ID: %d\n", first_fit_id);
  printf("Best fit block ID: %d\n", best_fit_id);
  printf("Worst fit block ID: %d\n", worst_fit_id);

  return 0;
}

/*
Pseudo-code: coalesce contiguous free blocks after freeing a block

Inputs:
  head: pointer to the first free block in the linked free list
  freed: pointer to the block that has just been returned to the free list

Algorithm:
1. Insert freed into the free list, keeping the list sorted by address.
2. prev <- NULL, curr <- head
3. while curr != NULL and curr < freed:
       prev <- curr
       curr <- curr->next
4. freed->next <- curr
   if prev != NULL: prev->next <- freed else head <- freed
5. // merge with previous block if adjacent in memory
   if prev != NULL and (prev + prev->size + header_size) == freed:
         prev->size += header_size + freed->size
         prev->next <- freed->next
         freed <- prev
6. // merge with next block if adjacent in memory
   if freed->next != NULL and (freed + freed->size + header_size) == freed->next:
         freed->size += header_size + freed->next->size
         freed->next <- freed->next->next
7. return head

Example (blocks ordered left-to-right in memory):
  Before free: [a][b][c][d][m][z][n][f][g][h]
  z is freed; suppose free list contains existing free blocks b, m, n, g.
  Steps:
    - Insert z between m and n because of address order.
    - No merge with previous (m) if m is allocated; merge occurs only with free neighbors.
    - Since n is free and contiguous with z, merge z+n into a single block sized |z|+|n|.
    - If that merged block now abuts f (allocated) no further merge; if g were free and adjacent
      it would also merge, producing a single larger free region where z and n (and possibly g)
      resided.
  After coalescing in the given layout (only n is free next to z):
    [a][b][c][d][m][(z+n merged)][f][g][h]
*/