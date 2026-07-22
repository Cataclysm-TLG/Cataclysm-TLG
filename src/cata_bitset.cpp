#include "cata_bitset.h"

#include "cata_assert.h"

namespace
{

size_t bit_count_to_block_count( size_t bits )
{
    return ( bits + tiny_bitset::kBitsPerBlock - 1 ) / tiny_bitset::kBitsPerBlock;
}

} // namespace

void tiny_bitset::resize_heap( size_t requested_bits ) noexcept
{
    size_t blocks_needed = bit_count_to_block_count( requested_bits );

    const size_t old_size = size();

    // Round up to a multiple of the block allocation size.
    // The allocation size is stored by malloc so realloc() can be optimized if we keep resizing by small amounts.
    // One extra block for prepending the capacity to the allocation, which is stored in the first block.
    blocks_needed = ( ( blocks_needed + ( kMinimumBlockPerAlloc - 1 ) ) / kMinimumBlockPerAlloc ) *
                    kMinimumBlockPerAlloc + 1;

    block_t *data;

    if( is_inline() ) {
        data = static_cast<block_t *>( malloc( sizeof( block_t ) * blocks_needed ) );
        cata_assert( data != nullptr );

        // Capacity goes at the front ahead of the actual bits.
        data[ 0 ] = requested_bits;
        // Copy inline bits to heap.
        data[ 1 ] = storage_ & ~kMetaMask;
    } else {
        data = static_cast<block_t *>( realloc( real_heap_allocation(),
                                                sizeof( block_t ) * blocks_needed ) );
        cata_assert( data != nullptr );
    }

    if (requested_bits > old_size) {
        size_t first_block = old_size / kBitsPerBlock;
        const size_t first_bit = old_size % kBitsPerBlock;
        block_t *new_storage = data + 1;

        if (first_bit != 0) {
            // Clear the bits in the first block that are beyond the old size.
            block_t new_suffix = ( kLowBit << ( kBitsPerBlock - first_bit ) ) - 1;
            new_storage[first_block] &= ~new_suffix;
            ++first_block;
        }

        const size_t end_block = blocks_needed - 1;

        if (first_block < end_block) {
            // Clear all blocks between the first and last block.
            memset( new_storage + first_block, 0, sizeof( block_t ) * ( end_block - first_block ) );
        }
    }

    set_storage( data + 1 );
    set_size( requested_bits );
}

void tiny_bitset::set_storage( block_t *data )
{
    memcpy( &storage_, &data, sizeof( data ) );
}