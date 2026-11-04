#include "assets/model.h"

#include <stdlib.h>
#include <string.h>

namespace mdl {

Model* load(const void* blob) {
	const ds::ModelHeader* h = (const ds::ModelHeader*)blob;
	if (!blob || h->magic != DS_FOURCC('D', 'M', 'D', 'L') || h->version != 2) return nullptr;

	const ds::ModelFrame* sframes = (const ds::ModelFrame*)(h + 1);
	const ds::ModelAtomic* atomics = (const ds::ModelAtomic*)(sframes + h->numFrames);
	const uint8_t* p = (const uint8_t*)(atomics + h->numAtomics);
	const ds::fx32* invBone = nullptr;
	const ds::ModelNode* nodes = nullptr;
	const ds::fx32* bindLocal = nullptr;
	const ds::fx32* bonePos = nullptr;
	if (h->flags & ds::MDL_SKINNED) {
		invBone = (const ds::fx32*)p;
		p += (size_t)h->numBones * 12 * sizeof(ds::fx32);
		nodes = (const ds::ModelNode*)p;
		p += (size_t)h->numBones * sizeof(ds::ModelNode);
		bindLocal = (const ds::fx32*)p;
		p += (size_t)h->numBones * 12 * sizeof(ds::fx32);
		bonePos = (const ds::fx32*)p;
		p += (size_t)h->numBones * 3 * sizeof(ds::fx32);
	}
	const uint32_t* geomOffs = (const uint32_t*)p;

	Model* m = (Model*)malloc(sizeof(Model));
	if (!m) return nullptr;
	m->frames = (Frame*)malloc(sizeof(Frame) * h->numFrames + sizeof(Geometry) * h->numGeometries);
	if (!m->frames) { ::free(m); return nullptr; }
	m->geoms = (Geometry*)(m->frames + h->numFrames);

	m->flags = h->flags;
	m->numFrames = h->numFrames;
	m->numGeoms = h->numGeometries;
	m->numAtomics = h->numAtomics;
	m->numBones = h->numBones;
	m->atomics = atomics;
	m->invBone = invBone;
	m->nodes = nodes;
	m->bindLocal = bindLocal;
	m->bonePos = bonePos;

	for (int i = 0; i < m->numFrames; i++) {
		m->frames[i].parent = sframes[i].parent;
		m->frames[i].boneId = sframes[i].boneId;
		memcpy(m->frames[i].rot, sframes[i].rot, sizeof m->frames[i].rot);
		memcpy(m->frames[i].pos, sframes[i].pos, sizeof m->frames[i].pos);
	}

	for (int g = 0; g < m->numGeoms; g++) {
		const ds::GeometryHeader* gh = (const ds::GeometryHeader*)((const uint8_t*)blob + geomOffs[g]);
		Geometry* d = &m->geoms[g];
		d->h = gh;
		d->mats = (const ds::GeometryMaterial*)(gh + 1);
		const uint8_t* gp = (const uint8_t*)(d->mats + gh->numMaterials);
		d->pos = (const int16_t(*)[3])gp;
		gp += ((size_t)gh->numVerts * 6 + 3) & ~3u;
		d->uv = nullptr;
		if (gh->vflags & ds::VF_UV) {
			d->uv = (const int16_t(*)[2])gp;
			gp += (size_t)gh->numVerts * 4;
		}
		d->normal = nullptr;
		if (gh->vflags & ds::VF_NORMAL) {
			d->normal = (const uint32_t*)gp;
			gp += (size_t)gh->numVerts * 4;
		}
		d->color = nullptr;
		if (gh->vflags & ds::VF_COLOR) {
			d->color = (const uint16_t*)gp;
			gp += ((size_t)gh->numVerts * 2 + 3) & ~3u;
		}
		d->bone = nullptr;
		if (gh->vflags & ds::VF_BONE) {
			d->bone = gp;
			gp += ((size_t)gh->numVerts + 3) & ~3u;
		}
		d->idx = (const uint16_t*)gp;
	}
	return m;
}

void free(Model* m) {
	if (m) {
		::free(m->frames); // frames and geoms live in one block
		::free(m);
	}
}

}
