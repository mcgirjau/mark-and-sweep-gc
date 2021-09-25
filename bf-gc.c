// ==============================================================================
/**
 * bf-gc.c
 **/
// ==============================================================================



// ==============================================================================
// INCLUDES

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "gc.h"
#include "safeio.h"
// ==============================================================================



// ==============================================================================
// TYPES AND STRUCTURES

/** The header for each allocated object. */
typedef struct header {

  /** Pointer to the next header in the list. */
  struct header* next;

  /** Pointer to the previous header in the list. */
  struct header* prev;

  /** The usable size of the block (exclusive of the header itself). */
  size_t         size;

  /** Is the block allocated or free? */
  bool           allocated;

  /** Whether the block has been visited during reachability analysis. */
  bool           marked;

  /** A map of the layout of pointers in the object. */
  gc_layout_s*   layout;

} header_s;

/** A link in a linked stack of pointers, used during heap traversal. */
typedef struct ptr_link {

  /** The next link in the stack. */
  struct ptr_link* next;

  /** The pointer itself. */
  void* ptr;

} ptr_link_s;
// ==============================================================================



// ==============================================================================
// MACRO CONSTANTS AND FUNCTIONS

/** Double word size. */
#define DBL_WORD_SIZE 16

/** The system's page size. */
#define PAGE_SIZE sysconf(_SC_PAGESIZE)

/**
 * Macros to easily calculate the number of bytes for larger scales (e.g., kilo,
 * mega, gigabytes).
 */
#define KB(size)  ((size_t)size * 1024)
#define MB(size)  (KB(size) * 1024)
#define GB(size)  (MB(size) * 1024)

/** The virtual address space reserved for the heap. */
#define HEAP_SIZE GB(2)

/** Given a pointer to a header, obtain a `void*` pointer to the block itself. */
#define HEADER_TO_BLOCK(hp) ((void*)((intptr_t)hp + sizeof(header_s)))

/** Given a pointer to a block, obtain a `header_s*` pointer to its header. */
#define BLOCK_TO_HEADER(bp) ((header_s*)((intptr_t)bp - sizeof(header_s)))
// ==============================================================================


// ==============================================================================
// GLOBALS

/** The address of the next available byte in the heap region. */
static intptr_t free_addr  = 0;

/** The beginning of the heap. */
static intptr_t start_addr = 0;

/** The end of the heap. */
static intptr_t end_addr   = 0;

/** The head of the free list. */
static header_s* free_list_head = NULL;

/** The head of the allocated list. */
static header_s* allocated_list_head = NULL;

/** The head of the root set stack. */
static ptr_link_s* root_set_head = NULL;
// ==============================================================================



// ==============================================================================
/**
 * Push a pointer onto root set stack.
 *
 * \param ptr The pointer to be pushed.
 */
void rs_push (void* ptr) {

  // Make a new link.
  ptr_link_s* link = malloc(sizeof(ptr_link_s));
  if (link == NULL) {
    ERROR("rs_push(): Failed to allocate link");
  }

  // Have it store the pointer and insert it at the front.
  link->ptr    = ptr;
  link->next   = root_set_head;
  root_set_head = link;
  
} // rs_push ()
// ==============================================================================



// ==============================================================================
/**
 * Pop a pointer from the root set stack.
 *
 * \return The top pointer being removed, if the stack is non-empty;
 *         <code>NULL</code>, otherwise.
 */
void* rs_pop () {

  // Grab the pointer from the link...if there is one.
  if (root_set_head == NULL) {
    return NULL;
  }
  void* ptr = root_set_head->ptr;

  // Remove and free the link.
  ptr_link_s* old_head = root_set_head;
  root_set_head = root_set_head->next;
  free(old_head);

  return ptr;
  
} // rs_pop ()
// ==============================================================================



// ==============================================================================
/**
 * Add a pointer to the _root set_, which are the starting points of the garbage
 * collection heap traversal.  *Only add pointers to objects that will be live
 * at the time of collection.*
 *
 * \param ptr A pointer to be added to the _root set_ of pointers.
 */
void gc_root_set_insert (void* ptr) {

  rs_push(ptr);
  
} // root_set_insert ()
// ==============================================================================



// ==============================================================================
/**
 * The initialization method.  If this is the first use of the heap, initialize it.
 */
void gc_init () {

  // Only do anything if there is no heap region (i.e., first time called).
  if (start_addr == 0) {

    DEBUG("Trying to initialize");
    
    // Allocate virtual address space in which the heap will reside. Make it
    // un-shared and not backed by any file (_anonymous_ space).  A failure to
    // map this space is fatal.
    void* heap = mmap(NULL,
		      HEAP_SIZE,
		      PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS,
		      -1,
		      0);
    if (heap == MAP_FAILED) {
      ERROR("Could not mmap() heap region");
    }

    // Hold onto the boundaries of the heap as a whole.
    start_addr = (intptr_t)heap;
    end_addr   = start_addr + HEAP_SIZE;
    free_addr  = start_addr;

    // DEBUG: Emit a message to indicate that this allocator is being called.
    DEBUG("bf-alloc initialized");

  }

} // gc_init ()
// ==============================================================================


// ==============================================================================
// COPY-AND-PASTE YOUR PROJECT-4 malloc() HERE.
//
//   Note that you may have to adapt small things.  For example, the `init()`
//   function is now `gc_init()` (above); the header is a little bit different
//   from the Project-4 one; my `allocated_list_head` may be a slightly
//   different name than the one you used.  Check the details.
//
/**
 * Allocate and return `size` bytes of heap space.  Specifically, search the
 * free list, choosing the _best fit_.  If no such block is available, expand
 * into the heap region via _pointer bumping_.
 *
 * \param size The number of bytes to allocate.
 * \return A pointer to the allocated block, if successful; `NULL` if unsuccessful.
 */
void* gc_malloc (size_t size) {
  
  /** Ensure that the heap is initialized. */
  gc_init();

  /** Ensure that the pointer that gets returned is double-word aligned.
   *  Specifically, free_addr should be sizeof(header_s) away from a
   *  double-word boundary, so that after the header is put in place, the
   *  usable block is aligned appropriately. */
  intptr_t padding = (sizeof(header_s) + DBL_WORD_SIZE - (free_addr % DBL_WORD_SIZE)) % DBL_WORD_SIZE;
  free_addr += padding;

  /** If trying to allocate a block of zero length, return a null pointer. */
  if (size == 0) {
    return NULL;
  }

  /** Start from the head of the list, and search for a best fit by hopping 
   *  from free block to free block (i.e. peruse the entire list of free blocks
   *  to find the best fit). */
  header_s* current = free_list_head;
  header_s* best    = NULL;

  /** Keep following pointers to the next free block until we reach a null pointer. */
  while (current != NULL) {

    /** If we find an allocated block in the list of free blocks, throw an error. */
    if (current->allocated) {
      ERROR("Allocated block on free list", (intptr_t)current);
    }
    
    /** The current block is a best fit if:
     *  (1) We haven't found a best fit yet, and the current block's size is
     *      at least as large as the requested size, OR
     *  (2) We have found a best fit, but the current block is an even better
     *      fit, i.e. it's smaller than the best fit we previously found, but
     *      still at least as big as the requested size. */
    if ( (best == NULL && size <= current->size) ||
	 (best != NULL && size <= current->size && current->size < best->size) ) {
      best = current;
    }

    /** If we have found a best fit that's exactly the requested size, we are
     *  done and can break out of the loop (to avoid unneccessary iterations). */
    if (best != NULL && best->size == size) {
      break;
    }

    /** Move on to the next free block in the linked list, and keep iterating. */
    current = current->next;
    
  }

  /** Pointer to the block we will return. Don't know what that is yet, so
   *  make it a null pointer. */
  void* new_block_ptr = NULL;

  /** If we have found a best fit... */
  if (best != NULL) {

    /** ...remove it from the linked list of free blocks. Specifically, if it
     *  is the first in the list (i.e. it has no previous block), then just 
     *  set the free list head to the free block immediately following it.
     *  If it is not the first block in the list (i.e. it has a block before
     *  it), make that previous block point to the block immediately following
     *  the best fit one (i.e. skip over the best fit block). */
    if (best->prev == NULL) {
      free_list_head   = best->next;
    } else {
      best->prev->next = best->next;
    }

    /** If the best fit block has a block following it, make that next block
     *  point to the block before the best fit one as its predecessor (i.e. skip
     *  over the best fit block). */
    if (best->next != NULL) {
      best->next->prev = best->prev;
    }
   
    /** Remove the best fit block's pointer to its predecessor. */ 
    best->prev = NULL;

    /** Remove the best fit block's pointer to its successor. */
    best->next = NULL;

    /** We have allocated the best fit block. Set a pointer to that block. */
    best->allocated = true;
    new_block_ptr   = HEADER_TO_BLOCK(best);
    
  } else {

    /** If we have not found a best fit, then we must pointer bump and keep
     *  growing the heap by creating a new block. Set a pointer to that block. */
    header_s* header_ptr = (header_s*)free_addr;
    new_block_ptr = HEADER_TO_BLOCK(header_ptr);

    /** The block will not be part of a linked list (since it is allocated),
     *  its size will be exactly the requested size, and we must signal that
     *  it is allocated. */
    header_ptr->next      = NULL;
    header_ptr->prev      = NULL;
    header_ptr->size      = size;
    header_ptr->allocated = true;
    
    /** Pointer bumping: find the next new free address in the heap by moving
     *  away from the block pointer by a translation equal to the block's size. */
    intptr_t new_free_addr = (intptr_t)new_block_ptr + size;

    /** Have we exceeded the maximum size of the heap? */
    if (new_free_addr > end_addr) {

      /** If yes, then return a null pointer - allocation failed. */
      return NULL;

    } else {

      /** If not, then the first free address gets updated accordingly. */
      free_addr = new_free_addr;

    }
    
  }

  /** Insert the block at the beginning of the linked list of allocated blocks, 
   *  i.e. make it point to the first allocated block currently in the list, and
   *  set the head of the allocated list to point at the block we are allocating.
   *  Also, since the newly allocated block is the first one in the list, there
   *  will be no block before it, so its previous pointer is a null pointer. */
  header_s* allocated_header_ptr = BLOCK_TO_HEADER(new_block_ptr);
  allocated_header_ptr->next = allocated_list_head;
  allocated_list_head = allocated_header_ptr;
  allocated_header_ptr->prev = NULL;

  /** If the next block is not null, then we must ensure its previous pointer
   *  will point to the block we are deallocating. */
  if (allocated_header_ptr->next != NULL) {
    allocated_header_ptr->next->prev = allocated_header_ptr;
  }

  /** Return a pointer to the newly allocated block - allocation succeeded. */
  return new_block_ptr;

} // gc_malloc ()
// ==============================================================================



// ==============================================================================
// COPY-AND-PASTE YOUR PROJECT-4 free() HERE.
//
//   See above.  Small details may have changed, but the code should largely be
//   unchanged.
//
/**
 * Deallocate a given block on the heap.  Add the given block (if any) to the
 * free list.
 *
 * \param ptr A pointer to the block to be deallocated.
 */
void gc_free (void* ptr) {
    
  /** If passed a null pointer, there's nothing to free. */
  if (ptr == NULL) {
    return;
  }

  /** Get a pointer to the block's header. */
  header_s* header_ptr = BLOCK_TO_HEADER(ptr);

  /** If the block is not allocated, there's no point in trying to free it,
   *  so throw an error. */
  if (!header_ptr->allocated) {
    ERROR("Double-free: ", (intptr_t)header_ptr);
  }

  /** Remove the block from the linked list of allocated blocks. Specifically, 
   *  if it is the first in the list (i.e. it has no previous block), then just 
   *  set the allocated list head to the allocated block immediately following it.
   *  If it is not the first block in the list (i.e. it has a block before
   *  it), make that previous block point to the block immediately following
   *  the one we are deallocating (i.e. skip over the deallocated block). */
   if (header_ptr->prev == NULL) {
     allocated_list_head = header_ptr->next;
   } else {
     header_ptr->prev->next = header_ptr->next;
   }

   /** If the deallocated block has a block following it, make that next block
    *  point to the block before the deallocated one as its predecessor (i.e. skip
    *  over the deallocated block). */
   if (header_ptr->next != NULL) {
     header_ptr->next->prev = header_ptr->prev;
   }
  
   /** Remove the deallocated block's pointer to its predecessor. */
   header_ptr->prev = NULL;

   /** Remove the deallocated block's pointer to its successor. */
   header_ptr->next = NULL;

  /** Insert the block at the beginning of the linked list of free blocks, i.e.
   *  make it point to the first free block currently in the list, and set the 
   *  head of the free list to point at the block we are deallocating. Also,
   *  since the newly deallocated block is the first one in the list, there
   *  will be no block before it, so its previous pointer is a null pointer. */
  header_ptr->next = free_list_head;
  free_list_head   = header_ptr;
  header_ptr->prev = NULL;

  /** If the next block is not null, then we must ensure its previous pointer
   *  will point to the block we are deallocating. */
  if (header_ptr->next != NULL) {
    header_ptr->next->prev = header_ptr;
  }

  /** We are done, so mark the block as officially deallocated. */
  header_ptr->allocated = false;

} // gc_free ()
// ==============================================================================



// ==============================================================================
/**
 * Allocate and return heap space for the structure defined by the given
 * `layout`.
 *
 * \param layout A descriptor of the fields
 * \return A pointer to the allocated block, if successful; `NULL` if unsuccessful.
 */
void* gc_new (gc_layout_s* layout) {

  // Get a block large enough for the requested layout.
  void*     block_ptr  = gc_malloc(layout->size);
  header_s* header_ptr = BLOCK_TO_HEADER(block_ptr);

  // Hold onto the layout for later, when a collection occurs.
  header_ptr->layout = layout;
  
  return block_ptr;
  
} // gc_new ()
// ==============================================================================



// ==============================================================================
/**
 * Traverse the heap, marking all live objects.
 */
void mark () {

  // WRITE ME.
  //
  //   Adapt the pseudocode from class for a copying collector to real code here
  //   for a non-copying collector.  Do the traversal, using the linked stack of
  //   pointers that starts at `root_set_head`, setting the `marked` field on
  //   each object you reach.
  
  /** We begin our depth-first search at the begining of our root set, and keep
   *  going until we ecounter a null pointer. */
  while (root_set_head != NULL) {

    /** Get the curret pointer. */
    void* current_ptr = rs_pop();

    /** If the curret pointer actually points to something in memory, then
     *  mark that place and add all its pointers to the stack for searching. */
    if (current_ptr != NULL) {

      header_s* header = BLOCK_TO_HEADER(current_ptr);

      if (!header->marked) {
        header->marked = true;
      }

      /** Where can we travel from here? */
      gc_layout_s* current_layout = header->layout;

      /** Add those places to our list, to be searched later. */
      for (int i = 0; i < current_layout->num_ptrs; i++) {

        /** Get handle. */
        void** handle = current_ptr + current_layout->ptr_offsets[i];

        /** Get pointer. */
        void* ptr = *handle;

        /** Add the pointer to the list. */
        rs_push(ptr);

      }

    }

  }  

} // mark ()
// ==============================================================================



// ==============================================================================
/**
 * Traverse the allocated list of objects.  Free each unmarked object;
 * unmark each marked object (preparing it for the next sweep.
 */
void sweep () {

  // WRITE ME
  //
  //   Walk the allocated list.  Each object that is marked is alive, so clear
  //   its mark.  Each object that is unmarked is dead, so free it with
  //   `gc_free()`.
  
  /** Start at the beginning of the allocated list, and free unmarked blocks. */
  header_s* current_ptr = allocated_list_head;

  /** Keep checking blocks until we reach the end of the allocated list. */
  while (current_ptr != NULL) {

    /** Get a hold of the next header before we free the block (if we do). */
    header_s* next_ptr = current_ptr->next;

    /** Get the curret block. */
    void* current_block = HEADER_TO_BLOCK(current_ptr);

    /** If the current header is unmarked, then we free it. If it is marked,
     *  we leave it alone, but we unmark it for future garbage collectios. */
    if (current_ptr->marked) {
      current_ptr->marked = false;
    } else {
      gc_free(current_block);
    }

    /** Move on to the next block. */
    current_ptr = next_ptr;

  }

} // sweep ()
// ==============================================================================



// ==============================================================================
/**
 * Garbage collect the heap.  Traverse and _mark_ live objects based on the
 * _root set_ passed, and then _sweep_ the unmarked, dead objects onto the free
 * list.  This function empties the _root set_.
 */
void gc () {

  // Traverse the heap, marking the objects visited as live.
  mark();

  // And then sweep the dead objects away.
  sweep();

  // Sanity check:  The root set should be empty now.
  assert(root_set_head == NULL);
  
} // gc ()
// ==============================================================================
