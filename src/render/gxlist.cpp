#include "render/gxlist.h"

#include <nds.h>

namespace gx {

ListBuilder::ListBuilder(u32* buf, u32 capWords)
	: m_buf(buf), m_cap(capWords), m_n(1), m_hdr(0), m_slot(4), m_overflow(false) {}

// Packed format: one word holds up to 4 command ids (unused slots = NOP),
// followed by the parameters of those commands in order.
void ListBuilder::open(u8 id, int params) {
	if (m_slot == 4) {
		if (m_n >= m_cap) { m_overflow = true; return; }
		m_hdr = m_n++;
		m_buf[m_hdr] = 0;
		m_slot = 0;
	}
	if (m_n + params > m_cap) { m_overflow = true; return; }
	m_buf[m_hdr] |= (u32)id << (8 * m_slot++);
}

void ListBuilder::cmd(u8 id) { open(id, 0); }

void ListBuilder::cmd(u8 id, u32 a) {
	open(id, 1);
	if (!m_overflow) m_buf[m_n++] = a;
}

void ListBuilder::cmd(u8 id, u32 a, u32 b) {
	open(id, 2);
	if (!m_overflow) {
		m_buf[m_n++] = a;
		m_buf[m_n++] = b;
	}
}

u32 ListBuilder::finish() {
	if (m_overflow) return 0;
	m_buf[0] = m_n - 1;
	return m_n;
}

void flushList(const u32* list) { DC_FlushRange(list, (list[0] + 1) * 4); }

void callList(const u32* list) {
	const u32 count = list[0];
	if (!count) return;
	// FIFO DMA must not overlap another transfer (libnds glCallList note)
	while ((DMA_CR(0) & DMA_BUSY) || (DMA_CR(1) & DMA_BUSY) || (DMA_CR(2) & DMA_BUSY) || (DMA_CR(3) & DMA_BUSY));
	DMA_SRC(0) = (u32)(list + 1);
	DMA_DEST(0) = 0x4000400;
	DMA_CR(0) = DMA_FIFO | count;
	while (DMA_CR(0) & DMA_BUSY);
}

}
