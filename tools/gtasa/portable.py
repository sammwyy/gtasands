import ctypes
import os
from functools import lru_cache
from pathlib import Path


class Options(ctypes.Structure):
    _fields_ = [('format', ctypes.c_int), ('alpha_threshold', ctypes.c_uint8),
                ('auto_colors', ctypes.c_uint16), ('opaque', ctypes.c_uint8),
                ('additive', ctypes.c_uint8)]


class Target(ctypes.Structure):
    _fields_ = [('width', ctypes.c_uint16), ('height', ctypes.c_uint16),
                ('options', Options)]


class IMGEntry(ctypes.Structure):
    _fields_ = [('name', ctypes.c_char * 25), ('offset', ctypes.c_uint64),
                ('size', ctypes.c_uint32)]


class DSPKEntry(ctypes.Structure):
    _fields_ = [('name', ctypes.c_char_p), ('size', ctypes.c_uint32),
                ('source', ctypes.c_void_p)]


DSPKRead = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p,
                           ctypes.c_uint32, ctypes.c_void_p, ctypes.c_size_t)
DSPKWrite = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p,
                            ctypes.c_size_t)


class IFPOptions(ctypes.Structure):
    _fields_ = [('keep_every', ctypes.c_uint32), ('max_frames', ctypes.c_uint32),
                ('drop_translation', ctypes.c_uint8),
                ('remove', ctypes.POINTER(ctypes.c_char_p)),
                ('remove_count', ctypes.c_size_t),
                ('rigid_clips', ctypes.POINTER(ctypes.c_char_p)),
                ('rigid_count', ctypes.c_size_t)]


class GeometryMaterial(ctypes.Structure):
    _fields_ = [('texture', ctypes.c_char * 32), ('color', ctypes.c_uint16),
                ('alpha', ctypes.c_uint8), ('reserved', ctypes.c_uint8),
                ('first', ctypes.c_uint32), ('count', ctypes.c_uint32)]


class DFFFrame(ctypes.Structure):
    _fields_ = [('name', ctypes.c_char * 128), ('rot', ctypes.c_double * 9),
                ('pos', ctypes.c_double * 3), ('parent', ctypes.c_int32),
                ('bone_id', ctypes.c_int32)]


class DFFMaterial(ctypes.Structure):
    _fields_ = [('rgba', ctypes.c_uint8 * 4), ('texture', ctypes.c_char * 128)]


class DFFSkin(ctypes.Structure):
    _fields_ = [('num_bones', ctypes.c_uint32), ('indices', ctypes.POINTER(ctypes.c_uint8)),
                ('weights', ctypes.POINTER(ctypes.c_double)),
                ('inv_matrices', ctypes.POINTER(ctypes.c_double))]


class DFFGeometry(ctypes.Structure):
    _fields_ = [('flags', ctypes.c_uint32), ('vertex_count', ctypes.c_uint32),
                ('triangle_count', ctypes.c_uint32), ('material_count', ctypes.c_uint32),
                ('sphere', ctypes.c_double * 4),
                ('vertices', ctypes.POINTER(ctypes.c_double)),
                ('uvs', ctypes.POINTER(ctypes.c_double)),
                ('normals', ctypes.POINTER(ctypes.c_double)),
                ('colors', ctypes.POINTER(ctypes.c_uint8)),
                ('triangles', ctypes.POINTER(ctypes.c_uint16)),
                ('materials', ctypes.POINTER(DFFMaterial)),
                ('skin', ctypes.POINTER(DFFSkin))]


class DFFAtomic(ctypes.Structure):
    _fields_ = [('frame', ctypes.c_uint32), ('geometry', ctypes.c_uint32)]


class DFFHAnim(ctypes.Structure):
    _fields_ = [('id', ctypes.c_int32), ('frame', ctypes.c_uint32), ('flags', ctypes.c_uint32)]


class DFFModel(ctypes.Structure):
    _fields_ = [('frame_count', ctypes.c_uint32), ('geometry_count', ctypes.c_uint32),
                ('atomic_count', ctypes.c_uint32), ('hanim_count', ctypes.c_uint32),
                ('frames', ctypes.POINTER(DFFFrame)),
                ('geometries', ctypes.POINTER(DFFGeometry)),
                ('atomics', ctypes.POINTER(DFFAtomic)),
                ('hanim', ctypes.POINTER(DFFHAnim))]


class ClusterVertex(ctypes.Structure):
    _fields_ = [('x', ctypes.c_double), ('y', ctypes.c_double), ('z', ctypes.c_double)]


class ClusterFace(ctypes.Structure):
    _fields_ = [('a', ctypes.c_uint32), ('b', ctypes.c_uint32), ('c', ctypes.c_uint32),
                ('material', ctypes.c_uint16)]


class ClusterResult(ctypes.Structure):
    _fields_ = [('representatives', ctypes.POINTER(ClusterVertex)),
                ('kept', ctypes.POINTER(ctypes.c_uint32)), ('count', ctypes.c_size_t)]


class DMDLOptions(ctypes.Structure):
    _fields_ = [('ratio', ctypes.c_double), ('min_area', ctypes.c_double),
                ('max_shift', ctypes.c_double), ('min_tris', ctypes.c_uint32),
                ('max_tris', ctypes.c_uint32), ('uvs', ctypes.c_uint8),
                ('normals', ctypes.c_uint8), ('colors', ctypes.c_uint8),
                ('skin', ctypes.c_uint8), ('drop_damage', ctypes.c_uint8),
                ('drop_vlo', ctypes.c_uint8), ('decimate', ctypes.c_uint8),
                ('ground_qem', ctypes.c_uint8), ('rigid_skin', ctypes.c_uint8),
                ('drop_frames', ctypes.POINTER(ctypes.c_char_p)),
                ('drop_count', ctypes.c_size_t)]


class WorldVec3(ctypes.Structure):
    _fields_ = [('x', ctypes.c_double), ('y', ctypes.c_double), ('z', ctypes.c_double)]


class WorldFace(ctypes.Structure):
    _fields_ = [('a', ctypes.c_uint32), ('b', ctypes.c_uint32),
                ('c', ctypes.c_uint32), ('material', ctypes.c_uint8),
                ('light', ctypes.c_uint8)]


class WorldSphere(ctypes.Structure):
    _fields_ = [('center', WorldVec3), ('radius', ctypes.c_double),
                ('surface', ctypes.c_uint8 * 4)]


class WorldBox(ctypes.Structure):
    _fields_ = [('min', WorldVec3), ('max', WorldVec3),
                ('surface', ctypes.c_uint8 * 4)]


class WorldColModel(ctypes.Structure):
    _fields_ = [('vertices', ctypes.POINTER(WorldVec3)), ('vertex_count', ctypes.c_size_t),
                ('faces', ctypes.POINTER(WorldFace)), ('face_count', ctypes.c_size_t),
                ('spheres', ctypes.POINTER(WorldSphere)), ('sphere_count', ctypes.c_size_t),
                ('boxes', ctypes.POINTER(WorldBox)), ('box_count', ctypes.c_size_t)]


class WorldColEntry(ctypes.Structure):
    _fields_ = [('name', ctypes.c_char * 24),
                ('record', ctypes.POINTER(ctypes.c_uint8)),
                ('length', ctypes.c_size_t), ('radius', ctypes.c_uint16),
                ('faces', ctypes.c_uint32)]


class WorldPackModel(ctypes.Structure):
    _fields_ = [('name', ctypes.c_char * 24), ('txd', ctypes.c_uint16),
                ('draw_distance', ctypes.c_uint16),
                ('time_on', ctypes.c_uint8), ('time_off', ctypes.c_uint8),
                ('radius', ctypes.c_uint16), ('flags', ctypes.c_uint32)]


class WorldPackTXD(ctypes.Structure):
    _fields_ = [('name', ctypes.c_char * 24), ('parent', ctypes.c_int16),
                ('pack', ctypes.c_uint16)]


class WorldPackInstance(ctypes.Structure):
    _fields_ = [('pos', ctypes.c_double * 3), ('rot', ctypes.c_double * 9),
                ('model', ctypes.c_uint16), ('switch_flags', ctypes.c_uint16),
                ('linked', ctypes.c_uint16), ('area', ctypes.c_uint16)]


class WorldPackCollision(ctypes.Structure):
    _fields_ = [('record', ctypes.POINTER(ctypes.c_uint8)),
                ('length', ctypes.c_size_t), ('radius', ctypes.c_uint16)]


class WorldPackInput(ctypes.Structure):
    _fields_ = [('ox', ctypes.c_int16), ('oy', ctypes.c_int16),
                ('sector', ctypes.c_uint16), ('grid_width', ctypes.c_uint16),
                ('grid_height', ctypes.c_uint16),
                ('models', ctypes.POINTER(WorldPackModel)), ('model_count', ctypes.c_size_t),
                ('txds', ctypes.POINTER(WorldPackTXD)), ('txd_count', ctypes.c_size_t),
                ('cell_counts', ctypes.POINTER(ctypes.c_uint16)),
                ('instances', ctypes.POINTER(WorldPackInstance)),
                ('instance_count', ctypes.c_size_t),
                ('collisions', ctypes.POINTER(WorldPackCollision))]


WorldReadFile = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p,
                                ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
                                ctypes.POINTER(ctypes.c_size_t))
WorldReadRange = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p,
                                 ctypes.c_uint32, ctypes.c_uint32,
                                 ctypes.c_uint32, ctypes.c_uint32,
                                 ctypes.POINTER(ctypes.c_uint8))


class WorldBuildOptions(ctypes.Structure):
    _fields_ = [('context', ctypes.c_void_p), ('file', WorldReadFile),
                ('range', WorldReadRange), ('bounds', ctypes.c_double * 4),
                ('sector', ctypes.c_uint16), ('use_bounds', ctypes.c_uint8),
                ('keep_lods', ctypes.c_uint8)]


class WorldBuildStats(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint32) for name in
                ('models', 'txds', 'instances', 'sectors', 'col_models',
                 'col_faces', 'col_dups', 'skip_models', 'skip_lods',
                 'skip_bounds')] + [('col_bytes', ctypes.c_uint64)]


class PedBuildOptions(ctypes.Structure):
    _fields_ = [('context', ctypes.c_void_p), ('file', WorldReadFile),
                ('bounds', ctypes.c_double * 4)]


class PedBuildStats(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint32) for name in
                ('models', 'stats', 'anims', 'groups', 'zones', 'nodes',
                 'links', 'road_nodes', 'road_links', 'scripted')] + [
                     ('script_start', ctypes.c_int32),
                     ('script_end', ctypes.c_int32),
                     ('grid_width', ctypes.c_uint16),
                     ('grid_height', ctypes.c_uint16)]


class VehBuildOptions(ctypes.Structure):
    _fields_ = [('context', ctypes.c_void_p), ('file', WorldReadFile)]


class VehBuildStats(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint32) for name in
                ('vehicles', 'colours', 'surfaces', 'no_handling',
                 'water', 'track_nodes')]


class EnexBuildOptions(ctypes.Structure):
    _fields_ = [('context', ctypes.c_void_p), ('file', WorldReadFile)]


class EnexBuildStats(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint32) for name in
                ('entries', 'exterior', 'linked')]


class IPLInstance(ctypes.Structure):
    _fields_ = [('model', ctypes.c_int32), ('transform', ctypes.c_double * 7),
                ('area', ctypes.c_int32), ('lod', ctypes.c_int32)]


class EnexEntry(ctypes.Structure):
    _fields_ = [('values', ctypes.c_double * 11), ('flags', ctypes.c_int32),
                ('area', ctypes.c_int32), ('sky', ctypes.c_int32),
                ('peds', ctypes.c_int32), ('on', ctypes.c_int32),
                ('off', ctypes.c_int32), ('name', ctypes.c_char * 128),
                ('source_area', ctypes.c_uint16)]


class WorldPlanModel(ctypes.Structure):
    _fields_ = [('id', ctypes.c_int32), ('name', ctypes.c_char * 128),
                ('available', ctypes.c_uint8)]


class WorldPlanInstance(ctypes.Structure):
    _fields_ = [('model', ctypes.c_int32), ('linked', ctypes.c_int32),
                ('position', ctypes.c_double * 3), ('rotation', ctypes.c_double * 9),
                ('is_lod', ctypes.c_uint8), ('area', ctypes.c_uint8)]


class WorldPlanFile(ctypes.Structure):
    _fields_ = [('path', ctypes.c_char * 256), ('kind', ctypes.c_uint8)]


class WorldPlanObject(ctypes.Structure):
    _fields_ = [('id', ctypes.c_int32), ('name', ctypes.c_char * 128),
                ('txd', ctypes.c_char * 128), ('draw_distance', ctypes.c_double),
                ('flags', ctypes.c_int64), ('on', ctypes.c_uint8),
                ('off', ctypes.c_uint8)]


class WorldPlanParent(ctypes.Structure):
    _fields_ = [('name', ctypes.c_char * 128), ('parent', ctypes.c_char * 128)]


class WorldPlanModelEntry(ctypes.Structure):
    _fields_ = [('name', ctypes.c_char * 128), ('draw_distance', ctypes.c_double),
                ('flags', ctypes.c_int64), ('offset', ctypes.c_uint64),
                ('size', ctypes.c_uint32), ('txd', ctypes.c_uint16),
                ('pack', ctypes.c_uint16), ('on', ctypes.c_uint8),
                ('off', ctypes.c_uint8)]


class WorldPlanTXDEntry(ctypes.Structure):
    _fields_ = [('name', ctypes.c_char * 128), ('parent', ctypes.c_int16),
                ('pack', ctypes.c_uint16)]


class IDEObject(ctypes.Structure):
    _fields_ = [('id', ctypes.c_int32), ('name', ctypes.c_char * 128),
                ('txd', ctypes.c_char * 128), ('draw_distance', ctypes.c_double),
                ('flags', ctypes.c_int64), ('on', ctypes.c_int32), ('off', ctypes.c_int32)]


class IDEParent(ctypes.Structure):
    _fields_ = [('name', ctypes.c_char * 128), ('parent', ctypes.c_char * 128)]


class DSNDOptions(ctypes.Structure):
    _fields_ = [('rate', ctypes.c_uint32), ('sample_rate', ctypes.c_uint32),
                ('format', ctypes.c_uint8), ('checkpoints', ctypes.c_uint8),
                ('volume', ctypes.c_double), ('max_seconds', ctypes.c_double),
                ('loop', ctypes.c_int32), ('headroom', ctypes.c_int16)]


class AudioTableName(ctypes.Structure):
    _fields_ = [('name', ctypes.c_char * 53)]


class AudioTableLookup(ctypes.Structure):
    _fields_ = [('pack', ctypes.c_uint32), ('offset', ctypes.c_uint32),
                ('size', ctypes.c_uint32)]


class OggVorbisInfo(ctypes.Structure):
    _fields_ = [('sample_rate', ctypes.c_uint32), ('samples', ctypes.c_uint64),
                ('small_block', ctypes.c_uint16), ('large_block', ctypes.c_uint16),
                ('channels', ctypes.c_uint8)]


class DSSTEntry(ctypes.Structure):
    _fields_ = [('track', ctypes.c_uint32), ('data', ctypes.POINTER(ctypes.c_uint8)),
                ('length', ctypes.c_size_t)]


class DSFXEntry(ctypes.Structure):
    _fields_ = [('bank', ctypes.c_uint16), ('data', ctypes.POINTER(ctypes.c_uint8)),
                ('size', ctypes.c_size_t)]


class DSBLEntry(ctypes.Structure):
    _fields_ = [('pack', ctypes.c_uint8), ('index', ctypes.c_uint16)]


class VehicleVec3(ctypes.Structure):
    _fields_ = [('x', ctypes.c_double), ('y', ctypes.c_double), ('z', ctypes.c_double)]


class VehicleModel(ctypes.Structure):
    _fields_ = [('model', ctypes.c_char * 24), ('txd', ctypes.c_char * 24),
                ('gxt', ctypes.c_char * 8), ('anims', ctypes.c_char * 16),
                ('id', ctypes.c_uint16), ('type', ctypes.c_uint8),
                ('color_count', ctypes.c_uint8), ('flags', ctypes.c_uint32),
                ('palette', ctypes.c_uint8 * 16),
                ('wheel_front', ctypes.c_double), ('wheel_rear', ctypes.c_double),
                ('handling', ctypes.c_double * 26),
                ('gears', ctypes.c_uint8), ('drive', ctypes.c_uint8),
                ('engine', ctypes.c_uint8), ('abs', ctypes.c_uint8),
                ('model_flags', ctypes.c_uint32), ('handling_flags', ctypes.c_uint32),
                ('value', ctypes.c_uint32), ('front_lights', ctypes.c_uint8),
                ('rear_lights', ctypes.c_uint8), ('anim_group', ctypes.c_uint8),
                ('submerged', ctypes.c_uint8), ('special_kind', ctypes.c_uint8),
                ('has_fly', ctypes.c_uint8), ('steer', ctypes.c_double),
                ('special', ctypes.c_double * 15), ('flying', ctypes.c_double * 21)]


class VehicleWater(ctypes.Structure):
    _fields_ = [('vertices', VehicleVec3 * 4),
                ('count', ctypes.c_uint8), ('flags', ctypes.c_uint8)]


class VehicleWaterEntry(ctypes.Structure):
    _fields_ = [('vertices', VehicleVec3 * 4),
                ('count', ctypes.c_uint8), ('flags', ctypes.c_int32)]


class VehicleTrack(ctypes.Structure):
    _fields_ = [('nodes', ctypes.POINTER(VehicleVec3)), ('count', ctypes.c_size_t)]


class VehicleInput(ctypes.Structure):
    _fields_ = [('models', ctypes.POINTER(VehicleModel)), ('model_count', ctypes.c_size_t),
                ('colors', ctypes.POINTER(ctypes.c_uint8)), ('color_count', ctypes.c_size_t),
                ('surfaces', ctypes.POINTER(ctypes.c_uint8)), ('surface_count', ctypes.c_size_t),
                ('adhesion', ctypes.POINTER(ctypes.c_double)),
                ('water', ctypes.POINTER(VehicleWater)), ('water_count', ctypes.c_size_t),
                ('tracks', ctypes.POINTER(VehicleTrack)), ('track_count', ctypes.c_size_t)]


class VehicleHandling(ctypes.Structure):
    _fields_ = [('mass', ctypes.c_double), ('drag', ctypes.c_double),
                ('engine_accel', ctypes.c_double), ('max_vel', ctypes.c_double),
                ('brake_decel', ctypes.c_double), ('damage_mult', ctypes.c_double),
                ('max_flat', ctypes.c_double), ('max_reverse', ctypes.c_double),
                ('handling_flags', ctypes.c_uint32), ('drive', ctypes.c_uint8)]


class VehicleIDE(ctypes.Structure):
    _fields_ = [('id', ctypes.c_int32), ('model', ctypes.c_char * 128),
                ('txd', ctypes.c_char * 128), ('handling', ctypes.c_char * 128),
                ('gxt', ctypes.c_char * 128), ('anims', ctypes.c_char * 128),
                ('type', ctypes.c_uint8), ('flags', ctypes.c_uint32),
                ('wheel_front', ctypes.c_double), ('wheel_rear', ctypes.c_double),
                ('steer', ctypes.c_double)]


class VehicleHandlingEntry(ctypes.Structure):
    _fields_ = [('name', ctypes.c_char * 32),
                ('mass', ctypes.c_double), ('turn_mass', ctypes.c_double),
                ('drag', ctypes.c_double), ('com', ctypes.c_double * 3),
                ('traction_mult', ctypes.c_double),
                ('traction_loss', ctypes.c_double),
                ('traction_bias', ctypes.c_double),
                ('max_vel', ctypes.c_double), ('engine_accel', ctypes.c_double),
                ('engine_inertia', ctypes.c_double),
                ('brake_decel', ctypes.c_double), ('brake_bias', ctypes.c_double),
                ('steering_lock', ctypes.c_double),
                ('suspension', ctypes.c_double * 7),
                ('seat_offset', ctypes.c_double), ('damage_mult', ctypes.c_double),
                ('max_flat', ctypes.c_double), ('max_reverse', ctypes.c_double),
                ('submerged', ctypes.c_int32), ('gears', ctypes.c_int32),
                ('abs', ctypes.c_int32), ('value', ctypes.c_int32),
                ('front_lights', ctypes.c_int32), ('rear_lights', ctypes.c_int32),
                ('anim_group', ctypes.c_int32),
                ('model_flags', ctypes.c_uint64), ('handling_flags', ctypes.c_uint64),
                ('drive', ctypes.c_uint8), ('engine', ctypes.c_uint8)]


class VehicleSpecialEntry(ctypes.Structure):
    _fields_ = [('name', ctypes.c_char * 32), ('kind', ctypes.c_uint8),
                ('values', ctypes.c_double * 21)]


class VehicleColor(ctypes.Structure):
    _fields_ = [('r', ctypes.c_int32), ('g', ctypes.c_int32), ('b', ctypes.c_int32)]


class VehicleCarColor(ctypes.Structure):
    _fields_ = [('name', ctypes.c_char * 128),
                ('pairs', (ctypes.c_int32 * 2) * 8), ('count', ctypes.c_uint8)]


class VehicleSurface(ctypes.Structure):
    _fields_ = [('group', ctypes.c_int32), ('wet', ctypes.c_int32),
                ('skid', ctypes.c_int32)]


class PedPathLink(ctypes.Structure):
    _fields_ = [('area', ctypes.c_uint16), ('node', ctypes.c_uint16)]


class PedPathNode(ctypes.Structure):
    _fields_ = [('index', ctypes.c_uint32), ('x', ctypes.c_int16),
                ('y', ctypes.c_int16), ('z', ctypes.c_int16),
                ('width', ctypes.c_uint8), ('link_count', ctypes.c_uint8),
                ('flags', ctypes.c_uint32), ('links', PedPathLink * 15)]


class PedPackInput(ctypes.Structure):
    _fields_ = [('models', ctypes.c_uint16), ('stats', ctypes.c_uint16),
                ('anims', ctypes.c_uint16), ('groups', ctypes.c_uint16),
                ('zones', ctypes.c_uint16),
                ('nodes', ctypes.c_uint32), ('links', ctypes.c_uint32),
                ('road_nodes', ctypes.c_uint32), ('road_links', ctypes.c_uint32),
                ('gx0', ctypes.c_int16), ('gy0', ctypes.c_int16),
                ('gw', ctypes.c_uint16), ('gh', ctypes.c_uint16),
                ('cell', ctypes.c_uint16),
                ('parts', ctypes.POINTER(ctypes.c_uint8) * 14),
                ('lengths', ctypes.c_size_t * 14)]


class PedSCMZone(ctypes.Structure):
    _fields_ = [('label', ctypes.c_char * 9), ('population', ctypes.c_int32),
                ('races', ctypes.c_int32), ('gang', ctypes.c_uint8 * 10),
                ('dealer', ctypes.c_uint8), ('has_population', ctypes.c_uint8)]


class PedGraphNode(ctypes.Structure):
    _fields_ = [('x', ctypes.c_int16), ('y', ctypes.c_int16),
                ('z', ctypes.c_int16), ('width', ctypes.c_uint8),
                ('flags', ctypes.c_uint32), ('cell', ctypes.c_uint32),
                ('links', ctypes.POINTER(ctypes.c_uint16)),
                ('link_count', ctypes.c_size_t)]


class PedIDEEntry(ctypes.Structure):
    _fields_ = [('id', ctypes.c_int32), ('model', ctypes.c_char * 128),
                ('txd', ctypes.c_char * 128), ('type', ctypes.c_char * 32),
                ('stat', ctypes.c_char * 32), ('anim', ctypes.c_char * 128),
                ('cars', ctypes.c_uint32), ('flags', ctypes.c_uint32)]


class PedStatEntry(ctypes.Structure):
    _fields_ = [('name', ctypes.c_char * 128),
                ('values', ctypes.c_double * 10)]


class PedZoneEntry(ctypes.Structure):
    _fields_ = [('name', ctypes.c_char * 128),
                ('bounds', ctypes.c_double * 4)]


class PedAnimEvent(ctypes.Structure):
    _fields_ = [('name', ctypes.c_char * 128), ('kind', ctypes.c_uint8)]


class PedNamedModel(ctypes.Structure):
    _fields_ = [('name', ctypes.c_char * 128), ('index', ctypes.c_uint16)]


class PedGroupMember(ctypes.Structure):
    _fields_ = [('group', ctypes.c_uint16), ('index', ctypes.c_uint16)]


class PedTableModel(ctypes.Structure):
    _fields_ = [('model', ctypes.c_char * 24), ('txd', ctypes.c_char * 24),
                ('id', ctypes.c_uint16), ('cars', ctypes.c_uint16),
                ('type', ctypes.c_uint8), ('stat', ctypes.c_uint8),
                ('anim', ctypes.c_uint8), ('race', ctypes.c_uint8),
                ('flags', ctypes.c_uint8)]


class PedTableStat(ctypes.Structure):
    _fields_ = [('value', ctypes.c_double * 10)]


class PedTableAnim(ctypes.Structure):
    _fields_ = [('name', ctypes.c_char * 16), ('clips', (ctypes.c_char * 24) * 6)]


class PedTableGroup(ctypes.Structure):
    _fields_ = [('members', ctypes.POINTER(ctypes.c_uint16)), ('count', ctypes.c_size_t)]


class PedTableZone(ctypes.Structure):
    _fields_ = [('bounds', ctypes.c_int16 * 4),
                ('population', ctypes.c_uint8), ('races', ctypes.c_uint8),
                ('dealer', ctypes.c_uint8), ('gang', ctypes.c_uint8 * 10)]


class PedTableInput(ctypes.Structure):
    _fields_ = [('models', ctypes.POINTER(PedTableModel)), ('model_count', ctypes.c_size_t),
                ('stats', ctypes.POINTER(PedTableStat)), ('stat_count', ctypes.c_size_t),
                ('anims', ctypes.POINTER(PedTableAnim)), ('anim_count', ctypes.c_size_t),
                ('groups', ctypes.POINTER(PedTableGroup)), ('group_count', ctypes.c_size_t),
                ('cars', ctypes.POINTER(PedTableGroup)), ('car_count', ctypes.c_size_t),
                ('popcycle', ctypes.POINTER(ctypes.c_int32)), ('pop_count', ctypes.c_size_t),
                ('zones', ctypes.POINTER(PedTableZone)), ('zone_count', ctypes.c_size_t)]


@lru_cache(maxsize=1)
def _library(path):
    lib = ctypes.CDLL(path)
    lib.dspk_write.argtypes = [ctypes.POINTER(DSPKEntry), ctypes.c_size_t,
                               DSPKRead, DSPKWrite, ctypes.c_void_p]
    lib.dspk_write.restype = ctypes.c_int
    lib.dvid_pack.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                              ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                              ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                              ctypes.c_uint16, ctypes.c_uint32,
                              ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
                              ctypes.POINTER(ctypes.c_size_t),
                              ctypes.POINTER(ctypes.c_uint32)]
    lib.dvid_pack.restype = ctypes.c_int
    lib.mpeg_movie_convert.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                                       ctypes.c_uint16, ctypes.c_uint32,
                                       ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
                                       ctypes.POINTER(ctypes.c_size_t),
                                       ctypes.POINTER(ctypes.c_uint32)]
    lib.mpeg_movie_convert.restype = ctypes.c_int
    lib.audio_stream_convert.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                                         ctypes.c_uint32, ctypes.c_uint32,
                                         ctypes.c_uint32, ctypes.c_uint8, ctypes.c_int,
                                         ctypes.c_double, ctypes.c_double,
                                         ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
                                         ctypes.POINTER(ctypes.c_size_t)]
    lib.audio_stream_convert.restype = ctypes.c_int
    lib.ogg_decode_vorbis.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                                     ctypes.POINTER(ctypes.POINTER(ctypes.c_int16)),
                                     ctypes.POINTER(ctypes.c_size_t),
                                     ctypes.POINTER(ctypes.c_uint32),
                                     ctypes.POINTER(ctypes.c_uint16)]
    lib.ogg_decode_vorbis.restype = ctypes.c_int
    lib.dtex_encode.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint16,
                                ctypes.c_uint16, Options,
                                ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
                                ctypes.POINTER(ctypes.c_size_t)]
    lib.dtex_encode.restype = ctypes.c_int
    lib.dtex_free.argtypes = [ctypes.c_void_p]
    lib.dtex_free.restype = None
    lib.texture_convert_txd.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                                        ctypes.POINTER(Target), ctypes.c_size_t,
                                        ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
                                        ctypes.POINTER(ctypes.c_size_t)]
    lib.texture_convert_txd.restype = ctypes.c_int
    lib.img_index_size.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                                   ctypes.POINTER(ctypes.c_size_t)]
    lib.img_index_size.restype = ctypes.c_int
    lib.img_parse_index.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                                    ctypes.POINTER(ctypes.POINTER(IMGEntry)),
                                    ctypes.POINTER(ctypes.c_size_t)]
    lib.img_parse_index.restype = ctypes.c_int
    lib.img_free.argtypes = [ctypes.c_void_p]
    lib.img_free.restype = None
    lib.radar_convert.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
                                  ctypes.POINTER(ctypes.c_size_t)]
    lib.radar_convert.restype = ctypes.c_int
    lib.adpcm_encode.argtypes = [ctypes.POINTER(ctypes.c_int16), ctypes.c_size_t,
                                 ctypes.c_int, ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
                                 ctypes.POINTER(ctypes.c_size_t),
                                 ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
                                 ctypes.POINTER(ctypes.c_size_t)]
    lib.adpcm_encode.restype = ctypes.c_int
    lib.points_from_denx.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                                     ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
                                     ctypes.POINTER(ctypes.c_size_t)]
    lib.points_from_denx.restype = ctypes.c_int
    lib.col_convert.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t, ctypes.c_int,
                                ctypes.c_double, ctypes.c_uint32,
                                ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
                                ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_uint32),
                                ctypes.POINTER(ctypes.c_uint32)]
    lib.col_convert.restype = ctypes.c_int
    lib.ifp_convert.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                                ctypes.POINTER(IFPOptions),
                                ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
                                ctypes.POINTER(ctypes.c_size_t),
                                ctypes.POINTER(ctypes.c_uint32),
                                ctypes.POINTER(ctypes.c_uint32)]
    lib.ifp_convert.restype = ctypes.c_int
    lib.dff_radius.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                               ctypes.POINTER(ctypes.c_uint16)]
    lib.dff_radius.restype = ctypes.c_int
    lib.geometry_encode.argtypes = [
        ctypes.POINTER(ctypes.c_double), ctypes.POINTER(ctypes.c_double),
        ctypes.POINTER(ctypes.c_double), ctypes.POINTER(ctypes.c_uint8),
        ctypes.POINTER(ctypes.c_uint8), ctypes.POINTER(ctypes.c_uint16),
        ctypes.c_size_t, ctypes.c_size_t, ctypes.POINTER(GeometryMaterial),
        ctypes.c_size_t, ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
        ctypes.POINTER(ctypes.c_size_t)]
    lib.geometry_encode.restype = ctypes.c_int
    lib.dff_parse_model.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                                    ctypes.POINTER(DFFModel)]
    lib.dff_parse_model.restype = ctypes.c_int
    lib.dff_free_model.argtypes = [ctypes.POINTER(DFFModel)]
    lib.dff_free_model.restype = None
    lib.cluster_decimate.argtypes = [ctypes.POINTER(ClusterVertex), ctypes.c_size_t,
                                     ctypes.POINTER(ClusterFace), ctypes.c_size_t,
                                     ctypes.c_size_t, ctypes.POINTER(ClusterResult)]
    lib.cluster_decimate.restype = ctypes.c_int
    lib.cluster_free.argtypes = [ctypes.POINTER(ClusterResult)]
    lib.cluster_free.restype = None
    lib.dmdl_convert.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                                        ctypes.POINTER(DMDLOptions),
                                        ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
                                        ctypes.POINTER(ctypes.c_size_t),
                                        ctypes.POINTER(ctypes.c_uint32),
                                        ctypes.POINTER(ctypes.c_uint32)]
    lib.dmdl_convert.restype = ctypes.c_int
    lib.qem_decimate.argtypes = [ctypes.POINTER(ClusterVertex), ctypes.c_size_t,
                                 ctypes.POINTER(ClusterFace), ctypes.c_size_t,
                                 ctypes.c_size_t, ctypes.POINTER(ctypes.c_uint8),
                                 ctypes.POINTER(ctypes.POINTER(ClusterFace)),
                                 ctypes.POINTER(ctypes.c_size_t)]
    lib.qem_decimate.restype = ctypes.c_int
    lib.wedge_decimate.argtypes = [ctypes.POINTER(ClusterVertex), ctypes.c_size_t,
                                   ctypes.POINTER(ClusterFace), ctypes.c_size_t,
                                   ctypes.c_size_t, ctypes.POINTER(ctypes.POINTER(ClusterFace)),
                                   ctypes.POINTER(ctypes.c_size_t)]
    lib.wedge_decimate.restype = ctypes.c_int
    lib.worldcol_pack.argtypes = [ctypes.POINTER(WorldColModel),
                                  ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
                                  ctypes.POINTER(ctypes.c_size_t),
                                  ctypes.POINTER(ctypes.c_uint16),
                                  ctypes.POINTER(ctypes.c_uint32)]
    lib.worldcol_pack.restype = ctypes.c_int
    lib.worldcol_parse_archive.argtypes = [
        ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
        ctypes.POINTER(ctypes.POINTER(WorldColEntry)), ctypes.POINTER(ctypes.c_size_t)]
    lib.worldcol_parse_archive.restype = ctypes.c_int
    lib.worldcol_free_archive.argtypes = [ctypes.POINTER(WorldColEntry), ctypes.c_size_t]
    lib.worldcol_free_archive.restype = None
    lib.worldpack_encode.argtypes = [ctypes.POINTER(WorldPackInput),
                                     ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
                                     ctypes.POINTER(ctypes.c_size_t)]
    lib.worldpack_encode.restype = ctypes.c_int
    lib.worldbuild_convert.argtypes = [ctypes.POINTER(WorldBuildOptions),
                                       ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
                                       ctypes.POINTER(ctypes.c_size_t),
                                       ctypes.POINTER(WorldBuildStats)]
    lib.worldbuild_convert.restype = ctypes.c_int
    lib.worldplan_instances.argtypes = [
        ctypes.POINTER(IPLInstance), ctypes.c_size_t,
        ctypes.POINTER(IPLInstance), ctypes.c_size_t,
        ctypes.POINTER(WorldPlanModel), ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_double), ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(WorldPlanInstance)),
        ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_uint32)]
    lib.worldplan_instances.restype = ctypes.c_int
    lib.worldplan_resolve.argtypes = [
        ctypes.c_char_p, ctypes.POINTER(IMGEntry), ctypes.c_size_t,
        ctypes.POINTER(IMGEntry), ctypes.c_size_t,
        ctypes.POINTER(IMGEntry), ctypes.POINTER(ctypes.c_int)]
    lib.worldplan_resolve.restype = ctypes.c_int
    lib.worldplan_parse_dat.argtypes = [
        ctypes.c_char_p, ctypes.c_size_t,
        ctypes.POINTER(ctypes.POINTER(WorldPlanFile)), ctypes.POINTER(ctypes.c_size_t)]
    lib.worldplan_parse_dat.restype = ctypes.c_int
    lib.worldplan_sectors.argtypes = [
        ctypes.POINTER(WorldPlanInstance), ctypes.c_size_t, ctypes.c_int,
        ctypes.POINTER(ctypes.c_int16), ctypes.POINTER(ctypes.c_int16),
        ctypes.POINTER(ctypes.c_uint16), ctypes.POINTER(ctypes.c_uint16),
        ctypes.POINTER(ctypes.POINTER(ctypes.c_uint16)),
        ctypes.POINTER(ctypes.POINTER(ctypes.c_size_t))]
    lib.worldplan_sectors.restype = ctypes.c_int
    lib.worldplan_tables.argtypes = [
        ctypes.POINTER(ctypes.c_int32), ctypes.c_size_t,
        ctypes.POINTER(WorldPlanObject), ctypes.c_size_t,
        ctypes.POINTER(WorldPlanParent), ctypes.c_size_t,
        ctypes.POINTER(IMGEntry), ctypes.c_size_t,
        ctypes.POINTER(IMGEntry), ctypes.c_size_t,
        ctypes.POINTER(ctypes.POINTER(WorldPlanModelEntry)),
        ctypes.POINTER(ctypes.POINTER(WorldPlanTXDEntry)),
        ctypes.POINTER(ctypes.c_size_t)]
    lib.worldplan_tables.restype = ctypes.c_int
    lib.ipl_parse_binary.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                                    ctypes.POINTER(ctypes.POINTER(IPLInstance)),
                                    ctypes.POINTER(ctypes.c_size_t)]
    lib.ipl_parse_binary.restype = ctypes.c_int
    lib.ipl_parse_text.argtypes = [ctypes.c_char_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.POINTER(IPLInstance)),
                                  ctypes.POINTER(ctypes.c_size_t)]
    lib.ipl_parse_text.restype = ctypes.c_int
    lib.ipl_rotation.argtypes = [ctypes.c_double, ctypes.c_double,
                                 ctypes.c_double, ctypes.c_double,
                                 ctypes.POINTER(ctypes.c_double)]
    lib.ipl_rotation.restype = None
    lib.enex_parse.argtypes = [
        ctypes.c_char_p, ctypes.c_size_t,
        ctypes.POINTER(IPLInstance), ctypes.c_size_t,
        ctypes.POINTER(ctypes.POINTER(EnexEntry)),
        ctypes.POINTER(ctypes.c_size_t)]
    lib.enex_parse.restype = ctypes.c_int
    lib.enex_pack.argtypes = [
        ctypes.POINTER(EnexEntry), ctypes.c_size_t,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint32)]
    lib.enex_pack.restype = ctypes.c_int
    lib.ide_parse.argtypes = [ctypes.c_char_p, ctypes.c_size_t,
                              ctypes.POINTER(ctypes.POINTER(IDEObject)),
                              ctypes.POINTER(ctypes.c_size_t),
                              ctypes.POINTER(ctypes.POINTER(IDEParent)),
                              ctypes.POINTER(ctypes.c_size_t)]
    lib.ide_parse.restype = ctypes.c_int
    lib.dsnd_encode.argtypes = [ctypes.POINTER(ctypes.c_int16), ctypes.c_size_t,
                                ctypes.POINTER(DSNDOptions),
                                ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
                                ctypes.POINTER(ctypes.c_size_t),
                                ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
                                ctypes.POINTER(ctypes.c_size_t)]
    lib.dsnd_encode.restype = ctypes.c_int
    lib.dsbk_convert.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                                 ctypes.POINTER(DSNDOptions),
                                 ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
                                 ctypes.POINTER(ctypes.c_size_t),
                                 ctypes.POINTER(ctypes.c_uint16)]
    lib.dsbk_convert.restype = ctypes.c_int
    lib.dsst_pack.argtypes = [ctypes.POINTER(DSSTEntry), ctypes.c_size_t,
                              ctypes.c_int, ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
                              ctypes.POINTER(ctypes.c_size_t)]
    lib.dsst_pack.restype = ctypes.c_int
    lib.dsfx_pack.argtypes = [
        ctypes.POINTER(DSFXEntry), ctypes.c_size_t,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
        ctypes.POINTER(ctypes.c_size_t)]
    lib.dsfx_pack.restype = ctypes.c_int
    lib.dsbl_pack.argtypes = [
        ctypes.POINTER(ctypes.c_char_p), ctypes.c_size_t,
        ctypes.POINTER(DSBLEntry), ctypes.c_size_t,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
        ctypes.POINTER(ctypes.c_size_t)]
    lib.dsbl_pack.restype = ctypes.c_int
    lib.audio_decrypt.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                                  ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint8)]
    lib.audio_decrypt.restype = None
    lib.audio_table_names.argtypes = [
        ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t, ctypes.c_size_t,
        ctypes.POINTER(ctypes.POINTER(AudioTableName)),
        ctypes.POINTER(ctypes.c_size_t)]
    lib.audio_table_names.restype = ctypes.c_int
    lib.audio_table_entries.argtypes = [
        ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
        ctypes.POINTER(ctypes.POINTER(AudioTableLookup)),
        ctypes.POINTER(ctypes.c_size_t)]
    lib.audio_table_entries.restype = ctypes.c_int
    lib.ogg_read_vorbis_info.argtypes = [
        ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
        ctypes.POINTER(OggVorbisInfo)]
    lib.ogg_read_vorbis_info.restype = ctypes.c_int
    lib.audio_aligned_offset.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                                         ctypes.c_uint64, ctypes.c_uint64,
                                         ctypes.POINTER(ctypes.c_uint64)]
    lib.audio_aligned_offset.restype = ctypes.c_int
    lib.vehpack_encode.argtypes = [ctypes.POINTER(VehicleInput),
                                   ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
                                   ctypes.POINTER(ctypes.c_size_t)]
    lib.vehpack_encode.restype = ctypes.c_int
    lib.vehpack_convert_handling.argtypes = [ctypes.POINTER(VehicleHandling)]
    lib.vehpack_convert_handling.restype = ctypes.c_int
    lib.vehparse_ide.argtypes = [ctypes.c_char_p, ctypes.c_size_t,
                                 ctypes.POINTER(ctypes.POINTER(VehicleIDE)),
                                 ctypes.POINTER(ctypes.c_size_t)]
    lib.vehparse_ide.restype = ctypes.c_int
    lib.vehparse_handling.argtypes = [ctypes.c_char_p, ctypes.c_size_t,
                                      ctypes.POINTER(ctypes.POINTER(VehicleHandlingEntry)),
                                      ctypes.POINTER(ctypes.c_size_t)]
    lib.vehparse_handling.restype = ctypes.c_int
    lib.vehparse_water.argtypes = [ctypes.c_char_p, ctypes.c_size_t,
                                   ctypes.POINTER(ctypes.POINTER(VehicleWaterEntry)),
                                   ctypes.POINTER(ctypes.c_size_t)]
    lib.vehparse_water.restype = ctypes.c_int
    lib.vehparse_track.argtypes = [ctypes.c_char_p, ctypes.c_size_t,
                                   ctypes.POINTER(ctypes.POINTER(VehicleVec3)),
                                   ctypes.POINTER(ctypes.c_size_t)]
    lib.vehparse_track.restype = ctypes.c_int
    lib.vehparse_special.argtypes = [ctypes.c_char_p, ctypes.c_size_t,
                                     ctypes.POINTER(ctypes.POINTER(VehicleSpecialEntry)),
                                     ctypes.POINTER(ctypes.c_size_t)]
    lib.vehparse_special.restype = ctypes.c_int
    lib.vehparse_carcols.argtypes = [
        ctypes.c_char_p, ctypes.c_size_t,
        ctypes.POINTER(ctypes.POINTER(VehicleColor)), ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.POINTER(VehicleCarColor)), ctypes.POINTER(ctypes.c_size_t)]
    lib.vehparse_carcols.restype = ctypes.c_int
    lib.vehparse_surfaces.argtypes = [
        ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_double), ctypes.POINTER(ctypes.POINTER(VehicleSurface)),
        ctypes.POINTER(ctypes.c_size_t)]
    lib.vehparse_surfaces.restype = ctypes.c_int
    lib.pedpath_parse.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_double), ctypes.c_int,
                                  ctypes.POINTER(ctypes.POINTER(PedPathNode)),
                                  ctypes.POINTER(ctypes.c_size_t)]
    lib.pedpath_parse.restype = ctypes.c_int
    lib.pedpack_encode.argtypes = [ctypes.POINTER(PedPackInput),
                                   ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
                                   ctypes.POINTER(ctypes.c_size_t)]
    lib.pedpack_encode.restype = ctypes.c_int
    lib.pedbuild_convert.argtypes = [ctypes.POINTER(PedBuildOptions),
                                     ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
                                     ctypes.POINTER(ctypes.c_size_t),
                                     ctypes.POINTER(PedBuildStats)]
    lib.pedbuild_convert.restype = ctypes.c_int
    lib.vehbuild_convert.argtypes = [ctypes.POINTER(VehBuildOptions),
                                     ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
                                     ctypes.POINTER(ctypes.c_size_t),
                                     ctypes.POINTER(VehBuildStats)]
    lib.vehbuild_convert.restype = ctypes.c_int
    lib.enexbuild_convert.argtypes = [ctypes.POINTER(EnexBuildOptions),
                                      ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
                                      ctypes.POINTER(ctypes.c_size_t),
                                      ctypes.POINTER(EnexBuildStats)]
    lib.enexbuild_convert.restype = ctypes.c_int
    lib.pedpop_rescale.argtypes = [ctypes.POINTER(ctypes.c_int32), ctypes.c_size_t]
    lib.pedpop_rescale.restype = ctypes.c_int
    lib.pedpop_parse.argtypes = [ctypes.c_char_p, ctypes.c_size_t,
                                 ctypes.POINTER(ctypes.POINTER(ctypes.c_int32)),
                                 ctypes.POINTER(ctypes.c_size_t)]
    lib.pedpop_parse.restype = ctypes.c_int
    for name, struct in (('pedparse_ide', PedIDEEntry),
                         ('pedparse_stats', PedStatEntry),
                         ('pedparse_zones', PedZoneEntry),
                         ('pedparse_animgrp', PedAnimEvent)):
        function = getattr(lib, name)
        function.argtypes = [ctypes.c_char_p, ctypes.c_size_t,
                             ctypes.POINTER(ctypes.POINTER(struct)),
                             ctypes.POINTER(ctypes.c_size_t)]
        function.restype = ctypes.c_int
    lib.pedparse_groups.argtypes = [
        ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(PedNamedModel),
        ctypes.c_size_t, ctypes.c_uint16, ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(PedGroupMember)),
        ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_uint16)]
    lib.pedparse_groups.restype = ctypes.c_int
    lib.pedscm_scan.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                                ctypes.POINTER(ctypes.POINTER(PedSCMZone)),
                                ctypes.POINTER(ctypes.c_size_t),
                                ctypes.POINTER(ctypes.c_int32), ctypes.POINTER(ctypes.c_int32)]
    lib.pedscm_scan.restype = ctypes.c_int
    lib.pedpack_paths.argtypes = [ctypes.POINTER(PedGraphNode), ctypes.c_size_t,
                                  ctypes.c_size_t, ctypes.c_int,
                                  ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
                                  ctypes.POINTER(ctypes.c_size_t),
                                  ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
                                  ctypes.POINTER(ctypes.c_size_t),
                                  ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
                                  ctypes.POINTER(ctypes.c_size_t),
                                  ctypes.POINTER(ctypes.c_uint32)]
    lib.pedpack_paths.restype = ctypes.c_int
    lib.pedtables_encode.argtypes = [ctypes.POINTER(PedTableInput),
                                     ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
                                     ctypes.POINTER(ctypes.c_size_t),
                                     ctypes.POINTER(ctypes.c_size_t)]
    lib.pedtables_encode.restype = ctypes.c_int
    return lib


def write_dspk(path, entries):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    names = [name.encode('ascii') for name in entries]
    values = list(entries.values())
    records = (DSPKEntry * len(names))(
        *(DSPKEntry(name, len(data), i + 1)
          for i, (name, data) in enumerate(zip(names, values))))
    errors = []

    def read(_context, source, offset, target, size):
        blob = values[source - 1]
        if offset + size > len(blob):
            return 0
        ctypes.memmove(target, blob[offset:offset + size], size)
        return 1

    with open(path, 'wb') as destination:
        def write(_context, data, size):
            try:
                return destination.write(ctypes.string_at(data, size)) == size
            except OSError as error:
                errors.append(error)
                return 0

        if not lib.dspk_write(records, len(records), DSPKRead(read),
                              DSPKWrite(write), None):
            if errors:
                raise errors[0]
            raise ValueError('native DSPK packing failed')


def pack_video(palette, frames, audio, fps, rate):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    sources = [(ctypes.c_uint8 * len(data)).from_buffer_copy(data)
               for data in (palette, frames, audio)]
    output = ctypes.POINTER(ctypes.c_uint8)()
    length, count = ctypes.c_size_t(), ctypes.c_uint32()
    if not lib.dvid_pack(sources[0], len(palette), sources[1], len(frames),
                         sources[2], len(audio), fps, rate, ctypes.byref(output),
                         ctypes.byref(length), ctypes.byref(count)):
        raise ValueError('native DVID packing failed')
    try:
        return ctypes.string_at(output, length.value), count.value
    finally:
        lib.dtex_free(output)


def convert_movie(data, fps=10, rate=22050):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    source = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    output = ctypes.POINTER(ctypes.c_uint8)()
    length, count = ctypes.c_size_t(), ctypes.c_uint32()
    if not lib.mpeg_movie_convert(source, len(data), fps, rate,
                                  ctypes.byref(output), ctypes.byref(length),
                                  ctypes.byref(count)):
        raise ValueError('native MPEG movie conversion failed')
    try:
        return ctypes.string_at(output, length.value), count.value
    finally:
        lib.dtex_free(output)


def convert_audio_stream(encrypted, offset, params):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    source = (ctypes.c_uint8 * len(encrypted)).from_buffer_copy(encrypted)
    output = ctypes.POINTER(ctypes.c_uint8)()
    length = ctypes.c_size_t()
    if not lib.audio_stream_convert(
            source, len(encrypted), offset & 0xffffffff, offset >> 32,
            params['sample_rate'],
            {'pcm8': 0, 'pcm16': 1, 'adpcm': 2}[params['format']],
            int(bool(params.get('seekable'))), params['volume'],
            params['max_seconds'], ctypes.byref(output), ctypes.byref(length)):
        raise ValueError('native Vorbis stream conversion failed')
    try:
        return ctypes.string_at(output, length.value)
    finally:
        lib.dtex_free(output)


def decode_ogg_vorbis(data):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    source = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    output = ctypes.POINTER(ctypes.c_int16)()
    samples, rate, channels = ctypes.c_size_t(), ctypes.c_uint32(), ctypes.c_uint16()
    if not lib.ogg_decode_vorbis(source, len(data), ctypes.byref(output),
                                 ctypes.byref(samples), ctypes.byref(rate),
                                 ctypes.byref(channels)):
        raise ValueError('native OGG/Vorbis decoding failed')
    try:
        return (ctypes.string_at(output, samples.value * channels.value * 2),
                rate.value, channels.value)
    finally:
        lib.dtex_free(output)


def _options(params):
    from .ds_texture import FORMATS
    return Options(FORMATS.get(params['format'], 0), params['alpha_threshold'],
                   params['auto_colors'], params['opaque'], params.get('additive', False))


def encode_texture(rgba, width, height, params):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    inp = (ctypes.c_uint8 * len(rgba)).from_buffer_copy(rgba)
    result = ctypes.POINTER(ctypes.c_uint8)()
    size = ctypes.c_size_t()
    if not lib.dtex_encode(inp, width, height, _options(params), ctypes.byref(result), ctypes.byref(size)):
        raise ValueError('native DTEX conversion failed')
    try:
        return ctypes.string_at(result, size.value)
    finally:
        lib.dtex_free(result)


def convert_dictionary(data, settings):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    inp = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    targets = (Target * len(settings))(
        *(Target(width, height, _options(params)) for width, height, params in settings))
    result = ctypes.POINTER(ctypes.c_uint8)()
    size = ctypes.c_size_t()
    if not lib.texture_convert_txd(inp, len(data), targets, len(settings),
                                   ctypes.byref(result), ctypes.byref(size)):
        raise ValueError('native TXD conversion failed')
    try:
        return ctypes.string_at(result, size.value)
    finally:
        lib.dtex_free(result)


def read_img_toc(path):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    with open(path, 'rb') as source:
        header = source.read(8)
        if len(header) != 8:
            raise ValueError(f'{path}: invalid IMG v2 archive')
        first = (ctypes.c_uint8 * 8).from_buffer_copy(header)
        needed = ctypes.c_size_t()
        if not lib.img_index_size(first, 8, ctypes.byref(needed)):
            raise ValueError(f'{path}: invalid IMG v2 archive')
        index = header + source.read(needed.value - 8)
    inp = (ctypes.c_uint8 * len(index)).from_buffer_copy(index)
    result = ctypes.POINTER(IMGEntry)()
    count = ctypes.c_size_t()
    if not lib.img_parse_index(inp, len(index), ctypes.byref(result), ctypes.byref(count)):
        raise ValueError(f'{path}: truncated IMG v2 archive')
    try:
        return [(result[i].name.split(b'\0', 1)[0].decode('latin1'),
                 result[i].offset, result[i].size) for i in range(count.value)]
    finally:
        lib.img_free(result)


def convert_radar(data):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    inp = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    result = ctypes.POINTER(ctypes.c_uint8)()
    length = ctypes.c_size_t()
    if not lib.radar_convert(inp, len(data), ctypes.byref(result), ctypes.byref(length)):
        raise ValueError('native radar conversion failed')
    try:
        return ctypes.string_at(result, length.value)
    finally:
        lib.dtex_free(result)


def encode_adpcm(samples, with_checkpoints=False):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    inp = (ctypes.c_int16 * len(samples)).from_buffer_copy(samples.tobytes())
    result = ctypes.POINTER(ctypes.c_uint8)()
    length = ctypes.c_size_t()
    checkpoints = ctypes.POINTER(ctypes.c_uint8)()
    checkpoint_length = ctypes.c_size_t()
    if not lib.adpcm_encode(inp, len(samples), with_checkpoints,
                            ctypes.byref(result), ctypes.byref(length),
                            ctypes.byref(checkpoints), ctypes.byref(checkpoint_length)):
        raise ValueError('native ADPCM conversion failed')
    try:
        return ctypes.string_at(result, length.value), (
            ctypes.string_at(checkpoints, checkpoint_length.value)
            if checkpoint_length.value else b'')
    finally:
        lib.dtex_free(result)
        lib.dtex_free(checkpoints)


def convert_points(data):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    inp = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    result = ctypes.POINTER(ctypes.c_uint8)()
    length = ctypes.c_size_t()
    if not lib.points_from_denx(inp, len(data), ctypes.byref(result), ctypes.byref(length)):
        raise ValueError('native map points conversion failed')
    try:
        return ctypes.string_at(result, length.value)
    finally:
        lib.dtex_free(result)


def convert_collision(data, params):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    inp = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    result = ctypes.POINTER(ctypes.c_uint8)()
    length = ctypes.c_size_t()
    faces = ctypes.c_uint32()
    written = ctypes.c_uint32()
    mode = {'full': 0, 'bounds': 1, 'simple': 2}[params['mode']]
    if not lib.col_convert(inp, len(data), mode, params['face_ratio'], params['min_faces'],
                           ctypes.byref(result), ctypes.byref(length),
                           ctypes.byref(faces), ctypes.byref(written)):
        raise ValueError('native COL conversion failed')
    try:
        return ctypes.string_at(result, length.value), {
            'faces_in': faces.value, 'faces_out': written.value}
    finally:
        lib.dtex_free(result)


def convert_animation(data, params):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    inp = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    removed = (ctypes.c_char_p * len(params['remove']))(
        *(pattern.encode('latin1') for pattern in params['remove']))
    clips = (ctypes.c_char_p * len(params['rigid_clips']))(
        *(pattern.encode('latin1') for pattern in params['rigid_clips']))
    options = IFPOptions(params['keep_every'], params['max_frames'], params['drop_translation'],
                         removed, len(removed), clips, len(clips))
    result = ctypes.POINTER(ctypes.c_uint8)()
    length = ctypes.c_size_t()
    before, after = ctypes.c_uint32(), ctypes.c_uint32()
    if not lib.ifp_convert(inp, len(data), ctypes.byref(options),
                           ctypes.byref(result), ctypes.byref(length),
                           ctypes.byref(before), ctypes.byref(after)):
        raise ValueError('native IFP conversion failed')
    try:
        return ctypes.string_at(result, length.value), {
            'keys_in': before.value, 'keys_out': after.value}
    finally:
        lib.dtex_free(result)


def model_radius(data):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    inp = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    radius = ctypes.c_uint16()
    if not lib.dff_radius(inp, len(data), ctypes.byref(radius)):
        raise ValueError('native DFF radius failed')
    return radius.value


def parse_model(data):
    from . import dff
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    inp = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    raw = DFFModel()
    if not lib.dff_parse_model(inp, len(data), ctypes.byref(raw)):
        raise ValueError('native DFF parsing failed')
    try:
        model = dff.Model()
        model.frames = []
        for source in raw.frames[:raw.frame_count]:
            item = dff.Frame()
            item.name = source.name.decode('latin1')
            item.rot = tuple(source.rot)
            item.pos = tuple(source.pos)
            item.parent = source.parent
            item.bone_id = source.bone_id
            model.frames.append(item)
        model.geometries = []
        for source in raw.geometries[:raw.geometry_count]:
            item = dff.Geometry()
            n = source.vertex_count
            item.flags = source.flags
            item.sphere = tuple(source.sphere)
            item.verts = [tuple(source.vertices[i * 3 + j] for j in range(3))
                          for i in range(n)] if source.vertices else []
            item.uvs = [tuple(source.uvs[i * 2 + j] for j in range(2))
                        for i in range(n)] if source.uvs else None
            item.normals = [tuple(source.normals[i * 3 + j] for j in range(3))
                            for i in range(n)] if source.normals else None
            item.colors = ctypes.string_at(source.colors, n * 4) if source.colors else None
            item.tris = [tuple(source.triangles[i * 4 + j] for j in range(4))
                         for i in range(source.triangle_count)]
            item.materials = []
            for material in source.materials[:source.material_count]:
                mat = dff.Material()
                mat.color = tuple(material.rgba)
                mat.texture = material.texture.decode('latin1')
                item.materials.append(mat)
            item.skin = None
            if source.skin:
                native = source.skin.contents
                skin = dff.Skin()
                skin.num_bones = native.num_bones
                skin.indices = ctypes.string_at(native.indices, n * 4)
                skin.weights = tuple(native.weights[:n * 4])
                skin.inv_matrices = [tuple(native.inv_matrices[i * 16 + j] for j in range(16))
                                     for i in range(skin.num_bones)]
                item.skin = skin
            model.geometries.append(item)
        model.atomics = [(a.frame, a.geometry) for a in raw.atomics[:raw.atomic_count]]
        model.hanim = [(node.id, node.frame, node.flags) for node in raw.hanim[:raw.hanim_count]]
        return model
    finally:
        lib.dff_free_model(ctypes.byref(raw))


def decimate_cluster(verts, tris, target):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    vertices = (ClusterVertex * len(verts))(*(ClusterVertex(*v) for v in verts))
    faces = (ClusterFace * len(tris))(
        *(ClusterFace(t[0], t[1], t[2], t[3]) for t in tris))
    result = ClusterResult()
    if not lib.cluster_decimate(vertices, len(verts), faces, len(tris),
                                target, ctypes.byref(result)):
        raise ValueError('native mesh clustering failed')
    try:
        rep = ([(result.representatives[i].x, result.representatives[i].y,
                 result.representatives[i].z) for i in range(len(verts))]
               if result.representatives else None)
        return rep, [tris[result.kept[i]] for i in range(result.count)]
    finally:
        lib.cluster_free(ctypes.byref(result))


def decimate_qem(verts, tris, target, bone):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    vertices = (ClusterVertex * len(verts))(*(ClusterVertex(*v) for v in verts))
    faces = (ClusterFace * len(tris))(
        *(ClusterFace(t[0], t[1], t[2], t[3]) for t in tris))
    bones = (ctypes.c_uint8 * len(bone))(*bone) if bone is not None else None
    result = ctypes.POINTER(ClusterFace)()
    count = ctypes.c_size_t()
    if not lib.qem_decimate(vertices, len(verts), faces, len(tris), target,
                            bones, ctypes.byref(result), ctypes.byref(count)):
        raise ValueError('native QEM decimation failed')
    try:
        return None, [(result[i].a, result[i].b, result[i].c, result[i].material)
                      for i in range(count.value)]
    finally:
        lib.dtex_free(result)


def decimate_wedge(verts, tris, target):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    vertices = (ClusterVertex * len(verts))(*(ClusterVertex(*v) for v in verts))
    faces = (ClusterFace * len(tris))(
        *(ClusterFace(t[0], t[1], t[2], t[3]) for t in tris))
    result = ctypes.POINTER(ClusterFace)()
    count = ctypes.c_size_t()
    if not lib.wedge_decimate(vertices, len(verts), faces, len(tris), target,
                              ctypes.byref(result), ctypes.byref(count)):
        raise ValueError('native wedge decimation failed')
    try:
        return None, [(result[i].a, result[i].b, result[i].c, result[i].material)
                      for i in range(count.value)]
    finally:
        lib.dtex_free(result)


def encode_world_collision(c):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    verts = (WorldVec3 * len(c.verts))(*(WorldVec3(*v) for v in c.verts))
    faces = (WorldFace * len(c.faces))(*(WorldFace(*f) for f in c.faces))
    spheres = (WorldSphere * len(c.spheres))(
        *(WorldSphere(WorldVec3(*center), r, (ctypes.c_uint8 * 4)(*surf))
          for center, r, surf in c.spheres))
    boxes = (WorldBox * len(c.boxes))(
        *(WorldBox(WorldVec3(*low), WorldVec3(*high), (ctypes.c_uint8 * 4)(*surf))
          for low, high, surf in c.boxes))
    model = WorldColModel(verts, len(verts), faces, len(faces),
                          spheres, len(spheres), boxes, len(boxes))
    result = ctypes.POINTER(ctypes.c_uint8)()
    length, radius, count = ctypes.c_size_t(), ctypes.c_uint16(), ctypes.c_uint32()
    if not lib.worldcol_pack(ctypes.byref(model), ctypes.byref(result),
                             ctypes.byref(length), ctypes.byref(radius),
                             ctypes.byref(count)):
        raise ValueError('native world collision conversion failed')
    try:
        return (ctypes.string_at(result, length.value) if length.value else None,
                radius.value, count.value)
    finally:
        lib.dtex_free(result)


def parse_world_collisions(data):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    inp = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    result = ctypes.POINTER(WorldColEntry)()
    count = ctypes.c_size_t()
    if not lib.worldcol_parse_archive(inp, len(data),
                                      ctypes.byref(result), ctypes.byref(count)):
        raise ValueError('native COL archive conversion failed')
    try:
        return [(entry.name.decode('latin1'),
                 ctypes.string_at(entry.record, entry.length) if entry.length else None,
                 entry.radius, entry.faces)
                for entry in result[:count.value]]
    finally:
        lib.worldcol_free_archive(result, count)


def encode_world(models, txds, txd_packs, cells, collisions, midx, ox, oy, sector, width, height):
    from .dsfmt import name as ds_name
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    model_records = (WorldPackModel * len(models))(
        *(WorldPackModel(ds_name(label, 24), txd, min(0xFFFF, int(distance)),
                         on, off, radius, flags & 0xFFFFFFFF)
          for label, txd, distance, flags, on, off, radius in models))
    txd_records = (WorldPackTXD * len(txds))(
        *(WorldPackTXD(ds_name(label, 24), parent, pack)
          for (label, parent), pack in zip(txds, txd_packs)))
    counts = (ctypes.c_uint16 * len(cells))(*(len(cell) for cell in cells))
    flat = [instance for cell in cells for instance in cell]
    instances = (WorldPackInstance * len(flat))(
        *(WorldPackInstance((ctypes.c_double * 3)(x, y, z),
                            (ctypes.c_double * 9)(*rot), midx[mid],
                            (100 if linked in midx else 0) | (0x8000 if is_lod else 0),
                            midx.get(linked, 0xFFFF), area)
          for mid, x, y, z, rot, is_lod, linked, area in flat))
    buffers = [(ctypes.c_uint8 * len(record)).from_buffer_copy(record) if record else None
               for record, _ in collisions]
    col_records = (WorldPackCollision * len(collisions))(
        *(WorldPackCollision(raw, len(record) if record else 0, radius)
          for raw, (record, radius) in zip(buffers, collisions)))
    packed = WorldPackInput(ox, oy, sector, width, height, model_records, len(models),
                            txd_records, len(txds), counts, instances, len(flat), col_records)
    result = ctypes.POINTER(ctypes.c_uint8)()
    length = ctypes.c_size_t()
    if not lib.worldpack_encode(ctypes.byref(packed), ctypes.byref(result),
                                ctypes.byref(length)):
        raise ValueError('native world pack conversion failed')
    try:
        return ctypes.string_at(result, length.value)
    finally:
        lib.dtex_free(result)


def build_world_files(game, section):
    from .world import resolve
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    libc = ctypes.CDLL(None)
    libc.malloc.argtypes = [ctypes.c_size_t]
    libc.malloc.restype = ctypes.c_void_p
    root = Path(game)
    paths = [resolve(root, name) for name in ('models/gta3.img', 'models/gta_int.img')]
    if not all(paths):
        raise FileNotFoundError('missing GTA IMG archives')
    errors = []

    def read_file(_context, name, destination, length):
        try:
            relative = name.decode('latin1').replace('\\', '/')
            if relative.startswith('/') or any(part in ('.', '..') or ':' in part
                                               for part in relative.split('/')):
                return 0
            path = resolve(root, relative)
            if not path:
                return 0
            data = path.read_bytes()
            address = libc.malloc(max(len(data), 1))
            if not address:
                return 0
            if data:
                ctypes.memmove(address, data, len(data))
            destination[0] = ctypes.cast(address, ctypes.POINTER(ctypes.c_uint8))
            length[0] = len(data)
            return 1
        except OSError as error:
            errors.append(error)
            return 0

    def read_range(_context, pack, low, high, length, destination):
        try:
            if pack > 1:
                return 0
            with paths[pack].open('rb') as archive:
                archive.seek(low | high << 32)
                data = archive.read(length)
            if len(data) != length:
                return 0
            if data:
                ctypes.memmove(destination, data, length)
            return 1
        except OSError as error:
            errors.append(error)
            return 0

    bounds = section.get('bounds')
    options = WorldBuildOptions(None, WorldReadFile(read_file),
                                WorldReadRange(read_range),
                                (ctypes.c_double * 4)(*(bounds or (0, 0, 0, 0))),
                                section.get('sector', 50), bool(bounds),
                                section.get('lods', False))
    output = ctypes.POINTER(ctypes.c_uint8)()
    length = ctypes.c_size_t()
    native = WorldBuildStats()
    result = lib.worldbuild_convert(ctypes.byref(options), ctypes.byref(output),
                                    ctypes.byref(length), ctypes.byref(native))
    if errors:
        if output:
            lib.dtex_free(output)
        raise errors[0]
    if not result:
        raise ValueError('native DWLD file conversion failed')
    try:
        stats = {'instances': native.instances, 'models': native.models,
                 'txds': native.txds, 'sectors': native.sectors,
                 'col_models': native.col_models, 'col_faces': native.col_faces,
                 'col_kb': native.col_bytes >> 10,
                 'col_dups': native.col_dups}
        stats.update({key: value for key, value in
                      (('skip_model', native.skip_models),
                       ('skip_lod', native.skip_lods),
                       ('skip_bounds', native.skip_bounds)) if value})
        return ctypes.string_at(output, length.value), stats
    finally:
        lib.dtex_free(output)


def _game_read_file(game):
    from .world import resolve
    libc = ctypes.CDLL(None)
    libc.malloc.argtypes = [ctypes.c_size_t]
    libc.malloc.restype = ctypes.c_void_p
    errors = []

    def read_file(_context, name, destination, length):
        try:
            relative = name.decode('latin1').replace('\\', '/')
            if relative.startswith('/') or any(part in ('.', '..') or ':' in part
                                               for part in relative.split('/')):
                return 0
            path = resolve(game, relative)
            if not path:
                return 0
            data = path.read_bytes()
            address = libc.malloc(max(len(data), 1))
            if not address:
                return 0
            if data:
                ctypes.memmove(address, data, len(data))
            destination[0] = ctypes.cast(address, ctypes.POINTER(ctypes.c_uint8))
            length[0] = len(data)
            return 1
        except OSError as error:
            errors.append(error)
            return 0

    return WorldReadFile(read_file), errors


def build_ped_files(game, bounds):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    callback, errors = _game_read_file(game)
    options = PedBuildOptions(None, callback,
                              (ctypes.c_double * 4)(*bounds))
    output = ctypes.POINTER(ctypes.c_uint8)()
    length = ctypes.c_size_t()
    native = PedBuildStats()
    result = lib.pedbuild_convert(ctypes.byref(options), ctypes.byref(output),
                                  ctypes.byref(length), ctypes.byref(native))
    if errors:
        if output:
            lib.dtex_free(output)
        raise errors[0]
    if not result:
        raise ValueError('native DPED file conversion failed')
    try:
        return ctypes.string_at(output, length.value), {
            'models': native.models, 'stats': native.stats,
            'anim_groups': native.anims, 'groups': native.groups,
            'zones': native.zones, 'scripted': native.scripted,
            'nodes': native.nodes, 'links': native.links,
            'road_nodes': native.road_nodes, 'road_links': native.road_links,
            'grid': f'{native.grid_width}x{native.grid_height}',
            'scm': f'{native.script_start}..{native.script_end}'
        }
    finally:
        lib.dtex_free(output)


def build_vehicle_files(game):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    callback, errors = _game_read_file(game)
    options = VehBuildOptions(None, callback)
    output = ctypes.POINTER(ctypes.c_uint8)()
    length = ctypes.c_size_t()
    native = VehBuildStats()
    result = lib.vehbuild_convert(ctypes.byref(options), ctypes.byref(output),
                                  ctypes.byref(length), ctypes.byref(native))
    if errors:
        if output:
            lib.dtex_free(output)
        raise errors[0]
    if not result:
        raise ValueError('native DVEH file conversion failed')
    try:
        return ctypes.string_at(output, length.value), {
            'vehicles': native.vehicles, 'rec': 380,
            'colours': native.colours, 'surfaces': native.surfaces,
            'no_handling': native.no_handling, 'water': native.water,
            'track_nodes': native.track_nodes
        }
    finally:
        lib.dtex_free(output)


def build_enex_files(game):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    callback, errors = _game_read_file(game)
    options = EnexBuildOptions(None, callback)
    output = ctypes.POINTER(ctypes.c_uint8)()
    length = ctypes.c_size_t()
    native = EnexBuildStats()
    result = lib.enexbuild_convert(ctypes.byref(options), ctypes.byref(output),
                                   ctypes.byref(length), ctypes.byref(native))
    if errors:
        if output:
            lib.dtex_free(output)
        raise errors[0]
    if not result:
        raise ValueError('native DENX file conversion failed')
    try:
        return ctypes.string_at(output, length.value), {
            'entries': native.entries, 'exterior': native.exterior,
            'linked': native.linked
        }
    finally:
        lib.dtex_free(output)


def plan_world_instances(text, stream, objects, toc, interior_toc, bounds, keep_lods):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    def instances(items):
        return (IPLInstance * len(items))(
            *(IPLInstance(mid, (ctypes.c_double * 7)(x, y, z, qx, qy, qz, qw),
                          area, lod)
              for mid, x, y, z, qx, qy, qz, qw, area, lod in items))
    native_text, native_stream = instances(text), instances(stream)
    models = (WorldPlanModel * len(objects))(
        *(WorldPlanModel(mid, name.encode('latin1'),
                         (name + '.dff' in toc or name + '.dff' in interior_toc))
          for mid, (name, *_rest) in sorted(objects.items())))
    limits = (ctypes.c_double * 4)(*bounds) if bounds else None
    output = ctypes.POINTER(WorldPlanInstance)()
    count = ctypes.c_size_t()
    skipped = (ctypes.c_uint32 * 3)()
    if not lib.worldplan_instances(native_text, len(text),
                                   native_stream, len(stream),
                                   models, len(models), limits, keep_lods,
                                   ctypes.byref(output), ctypes.byref(count),
                                   skipped):
        raise ValueError('native world planning failed')
    try:
        result = [(m.model, *m.position, tuple(m.rotation), bool(m.is_lod),
                   None if m.linked == -2147483648 else m.linked, m.area)
                  for m in output[:count.value]]
        return result, dict(zip(('model', 'lod', 'bounds'), skipped))
    finally:
        lib.dtex_free(output)


class WorldAssetIndex:
    def __init__(self, main, interior):
        self.lib = _library(os.environ['GTASADS_NATIVE_LIB'])
        def table(entries):
            return (IMGEntry * len(entries))(
                *(IMGEntry(name.encode('latin1'), offset, size)
                  for name, (offset, size) in sorted(entries.items())))
        self.main = table(main)
        self.interior = table(interior)

    def resolve(self, name):
        entry = IMGEntry()
        pack = ctypes.c_int()
        if not self.lib.worldplan_resolve(
                name.encode('latin1'), self.main, len(self.main),
                self.interior, len(self.interior),
                ctypes.byref(entry), ctypes.byref(pack)):
            return None
        return (entry.offset, entry.size), pack.value


def parse_world_dat(data):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    files = ctypes.POINTER(WorldPlanFile)()
    count = ctypes.c_size_t()
    if not lib.worldplan_parse_dat(data, len(data), ctypes.byref(files),
                                   ctypes.byref(count)):
        raise ValueError('native GTA data manifest parsing failed')
    try:
        paths = [(entry.kind, entry.path.decode('latin1'))
                 for entry in files[:count.value]]
        return ([path for kind, path in paths if kind == 0],
                [path for kind, path in paths if kind == 1])
    finally:
        lib.dtex_free(files)


def plan_world_sectors(instances, sector):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    native = (WorldPlanInstance * len(instances))(
        *(WorldPlanInstance(mid, linked if linked is not None else -2147483648,
                            (ctypes.c_double * 3)(x, y, z),
                            (ctypes.c_double * 9)(*rot), is_lod, area)
          for mid, x, y, z, rot, is_lod, linked, area in instances))
    ox, oy = ctypes.c_int16(), ctypes.c_int16()
    width, height = ctypes.c_uint16(), ctypes.c_uint16()
    counts = ctypes.POINTER(ctypes.c_uint16)()
    order = ctypes.POINTER(ctypes.c_size_t)()
    if not lib.worldplan_sectors(native, len(instances), sector,
                                 ctypes.byref(ox), ctypes.byref(oy),
                                 ctypes.byref(width), ctypes.byref(height),
                                 ctypes.byref(counts), ctypes.byref(order)):
        raise ValueError('native world sector planning failed')
    try:
        cells = []
        offset = 0
        for n in counts[:width.value * height.value]:
            cells.append([instances[order[i]] for i in range(offset, offset + n)])
            offset += n
        return ox.value, oy.value, width.value, height.value, cells
    finally:
        lib.dtex_free(counts)
        lib.dtex_free(order)


def plan_world_tables(ids, objects, parents, index):
    lib = index.lib
    mids = (ctypes.c_int32 * len(ids))(*ids)
    definitions = (WorldPlanObject * len(objects))(
        *(WorldPlanObject(mid, name.encode('latin1'), txd.encode('latin1'),
                           distance, flags, on, off)
          for mid, (name, txd, distance, flags, on, off) in sorted(objects.items())))
    links = (WorldPlanParent * len(parents))(
        *(WorldPlanParent(name.encode('latin1'), parent.encode('latin1'))
          for name, parent in sorted(parents.items())))
    models = ctypes.POINTER(WorldPlanModelEntry)()
    txds = ctypes.POINTER(WorldPlanTXDEntry)()
    count = ctypes.c_size_t()
    if not lib.worldplan_tables(
            mids, len(ids), definitions, len(definitions), links, len(links),
            index.main, len(index.main), index.interior, len(index.interior),
            ctypes.byref(models), ctypes.byref(txds), ctypes.byref(count)):
        raise ValueError('native world model and texture tables failed')
    try:
        planned = [(m.name.decode('latin1'), m.txd, m.draw_distance,
                    m.flags, m.on, m.off, m.offset, m.size, m.pack)
                   for m in models[:len(ids)]]
        textures = [(t.name.decode('latin1'), t.parent, t.pack)
                    for t in txds[:count.value]]
        return planned, textures
    finally:
        lib.dtex_free(models)
        lib.dtex_free(txds)


def parse_ipl(data):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    inp = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    result = ctypes.POINTER(IPLInstance)()
    count = ctypes.c_size_t()
    if not lib.ipl_parse_binary(inp, len(data), ctypes.byref(result), ctypes.byref(count)):
        raise ValueError('native IPL parsing failed')
    try:
        return [(entry.model, *entry.transform, entry.area, entry.lod)
                for entry in result[:count.value]]
    finally:
        lib.dtex_free(result)


def parse_enex(data, instances):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    placed = (IPLInstance * len(instances))(
        *(IPLInstance(mid, (ctypes.c_double * 7)(x, y, z, qx, qy, qz, qw),
                      area, lod)
          for mid, x, y, z, qx, qy, qz, qw, area, lod in instances))
    entries = ctypes.POINTER(EnexEntry)()
    count = ctypes.c_size_t()
    if not lib.enex_parse(data, len(data), placed, len(placed),
                           ctypes.byref(entries), ctypes.byref(count)):
        raise ValueError('native IPL enex parsing failed')
    try:
        return [(tuple(e.values), e.flags, e.area, e.name.decode('latin1'),
                 e.sky, e.peds, e.on, e.off, e.source_area)
                for e in entries[:count.value]]
    finally:
        lib.dtex_free(entries)


def encode_enex(records):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    entries = (EnexEntry * len(records))(
        *(EnexEntry((ctypes.c_double * 11)(*values), flags, area, sky, peds,
                    on, off, name.encode('latin1'), src)
          for values, flags, area, name, sky, peds, on, off, src in records))
    output = ctypes.POINTER(ctypes.c_uint8)()
    length, exterior, linked = ctypes.c_size_t(), ctypes.c_uint32(), ctypes.c_uint32()
    if not lib.enex_pack(entries, len(records), ctypes.byref(output),
                          ctypes.byref(length), ctypes.byref(exterior),
                          ctypes.byref(linked)):
        raise ValueError('native DENX encoding failed')
    try:
        return bytes(output[:length.value]), {
            'entries': len(records), 'exterior': exterior.value, 'linked': linked.value}
    finally:
        lib.dtex_free(output)


def parse_text_ipl(data):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    result = ctypes.POINTER(IPLInstance)()
    count = ctypes.c_size_t()
    if not lib.ipl_parse_text(data, len(data), ctypes.byref(result), ctypes.byref(count)):
        raise ValueError('native text IPL parsing failed')
    try:
        return [(entry.model, *entry.transform, entry.area, entry.lod)
                for entry in result[:count.value]]
    finally:
        lib.dtex_free(result)


def parse_ide(data):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    objects, parents = ctypes.POINTER(IDEObject)(), ctypes.POINTER(IDEParent)()
    n_objects, n_parents = ctypes.c_size_t(), ctypes.c_size_t()
    if not lib.ide_parse(data, len(data), ctypes.byref(objects), ctypes.byref(n_objects),
                         ctypes.byref(parents), ctypes.byref(n_parents)):
        raise ValueError('native IDE parsing failed')
    try:
        return ([(item.id, item.name.decode('latin1'), item.txd.decode('latin1'),
                  item.draw_distance, item.flags, item.on, item.off)
                 for item in objects[:n_objects.value]],
                [(item.name.decode('latin1'), item.parent.decode('latin1'))
                 for item in parents[:n_parents.value]])
    finally:
        lib.dtex_free(objects)
        lib.dtex_free(parents)


def rotate_ipl(x, y, z, w):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    out = (ctypes.c_double * 9)()
    lib.ipl_rotation(x, y, z, w, out)
    return tuple(out)


def encode_sound(pcm, rate, params, loop, headroom):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    samples = (ctypes.c_int16 * len(pcm))(*pcm)
    checkpoints = params.get('checkpoints')
    opts = DSNDOptions(rate, params['sample_rate'],
                       {'pcm8': 0, 'pcm16': 1, 'adpcm': 2}[params['format']],
                       checkpoints is not None, params['volume'],
                       params['max_seconds'], loop, headroom)
    output = ctypes.POINTER(ctypes.c_uint8)()
    length = ctypes.c_size_t()
    saved = ctypes.POINTER(ctypes.c_uint8)()
    saved_length = ctypes.c_size_t()
    if not lib.dsnd_encode(samples, len(pcm), ctypes.byref(opts),
                           ctypes.byref(output), ctypes.byref(length),
                           ctypes.byref(saved), ctypes.byref(saved_length)):
        raise ValueError('native sound conversion failed')
    try:
        result = ctypes.string_at(output, length.value)
        if checkpoints is not None:
            checkpoints.extend(ctypes.string_at(saved, saved_length.value))
            if len(checkpoints) != saved_length.value:
                result = result[:length.value - saved_length.value] + bytes(checkpoints)
        return result
    finally:
        lib.dtex_free(output)
        lib.dtex_free(saved)


def encode_bank(raw, params):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    inp = (ctypes.c_uint8 * len(raw)).from_buffer_copy(raw)
    opts = DSNDOptions(22050, params['sample_rate'],
                       {'pcm8': 0, 'pcm16': 1, 'adpcm': 2}[params['format']],
                       False, params['volume'], params['max_seconds'], -1, 0)
    result = ctypes.POINTER(ctypes.c_uint8)()
    length = ctypes.c_size_t()
    sounds = ctypes.c_uint16()
    if not lib.dsbk_convert(inp, len(raw), ctypes.byref(opts),
                            ctypes.byref(result), ctypes.byref(length),
                            ctypes.byref(sounds)):
        raise ValueError('native sound bank conversion failed')
    try:
        return ctypes.string_at(result, length.value), {
            'sounds': sounds.value, 'bytes_in': len(raw)}
    finally:
        lib.dtex_free(result)


def pack_streams(items, seekable=False):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    buffers = [(ctypes.c_uint8 * len(data)).from_buffer_copy(data) for _, data in items]
    entries = (DSSTEntry * len(items))(
        *(DSSTEntry(track, raw, len(data))
          for (track, data), raw in zip(items, buffers)))
    result = ctypes.POINTER(ctypes.c_uint8)()
    length = ctypes.c_size_t()
    if not lib.dsst_pack(entries, len(items), seekable,
                         ctypes.byref(result), ctypes.byref(length)):
        raise ValueError('native sound streams packing failed')
    try:
        return ctypes.string_at(result, length.value)
    finally:
        lib.dtex_free(result)


def pack_sound_banks(items):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    items = sorted(items)
    buffers = [(ctypes.c_uint8 * len(data)).from_buffer_copy(data)
               for _, data in items]
    entries = (DSFXEntry * len(items))(
        *(DSFXEntry(bank, raw, len(data))
          for (bank, data), raw in zip(items, buffers)))
    result = ctypes.POINTER(ctypes.c_uint8)()
    length = ctypes.c_size_t()
    if not lib.dsfx_pack(entries, len(items), ctypes.byref(result),
                         ctypes.byref(length)):
        raise ValueError('native sound bank package failed')
    try:
        return ctypes.string_at(result, length.value)
    finally:
        lib.dtex_free(result)


def pack_sound_lookup(names, lookup):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    encoded = [name.encode('latin1', errors='replace') for name in names]
    c_names = (ctypes.c_char_p * len(encoded))(*encoded)
    entries = (DSBLEntry * len(lookup))(
        *(DSBLEntry(pack, index) for pack, index in lookup))
    result = ctypes.POINTER(ctypes.c_uint8)()
    length = ctypes.c_size_t()
    if not lib.dsbl_pack(c_names, len(names), entries, len(lookup),
                         ctypes.byref(result), ctypes.byref(length)):
        raise ValueError('native sound bank lookup failed')
    try:
        return ctypes.string_at(result, length.value)
    finally:
        lib.dtex_free(result)


def decrypt_audio(data, offset):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    source = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    output = (ctypes.c_uint8 * len(data))()
    lib.audio_decrypt(source, len(data), offset, output)
    return bytes(output)


def parse_audio_names(data, size):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    raw = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    result = ctypes.POINTER(AudioTableName)()
    count = ctypes.c_size_t()
    if not lib.audio_table_names(raw, len(data), size, ctypes.byref(result),
                                  ctypes.byref(count)):
        raise ValueError('native audio package names parsing failed')
    try:
        return [value.name.decode('latin1') for value in result[:count.value]]
    finally:
        lib.dtex_free(result)


def parse_audio_lookup(data):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    raw = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    result = ctypes.POINTER(AudioTableLookup)()
    count = ctypes.c_size_t()
    if not lib.audio_table_entries(raw, len(data), ctypes.byref(result),
                                    ctypes.byref(count)):
        raise ValueError('native audio package lookup parsing failed')
    try:
        return [(value.pack, value.offset, value.size)
                for value in result[:count.value]]
    finally:
        lib.dtex_free(result)


def parse_ogg_vorbis_info(data):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    raw = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    info = OggVorbisInfo()
    if not lib.ogg_read_vorbis_info(raw, len(data), ctypes.byref(info)):
        raise ValueError('invalid Ogg Vorbis stream')
    return (info.sample_rate, info.samples, info.channels,
            info.small_block, info.large_block)


def aligned_audio_offset(data, start, expected):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    source = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    result = ctypes.c_uint64()
    if not lib.audio_aligned_offset(source, len(data), start, expected,
                                    ctypes.byref(result)):
        return None
    return result.value


def encode_vehicles(ide, hand, bike, boat, fly, table, cars, limits, surfaces,
                    water, tracks):
    from .dsfmt import name as ds_name
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    models = []
    for v in ide:
        h = hand.get(v['handling']) or hand.get('LANDSTAL') or next(iter(hand.values()))
        colors = cars.get(v['model'], [(1, 1)])
        palette = []
        for i in range(8):
            a, b = colors[i] if i < len(colors) else (0, 0)
            palette.extend((min(255, a), min(255, b)))
        values = (h['mass'], h['turnMass'], h['drag'], *h['com'],
                  h['tractionMult'], h['tractionLoss'], h['tractionBias'],
                  h['maxVel'], h['maxFlat'], h['maxRev'],
                  h['engineAccel'], h['engineInertia'],
                  h['brakeDecel'], h['brakeBias'], h['steeringLock'],
                  h['suspForce'], h['suspDamp'], h['suspHighSpd'],
                  h['suspUpper'], h['suspLower'], h['suspBias'], h['antiDive'],
                  h['seatOffset'], h['damageMult'])
        hid = v['handling']
        special = bike.get(hid) or boat.get(hid) or []
        flight = fly.get(hid) or []
        models.append(VehicleModel(
            ds_name(v['model'], 24), ds_name(v['txd'], 24),
            ds_name(v['gxt'], 8), ds_name(v['anims'], 16),
            v['id'], v['type'], len(colors), v['flags'],
            (ctypes.c_uint8 * 16)(*palette), v['wheelF'], v['wheelR'],
            (ctypes.c_double * 26)(*values), h['gears'], ord(h['drive']),
            ord(h['engine']), h['abs'], h['modelFlags'] & 0xFFFFFFFF,
            h['handlingFlags'] & 0xFFFFFFFF, max(0, h['value']),
            h['frontLights'], h['rearLights'], h['animGroup'], h['submerged'] & 0xFF,
            1 if hid in bike else 2 if hid in boat else 0,
            bool(flight), v['steer'],
            (ctypes.c_double * 15)(*(special + [0.0] * (15 - len(special)))),
            (ctypes.c_double * 21)(*(flight or [0.0] * 21))))
    model_array = (VehicleModel * len(models))(*models)
    rgb = (ctypes.c_uint8 * (len(table) * 4))(
        *(channel for r, g, b in table for channel in (r, g, b, 0)))
    surf = (ctypes.c_uint8 * (len(surfaces) * 4))(
        *(channel & 255 for g, wet, sk in surfaces
          for channel in (g, wet, sk, 0)))
    adhesion = (ctypes.c_double * 36)(*(value for row in limits for value in row))
    polygons = (VehicleWater * len(water))(
        *(VehicleWater((VehicleVec3 * 4)(
            *(VehicleVec3(*v) for v in vertices + [vertices[-1]] * (4 - len(vertices)))),
            len(vertices), flags) for vertices, flags in water))
    nodes = [(VehicleVec3 * len(track))(*(VehicleVec3(*v) for v in track))
             for track in tracks]
    track_array = (VehicleTrack * len(nodes))(
        *(VehicleTrack(raw, len(raw)) for raw in nodes))
    input = VehicleInput(model_array, len(models), rgb, len(table), surf, len(surfaces),
                         adhesion, polygons, len(water), track_array, len(nodes))
    result = ctypes.POINTER(ctypes.c_uint8)()
    length = ctypes.c_size_t()
    if not lib.vehpack_encode(ctypes.byref(input), ctypes.byref(result),
                              ctypes.byref(length)):
        raise ValueError('native vehicle conversion failed')
    try:
        return ctypes.string_at(result, length.value)
    finally:
        lib.dtex_free(result)


def convert_handling(data):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    record = VehicleHandling(data['mass'], data['drag'], data['engineAccel'],
                             data['maxVel'], data['brakeDecel'],
                             data['damageMult'], 0, 0,
                             data['handlingFlags'], ord(data['drive']))
    if not lib.vehpack_convert_handling(ctypes.byref(record)):
        raise ValueError('native handling conversion failed')
    data.update(engineAccel=record.engine_accel, maxVel=record.max_vel,
                brakeDecel=record.brake_decel, damageMult=record.damage_mult,
                maxFlat=record.max_flat, maxRev=record.max_reverse)


def parse_vehicle_ide(data):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    result = ctypes.POINTER(VehicleIDE)()
    count = ctypes.c_size_t()
    if not lib.vehparse_ide(data, len(data), ctypes.byref(result),
                            ctypes.byref(count)):
        raise ValueError('native vehicle IDE parsing failed')
    try:
        return [dict(id=v.id, model=v.model.decode('latin1'),
                     txd=v.txd.decode('latin1'), handling=v.handling.decode('latin1'),
                     gxt=v.gxt.decode('latin1'), anims=v.anims.decode('latin1'),
                     type=v.type, flags=v.flags, wheelF=v.wheel_front,
                     wheelR=v.wheel_rear, steer=v.steer)
                for v in result[:count.value]]
    finally:
        lib.dtex_free(result)


def parse_vehicle_handling(data):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    entries = ctypes.POINTER(VehicleHandlingEntry)()
    count = ctypes.c_size_t()
    if not lib.vehparse_handling(data, len(data), ctypes.byref(entries),
                                 ctypes.byref(count)):
        raise ValueError('native handling.cfg parsing failed')
    try:
        out = {}
        for h in entries[:count.value]:
            out[h.name.decode('latin1')] = dict(
                mass=h.mass, turnMass=h.turn_mass, drag=h.drag, com=tuple(h.com),
                submerged=h.submerged, tractionMult=h.traction_mult,
                tractionLoss=h.traction_loss, tractionBias=h.traction_bias,
                gears=h.gears, maxVel=h.max_vel, engineAccel=h.engine_accel,
                engineInertia=h.engine_inertia, drive=chr(h.drive), engine=chr(h.engine),
                brakeDecel=h.brake_decel, brakeBias=h.brake_bias, abs=h.abs,
                steeringLock=h.steering_lock,
                suspForce=h.suspension[0], suspDamp=h.suspension[1],
                suspHighSpd=h.suspension[2], suspUpper=h.suspension[3],
                suspLower=h.suspension[4], suspBias=h.suspension[5],
                antiDive=h.suspension[6], seatOffset=h.seat_offset,
                damageMult=h.damage_mult, value=h.value,
                modelFlags=h.model_flags, handlingFlags=h.handling_flags,
                frontLights=h.front_lights, rearLights=h.rear_lights,
                animGroup=h.anim_group, maxFlat=h.max_flat, maxRev=h.max_reverse)
        return out
    finally:
        lib.dtex_free(entries)


def parse_vehicle_water(data):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    result = ctypes.POINTER(VehicleWaterEntry)()
    count = ctypes.c_size_t()
    if not lib.vehparse_water(data, len(data), ctypes.byref(result),
                              ctypes.byref(count)):
        raise ValueError('native water.dat parsing failed')
    try:
        return [([(v.x, v.y, v.z) for v in row.vertices[:row.count]], row.flags)
                for row in result[:count.value]]
    finally:
        lib.dtex_free(result)


def parse_vehicle_track(data):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    result = ctypes.POINTER(VehicleVec3)()
    count = ctypes.c_size_t()
    if not lib.vehparse_track(data, len(data), ctypes.byref(result),
                              ctypes.byref(count)):
        raise ValueError('native train track parsing failed')
    try:
        return [(v.x, v.y, v.z) for v in result[:count.value]]
    finally:
        lib.dtex_free(result)


def parse_vehicle_special(data):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    result = ctypes.POINTER(VehicleSpecialEntry)()
    count = ctypes.c_size_t()
    if not lib.vehparse_special(data, len(data), ctypes.byref(result),
                                 ctypes.byref(count)):
        raise ValueError('native special handling parsing failed')
    try:
        bike, boat, fly = {}, {}, {}
        for entry in result[:count.value]:
            kind = entry.kind
            target = bike if kind == 1 else boat if kind == 2 else fly
            target[entry.name.decode('latin1')] = list(
                entry.values[:15 if kind == 1 else 14 if kind == 2 else 21])
        return bike, boat, fly
    finally:
        lib.dtex_free(result)


def parse_vehicle_carcols(data):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    colors, cars = ctypes.POINTER(VehicleColor)(), ctypes.POINTER(VehicleCarColor)()
    color_count, car_count = ctypes.c_size_t(), ctypes.c_size_t()
    if not lib.vehparse_carcols(data, len(data), ctypes.byref(colors),
                                ctypes.byref(color_count), ctypes.byref(cars),
                                ctypes.byref(car_count)):
        raise ValueError('native vehicle colours parsing failed')
    try:
        palette = [(v.r, v.g, v.b) for v in colors[:color_count.value]]
        selected = {v.name.decode('latin1'):
                    [tuple(pair) for pair in v.pairs[:v.count]]
                    for v in cars[:car_count.value]}
        return palette, selected
    finally:
        lib.dtex_free(colors)
        lib.dtex_free(cars)


def parse_vehicle_surfaces(limits, info):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    adhesion = (ctypes.c_double * 36)()
    result = ctypes.POINTER(VehicleSurface)()
    count = ctypes.c_size_t()
    if not lib.vehparse_surfaces(limits, len(limits), info, len(info),
                                 adhesion, ctypes.byref(result), ctypes.byref(count)):
        raise ValueError('native vehicle surface parsing failed')
    try:
        return ([list(adhesion[row * 6:row * 6 + 6]) for row in range(6)],
                [(v.group, v.wet, v.skid) for v in result[:count.value]])
    finally:
        lib.dtex_free(result)


def parse_ped_paths(data, bounds, vehicles=False):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    raw = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    limits = (ctypes.c_double * 4)(*bounds)
    result = ctypes.POINTER(PedPathNode)()
    length = ctypes.c_size_t()
    if not lib.pedpath_parse(raw, len(data), limits, vehicles,
                             ctypes.byref(result), ctypes.byref(length)):
        raise ValueError('native pedestrian path conversion failed')
    try:
        return [(v.index, v.x, v.y, v.z, v.width, v.flags,
                 [(link.area, link.node) for link in v.links[:v.link_count]])
                for v in result[:length.value]]
    finally:
        lib.dtex_free(result)


def pack_peds(parts, models, stats, anims, groups, zones, nodes, links,
              road_nodes, road_links, gx0, gy0, gw, gh, cell):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    buffers = [(ctypes.c_uint8 * len(part)).from_buffer_copy(part) for part in parts]
    input = PedPackInput(models, stats, anims, groups, zones, nodes, links,
                         road_nodes, road_links, gx0, gy0, gw, gh, cell,
                         (ctypes.POINTER(ctypes.c_uint8) * 14)(*buffers),
                         (ctypes.c_size_t * 14)(*(len(part) for part in parts)))
    result = ctypes.POINTER(ctypes.c_uint8)()
    length = ctypes.c_size_t()
    if not lib.pedpack_encode(ctypes.byref(input), ctypes.byref(result),
                              ctypes.byref(length)):
        raise ValueError('native pedestrian packing failed')
    try:
        return ctypes.string_at(result, length.value)
    finally:
        lib.dtex_free(result)


def scan_zone_script(data):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    source = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    records = ctypes.POINTER(PedSCMZone)()
    count = ctypes.c_size_t()
    start, end = ctypes.c_int32(), ctypes.c_int32()
    if not lib.pedscm_scan(source, len(data), ctypes.byref(records),
                           ctypes.byref(count), ctypes.byref(start),
                           ctypes.byref(end)):
        raise ValueError('native SCM zone scan failed')
    try:
        return ({zone.label.decode('latin1'): {
            'pop': zone.population if zone.has_population else None,
            'races': zone.races if zone.races >= 0 else None,
            'dealer': zone.dealer, 'gang': list(zone.gang)}
                 for zone in records[:count.value]}, (start.value, end.value))
    finally:
        lib.dtex_free(records)


def rescale_population(rows):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    values = (ctypes.c_int32 * (len(rows) * 24))(
        *(value for row in rows for value in row))
    if not lib.pedpop_rescale(values, len(rows)):
        raise ValueError('native population rescaling failed')
    return [list(values[i * 24:(i + 1) * 24]) for i in range(len(rows))]


def parse_ped_population(data):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    rows = ctypes.POINTER(ctypes.c_int32)()
    count = ctypes.c_size_t()
    if not lib.pedpop_parse(data, len(data), ctypes.byref(rows),
                            ctypes.byref(count)):
        raise ValueError('native population cycle parsing failed')
    try:
        return [list(rows[i * 24:(i + 1) * 24]) for i in range(count.value)]
    finally:
        lib.dtex_free(rows)


def parse_ped_text(data, kind):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    struct = {'ide': PedIDEEntry, 'stats': PedStatEntry,
              'zones': PedZoneEntry, 'animgrp': PedAnimEvent}[kind]
    result = ctypes.POINTER(struct)()
    count = ctypes.c_size_t()
    function = getattr(lib, 'pedparse_' + kind)
    if not function(data, len(data), ctypes.byref(result),
                    ctypes.byref(count)):
        raise ValueError('native pedestrian ' + kind + ' parsing failed')
    try:
        if kind == 'ide':
            return [dict(id=v.id, model=v.model.decode('latin1'),
                         txd=v.txd.decode('latin1'),
                         type=v.type.decode('latin1'),
                         stat=v.stat.decode('latin1'),
                         anim=v.anim.decode('latin1'),
                         cars=v.cars, flags=v.flags)
                    for v in result[:count.value]]
        if kind == 'stats':
            return [(v.name.decode('latin1'), list(v.values))
                    for v in result[:count.value]]
        if kind == 'animgrp':
            groups, current, clips = {}, None, []
            for event in result[:count.value]:
                if event.kind == 0:
                    current, clips = event.name.decode('latin1'), []
                elif event.kind == 1:
                    clips.append(event.name.decode('latin1'))
                else:
                    groups[current] = clips
                    current = None
            return groups
        return [(v.name.decode('latin1'), *v.bounds)
                for v in result[:count.value]]
    finally:
        lib.dtex_free(result)


def parse_ped_groups(data, by_name, count, skip_slashes=False):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    names = (PedNamedModel * len(by_name))(
        *(PedNamedModel(name.encode('latin1'), index)
          for name, index in sorted(by_name.items())))
    members = ctypes.POINTER(PedGroupMember)()
    n_members = ctypes.c_size_t()
    n_groups = ctypes.c_uint16()
    if not lib.pedparse_groups(data, len(data), names, len(names), count,
                               skip_slashes, ctypes.byref(members),
                               ctypes.byref(n_members), ctypes.byref(n_groups)):
        raise ValueError('native pedestrian group parsing failed')
    try:
        result = [[] for _ in range(count)]
        for member in members[:n_members.value]:
            result[member.group].append(member.index)
        return result, n_groups.value
    finally:
        lib.dtex_free(members)


def encode_ped_graph(records, cell_count, road=False):
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    links = [(ctypes.c_uint16 * len(row[6]))(*row[6]) for row in records]
    nodes = (PedGraphNode * len(records))(
        *(PedGraphNode(x, y, z, width, flags, cell, raw, len(indices))
          for (x, y, z, width, flags, cell, indices), raw in zip(records, links)))
    output = [ctypes.POINTER(ctypes.c_uint8)() for _ in range(3)]
    lengths = [ctypes.c_size_t() for _ in range(3)]
    count = ctypes.c_uint32()
    if not lib.pedpack_paths(nodes, len(records), cell_count, road,
                             ctypes.byref(output[0]), ctypes.byref(lengths[0]),
                             ctypes.byref(output[1]), ctypes.byref(lengths[1]),
                             ctypes.byref(output[2]), ctypes.byref(lengths[2]),
                             ctypes.byref(count)):
        raise ValueError('native pedestrian graph packing failed')
    try:
        blobs = [ctypes.string_at(ptr, n.value) for ptr, n in zip(output, lengths)]
        cells = [tuple(int.from_bytes(blobs[2][i:i + 2], 'little')
                       for i in (start, start + 2))
                 for start in range(0, lengths[2].value, 4)]
        return blobs[0], blobs[1], cells, count.value
    finally:
        for ptr in output:
            lib.dtex_free(ptr)


def encode_ped_tables(peds, stats, used_anims, all_anims, groups, cars,
                      popcycle, zones, stat_idx, anim_idx):
    from .dsfmt import name as ds_name
    from .peds import PED_TYPES, race_of
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    models = (PedTableModel * len(peds))(
        *(PedTableModel(ds_name(p['model'], 24), ds_name(p['txd'], 24),
                        p['id'], p['cars'] & 0xFFFF,
                        PED_TYPES.index(p['type']) if p['type'] in PED_TYPES else 4,
                        stat_idx.get(p['stat'], 0), anim_idx.get(p['anim'], 0),
                        race_of(p['model']), p['flags'] & 0xFF)
          for p in peds))
    stat_records = (PedTableStat * len(stats))(
        *(PedTableStat((ctypes.c_double * 10)(*values)) for _, values in stats))
    clip_type = (ctypes.c_char * 24) * 6
    anim_records = (PedTableAnim * len(used_anims))(
        *(PedTableAnim(ds_name(name, 16),
                       clip_type(*((ctypes.c_char * 24).from_buffer_copy(
                                       ds_name(clip, 24))
                                   for clip in (all_anims[name] + [''] * 6)[:6])))
          for name in used_anims))
    members = [(ctypes.c_uint16 * len(group))(*group) for group in groups]
    ped_groups = (PedTableGroup * len(groups))(
        *(PedTableGroup(raw, len(raw)) for raw in members))
    car_members = [(ctypes.c_uint16 * len(group))(*group) for group in cars]
    car_groups = (PedTableGroup * len(cars))(
        *(PedTableGroup(raw, len(raw)) for raw in car_members))
    pop = (ctypes.c_int32 * (len(popcycle) * 24))(
        *(value for row in popcycle for value in row))
    zone_records = (PedTableZone * len(zones))(
        *(PedTableZone((ctypes.c_int16 * 4)(x1, y1, x2, y2), pt, races,
                       dealer, (ctypes.c_uint8 * 10)(*gang))
          for x1, y1, x2, y2, pt, races, dealer, gang, _ in zones))
    inp = PedTableInput(models, len(models), stat_records, len(stats),
                        anim_records, len(used_anims), ped_groups, len(groups),
                        car_groups, len(cars), pop, len(popcycle),
                        zone_records, len(zones))
    output = ctypes.POINTER(ctypes.c_uint8)()
    length = ctypes.c_size_t()
    offsets = (ctypes.c_size_t * 9)()
    if not lib.pedtables_encode(ctypes.byref(inp), ctypes.byref(output),
                                ctypes.byref(length), offsets):
        raise ValueError('native pedestrian tables failed')
    try:
        data = ctypes.string_at(output, length.value)
        return tuple(data[offsets[i]:offsets[i + 1]] for i in range(8))
    finally:
        lib.dtex_free(output)


def convert_model(data, params):
    if (params['decimate'] not in ('cluster', 'qem', 'wedge') or
            any('[' in pattern for pattern in params['drop_frames'])):
        return None
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    inp = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    dropped = (ctypes.c_char_p * len(params['drop_frames']))(
        *(pattern.encode('latin1', 'replace') for pattern in params['drop_frames']))
    opts = DMDLOptions(params['ratio'], params['min_area'], params['max_shift'],
                       params['min_tris'], params['max_tris'],
                       params['uvs'], params['normals'], params['vertex_colors'],
                       params['skin'], params['drop_damage'], params['drop_vlo'],
                       {'cluster': 0, 'qem': 1, 'wedge': 2}[params['decimate']],
                       params['ground_qem'], params.get('rigid_skin', False),
                       dropped, len(dropped))
    result = ctypes.POINTER(ctypes.c_uint8)()
    length, before, after = ctypes.c_size_t(), ctypes.c_uint32(), ctypes.c_uint32()
    if not lib.dmdl_convert(inp, len(data), ctypes.byref(opts),
                            ctypes.byref(result), ctypes.byref(length),
                            ctypes.byref(before), ctypes.byref(after)):
        return None
    try:
        return ctypes.string_at(result, length.value), {
            'tris_in': before.value, 'tris_out': after.value}
    finally:
        lib.dtex_free(result)


def encode_geometry(vertices, materials, indices, use_uv, use_n, use_c, use_b):
    from .dsfmt import name as ds_name
    lib = _library(os.environ['GTASADS_NATIVE_LIB'])
    positions = (ctypes.c_double * (len(vertices) * 3))(
        *(c for _, attrs in vertices for c in attrs[0]))
    uvs = ((ctypes.c_double * (len(vertices) * 2))(
        *(c for _, attrs in vertices for c in attrs[1])) if use_uv else None)
    normals = ((ctypes.c_double * (len(vertices) * 3))(
        *(c for _, attrs in vertices for c in attrs[4])) if use_n else None)
    colors = ((ctypes.c_uint8 * (len(vertices) * 4))(
        *(c for _, attrs in vertices for c in attrs[2])) if use_c else None)
    bones = ((ctypes.c_uint8 * len(vertices))(
        *(attrs[3] for _, attrs in vertices)) if use_b else None)
    idx = (ctypes.c_uint16 * len(indices))(*indices)
    mats = (GeometryMaterial * len(materials))(
        *(GeometryMaterial(ds_name(texture, 32), color, alpha, 0, first, count)
          for texture, color, alpha, first, count in materials))
    result = ctypes.POINTER(ctypes.c_uint8)()
    length = ctypes.c_size_t()
    if not lib.geometry_encode(positions, uvs, normals, colors, bones, idx, len(vertices),
                               len(indices), mats, len(materials),
                               ctypes.byref(result), ctypes.byref(length)):
        raise ValueError('native DMDL geometry encoding failed')
    try:
        return ctypes.string_at(result, length.value)
    finally:
        lib.dtex_free(result)
