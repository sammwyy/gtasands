CC ?= cc
EMCC ?= emcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror
WASM_FLAGS = -O2 -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORTED_RUNTIME_METHODS='["FS","callMain"]' -sINVOKE_RUN=0 -sEXIT_RUNTIME=0 -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=1048576
TEXTURE = tools/portable/dtex.c tools/portable/txd.c tools/portable/texture.c tools/portable/radar.c
LIB_SOURCES = $(TEXTURE) tools/portable/dspk.c tools/portable/img.c tools/portable/adpcm.c tools/portable/dsnd.c tools/portable/resample.c tools/portable/dvid.c tools/portable/mpeg_video.c tools/portable/mpeg_audio.c tools/portable/movie_palette.c tools/portable/mpeg_movie.c tools/portable/audio_pack.c tools/portable/audio_tables.c tools/portable/ogg_info.c tools/portable/ogg_decode.c tools/portable/audio_stream.c tools/portable/points.c tools/portable/enex.c tools/portable/col.c tools/portable/cluster.c tools/portable/ifp.c tools/portable/dff.c tools/portable/dff_mesh.c tools/portable/geometry.c tools/portable/dmdl.c tools/portable/qem.c tools/portable/worldcol.c tools/portable/worldcol_archive.c tools/portable/worldpack.c tools/portable/worldplan.c tools/portable/worldbuild.c tools/portable/vehpack.c tools/portable/vehparse.c tools/portable/vehbuild.c tools/portable/pedpath.c tools/portable/pedpack.c tools/portable/pedparse.c tools/portable/pedtables.c tools/portable/pedscm.c tools/portable/pedbuild.c tools/portable/ipl.c tools/portable/ide.c
LIB_SOURCES += tools/portable/enexbuild.c
LIB_HEADERS = $(filter-out tools/portable/worldcol_archive.h,$(LIB_SOURCES:.c=.h))
WASM_LIB_FLAGS = -O2 -sMODULARIZE=1 -sEXPORT_ES6=1 --no-entry -sALLOW_MEMORY_GROWTH=1 -sALLOW_TABLE_GROWTH=1 -sSTACK_SIZE=1048576 -sEXPORTED_RUNTIME_METHODS='["FS","addFunction","removeFunction","HEAPU8","HEAP16","HEAPU32"]'
WASM_EXPORTS = -sEXPORTED_FUNCTIONS='["_malloc","_free","_dtex_free","_dtex_encode","_texture_convert_txd","_texture_inspect","_img_index_size","_img_parse_index","_img_free","_radar_convert","_adpcm_encode","_dsnd_encode","_dsnd_encode_simple","_resample_pcm16","_dsbk_convert","_dsst_pack","_dvid_pack","_mpeg_video_open","_mpeg_video_next","_mpeg_video_close","_mpeg_audio_open","_mpeg_audio_next","_mpeg_audio_close","_mpeg_movie_convert","_audio_stream_convert","_dsfx_pack","_dsbl_pack","_audio_decrypt","_audio_decrypt_parts","_audio_aligned_offset","_audio_table_names","_audio_table_entries","_ogg_read_vorbis_info","_ogg_read_stream_info","_ogg_decode_vorbis","_points_from_denx","_enex_parse","_enex_pack","_enexbuild_convert","_col_convert","_ifp_convert","_dff_radius","_dff_parse_model","_dff_free_model","_geometry_encode","_dmdl_convert","_cluster_decimate","_cluster_decimate_limited","_cluster_free","_qem_decimate","_wedge_decimate","_worldcol_pack","_worldcol_parse_archive","_worldcol_free_archive","_worldpack_encode","_worldplan_instances","_worldplan_resolve","_worldplan_parse_dat","_worldplan_sectors","_worldplan_tables","_worldbuild_convert","_vehpack_encode","_vehpack_convert_handling","_vehparse_ide","_vehparse_handling","_vehparse_water","_vehparse_track","_vehparse_special","_vehparse_carcols","_vehparse_surfaces","_vehbuild_convert","_pedpath_parse","_pedpack_encode","_pedpack_paths","_pedpop_rescale","_pedpop_parse","_pedparse_ide","_pedparse_stats","_pedparse_zones","_pedparse_animgrp","_pedparse_groups","_pedtables_encode","_pedscm_scan","_pedbuild_convert","_ipl_parse_binary","_ipl_parse_text","_ipl_rotation","_ide_parse","_dspk_write"]'

.PHONY: native wasm mirror-roms
mirror-roms:
	python3 tools/mirror_rom_release.py

tools/portable/resample_coeffs.h: tools/generate_resample_table.py
	python3 tools/generate_resample_table.py

native: build/dspk_cli build/texture_cli build/img_cli build/adpcm_cli build/audio_cli build/audio_pack_cli build/audio_stream_cli build/ogg_cli build/dvid_cli build/mpeg_video_cli build/mpeg_audio_cli build/mpeg_movie_cli build/worldbuild_cli build/pedbuild_cli build/vehbuild_cli build/enexbuild_cli build/veh_cli build/enex_cli build/ifp_cli build/dff_radius_cli build/dmdl_cli build/libgtasads_assets.so
wasm: web/resources.json web/dspk.mjs web/texture.mjs web/adpcm.mjs web/audio.mjs web/audio_pack.mjs web/audio_stream.mjs web/ogg.mjs web/dvid.mjs web/mpeg_video.mjs web/mpeg_audio.mjs web/mpeg_movie.mjs web/veh.mjs web/enex.mjs web/ifp.mjs web/dff_radius.mjs web/dmdl.mjs web/assets.mjs

web/resources.json: resources.toml tools/export_web_manifest.py
	python3 tools/export_web_manifest.py

build:
	mkdir -p build

build/dspk_cli: tools/portable/dspk.c tools/portable/dspk_cli.c tools/portable/dspk.h | build
	$(CC) $(CFLAGS) $< tools/portable/dspk_cli.c -o $@

build/texture_cli: $(TEXTURE) tools/portable/texture_cli.c tools/portable/dtex.h tools/portable/txd.h tools/portable/texture.h tools/portable/radar.h | build
	$(CC) $(CFLAGS) $(TEXTURE) tools/portable/texture_cli.c -o $@

build/img_cli: tools/portable/img.c tools/portable/img_cli.c tools/portable/img.h | build
	$(CC) $(CFLAGS) tools/portable/img.c tools/portable/img_cli.c -o $@

build/adpcm_cli: tools/portable/adpcm.c tools/portable/adpcm_cli.c tools/portable/adpcm.h | build
	$(CC) $(CFLAGS) tools/portable/adpcm.c tools/portable/adpcm_cli.c -o $@

build/audio_cli: tools/portable/dsnd.c tools/portable/dsnd.h tools/portable/resample.c tools/portable/resample.h tools/portable/resample_coeffs.h tools/portable/adpcm.c tools/portable/adpcm.h tools/portable/audio_cli.c | build
	$(CC) $(CFLAGS) tools/portable/dsnd.c tools/portable/resample.c tools/portable/adpcm.c tools/portable/audio_cli.c -lm -o $@

build/audio_stream_cli: tools/portable/dsnd.c tools/portable/resample.c tools/portable/resample_coeffs.h tools/portable/adpcm.c tools/portable/ogg_decode.c tools/portable/ogg_info.c tools/portable/audio_stream.c tools/portable/audio_stream_cli.c tools/portable/vendor/stb_vorbis.c | build
	$(CC) $(CFLAGS) tools/portable/dsnd.c tools/portable/resample.c tools/portable/adpcm.c tools/portable/ogg_decode.c tools/portable/ogg_info.c tools/portable/audio_stream.c tools/portable/audio_stream_cli.c -lm -o $@

build/audio_pack_cli: tools/portable/audio_pack.c tools/portable/audio_pack_cli.c tools/portable/audio_pack.h | build
	$(CC) $(CFLAGS) tools/portable/audio_pack.c tools/portable/audio_pack_cli.c -o $@

build/ogg_cli: tools/portable/ogg_decode.c tools/portable/ogg_cli.c tools/portable/ogg_decode.h tools/portable/vendor/stb_vorbis.c | build
	$(CC) $(CFLAGS) tools/portable/ogg_decode.c tools/portable/ogg_cli.c -lm -o $@

build/dvid_cli: tools/portable/dvid.c tools/portable/dvid_cli.c tools/portable/dvid.h | build
	$(CC) $(CFLAGS) tools/portable/dvid.c tools/portable/dvid_cli.c -o $@

build/mpeg_video_cli: tools/portable/mpeg_video.c tools/portable/mpeg_video_cli.c tools/portable/mpeg_video.h tools/portable/vendor/pl_mpeg.h | build
	$(CC) $(CFLAGS) tools/portable/mpeg_video.c tools/portable/mpeg_video_cli.c -lm -o $@

build/mpeg_audio_cli: tools/portable/mpeg_video.c tools/portable/mpeg_audio.c tools/portable/mpeg_audio_cli.c tools/portable/resample.c tools/portable/resample_coeffs.h tools/portable/vendor/pl_mpeg.h | build
	$(CC) $(CFLAGS) tools/portable/mpeg_video.c tools/portable/mpeg_audio.c tools/portable/mpeg_audio_cli.c tools/portable/resample.c -lm -o $@

build/mpeg_movie_cli: tools/portable/mpeg_video.c tools/portable/mpeg_audio.c tools/portable/mpeg_movie.c tools/portable/mpeg_movie_cli.c tools/portable/movie_palette.c tools/portable/dvid.c tools/portable/resample.c tools/portable/resample_coeffs.h tools/portable/vendor/pl_mpeg.h | build
	$(CC) $(CFLAGS) tools/portable/mpeg_video.c tools/portable/mpeg_audio.c tools/portable/mpeg_movie.c tools/portable/mpeg_movie_cli.c tools/portable/movie_palette.c tools/portable/dvid.c tools/portable/resample.c -lm -o $@

build/worldbuild_cli: tools/portable/worldbuild.c tools/portable/worldbuild_cli.c tools/portable/worldbuild.h tools/portable/gamefiles.c tools/portable/gamefiles.h tools/portable/dff.c tools/portable/ide.c tools/portable/img.c tools/portable/ipl.c tools/portable/worldplan.c tools/portable/worldpack.c tools/portable/worldcol.c tools/portable/worldcol_archive.c | build
	$(CC) $(CFLAGS) tools/portable/worldbuild.c tools/portable/worldbuild_cli.c tools/portable/gamefiles.c tools/portable/dff.c tools/portable/ide.c tools/portable/img.c tools/portable/ipl.c tools/portable/worldplan.c tools/portable/worldpack.c tools/portable/worldcol.c tools/portable/worldcol_archive.c -lm -o $@

build/pedbuild_cli: tools/portable/pedbuild.c tools/portable/pedbuild_cli.c tools/portable/pedbuild.h tools/portable/gamefiles.c tools/portable/gamefiles.h tools/portable/pedpack.c tools/portable/pedparse.c tools/portable/pedpath.c tools/portable/pedscm.c tools/portable/pedtables.c tools/portable/vehparse.c tools/portable/vehpack.c | build
	$(CC) $(CFLAGS) tools/portable/pedbuild.c tools/portable/pedbuild_cli.c tools/portable/gamefiles.c tools/portable/pedpack.c tools/portable/pedparse.c tools/portable/pedpath.c tools/portable/pedscm.c tools/portable/pedtables.c tools/portable/vehparse.c tools/portable/vehpack.c -lm -o $@

build/vehbuild_cli: tools/portable/vehbuild.c tools/portable/vehbuild_cli.c tools/portable/vehbuild.h tools/portable/gamefiles.c tools/portable/gamefiles.h tools/portable/vehparse.c tools/portable/vehpack.c | build
	$(CC) $(CFLAGS) tools/portable/vehbuild.c tools/portable/vehbuild_cli.c tools/portable/gamefiles.c tools/portable/vehparse.c tools/portable/vehpack.c -lm -o $@

build/enexbuild_cli: tools/portable/enexbuild.c tools/portable/enexbuild_cli.c tools/portable/enexbuild.h tools/portable/gamefiles.c tools/portable/gamefiles.h tools/portable/enex.c tools/portable/ipl.c tools/portable/worldplan.c tools/portable/img.c | build
	$(CC) $(CFLAGS) tools/portable/enexbuild.c tools/portable/enexbuild_cli.c tools/portable/gamefiles.c tools/portable/enex.c tools/portable/ipl.c tools/portable/worldplan.c tools/portable/img.c -lm -o $@

build/veh_cli: tools/portable/vehpack.c tools/portable/vehparse.c tools/portable/veh_cli.c tools/portable/vehpack.h tools/portable/vehparse.h | build
	$(CC) $(CFLAGS) tools/portable/vehpack.c tools/portable/vehparse.c tools/portable/veh_cli.c -lm -o $@

build/enex_cli: tools/portable/enex.c tools/portable/ipl.c tools/portable/enex_cli.c tools/portable/enex.h tools/portable/ipl.h | build
	$(CC) $(CFLAGS) tools/portable/enex.c tools/portable/ipl.c tools/portable/enex_cli.c -lm -o $@

build/ifp_cli: tools/portable/ifp.c tools/portable/ifp_cli.c tools/portable/ifp.h | build
	$(CC) $(CFLAGS) tools/portable/ifp.c tools/portable/ifp_cli.c -lm -o $@

build/dff_radius_cli: tools/portable/dff.c tools/portable/dff_radius_cli.c tools/portable/dff.h | build
	$(CC) $(CFLAGS) tools/portable/dff.c tools/portable/dff_radius_cli.c -lm -o $@

build/dmdl_cli: tools/portable/dmdl.c tools/portable/dmdl_cli.c tools/portable/dmdl.h tools/portable/dff_mesh.c tools/portable/geometry.c tools/portable/cluster.c tools/portable/qem.c | build
	$(CC) $(CFLAGS) tools/portable/dmdl.c tools/portable/dmdl_cli.c tools/portable/dff_mesh.c tools/portable/geometry.c tools/portable/cluster.c tools/portable/qem.c -lm -o $@

build/libgtasads_assets.so: $(LIB_SOURCES) $(LIB_HEADERS) tools/portable/Makefile tools/portable/resample_coeffs.h tools/portable/vendor/stb_vorbis.c tools/portable/vendor/pl_mpeg.h | build
	$(CC) $(CFLAGS) -shared -fPIC $(LIB_SOURCES) -lm -o $@

web/dspk.mjs: tools/portable/dspk.c tools/portable/dspk_cli.c tools/portable/dspk.h
	$(EMCC) $(WASM_FLAGS) tools/portable/dspk.c tools/portable/dspk_cli.c -o $@

web/texture.mjs: $(TEXTURE) tools/portable/texture_cli.c tools/portable/dtex.h tools/portable/txd.h tools/portable/texture.h tools/portable/radar.h
	$(EMCC) $(WASM_FLAGS) $(TEXTURE) tools/portable/texture_cli.c -o $@

web/adpcm.mjs: tools/portable/adpcm.c tools/portable/adpcm_cli.c tools/portable/adpcm.h
	$(EMCC) $(WASM_FLAGS) tools/portable/adpcm.c tools/portable/adpcm_cli.c -o $@

web/audio.mjs: tools/portable/dsnd.c tools/portable/dsnd.h tools/portable/resample.c tools/portable/resample.h tools/portable/resample_coeffs.h tools/portable/adpcm.c tools/portable/adpcm.h tools/portable/audio_cli.c
	$(EMCC) $(WASM_FLAGS) tools/portable/dsnd.c tools/portable/resample.c tools/portable/adpcm.c tools/portable/audio_cli.c -lm -o $@

web/audio_stream.mjs: tools/portable/dsnd.c tools/portable/resample.c tools/portable/resample_coeffs.h tools/portable/adpcm.c tools/portable/ogg_decode.c tools/portable/ogg_info.c tools/portable/audio_stream.c tools/portable/audio_stream_cli.c tools/portable/vendor/stb_vorbis.c
	$(EMCC) $(WASM_FLAGS) tools/portable/dsnd.c tools/portable/resample.c tools/portable/adpcm.c tools/portable/ogg_decode.c tools/portable/ogg_info.c tools/portable/audio_stream.c tools/portable/audio_stream_cli.c -lm -o $@

web/audio_pack.mjs: tools/portable/audio_pack.c tools/portable/audio_pack_cli.c tools/portable/audio_pack.h
	$(EMCC) $(WASM_FLAGS) tools/portable/audio_pack.c tools/portable/audio_pack_cli.c -o $@

web/ogg.mjs: tools/portable/ogg_decode.c tools/portable/ogg_cli.c tools/portable/ogg_decode.h tools/portable/vendor/stb_vorbis.c
	$(EMCC) $(WASM_FLAGS) tools/portable/ogg_decode.c tools/portable/ogg_cli.c -lm -o $@

web/dvid.mjs: tools/portable/dvid.c tools/portable/dvid_cli.c tools/portable/dvid.h
	$(EMCC) $(WASM_FLAGS) tools/portable/dvid.c tools/portable/dvid_cli.c -o $@

web/mpeg_video.mjs: tools/portable/mpeg_video.c tools/portable/mpeg_video_cli.c tools/portable/mpeg_video.h tools/portable/vendor/pl_mpeg.h
	$(EMCC) $(WASM_FLAGS) tools/portable/mpeg_video.c tools/portable/mpeg_video_cli.c -lm -o $@

web/mpeg_audio.mjs: tools/portable/mpeg_video.c tools/portable/mpeg_audio.c tools/portable/mpeg_audio_cli.c tools/portable/resample.c tools/portable/resample_coeffs.h tools/portable/vendor/pl_mpeg.h
	$(EMCC) $(WASM_FLAGS) tools/portable/mpeg_video.c tools/portable/mpeg_audio.c tools/portable/mpeg_audio_cli.c tools/portable/resample.c -lm -o $@

web/mpeg_movie.mjs: tools/portable/mpeg_video.c tools/portable/mpeg_audio.c tools/portable/mpeg_movie.c tools/portable/mpeg_movie_cli.c tools/portable/movie_palette.c tools/portable/dvid.c tools/portable/resample.c tools/portable/resample_coeffs.h tools/portable/vendor/pl_mpeg.h
	$(EMCC) $(WASM_FLAGS) tools/portable/mpeg_video.c tools/portable/mpeg_audio.c tools/portable/mpeg_movie.c tools/portable/mpeg_movie_cli.c tools/portable/movie_palette.c tools/portable/dvid.c tools/portable/resample.c -lm -o $@

web/veh.mjs: tools/portable/vehpack.c tools/portable/vehparse.c tools/portable/veh_cli.c tools/portable/vehpack.h tools/portable/vehparse.h
	$(EMCC) $(WASM_FLAGS) tools/portable/vehpack.c tools/portable/vehparse.c tools/portable/veh_cli.c -lm -o $@

web/enex.mjs: tools/portable/enex.c tools/portable/ipl.c tools/portable/enex_cli.c tools/portable/enex.h tools/portable/ipl.h
	$(EMCC) $(WASM_FLAGS) tools/portable/enex.c tools/portable/ipl.c tools/portable/enex_cli.c -lm -o $@

web/ifp.mjs: tools/portable/ifp.c tools/portable/ifp_cli.c tools/portable/ifp.h
	$(EMCC) $(WASM_FLAGS) tools/portable/ifp.c tools/portable/ifp_cli.c -lm -o $@

web/dff_radius.mjs: tools/portable/dff.c tools/portable/dff_radius_cli.c tools/portable/dff.h
	$(EMCC) $(WASM_FLAGS) tools/portable/dff.c tools/portable/dff_radius_cli.c -lm -o $@

web/dmdl.mjs: tools/portable/dmdl.c tools/portable/dmdl_cli.c tools/portable/dmdl.h tools/portable/dff_mesh.c tools/portable/geometry.c tools/portable/cluster.c tools/portable/qem.c
	$(EMCC) $(WASM_FLAGS) tools/portable/dmdl.c tools/portable/dmdl_cli.c tools/portable/dff_mesh.c tools/portable/geometry.c tools/portable/cluster.c tools/portable/qem.c -lm -o $@

web/assets.mjs: $(LIB_SOURCES) $(LIB_HEADERS) tools/portable/Makefile tools/portable/resample_coeffs.h tools/portable/vendor/stb_vorbis.c tools/portable/vendor/pl_mpeg.h
	$(EMCC) $(WASM_LIB_FLAGS) $(WASM_EXPORTS) $(LIB_SOURCES) -lm -o $@
