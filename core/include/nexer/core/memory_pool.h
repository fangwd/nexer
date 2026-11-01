#ifndef NEXER_MEMORY_POOL_H_
#define NEXER_MEMORY_POOL_H_

#include <cstddef>
#include <cstdlib>

namespace nexer {

class FixedSizeMemoryPool {
  private:
    struct Block {
        Block *prev;
        Block *next;
    };

    size_t max_size_;
    Block *free_;
    Block *allocated_;

    void free_block(Block *block) {
        Block *current = block;
        while (current != nullptr) {
            Block *next = current->next;
            reclaim(current);
            current = next;
        }
    }

    void *allocate(Block *block, size_t &size) {
        if (!block) {
            return nullptr; // Allocation failed
        }
        block->prev = nullptr; // Initialize prev to prevent list corruption
        if (block == free_) {
            if (free_->next) {
                free_->next->prev = nullptr;
            }
            free_ = free_->next;
        }
        // Add to head of allocated list
        if (allocated_) {
            allocated_->prev = block;
        }
        block->next = allocated_;
        allocated_ = block;

        size = max_size_ - sizeof(Block);
        return (char *)block + sizeof(Block);
    }

    void reclaim(Block *block) {
        // Remove from allocated list
        if (block == allocated_) {
            allocated_ = block->next;
            if (allocated_) {
                allocated_->prev = nullptr;
            }
        } else {
            // Check consistency or assume correct usage?
            // In a tight loop, we assume correctness.
            if (block->next) {
                block->next->prev = block->prev;
            }
            if (block->prev) {
                block->prev->next = block->next;
            }
        }

        // Add to head of free list
        block->prev = nullptr;
        block->next = free_;
        if (free_) {
            free_->prev = block;
        }
        free_ = block;
    }

  public:
    FixedSizeMemoryPool(size_t max_size = 65536)
        : max_size_(max_size), free_(nullptr), allocated_(nullptr) {}

    ~FixedSizeMemoryPool() {
        free_block(free_);
        free_block(allocated_);
    }

    void *allocate(size_t &size) {
        Block *block = free_;
        if (!block) {
            block = (Block *)malloc(max_size_);
            // Don't add to free list immediately, just use it.
            // Allocate() helper expects a block that is either in free_ or
            // detached. If it's new, it's detached.
        }
        return allocate(block, size);
    }

    void reclaim(void *ptr) {
        if (ptr != nullptr) {
            reclaim((Block *)((char *)ptr - sizeof(Block)));
        }
    }
};

} // namespace nexer

#endif // NEXER_MEMORY_POOL_H_
