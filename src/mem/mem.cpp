#include "mem/mem.h"

#include <reent.h>
#include <stdio.h>
#include <string.h>

#include "mem/tlsf.h"

#ifdef ARM9
#include <nds.h>
extern "C" char *fake_heap_start, *fake_heap_end;
#define MEM_LOCK() ArmIrqState mem_irq_ = armIrqLockByPsr()
#define MEM_UNLOCK() armIrqUnlockByPsr(mem_irq_)
#else
// host builds (tools/hosthud): a plain static arena stands in for the heap
alignas(32) static uint8_t s_hostHeap[12 * 1024 * 1024];
#define MEM_LOCK() do {} while (0)
#define MEM_UNLOCK() do {} while (0)
#endif

namespace mem {

static Tlsf s_sys, s_stream;
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
	s_ready = true;
#ifdef ARM9
	uint8_t* lo = (uint8_t*)fake_heap_start;
	uint8_t* hi = (uint8_t*)fake_heap_end;
#else
	uint8_t* lo = s_hostHeap;
	uint8_t* hi = s_hostHeap + sizeof s_hostHeap;
#endif
	lo = alignUp(lo, 32);
	hi = (uint8_t*)((uintptr_t)hi & ~(uintptr_t)31);
	s_total = hi - lo;
	s_sys.init(lo, budget::SYS_BYTES);
	lo += budget::SYS_BYTES;
	s_scrBase = s_scrTop = s_scrPeak = lo;
	s_scrEnd = lo + budget::SCRATCH_BYTES;
	lo = s_scrEnd;
	s_permBase = s_permTop = lo;
	s_permEnd = lo + budget::PERM_BYTES;
	lo = s_permEnd;
	s_stream.init(lo, hi - lo);
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
	else if (s_sys.owns(p)) s_sys.free(p);
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
	case REGION_SYS: return tlsfStats(s_sys);
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
	const bool ok = s_sys.check() && s_stream.check();
	MEM_UNLOCK();
	return ok;
}

}

// ---------------------------------------------------------------------------
// libc allocator replacement. newlib's malloc/free/realloc/calloc/memalign
// are thin wrappers over these reentrant entry points, so defining them here
// keeps newlib's own dlmalloc (and its sbrk heap) out of the link entirely.
// SYS overflows into STREAM rather than failing.
#ifdef ARM9
using mem::s_stream;
using mem::s_sys;

static void* sysAlloc(size_t n, size_t align) {
	mem::init();
	MEM_LOCK();
	void* p = s_sys.allocAligned(n, align, mem::TAG_SYS);
	if (!p) p = s_stream.allocAligned(n, align, mem::TAG_SYS);
	MEM_UNLOCK();
	return p;
}

extern "C" {

void* _malloc_r(struct _reent*, size_t n) { return sysAlloc(n, 8); }

void _free_r(struct _reent*, void* p) { mem::free(p); }

void* _calloc_r(struct _reent*, size_t a, size_t b) {
	const size_t n = a * b;
	if (b && n / b != a) return nullptr;
	void* p = sysAlloc(n, 8);
	if (p) memset(p, 0, n);
	return p;
}

void* _memalign_r(struct _reent*, size_t align, size_t n) { return sysAlloc(n, align); }

size_t _malloc_usable_size_r(struct _reent*, void* p) {
	if (s_sys.owns(p)) return s_sys.usable(p);
	return s_stream.owns(p) ? s_stream.usable(p) : 0;
}

void* _realloc_r(struct _reent*, void* p, size_t n) {
	if (!p) return sysAlloc(n, 8);
	if (!n) { mem::free(p); return nullptr; }
	mem::Tlsf* h = s_sys.owns(p) ? &s_sys : &s_stream;
	MEM_LOCK();
	void* q = h->realloc(p, n, h->tagOf(p));
	MEM_UNLOCK();
	if (q) return q;
	q = sysAlloc(n, 8);
	if (q) {
		memcpy(q, p, h->usable(p));
		mem::free(p);
	}
	return q;
}

}
#endif
