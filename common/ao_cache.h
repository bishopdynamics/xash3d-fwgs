/*
ao_cache.h - on-disk baked world ambient occlusion cache format
Copyright (C) 2026 James Bishop

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

==============================================================================

xash3d-streaming: world AO is raycast once, offline, by the engine generator
(host_aobake.c) and written to cache/ao/<map>.ao. At map load the GL renderer
(gl_ao.c) reads the cache straight into each surface's occlusion layer instead
of raycasting, so a seamless level change costs a file read, not a hitch.

The file is: one ao_cache_header_t, then for every baked surface an
ao_cache_surf_t followed by smax*tmax occlusion bytes (0 = open, 255 = fully
occluded). Surfaces with no AO (sky/turb/degenerate) are simply omitted.
==============================================================================
*/
#ifndef AO_CACHE_H
#define AO_CACHE_H

// 'X''A''O''2' little-endian; bump AO_CACHE_VERSION when the bake math, the ray
// kernel, or this layout changes so stale caches are rejected and re-baked
#define AO_CACHE_MAGIC   (((int)'2' << 24) | ((int)'O' << 16) | ((int)'A' << 8) | (int)'X')
#define AO_CACHE_VERSION 1

typedef struct
{
	int   magic;       // AO_CACHE_MAGIC
	int   version;     // AO_CACHE_VERSION
	int   rays;        // ray-kernel size the bake used (re-bake if it changes)
	float dist;        // r_ao_world_dist the bake used (re-bake if it changes)
	int   numsurfaces; // model->numsurfaces at bake time: guards against a
	                   // different BSP loading under the same map name
	int   numbaked;    // ao_cache_surf_t records that follow
} ao_cache_header_t;

typedef struct
{
	int            surf; // index into model->surfaces[]
	unsigned short smax; // luxel grid width
	unsigned short tmax; // luxel grid height; smax*tmax occlusion bytes follow
} ao_cache_surf_t;

#endif // AO_CACHE_H
