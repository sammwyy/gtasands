#pragma once

#include <new>
#include <stdint.h>
#include <string.h>

#include "mem/mem.h"

namespace mem {

// Fixed-capacity object pool, a port of gta-reversed's CPool (Core/Pool.h):
// one contiguous array of T plus a byte map (bit 7 = free, bits 0..6 = a
// reuse counter that makes stale handles detectable). Storage is allocated
// once (STREAM or PERM), so creating/destroying objects never touches a heap
// and can never fragment it. Handles are (index << 8) | counter, like
// CPool::GetRef / GetAtRef, so scripts and saves can refer to objects.
template <class T>
class Pool {
public:
	bool init(int capacity, Tag tag, bool permanent = false) {
		const size_t bytes = sizeof(T) * capacity;
		m_objects = (T*)(permanent ? mem::perm(bytes, tag, alignof(T) < 4 ? 4 : alignof(T))
		                           : mem::alloc(bytes, tag, alignof(T) < 8 ? 8 : alignof(T)));
		m_flags = (uint8_t*)(permanent ? mem::perm(capacity, tag) : mem::alloc(capacity, tag));
		if (!m_objects || !m_flags) return false;
		m_capacity = capacity;
		m_permanent = permanent;
		for (int i = 0; i < capacity; i++) m_flags[i] = 0x80;
		m_first = -1;
		m_count = m_peak = 0;
		return true;
	}

	void shutdown() {
		for (int i = 0; i < m_capacity; i++)
			if (!(m_flags[i] & 0x80)) m_objects[i].~T();
		if (!m_permanent) {
			mem::free(m_objects);
			mem::free(m_flags);
		}
		m_objects = nullptr;
		m_flags = nullptr;
		m_capacity = m_count = 0;
	}

	// CPool::New: scan from the last allocation, wrapping once.
	template <class... A>
	T* create(A&&... args) {
		for (int n = 0; n < m_capacity; n++) {
			if (++m_first >= m_capacity) m_first = 0;
			uint8_t& f = m_flags[m_first];
			if (!(f & 0x80)) continue;
			f = (uint8_t)((f + 1) & 0x7F);
			if (++m_count > m_peak) m_peak = m_count;
			return new (&m_objects[m_first]) T(static_cast<A&&>(args)...);
		}
		return nullptr;
	}

	void destroy(T* obj) {
		const int i = indexOf(obj);
		if (i < 0 || (m_flags[i] & 0x80)) return;
		obj->~T();
		m_flags[i] |= 0x80;
		m_count--;
	}

	T* at(int i) const { return (unsigned)i < (unsigned)m_capacity && !(m_flags[i] & 0x80) ? &m_objects[i] : nullptr; }
	int indexOf(const T* obj) const {
		const int i = (int)(obj - m_objects);
		return obj && i >= 0 && i < m_capacity ? i : -1;
	}
	uint32_t handleOf(const T* obj) const {
		const int i = indexOf(obj);
		return i < 0 ? 0 : ((uint32_t)i << 8) | (m_flags[i] & 0x7F);
	}
	T* fromHandle(uint32_t h) const {
		const int i = (int)(h >> 8);
		return (unsigned)i < (unsigned)m_capacity && m_flags[i] == (h & 0x7F) ? &m_objects[i] : nullptr;
	}

	template <class F>
	void forEach(F f) {
		for (int i = 0; i < m_capacity; i++)
			if (!(m_flags[i] & 0x80)) f(m_objects[i]);
	}

	int capacity() const { return m_capacity; }
	int count() const { return m_count; }
	int peak() const { return m_peak; }
	bool full() const { return m_count >= m_capacity; }

private:
	T* m_objects = nullptr;
	uint8_t* m_flags = nullptr;
	int m_capacity = 0, m_first = -1, m_count = 0, m_peak = 0;
	bool m_permanent = false;
};

}
