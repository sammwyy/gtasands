#pragma once

// Binary formats produced by tools/assets.py (tools/gtasa/*). Little endian,
// every block 4-byte aligned. Fixed point: fx32 = 20.12, fx16 = 4.12.
// Keep in sync with the Python writers; bump `version` on layout changes.

#include <stdint.h>

namespace ds {

typedef int32_t fx32;
typedef int16_t fx16;

#define DS_FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

// ---------------------------------------------------------------- DSPK (pack)
// Replaces IMG archives. Entries keep original names ("infernus.dff") but the
// payload is the converted DS format, identified by its own magic.
// TOC is sorted by (fnv1a(lowercase name), name) for binary search.
struct PackHeader {
	uint32_t magic;          // 'DSPK'
	uint16_t version, pad;
	uint32_t count;
	uint32_t namesOffset;    // NUL-terminated names blob
};
struct PackEntry {
	uint32_t hash;           // fnv1a of lowercase name
	uint32_t nameOffset;     // relative to namesOffset
	uint32_t offset, size;   // absolute in file
};

inline uint32_t fnv1a(const char* s) {
	uint32_t h = 0x811C9DC5u;
	for (; *s; s++) {
		uint8_t c = (uint8_t)*s;
		if (c >= 'A' && c <= 'Z') c += 32;
		h = (h ^ c) * 0x01000193u;
	}
	return h;
}

// ---------------------------------------------------------------- DTXD / DTEX (textures, from .txd)
struct TxdHeader {
	uint32_t magic;          // 'DTXD'
	uint16_t version, count;
	// TxdEntry entries[count]
};
struct TxdEntry {
	char name[32];
	uint32_t offset, size;   // relative to TXD start, points at a TexHeader
};
enum TexFlags : uint8_t { TEX_COLOR0_TRANSPARENT = 1, TEX_HAS_ALPHA = 2 };
struct TexHeader {
	uint32_t magic;          // 'DTEX'
	uint16_t width, height;  // powers of two, 8..1024
	uint8_t format;          // GL_TEXTURE_TYPE_ENUM: 1 A3I5, 2 PAL4, 3 PAL16, 4 PAL256, 6 A5I3, 7 RGBA(direct)
	uint8_t flags;           // TexFlags
	uint16_t numColors;      // palette entries (RGB15), 0 for direct
	uint32_t texelBytes;
	// uint8_t texels[texelBytes] (padded to 4); uint16_t palette[numColors]
};

// ---------------------------------------------------------------- DMDL (models, from .dff)
enum ModelFlags : uint16_t { MDL_SKINNED = 1 };
enum VertexFlags : uint8_t { VF_UV = 1, VF_NORMAL = 2, VF_COLOR = 4, VF_BONE = 8 };
struct ModelHeader {
	uint32_t magic;          // 'DMDL'
	uint16_t version, flags;
	uint16_t numFrames, numGeometries, numAtomics, numBones;
	fx32 sphere[4];          // bounding sphere center xyz + radius
	// ModelFrame frames[numFrames]
	// ModelAtomic atomics[numAtomics]
	// -- v2, if MDL_SKINNED, numBones entries each:
	// fx32 invBone[numBones][4][3]   (rows: right, up, at, pos)
	// ModelNode nodes[numBones]      (hAnim order; vert bone index == node index)
	// fx32 bindLocal[numBones][4][3] (bindW * invBone[parent]; root = bindW)
	// fx32 bonePos[numBones][3]      (bindW.pos * invBone[parent]; root = 0)
	// uint32_t geometryOffsets[numGeometries]  (absolute in blob)
};
struct ModelNode {
	int16_t parent;          // parent node index, -1 at the hierarchy root
	int16_t boneId;          // hAnim bone id (matches AnimSeq::boneTag)
};
struct ModelFrame {
	char name[24];
	int16_t parent;          // -1 = root
	int16_t boneId;          // HAnim bone id, -1 if none
	fx16 rot[9];             // RW matrix right, up, at
	uint16_t pad;
	fx32 pos[3];
};
struct ModelAtomic {
	uint16_t frame, geometry;
};
struct GeometryHeader {
	uint16_t numVerts, numMaterials;
	uint32_t numIndices;     // triangle list, grouped per material
	uint8_t vflags;          // VertexFlags
	int8_t posExp;           // pos = offset + s16 * 2^posExp / 4096
	int8_t uvExp;            // uv  = s16 * 2^uvExp / 4096   (normalised, 1.0 = full texture)
	uint8_t pad;
	fx32 offset[3];
	// GeometryMaterial materials[numMaterials]
	// int16_t pos[numVerts][3]                (padded to 4)
	// int16_t uv[numVerts][2]                 (VF_UV)
	// uint32_t normal[numVerts]               (VF_NORMAL, GX packed 10.10.10)
	// uint16_t color[numVerts]                (VF_COLOR, RGB15, padded to 4)
	// uint8_t bone[numVerts]                  (VF_BONE, dominant skin bone, padded to 4)
	// uint16_t indices[numIndices]            (padded to 4)
};
struct GeometryMaterial {
	char texture[32];        // texture name inside the model's TXD ("" = untextured)
	uint16_t color;          // RGB15
	uint8_t alpha, pad;
	uint32_t firstIndex, numIndices;
};

// ---------------------------------------------------------------- DCOL (collision, from .col)
struct ColHeader {
	uint32_t magic;          // 'DCOL'
	uint16_t version, count;
	// repeated count times: uint32_t size; ColModel + arrays
};
struct ColSurface {
	uint8_t material, flag, brightness, light;
};
struct ColModel {
	char name[24];
	uint16_t modelId;
	uint8_t srcVersion, pad; // 1..4 = COLL..COL4
	fx32 bmin[3], bmax[3], center[3], radius;
	uint16_t numSpheres, numBoxes, numVerts, numFaces;
	// ColSphere spheres[numSpheres]; ColBox boxes[numBoxes]
	// int16_t verts[numVerts][3]  (1/128 units, padded to 4)
	// ColFace faces[numFaces]
};
struct ColSphere { fx32 center[3], radius; ColSurface surf; };
struct ColBox { fx32 min[3], max[3]; ColSurface surf; };
struct ColFace { uint16_t a, b, c; uint8_t material, light; };

// ---------------------------------------------------------------- DANM (animations, from .ifp ANP3)
struct AnimHeader {
	uint32_t magic;          // 'DANM'
	uint16_t version, numAnims;
	char block[24];
	// per anim: AnimEntry, then per sequence: AnimSeq + frames (padded to 4)
};
struct AnimEntry {
	char name[24];
	uint16_t numSeqs, pad;
};
struct AnimSeq {
	char name[24];
	int16_t boneTag;         // eBoneTag, -1 = match by name
	uint8_t hasTrans, pad;
	uint16_t numFrames, pad2;
	// frames: int16 q[4] (x,y,z,w * 4096), int16 time (absolute, 1/60 s)
	//         [+ int16 t[3] (* 1024) if hasTrans]
};

// ---------------------------------------------------------------- audio
enum SoundFormat : uint8_t { SND_PCM8 = 0, SND_PCM16 = 1, SND_ADPCM = 2 };  // = DS SOUNDxCNT format bits
struct SoundHeader {
	uint32_t magic;          // 'DSND'
	uint8_t format, pad;
	uint16_t sampleRate;
	uint32_t numSamples;
	int32_t loopStart;       // samples, -1 = one shot
	int16_t headroom;        // SA headroom (volume normalisation)
	uint16_t pad2;
	uint32_t dataBytes;      // ADPCM: 4-byte header (s16 sample, u8 index, u8 0) + nibbles, low first
};
// audio/sfx/<pak>.dsfx: SFX pak with its banks
struct SfxPakHeader {
	uint32_t magic;          // 'DSFX'
	uint16_t version, numBanks;
	// SfxPakBank banks[numBanks]
};
struct SfxPakBank {
	uint32_t offset, size;   // -> BankHeader
	uint16_t bankId, pad;    // global bank id (eSoundBank)
};
struct BankHeader {
	uint32_t magic;          // 'DSBK'
	uint16_t version, numSounds;
	// uint32_t soundOffsets[numSounds] (relative to bank) -> SoundHeader
};
// audio/sfx/banks.dsbl: global bank id -> pak + index inside pak
struct BankLookupHeader {
	uint32_t magic;          // 'DSBL'
	uint16_t version;
	uint8_t numPaks, pad;
	uint16_t numBanks, pad2;
	// char pakNames[numPaks][16]; BankLookup banks[numBanks]
};
struct BankLookup {
	uint8_t pak;             // 0xFF = removed by resources.toml
	uint8_t pad;
	uint16_t index;
};
// audio/streams/<station>.dsst
struct StreamPackHeader {
	uint32_t magic;          // 'DSST'
	uint16_t version, numTracks;
	// StreamTrack tracks[numTracks]
};
struct StreamTrack {
	uint32_t trackId;        // global SA track id (TrakLkup.dat index)
	uint32_t offset, size;   // -> SoundHeader
};

static_assert(sizeof(PackHeader) == 16 && sizeof(PackEntry) == 16);
static_assert(sizeof(TxdEntry) == 40 && sizeof(TexHeader) == 16);
static_assert(sizeof(ModelHeader) == 32 && sizeof(ModelFrame) == 60 && sizeof(ModelNode) == 4 &&
              sizeof(GeometryHeader) == 24 && sizeof(GeometryMaterial) == 44);
static_assert(sizeof(ColModel) == 76 && sizeof(ColSphere) == 20 && sizeof(ColBox) == 28 && sizeof(ColFace) == 8);
static_assert(sizeof(AnimHeader) == 32 && sizeof(AnimEntry) == 28 && sizeof(AnimSeq) == 32);
static_assert(sizeof(SoundHeader) == 24 && sizeof(SfxPakBank) == 12 && sizeof(BankLookupHeader) == 12 && sizeof(StreamTrack) == 12);

}
