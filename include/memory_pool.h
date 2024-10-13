#ifndef NEXER_FREELIST_H_
#define NEXER_FREELIST_H_

#include <cstddef>
#include <iostream>

namespace nexer {

class FixedSizeMemoryPool {
  private:
    struct Block {
        Block* prev;
        Block* next;
    };

    size_t max_size_;
    Block* free_;
    Block* allocated_;

    void FreeBlock(Block* block) {
        Block* current = block;
        while (current != nullptr) {
            Block* next = current->next;
            free(current);
            current = next;
        }
    }

    void* Allocate(Block* block, size_t& size) {
        if (block == free_) {
            if (free_->next) {
                free_->next->prev = nullptr;
            }
            free_ = free_->next;
        }
        if (allocated_) {
            allocated_->prev = block;
        }
        block->next = allocated_;
        allocated_ = block;
        size = max_size_ - sizeof(Block);
        return (char*)block + sizeof(Block);
    }

    void Free(Block* block) {
        if (block == allocated_) {
            if (allocated_->next) {
                allocated_->next->prev = nullptr;
            }
            allocated_ = allocated_->next;
        }
        if (block->next) {
            block->next->prev = block->prev;
        }
        if (block->prev) {
            block->prev->next = block->next;
        }
        block->prev = nullptr;
        block->next = free_;
        if (free_) {
            free_->prev = block;
        }
        free_ = block;
    }

  public:
    FixedSizeMemoryPool(size_t max_size = 65536) : max_size_(max_size), free_(nullptr), allocated_(nullptr) {}

    ~FixedSizeMemoryPool() {
        FreeBlock(free_);
        FreeBlock(allocated_);
    }

    void* Allocate(size_t& size) {
        return Allocate(free_ ? free_ : (Block*)malloc(max_size_), size);
    }

    void Free(void* ptr) {
        if (ptr != nullptr) {
            Free((Block*)((char*)ptr - sizeof(Block)));
        }
    }
};

}  // namespace nexer

#endif  // NEXER_FREELIST_H_