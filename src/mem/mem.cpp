#include "mem/mem.h"

#include <stdio.h>
#include <string.h>

#include "mem/tlsf.h"

#ifdef ARM9
#include <nds.h>
#define MEM_LOCK() int mem_irq_ = enterCriticalSection()
#define MEM_UNLOCK() leaveCriticalSection(mem_irq_)
#else
// host builds (tools/hosthud): a plain static arena stands in for the heap
alignas(32) static uint8_t s_hostHeap[12 * 1024 * 1024];
#define MEM_LOCK() do {} while (0)
#define MEM_UNLOCK() do {} while (0)
#endif

namespace mem {

static Tlsf s_stream;
static uint8_t *s_permBase, *s_permTop, *s_permEnd;
static uint8_t *s_scrBase, *s_scrTop, *s_scrEnd, *s_scrPeak;
static uint32_t s_permTag[TAG_COUNT];
static uint32_t s_permFallback, s_scrSpills;
static size_t s_total;
static bool s_ready;

static inline uint8_t* alignUp(uint8_t* p, size_t a) {
	return (uint8_t*)(((uintptr_t)p + a - 1) & ~(uintptr_t)(a - 1));
}

void init() {
	if (s_ready) return;
#ifdef ARM9
	uint8_t* hi = getHeapLimit();
	const size_t space = hi - getHeapEnd();
	const size_t reserved = space > budget::SYS_BYTES ? (space - budget::SYS_BYTES) & ~(size_t)31 : 0;
	if (reserved < budget::SCRATCH_BYTES + budget::PERM_BYTES + budget::SYS_BYTES ||
	    reduceHeapSize(reserved) != 0) __builtin_trap();
	uint8_t* lo = getHeapLimit();
#else
	uint8_t* lo = s_hostHeap;
	uint8_t* hi = s_hostHeap + sizeof s_hostHeap;
#endif
	lo = alignUp(lo, 32);
	hi = (uint8_t*)((uintptr_t)hi & ~(uintptr_t)31);
	s_total = hi - lo;
	s_scrBase = s_scrTop = s_scrPeak = lo;
	s_scrEnd = lo + budget::SCRATCH_BYTES;
	lo = s_scrEnd;
	s_permBase = s_permTop = lo;
	s_permEnd = lo + budget::PERM_BYTES;
	lo = s_permEnd;
	s_stream.init(lo, hi - lo);
	s_ready = true;
}

void* alloc(size_t n, Tag tag, size_t align) {
	init();
	MEM_LOCK();
	void* p = s_stream.allocAligned(n, align, tag);
	MEM_UNLOCK();
	return p;
}

void* zalloc(size_t n, Tag tag) {
	void* p = alloc(n, tag);
	if (p) memset(p, 0, n);
	return p;
}

void* realloc(void* p, size_t n, Tag tag) {
	init();
	MEM_LOCK();
	void* q = s_stream.realloc(p, n, tag);
	MEM_UNLOCK();
	return q;
}

void free(void* p) {
	if (!p) return;
	MEM_LOCK();
	if (s_stream.owns(p)) s_stream.free(p);
	MEM_UNLOCK();
}

void* perm(size_t n, Tag tag, size_t align) {
	init();
	MEM_LOCK();
	uint8_t* p = alignUp(s_permTop, align < 4 ? 4 : align);
	void* r = nullptr;
	if (p + n <= s_permEnd) {
		s_permTop = p + n;
		s_permTag[tag] += (uint32_t)n;
		r = p;
	}
	MEM_UNLOCK();
	if (r) return r;
	// over budget: keep running from STREAM (never freed either) and count it
	s_permFallback++;
	return alloc(n, tag, align);
}

Scratch::Scratch() : m_mark(nullptr), m_nspill(0) {
	init();
	m_mark = s_scrTop;
}

Scratch::~Scratch() {
	s_scrTop = m_mark;
	for (int i = 0; i < m_nspill; i++) mem::free(m_spill[i]);
}

void* Scratch::alloc(size_t n, size_t align) {
	uint8_t* p = alignUp(s_scrTop, align < 4 ? 4 : align);
	if (p + n <= s_scrEnd) {
		s_scrTop = p + n;
		if (s_scrTop > s_scrPeak) s_scrPeak = s_scrTop;
		return p;
	}
	if (m_nspill >= (int)(sizeof m_spill / sizeof m_spill[0])) return nullptr;
	void* q = mem::alloc(n, TAG_TEMP, align);
	if (q) {
		m_spill[m_nspill++] = q;
		s_scrSpills++;
	}
	return q;
}

static RegionStats tlsfStats(const Tlsf& h) {
	return { h.capacity(), h.used(), h.peak(), h.largestFree(), h.blocks(), h.failures() };
}

RegionStats stats(Region r) {
	init();
	switch (r) {
	case REGION_SYS:
#ifdef ARM9
		return { (size_t)(getHeapLimit() - getHeapStart()), (size_t)(getHeapEnd() - getHeapStart()),
		         (size_t)(getHeapEnd() - getHeapStart()), (size_t)(getHeapLimit() - getHeapEnd()), 0, 0 };
#else
		return {};
#endif
	case REGION_STREAM: return tlsfStats(s_stream);
	case REGION_SCRATCH:
		return { (size_t)(s_scrEnd - s_scrBase), (size_t)(s_scrTop - s_scrBase), (size_t)(s_scrPeak - s_scrBase),
		         (size_t)(s_scrEnd - s_scrTop), 0, s_scrSpills };
	default:
		return { (size_t)(s_permEnd - s_permBase), (size_t)(s_permTop - s_permBase), (size_t)(s_permTop - s_permBase),
		         (size_t)(s_permEnd - s_permTop), 0, s_permFallback };
	}
}

uint32_t tagBytes(Tag t) { return s_stream.tagBytes(t) + s_permTag[t]; }

const char* tagName(Tag t) {
	static const char* const k[TAG_COUNT] = { "sys", "text", "model", "tex", "anim", "audio",
	                                          "radar", "hud", "pack", "world", "col", "ped", "temp" };
	return t < TAG_COUNT ? k[t] : "?";
}

size_t totalBytes() { init(); return s_total; }

void summary(char* buf, int n) {
	const RegionStats st = stats(REGION_STREAM), sy = stats(REGION_SYS);
	const RegionStats pm = stats(REGION_PERM), sc = stats(REGION_SCRATCH);
	snprintf(buf, n, "str %u/%uK lf %uK pk %uK | perm %u/%uK | sys %u/%uK | scr pk %u/%uK",
	         (unsigned)(st.used >> 10), (unsigned)(st.capacity >> 10), (unsigned)(st.largestFree >> 10),
	         (unsigned)(st.peak >> 10), (unsigned)(pm.used >> 10), (unsigned)(pm.capacity >> 10),
	         (unsigned)(sy.used >> 10), (unsigned)(sy.capacity >> 10),
	         (unsigned)(sc.peak >> 10), (unsigned)(sc.capacity >> 10));
}

bool check() {
	MEM_LOCK();
	const bool ok = s_stream.check();
	MEM_UNLOCK();
	return ok;
}

}
