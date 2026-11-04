#pragma once

#include <stddef.h>
#include <stdint.h>

namespace mem {

// Two-Level Segregated Fit allocator (Masmano et al. 2004). One contiguous
// region; O(1) alloc/free; good-fit search over 16 sub-classes per power of
// two plus immediate coalescing keep fragmentation bounded, which matters on
// a 4 MB console that runs for hours. 8-byte granularity, 8-byte header per
// used block (previous physical block + size|tag|flags).
// Not thread/IRQ safe by itself: mem.cpp wraps it in an IRQ lock.
class Tlsf {
public:
	static constexpr int TAG_BITS = 8;

	void init(void* base, size_t bytes);
	void* alloc(size_t n, uint8_t tag);
	void* allocAligned(size_t n, size_t align, uint8_t tag);
	void free(void* p);
	// Grows/shrinks in place when the next physical block allows it.
	void* realloc(void* p, size_t n, uint8_t tag);

	bool owns(const void* p) const { return p >= m_base && p < m_end; }
	size_t usable(const void* p) const;
	uint8_t tagOf(const void* p) const;

	size_t capacity() const { return m_capacity; }
	size_t used() const { return m_used; }      // payload + headers
	size_t peak() const { return m_peak; }
	size_t largestFree() const;
	uint32_t blocks() const { return m_blocks; }
	uint32_t failures() const { return m_failures; }
	uint32_t tagBytes(uint8_t tag) const { return m_tagBytes[tag & 15]; }
	// Walks every physical block and checks the invariants (debug).
	bool check() const;

	struct Block;   // opaque, defined in tlsf.cpp

private:
	static constexpr int ALIGN_LOG2 = 3;
	static constexpr int SL_LOG2 = 4;
	static constexpr int SL_COUNT = 1 << SL_LOG2;
	static constexpr int FL_SHIFT = SL_LOG2 + ALIGN_LOG2 - 1;
	static constexpr int FL_COUNT = 20;          // up to 2^(FL_COUNT+FL_SHIFT) = 64 MB
	static constexpr size_t SMALL = (size_t)1 << (SL_LOG2 + ALIGN_LOG2);

	static void mapping(size_t size, int& fl, int& sl);
	Block* findSuitable(int& fl, int& sl);
	void insertFree(Block* b);
	void removeFree(Block* b);
	void split(Block* b, size_t size);
	void markUsed(Block* b, uint8_t tag);

	Block* m_heads[FL_COUNT][SL_COUNT];
	uint32_t m_flMap;
	uint32_t m_slMap[FL_COUNT];
	uint8_t* m_base = nullptr;
	uint8_t* m_end = nullptr;
	size_t m_capacity = 0, m_used = 0, m_peak = 0;
	uint32_t m_blocks = 0, m_failures = 0;
	uint32_t m_tagBytes[16];
};

}
