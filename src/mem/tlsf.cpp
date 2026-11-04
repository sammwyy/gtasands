#include "mem/tlsf.h"

#include <stddef.h>
#include <string.h>

namespace mem {

// Physical block. The first two fields are the 8-byte header of every block;
// the free-list links overlay the payload and only exist while it is free.
// info: bit 0 = free, bits 3..23 = payload size, bits 24..31 = tag.
struct Tlsf::Block {
	Block* prevPhys;
	uint32_t info;
	Block* nextFree;
	Block* prevFree;
};

static constexpr uint32_t FREE_BIT = 1u;
static constexpr uint32_t SIZE_MASK = 0x00FFFFF8u;
static constexpr size_t HDR = offsetof(Tlsf::Block, nextFree);
static constexpr size_t MIN_PAYLOAD = sizeof(Tlsf::Block) - HDR;

static inline size_t sizeOf(const Tlsf::Block* b) { return b->info & SIZE_MASK; }
static inline bool isFree(const Tlsf::Block* b) { return b->info & FREE_BIT; }
static inline uint8_t* payload(const Tlsf::Block* b) { return (uint8_t*)b + HDR; }
static inline Tlsf::Block* fromPayload(const void* p) { return (Tlsf::Block*)((uint8_t*)p - HDR); }
static inline Tlsf::Block* nextPhys(const Tlsf::Block* b) { return (Tlsf::Block*)(payload(b) + sizeOf(b)); }
static inline void setSize(Tlsf::Block* b, size_t s) { b->info = (b->info & ~SIZE_MASK) | (uint32_t)s; }
static inline int fls32(uint32_t v) { return 31 - __builtin_clz(v); }
static inline int ffs32(uint32_t v) { return __builtin_ctz(v); }

static inline size_t adjust(size_t n) {
	n = (n + 7) & ~(size_t)7;
	return n < MIN_PAYLOAD ? MIN_PAYLOAD : n;
}

void Tlsf::init(void* base, size_t bytes) {
	memset(m_heads, 0, sizeof m_heads);
	memset(m_slMap, 0, sizeof m_slMap);
	memset(m_tagBytes, 0, sizeof m_tagBytes);
	m_flMap = 0;
	m_used = m_peak = 0;
	m_blocks = m_failures = 0;
	uintptr_t lo = ((uintptr_t)base + 7) & ~(uintptr_t)7;
	uintptr_t hi = ((uintptr_t)base + bytes) & ~(uintptr_t)7;
	m_base = (uint8_t*)lo;
	m_end = (uint8_t*)hi;
	m_capacity = 0;
	if (hi <= lo || hi - lo < 2 * HDR + MIN_PAYLOAD) return;
	size_t avail = hi - lo - 2 * HDR;
	if (avail > SIZE_MASK) avail = SIZE_MASK;
	Block* first = (Block*)lo;
	first->prevPhys = nullptr;
	first->info = (uint32_t)avail;
	Block* sentinel = nextPhys(first);  // size 0, never free: ends every walk
	sentinel->prevPhys = first;
	sentinel->info = 0;
	m_end = (uint8_t*)sentinel + HDR;
	m_capacity = avail;
	insertFree(first);
}

void Tlsf::mapping(size_t size, int& fl, int& sl) {
	if (size < SMALL) {
		fl = 0;
		sl = (int)(size >> ALIGN_LOG2);
	} else {
		const int f = fls32((uint32_t)size);
		sl = (int)((size >> (f - SL_LOG2)) ^ SL_COUNT);
		fl = f - FL_SHIFT;
	}
}

Tlsf::Block* Tlsf::findSuitable(int& fl, int& sl) {
	if (fl >= FL_COUNT) return nullptr;
	uint32_t slMap = m_slMap[fl] & (~0u << sl);
	if (!slMap) {
		const uint32_t flMap = m_flMap & (~0u << (fl + 1));
		if (!flMap) return nullptr;
		fl = ffs32(flMap);
		slMap = m_slMap[fl];
	}
	sl = ffs32(slMap);
	return m_heads[fl][sl];
}

void Tlsf::insertFree(Block* b) {
	int fl, sl;
	mapping(sizeOf(b), fl, sl);
	Block* head = m_heads[fl][sl];
	b->info |= FREE_BIT;
	b->prevFree = nullptr;
	b->nextFree = head;
	if (head) head->prevFree = b;
	m_heads[fl][sl] = b;
	m_flMap |= 1u << fl;
	m_slMap[fl] |= 1u << sl;
}

void Tlsf::removeFree(Block* b) {
	int fl, sl;
	mapping(sizeOf(b), fl, sl);
	if (b->prevFree) b->prevFree->nextFree = b->nextFree;
	if (b->nextFree) b->nextFree->prevFree = b->prevFree;
	if (m_heads[fl][sl] == b) {
		m_heads[fl][sl] = b->nextFree;
		if (!b->nextFree) {
			m_slMap[fl] &= ~(1u << sl);
			if (!m_slMap[fl]) m_flMap &= ~(1u << fl);
		}
	}
	b->info &= ~FREE_BIT;
}

// Trims b to `size` payload bytes; the tail becomes a free block. Callers
// guarantee the block after b is in use (TLSF never leaves two free
// neighbours), so the tail needs no merge.
void Tlsf::split(Block* b, size_t size) {
	const size_t cur = sizeOf(b);
	if (cur < size + HDR + MIN_PAYLOAD) return;
	Block* r = (Block*)(payload(b) + size);
	r->prevPhys = b;
	r->info = (uint32_t)(cur - size - HDR);
	nextPhys(r)->prevPhys = r;
	setSize(b, size);
	insertFree(r);
}

void Tlsf::markUsed(Block* b, uint8_t tag) {
	const size_t s = sizeOf(b);
	b->info = (uint32_t)s | ((uint32_t)tag << 24);
	m_used += s + HDR;
	if (m_used > m_peak) m_peak = m_used;
	m_blocks++;
	m_tagBytes[tag & 15] += (uint32_t)s;
}

void* Tlsf::alloc(size_t n, uint8_t tag) {
	const size_t size = adjust(n);
	if (size > SIZE_MASK) { m_failures++; return nullptr; }
	// round the request up to the next class boundary so any block of the
	// found list fits (good-fit, no list walk)
	size_t search = size;
	if (search >= SMALL) search += ((size_t)1 << (fls32((uint32_t)search) - SL_LOG2)) - 1;
	int fl, sl;
	mapping(search, fl, sl);
	Block* b = findSuitable(fl, sl);
	if (!b) { m_failures++; return nullptr; }
	removeFree(b);
	split(b, size);
	markUsed(b, tag);
	return payload(b);
}

void* Tlsf::allocAligned(size_t n, size_t align, uint8_t tag) {
	if (align <= 8) return alloc(n, tag);
	const size_t size = adjust(n);
	const size_t gapMin = HDR + MIN_PAYLOAD;
	size_t search = size + align + gapMin;
	if (search > SIZE_MASK) { m_failures++; return nullptr; }
	search += ((size_t)1 << (fls32((uint32_t)search) - SL_LOG2)) - 1;
	int fl, sl;
	mapping(search, fl, sl);
	Block* b = findSuitable(fl, sl);
	if (!b) { m_failures++; return nullptr; }
	removeFree(b);
	uintptr_t p = (uintptr_t)payload(b);
	uintptr_t a = (p + align - 1) & ~(uintptr_t)(align - 1);
	if (a != p && a - p < gapMin) a = (p + gapMin + align - 1) & ~(uintptr_t)(align - 1);
	if (a != p) {
		// leading gap becomes its own free block (its physical predecessor is
		// in use: b was a coalesced free block)
		const size_t gap = a - p;
		Block* nb = (Block*)(a - HDR);
		nb->prevPhys = b;
		nb->info = (uint32_t)(sizeOf(b) - gap);
		nextPhys(nb)->prevPhys = nb;
		b->info = (uint32_t)(gap - HDR);
		insertFree(b);
		b = nb;
	}
	split(b, size);
	markUsed(b, tag);
	return payload(b);
}

void Tlsf::free(void* p) {
	if (!p) return;
	Block* b = fromPayload(p);
	const size_t s = sizeOf(b);
	m_used -= s + HDR;
	m_blocks--;
	m_tagBytes[(b->info >> 24) & 15] -= (uint32_t)s;
	b->info = (uint32_t)s;
	Block* prev = b->prevPhys;
	if (prev && isFree(prev)) {
		removeFree(prev);
		setSize(prev, sizeOf(prev) + HDR + sizeOf(b));
		b = prev;
		nextPhys(b)->prevPhys = b;
	}
	Block* nx = nextPhys(b);
	if (isFree(nx)) {
		removeFree(nx);
		setSize(b, sizeOf(b) + HDR + sizeOf(nx));
		nextPhys(b)->prevPhys = b;
	}
	insertFree(b);
}

void* Tlsf::realloc(void* p, size_t n, uint8_t tag) {
	if (!p) return alloc(n, tag);
	if (!n) { free(p); return nullptr; }
	Block* b = fromPayload(p);
	const size_t cur = sizeOf(b);
	const size_t size = adjust(n);
	if (size <= cur) return p;
	Block* nx = nextPhys(b);
	if (isFree(nx) && cur + HDR + sizeOf(nx) >= size) {
		const uint8_t t = (uint8_t)(b->info >> 24);
		removeFree(nx);
		setSize(b, cur + HDR + sizeOf(nx));
		nextPhys(b)->prevPhys = b;
		split(b, size);
		const size_t grown = sizeOf(b) - cur;
		m_used += grown;
		if (m_used > m_peak) m_peak = m_used;
		m_tagBytes[t & 15] += (uint32_t)grown;
		return p;
	}
	void* q = alloc(n, (uint8_t)(b->info >> 24));
	if (!q) return nullptr;
	memcpy(q, p, cur);
	free(p);
	return q;
}

size_t Tlsf::usable(const void* p) const { return p ? sizeOf(fromPayload(p)) : 0; }
uint8_t Tlsf::tagOf(const void* p) const { return p ? (uint8_t)(fromPayload(p)->info >> 24) : 0; }

size_t Tlsf::largestFree() const {
	if (!m_flMap) return 0;
	const int fl = fls32(m_flMap);
	const int sl = fls32(m_slMap[fl]);
	size_t best = 0;
	for (const Block* b = m_heads[fl][sl]; b; b = b->nextFree)
		if (sizeOf(b) > best) best = sizeOf(b);
	return best;
}

bool Tlsf::check() const {
	if (!m_capacity) return true;
	const Block* b = (const Block*)m_base;
	const Block* end = (const Block*)(m_end - HDR);
	size_t used = 0;
	while (b != end) {
		const Block* nx = nextPhys(b);
		if ((const uint8_t*)nx > (const uint8_t*)end || nx->prevPhys != b) return false;
		if (isFree(b) && isFree(nx)) return false;
		if (!isFree(b)) used += sizeOf(b) + HDR;
		b = nx;
	}
	return used == m_used;
}

}
