#include "alloc.h"
#include <stdio.h>
#include <unistd.h>
#include <stddef.h>
#include <stdint.h>

#define BLOCK_SIZE offsetof(struct block, anchor)

typedef struct block *meta_block;

meta_block base = NULL;

size_t align_64b(ssize_t x);
meta_block find_block(meta_block *last, size_t size);
meta_block extend_heap(meta_block last, size_t new_size);
void split_block(meta_block b, size_t new_size);
void *my_malloc(size_t new_size);
void *my_calloc(size_t num, size_t size);
meta_block fusion(meta_block block, int ok);
meta_block get_pointer_to_meta_block(void *ptr);
int valid_addr(void *p);
void my_free(void *p);
void copy_block(meta_block original, meta_block copy);
meta_block find_last_block(void);
void *my_realloc(void *p, size_t new_size);
/*
Metadata block
@param size Size that the user allocates
@param next Pointer to the next block in the double-linked list
@param prev Pointer to the previous block in the double-linked list
@param free Int(otherwise padding) if the chunk is free 1->free | 0->claimed 
@param anchor Pointer to the first byte after the metadata block
*/
struct block {
    size_t  size;
    meta_block next;
    meta_block prev;
    int free;
    int padding;
    char anchor[1];
};


/*
Custom malloc function
@param new_size The bytes allocated by the user
@return Pointer to the begining of the new allocated heap memory
*/
void *my_malloc(size_t new_size) {
    meta_block block = NULL;
    meta_block last = NULL;
    new_size = align_64b(new_size);          // 8-byte aligned input for sbrk()  
    if(!new_size)
        return NULL;
    if (base == NULL) {
    // First block allocation
        base = extend_heap(NULL, new_size);
        if(!base)
            return NULL;
        block = base;
    } else {
        block = find_block(&last, new_size);
        if(block) {
            if(block->size - new_size >= BLOCK_SIZE + 8)
                split_block(block, new_size);
            block->free = 0;
        } else {
            block = extend_heap(last, new_size);
            if(!block)
                return NULL; 
        }
    }
    return (void*) block->anchor;  
}

/*
 Allocates memory for an array and initializes all bytes in the allocated block to zero
 @param num Number of elements to allocate
 @param size Size of each element
 @return Pointer to the begining of the new allocated heap memory
*/
void *my_calloc(size_t num, size_t size) {
    size_t *new;
    size_t size_8, i;
    new = my_malloc(num * size);
    if(new) {
        size_8 = align_64b(num*size)>>3; 
        for(i=0; i<size_8; i++)
            new[i]=0;
    }
    return new;
}

/*
Takes the value of the chunk size and aligns it to 8 bytes
@param x The number of bytes as ssize_t
@return size_t of the aligned value or 0 if the provided value is negative
*/
size_t align_64b(ssize_t x) {
    if(x<=0) {
        fprintf(stderr, "Error: Negative byte amount allocation is not permited!\n");
        return 0;
    }
    return (((x-1)>>3)<<3)+8;
}

/*
Find the first free block that matches the size 
Modifies content of the caller param to the block prior to the first valid block
@param last Pointer to a meta_block pointer
@param size Bytes allocated by the user
@return Pointer to the first block with necessary size
*/
meta_block find_block(meta_block *last, size_t size) {
    meta_block b = base;
    while(b && !(b->free && b->size >= size)) {
         *last = b;
        b = b->next;
    }  
    return b;
}
/*
Extends the heap if the OS allows it
@param last Last created block
@param size Bytes allocated by the user
@return Pointer to the newly added block
*/
meta_block extend_heap(meta_block last, size_t new_size) {
    meta_block new_b = sbrk(0);
    if(sbrk(new_size + BLOCK_SIZE) == (void*)-1) 
        return NULL;
    new_b->size = new_size;
    new_b->next = NULL;
    new_b->free = 0;
    new_b->prev = last;
    if(last)
        last->next = new_b;
    return new_b;
}

/*
Split a block in 2 to maximize space usage and the first block is used
@param b Pointer to the block to split
@param new_size Bytes allocated by the user
*/
void split_block(meta_block b, size_t new_size) {
    meta_block new_b = (meta_block)((char*)b->anchor + new_size);
// set the metadata of the new block
    new_b->size = b->size - new_size - BLOCK_SIZE;
    new_b->prev = b;
    new_b->next = b->next;
    new_b->free = 1;
// set metadata of the next block if it exists
    if (new_b->next)
        new_b->next->prev = new_b;
// set metadata of partial block
    b->size = new_size;
    b->next = new_b;
    b->free = 0;
}

/*
After freeing a block, fuse(merge) all adjacent free blocks into a single block
@param block The block that was freed
@param ok Flag for recursive call that has the value 1 for the first call
@return Pointer to the merged block
*/
meta_block fusion(meta_block block, int ok) {
    if(ok){
        ok=0;
        if(block->next && block->next->free){
            block->size += block->next->size + BLOCK_SIZE;
            block->next = block->next->next;
            if(block->next)
                block->next->prev = block;
            ok = 1;
        }
        if(block->prev && block->prev->free) {
            block->prev->size += block->size + BLOCK_SIZE;
            if(block->next)
                block->next->prev = block->prev;
            block->prev->next = block->next;
            block = block->prev;
            ok=1;
        }
        if(ok) 
            return fusion(block, ok);
    }
    return block;
}

/* 
Receives a pointer to a valid allocated memory and returns a pointer to the begining of the metablock
@param ptr Pointer to the allocated memory
@return Pointer to the begining of the metablock
 */
meta_block get_pointer_to_meta_block(void *ptr) {
    return (meta_block)((char*)ptr - BLOCK_SIZE);
}

/*
Checks if a pointer points to an address from the heap and that points to an allocated memory
@param p Pointer to check if it's valid
@return 0 if the pointer is not valid or 1 if the pointer is valid 
 */
int valid_addr(void *p) {
    if(base) {
        if(p > (void*)base && p < sbrk(0) && ((uintptr_t)p % 8 == 0)) {
            return (p == (void*)(get_pointer_to_meta_block(p))->anchor);
        }    
    }
    return 0;
}

/*
Mark the block as free, merges adjacent blocks and shrinks the heap if the block is at the end
@param p Pointer to the block that is being freed
*/
void my_free(void *p) {
    meta_block b;
    if(valid_addr(p)) {
        b = get_pointer_to_meta_block(p);
        b->free = 1;
        b = fusion(b, 1);
        // free the end of the heap
        if(b->next == NULL) {
            if(b->prev)
                b->prev->next = NULL;
            else    
                base = NULL;

            brk(b);
        }
    }
}

/*
Copy the data from a block to another
@param original The original block containing the data
@param copy The copy of the original block
*/
void copy_block(meta_block original, meta_block copy) {
    if(!original || !copy || original->size > copy->size) 
        return;
    char *original_p = original->anchor;
    char *copy_p = copy->anchor;
    while(original_p < ((char*)original->anchor + original->size)) {
        *copy_p = *original_p;
        copy_p++;
        original_p++;
    }
}
/*
Finds the last allocated memory block
@return Pointer to the last meta_block
*/
meta_block find_last_block(void) {
    meta_block b = base;
    meta_block last;
    while(b) {
        last = b;
        b = b->next;
    }  
    return last;
}

/*
Reallocate the memory for the new given size and pointer to previously allocated memory
The data from the previously allocated memory is coppied or truncated in the new allocated block 
@param new_size Size provided by the user
@param p Pointer to the memory that has to be reallocated
@return Pointer to the new allocated memory
*/
void *my_realloc(void *p, size_t new_size) {
    meta_block block, new_block;
    void *new_p;
    if(!p)
        return my_malloc(new_size); 
    if(valid_addr(p)) {
        new_size = align_64b(new_size);
         block = get_pointer_to_meta_block(p);
        if(block->size >= new_size){
            if(block->size >= new_size + BLOCK_SIZE + 8)
                split_block(block, new_size);
        }  
        else {
            meta_block copy = extend_heap(find_last_block(), block->size);
            copy_block(block, copy);
            block = fusion(block, 1);
            if(block->size >= new_size){
                if(block->size >= new_size + BLOCK_SIZE + 8)
                    split_block(block, new_size);
                p = block->anchor;
                copy_block(copy, block);
                my_free(copy->anchor);
            }
            else {
                my_free(copy->anchor);
                new_p = my_malloc(new_size);
                if(!new_p)
                    return NULL;
                new_block = get_pointer_to_meta_block(new_p);
                copy_block(block, new_block);
                my_free(p);
                return new_p;
            }
        }
        return p;
    }
    return NULL;   
}


