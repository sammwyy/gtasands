#pragma once

#include <nds/ndstypes.h>

// Packed GX display lists (GBATEK "DS 3D Geometry Commands"). Built once at
// load time, flushed from the D-cache once, then fed to the geometry FIFO by
// DMA every frame: zero CPU work per vertex. List format matches glCallList:
// word 0 = number of words that follow.
namespace gx {

enum : u8 {
	CMD_NOP = 0x00,
	CMD_MTX_RESTORE = 0x14,
	CMD_COLOR = 0x20,
	CMD_TEXCOORD = 0x22,
	CMD_VTX_16 = 0x23,
	CMD_POLYGON_ATTR = 0x29,
	CMD_BEGIN_VTXS = 0x40,
};

class ListBuilder {
public:
	ListBuilder(u32* buf, u32 capWords);
	void cmd(u8 id);
	void cmd(u8 id, u32 a);
	void cmd(u8 id, u32 a, u32 b);
	// Finalizes the count word. Returns total words (incl. the count word),
	// or 0 if the buffer overflowed.
	u32 finish();

private:
	void open(u8 id, int params);
	u32* m_buf;
	u32 m_cap, m_n, m_hdr;
	int m_slot;
	bool m_overflow;
};

// DMA a finished list into the geometry FIFO and wait for it to drain into
// the FIFO. The list must already be flushed from the D-cache (flushList).
void callList(const u32* list);
void flushList(const u32* list);

}
