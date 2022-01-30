// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 1998-2000 by DooM Legacy Team.
// Copyright (C) 1999-2021 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file hw_main.c
/// \brief hardware renderer, using the standard HardWareRender driver DLL for SRB2

#include <math.h>

#include "../doomstat.h"

#ifdef HWRENDER
#include "hw_glob.h"
#include "hw_light.h"
#include "hw_drv.h"
#include "hw_batching.h"

#include "../i_video.h" // for rendermode == render_glide
#include "../v_video.h"
#include "../p_local.h"
#include "../p_setup.h"
#include "../r_local.h"
#include "../r_patch.h"
#include "../r_picformats.h"
#include "../r_bsp.h"
#include "../d_clisrv.h"
#include "../w_wad.h"
#include "../z_zone.h"
#include "../r_splats.h"
#include "../g_game.h"
#include "../st_stuff.h"
#include "../i_system.h"
#include "../m_cheat.h"
#include "../f_finale.h"
#include "../r_things.h" // R_GetShadowZ
#include "../p_slopes.h"
#include "hw_md2.h"
#include "hw_clip.h"

// ==========================================================================
// the hardware driver object
// ==========================================================================
struct hwdriver_s hwdriver;

// ==========================================================================
//                                                                     PROTOS
// ==========================================================================

void HWR_AddTransparentFloor(levelflat_t *levelflat, extrasubsector_t *xsub, boolean isceiling, fixed_t fixedheight, INT32 lightlevel, INT32 alpha, sector_t *FOFSector, FBITFIELD blend, boolean fogplane, extracolormap_t *planecolormap);
void HWR_AddTransparentPolyobjectFloor(levelflat_t *levelflat, polyobj_t *polysector, boolean isceiling, fixed_t fixedheight,
                             INT32 lightlevel, INT32 alpha, sector_t *FOFSector, FBITFIELD blend, extracolormap_t *planecolormap);

boolean drawsky = true;

#define ABS(x) ((x) < 0 ? -(x) : (x))

// ==========================================================================
//                                                                    GLOBALS
// ==========================================================================

// base values set at SetViewSize
static float gl_basecentery;

float gl_baseviewwindowy, gl_basewindowcentery;
float gl_viewwidth, gl_viewheight; // viewport clipping boundaries (screen coords)
float gl_viewwindowx;

static float gl_centerx, gl_centery;
static float gl_viewwindowy; // top left corner of view window
static float gl_windowcenterx; // center of view window, for projection
static float gl_windowcentery;

static float gl_pspritexscale, gl_pspriteyscale;

static seg_t *gl_curline;
static side_t *gl_sidedef;
static line_t *gl_linedef;
sector_t *gl_frontsector;
sector_t *gl_backsector;

// Render stats
ps_metric_t ps_hw_skyboxtime = {0};
ps_metric_t ps_hw_nodesorttime = {0};
ps_metric_t ps_hw_nodedrawtime = {0};
ps_metric_t ps_hw_spritesorttime = {0};
ps_metric_t ps_hw_spritedrawtime = {0};

// Render stats for batching
ps_metric_t ps_hw_numpolys = {0};
ps_metric_t ps_hw_numverts = {0};
ps_metric_t ps_hw_numcalls = {0};
ps_metric_t ps_hw_numshaders = {0};
ps_metric_t ps_hw_numtextures = {0};
ps_metric_t ps_hw_numpolyflags = {0};
ps_metric_t ps_hw_numcolors = {0};
ps_metric_t ps_hw_batchsorttime = {0};
ps_metric_t ps_hw_batchdrawtime = {0};

boolean gl_init = false;
boolean gl_maploaded = false;
boolean gl_sessioncommandsadded = false;
// false if shaders have not been initialized yet, or if shaders are not available
boolean gl_shadersavailable = false;

// Whether the internal state is set to palette rendering or not.
static boolean gl_palette_rendering_state = false;

// --------------------------------------------------------------------------
//                                              STUFF FOR THE PROJECTION CODE
// --------------------------------------------------------------------------

FTransform atransform;
// duplicates of the main code, set after R_SetupFrame() passed them into sharedstruct,
// copied here for local use
fixed_t dup_viewx, dup_viewy, dup_viewz;
angle_t dup_viewangle;

float gl_viewx, gl_viewy, gl_viewz;
float gl_viewsin, gl_viewcos;

// Maybe not necessary with the new T&L code (needs to be checked!)
float gl_viewludsin, gl_viewludcos; // look up down kik test
static float gl_fovlud;

static angle_t gl_aimingangle;

// ==========================================================================
// Lighting
// ==========================================================================

// Returns true if shaders can be used.
boolean HWR_UseShader(void)
{
	return (cv_glshaders.value && gl_shadersavailable);
}

void HWR_Lighting(FSurfaceInfo *Surface, INT32 light_level, extracolormap_t *colormap)
{
	RGBA_t poly_color, tint_color, fade_color;

	poly_color.rgba = 0xFFFFFFFF;
	tint_color.rgba = (colormap != NULL) ? (UINT32)colormap->rgba : GL_DEFAULTMIX;
	fade_color.rgba = (colormap != NULL) ? (UINT32)colormap->fadergba : GL_DEFAULTFOG;

	// Crappy backup coloring if you can't do shaders
	if (!HWR_UseShader())
	{
		// be careful, this may get negative for high lightlevel values.
		float tint_alpha, fade_alpha;
		float red, green, blue;

		red = (float)poly_color.s.red;
		green = (float)poly_color.s.green;
		blue = (float)poly_color.s.blue;

		// 48 is just an arbritrary value that looked relatively okay.
		tint_alpha = (float)(sqrt(tint_color.s.alpha) * 48) / 255.0f;

		// 8 is roughly the brightness of the "close" color in Software, and 16 the brightness of the "far" color.
		// 8 is too bright for dark levels, and 16 is too dark for bright levels.
		// 12 is the compromise value. It doesn't look especially good anywhere, but it's the most balanced.
		// (Also, as far as I can tell, fade_color's alpha is actually not used in Software, so we only use light level.)
		fade_alpha = (float)(sqrt(255-light_level) * 12) / 255.0f;

		// Clamp the alpha values
		tint_alpha = min(max(tint_alpha, 0.0f), 1.0f);
		fade_alpha = min(max(fade_alpha, 0.0f), 1.0f);

		red = (tint_color.s.red * tint_alpha) + (red * (1.0f - tint_alpha));
		green = (tint_color.s.green * tint_alpha) + (green * (1.0f - tint_alpha));
		blue = (tint_color.s.blue * tint_alpha) + (blue * (1.0f - tint_alpha));

		red = (fade_color.s.red * fade_alpha) + (red * (1.0f - fade_alpha));
		green = (fade_color.s.green * fade_alpha) + (green * (1.0f - fade_alpha));
		blue = (fade_color.s.blue * fade_alpha) + (blue * (1.0f - fade_alpha));

		poly_color.s.red = (UINT8)red;
		poly_color.s.green = (UINT8)green;
		poly_color.s.blue = (UINT8)blue;
	}

	// Clamp the light level, since it can sometimes go out of the 0-255 range from animations
	light_level = min(max(light_level, 0), 255);

	Surface->PolyColor.rgba = poly_color.rgba;
	Surface->TintColor.rgba = tint_color.rgba;
	Surface->FadeColor.rgba = fade_color.rgba;
	Surface->LightInfo.light_level = light_level;
	Surface->LightInfo.fade_start = (colormap != NULL) ? colormap->fadestart : 0;
	Surface->LightInfo.fade_end = (colormap != NULL) ? colormap->fadeend : 31;

	if (HWR_ShouldUsePaletteRendering())
	{
		boolean default_colormap = false;
		if (!colormap)
		{
			colormap = R_GetDefaultColormap(); // a place to store the hw lighttable id
			// alternatively could just store the id in a global variable if there are issues
			default_colormap = true;
		}
		// create hw lighttable if there isn't one
		if (!colormap->gl_lighttable_id)
		{
			UINT8 *colormap_pointer;

			if (default_colormap)
				colormap_pointer = colormaps; // don't actually use the data from the "default colormap"
			else
				colormap_pointer = colormap->colormap;
			colormap->gl_lighttable_id = HWR_CreateLightTable(colormap_pointer);
		}
		Surface->LightTableId = colormap->gl_lighttable_id;
	}
	else
	{
		Surface->LightTableId = 0;
	}
}

UINT8 HWR_FogBlockAlpha(INT32 light, extracolormap_t *colormap) // Let's see if this can work
{
	RGBA_t realcolor, surfcolor;
	INT32 alpha;

	realcolor.rgba = (colormap != NULL) ? colormap->rgba : GL_DEFAULTMIX;

	if (cv_glshaders.value && gl_shadersavailable)
	{
		surfcolor.s.alpha = (255 - light);
	}
	else
	{
		light = light - (255 - light);

		// Don't go out of bounds
		if (light < 0)
			light = 0;
		else if (light > 255)
			light = 255;

		alpha = (realcolor.s.alpha*255)/25;

		// at 255 brightness, alpha is between 0 and 127, at 0 brightness alpha will always be 255
		surfcolor.s.alpha = (alpha*light) / (2*256) + 255-light;
	}

	return surfcolor.s.alpha;
}

static FUINT HWR_CalcWallLight(FUINT lightnum, fixed_t v1x, fixed_t v1y, fixed_t v2x, fixed_t v2y)
{
	INT16 finallight = lightnum;

	if (cv_glfakecontrast.value != 0)
	{
		const UINT8 contrast = 8;
		fixed_t extralight = 0;

		if (cv_glfakecontrast.value == 2) // Smooth setting
		{
			extralight = (-(contrast<<FRACBITS) +
			FixedDiv(AngleFixed(R_PointToAngle2(0, 0,
				abs(v1x - v2x),
				abs(v1y - v2y))), 90<<FRACBITS)
			* (contrast * 2)) >> FRACBITS;
		}
		else
		{
			if (v1y == v2y)
				extralight = -contrast;
			else if (v1x == v2x)
				extralight = contrast;
		}

		if (extralight != 0)
		{
			finallight += extralight;

			if (finallight < 0)
				finallight = 0;
			if (finallight > 255)
				finallight = 255;
		}
	}

	return (FUINT)finallight;
}

FBITFIELD HWR_GetBlendModeFlag(INT32 style)
{
	switch (style)
	{
		case AST_TRANSLUCENT:
			return PF_Translucent;
		case AST_ADD:
			return PF_Additive;
		case AST_SUBTRACT:
			return PF_Subtractive;
		case AST_REVERSESUBTRACT:
			return PF_ReverseSubtract;
		case AST_MODULATE:
			return PF_Multiplicative;
		default:
			return PF_Masked;
	}
}

UINT8 HWR_GetTranstableAlpha(INT32 transtablenum)
{
	transtablenum = max(min(transtablenum, tr_trans90), 0);

	switch (transtablenum)
	{
		case 0          : return 0xff;
		case tr_trans10 : return 0xe6;
		case tr_trans20 : return 0xcc;
		case tr_trans30 : return 0xb3;
		case tr_trans40 : return 0x99;
		case tr_trans50 : return 0x80;
		case tr_trans60 : return 0x66;
		case tr_trans70 : return 0x4c;
		case tr_trans80 : return 0x33;
		case tr_trans90 : return 0x19;
	}

	return 0xff;
}

FBITFIELD HWR_SurfaceBlend(INT32 style, INT32 transtablenum, FSurfaceInfo *pSurf)
{
	if (!transtablenum || style <= AST_COPY || style >= AST_OVERLAY)
	{
		pSurf->PolyColor.s.alpha = 0xff;
		return PF_Masked;
	}

	pSurf->PolyColor.s.alpha = HWR_GetTranstableAlpha(transtablenum);
	return HWR_GetBlendModeFlag(style);
}

FBITFIELD HWR_TranstableToAlpha(INT32 transtablenum, FSurfaceInfo *pSurf)
{
	if (!transtablenum)
	{
		pSurf->PolyColor.s.alpha = 0x00;
		return PF_Masked;
	}

	pSurf->PolyColor.s.alpha = HWR_GetTranstableAlpha(transtablenum);
	return PF_Translucent;
}

static void HWR_AddTransparentWall(FOutVector *wallVerts, FSurfaceInfo *pSurf, INT32 texnum, FBITFIELD blend, boolean fogwall, INT32 lightlevel, extracolormap_t *wallcolormap);

// ==========================================================================
// Wall generation from subsector segs
// ==========================================================================

/*
   wallVerts order is :
		3--2
		| /|
		|/ |
		0--1
*/

//
// HWR_ProjectWall
//
static void HWR_ProjectWall(FOutVector *wallVerts, FSurfaceInfo *pSurf, FBITFIELD blendmode, INT32 lightlevel, extracolormap_t *wallcolormap)
{
	INT32 shader = -1;

	HWR_Lighting(pSurf, lightlevel, wallcolormap);

	if (HWR_UseShader())
	{
		shader = SHADER_WALL;
		blendmode |= PF_ColorMapped;
	}

	HWR_ProcessPolygon(pSurf, wallVerts, 4, blendmode|PF_Modulated|PF_Occlude, shader, false);
}

// ==========================================================================
//                                                          BSP, CULL, ETC..
// ==========================================================================

//
// HWR_SplitWall
//
static void HWR_SplitWall(sector_t *sector, FOutVector *wallVerts, INT32 texnum, FSurfaceInfo* Surf, INT32 cutflag, ffloor_t *pfloor, FBITFIELD polyflags)
{
	/* SoM: split up and light walls according to the
	 lightlist. This may also include leaving out parts
	 of the wall that can't be seen */

	float realtop, realbot, top, bot;
	float pegt, pegb, pegmul;
	float height = 0.0f, bheight = 0.0f;

	float endrealtop, endrealbot, endtop, endbot;
	float endpegt, endpegb, endpegmul;
	float endheight = 0.0f, endbheight = 0.0f;

	// compiler complains when P_GetSlopeZAt is used in FLOAT_TO_FIXED directly
	// use this as a temp var to store P_GetSlopeZAt's return value each time
	fixed_t temp;

	fixed_t v1x = FLOAT_TO_FIXED(wallVerts[0].x);
	fixed_t v1y = FLOAT_TO_FIXED(wallVerts[0].z); // not a typo
	fixed_t v2x = FLOAT_TO_FIXED(wallVerts[1].x);
	fixed_t v2y = FLOAT_TO_FIXED(wallVerts[1].z); // not a typo

	INT32 solid, i;
	lightlist_t *  list = sector->lightlist;
	const UINT8 alpha = Surf->PolyColor.s.alpha;
	FUINT lightnum = HWR_CalcWallLight(sector->lightlevel, v1x, v1y, v2x, v2y);
	extracolormap_t *colormap = NULL;

	realtop = top = wallVerts[3].y;
	realbot = bot = wallVerts[0].y;
	pegt = wallVerts[3].t;
	pegb = wallVerts[0].t;
	pegmul = (pegb - pegt) / (top - bot);

	endrealtop = endtop = wallVerts[2].y;
	endrealbot = endbot = wallVerts[1].y;
	endpegt = wallVerts[2].t;
	endpegb = wallVerts[1].t;
	endpegmul = (endpegb - endpegt) / (endtop - endbot);

	for (i = 0; i < sector->numlights; i++)
	{
		if (endtop < endrealbot && top < realbot)
			return;

		if (!(list[i].flags & FF_NOSHADE))
		{
			if (pfloor && (pfloor->flags & FF_FOG))
			{
				lightnum = pfloor->master->frontsector->lightlevel;
				colormap = pfloor->master->frontsector->extra_colormap;
				lightnum = colormap ? lightnum : HWR_CalcWallLight(lightnum, v1x, v1y, v2x, v2y);
			}
			else
			{
				lightnum = *list[i].lightlevel;
				colormap = *list[i].extra_colormap;
				lightnum = colormap ? lightnum : HWR_CalcWallLight(lightnum, v1x, v1y, v2x, v2y);
			}
		}

		solid = false;

		if ((sector->lightlist[i].flags & FF_CUTSOLIDS) && !(cutflag & FF_EXTRA))
			solid = true;
		else if ((sector->lightlist[i].flags & FF_CUTEXTRA) && (cutflag & FF_EXTRA))
		{
			if (sector->lightlist[i].flags & FF_EXTRA)
			{
				if ((sector->lightlist[i].flags & (FF_FOG|FF_SWIMMABLE)) == (cutflag & (FF_FOG|FF_SWIMMABLE))) // Only merge with your own types
					solid = true;
			}
			else
				solid = true;
		}
		else
			solid = false;

		temp = P_GetLightZAt(&list[i], v1x, v1y);
		height = FIXED_TO_FLOAT(temp);
		temp = P_GetLightZAt(&list[i], v2x, v2y);
		endheight = FIXED_TO_FLOAT(temp);
		if (solid)
		{
			temp = P_GetFFloorBottomZAt(list[i].caster, v1x, v1y);
			bheight = FIXED_TO_FLOAT(temp);
			temp = P_GetFFloorBottomZAt(list[i].caster, v2x, v2y);
			endbheight = FIXED_TO_FLOAT(temp);
		}

		if (endheight >= endtop && height >= top)
		{
			if (solid && top > bheight)
				top = bheight;
			if (solid && endtop > endbheight)
				endtop = endbheight;
		}

		if (i + 1 < sector->numlights)
		{
			temp = P_GetLightZAt(&list[i+1], v1x, v1y);
			bheight = FIXED_TO_FLOAT(temp);
			temp = P_GetLightZAt(&list[i+1], v2x, v2y);
			endbheight = FIXED_TO_FLOAT(temp);
		}
		else
		{
			bheight = realbot;
			endbheight = endrealbot;
		}

		if (endbheight >= endtop && bheight >= top)
			continue;

		//Found a break;
		bot = bheight;

		if (bot < realbot)
			bot = realbot;

		endbot = endbheight;

		if (endbot < endrealbot)
			endbot = endrealbot;

		Surf->PolyColor.s.alpha = alpha;

		wallVerts[3].t = pegt + ((realtop - top) * pegmul);
		wallVerts[2].t = endpegt + ((endrealtop - endtop) * endpegmul);
		wallVerts[0].t = pegt + ((realtop - bot) * pegmul);
		wallVerts[1].t = endpegt + ((endrealtop - endbot) * endpegmul);

		// set top/bottom coords
		wallVerts[3].y = top;
		wallVerts[2].y = endtop;
		wallVerts[0].y = bot;
		wallVerts[1].y = endbot;

		if (cutflag & FF_FOG)
			HWR_AddTransparentWall(wallVerts, Surf, texnum, PF_Fog|PF_NoTexture|polyflags, true, lightnum, colormap);
		else if (polyflags & (PF_Translucent|PF_Additive|PF_Subtractive|PF_ReverseSubtract|PF_Multiplicative|PF_Environment))
			HWR_AddTransparentWall(wallVerts, Surf, texnum, polyflags, false, lightnum, colormap);
		else
			HWR_ProjectWall(wallVerts, Surf, PF_Masked|polyflags, lightnum, colormap);

		top = bot;
		endtop = endbot;
	}

	bot = realbot;
	endbot = endrealbot;
	if (endtop <= endrealbot && top <= realbot)
		return;

	Surf->PolyColor.s.alpha = alpha;

	wallVerts[3].t = pegt + ((realtop - top) * pegmul);
	wallVerts[2].t = endpegt + ((endrealtop - endtop) * endpegmul);
	wallVerts[0].t = pegt + ((realtop - bot) * pegmul);
	wallVerts[1].t = endpegt + ((endrealtop - endbot) * endpegmul);

	// set top/bottom coords
	wallVerts[3].y = top;
	wallVerts[2].y = endtop;
	wallVerts[0].y = bot;
	wallVerts[1].y = endbot;

	if (cutflag & FF_FOG)
		HWR_AddTransparentWall(wallVerts, Surf, texnum, PF_Fog|PF_NoTexture|polyflags, true, lightnum, colormap);
	else if (polyflags & (PF_Translucent|PF_Additive|PF_Subtractive|PF_ReverseSubtract|PF_Multiplicative|PF_Environment))
		HWR_AddTransparentWall(wallVerts, Surf, texnum, polyflags, false, lightnum, colormap);
	else
		HWR_ProjectWall(wallVerts, Surf, PF_Masked|polyflags, lightnum, colormap);
}

// HWR_DrawSkyWall
// Draw walls into the depth buffer so that anything behind is culled properly
static void HWR_DrawSkyWall(FOutVector *wallVerts, FSurfaceInfo *Surf)
{
	HWR_SetCurrentTexture(NULL);
	// no texture
	wallVerts[3].t = wallVerts[2].t = 0;
	wallVerts[0].t = wallVerts[1].t = 0;
	wallVerts[0].s = wallVerts[3].s = 0;
	wallVerts[2].s = wallVerts[1].s = 0;
	// this no longer sets top/bottom coords, this should be done before caling the function
	HWR_ProjectWall(wallVerts, Surf, PF_Invisible|PF_NoTexture, 255, NULL);
	// PF_Invisible so it's not drawn into the colour buffer
	// PF_NoTexture for no texture
	// PF_Occlude is set in HWR_ProjectWall to draw into the depth buffer
}

//
// HWR_ProcessSeg
// A portion or all of a wall segment will be drawn, from startfrac to endfrac,
//  where 0 is the start of the segment, 1 the end of the segment
// Anything between means the wall segment has been clipped with solidsegs,
//  reducing wall overdraw to a minimum
//
static void HWR_ProcessSeg(void) // Sort of like GLWall::Process in GZDoom
{
	FOutVector wallVerts[4];
	v2d_t vs, ve; // start, end vertices of 2d line (view from above)

	fixed_t worldtop, worldbottom;
	fixed_t worldhigh = 0, worldlow = 0;
	fixed_t worldtopslope, worldbottomslope;
	fixed_t worldhighslope = 0, worldlowslope = 0;
	fixed_t v1x, v1y, v2x, v2y;

	GLMapTexture_t *grTex = NULL;
	float cliplow = 0.0f, cliphigh = 0.0f;
	INT32 gl_midtexture;
	fixed_t h, l; // 3D sides and 2s middle textures
	fixed_t hS, lS;

	FUINT lightnum = 0; // shut up compiler
	extracolormap_t *colormap;
	FSurfaceInfo Surf;

	gl_sidedef = gl_curline->sidedef;
	gl_linedef = gl_curline->linedef;

	vs.x = ((polyvertex_t *)gl_curline->pv1)->x;
	vs.y = ((polyvertex_t *)gl_curline->pv1)->y;
	ve.x = ((polyvertex_t *)gl_curline->pv2)->x;
	ve.y = ((polyvertex_t *)gl_curline->pv2)->y;

	v1x = FLOAT_TO_FIXED(vs.x);
	v1y = FLOAT_TO_FIXED(vs.y);
	v2x = FLOAT_TO_FIXED(ve.x);
	v2y = FLOAT_TO_FIXED(ve.y);

#define SLOPEPARAMS(slope, end1, end2, normalheight) \
	end1 = P_GetZAt(slope, v1x, v1y, normalheight); \
	end2 = P_GetZAt(slope, v2x, v2y, normalheight);

	SLOPEPARAMS(gl_frontsector->c_slope, worldtop,    worldtopslope,    gl_frontsector->ceilingheight)
	SLOPEPARAMS(gl_frontsector->f_slope, worldbottom, worldbottomslope, gl_frontsector->floorheight)

	// remember vertices ordering
	//  3--2
	//  | /|
	//  |/ |
	//  0--1
	// make a wall polygon (with 2 triangles), using the floor/ceiling heights,
	// and the 2d map coords of start/end vertices
	wallVerts[0].x = wallVerts[3].x = vs.x;
	wallVerts[0].z = wallVerts[3].z = vs.y;
	wallVerts[2].x = wallVerts[1].x = ve.x;
	wallVerts[2].z = wallVerts[1].z = ve.y;

	// x offset the texture
	{
		fixed_t texturehpeg = gl_sidedef->textureoffset + gl_curline->offset;
		cliplow = (float)texturehpeg;
		cliphigh = (float)(texturehpeg + (gl_curline->flength*FRACUNIT));
	}

	lightnum = gl_frontsector->lightlevel;
	colormap = gl_frontsector->extra_colormap;
	lightnum = colormap ? lightnum : HWR_CalcWallLight(lightnum, vs.x, vs.y, ve.x, ve.y);

	if (gl_frontsector)
		Surf.PolyColor.s.alpha = 255;

	if (gl_backsector)
	{
		INT32 gl_toptexture = 0, gl_bottomtexture = 0;
		// two sided line
		boolean bothceilingssky = false; // turned on if both back and front ceilings are sky
		boolean bothfloorssky = false; // likewise, but for floors

		SLOPEPARAMS(gl_backsector->c_slope, worldhigh, worldhighslope, gl_backsector->ceilingheight)
		SLOPEPARAMS(gl_backsector->f_slope, worldlow,  worldlowslope,  gl_backsector->floorheight)

		// hack to allow height changes in outdoor areas
		// This is what gets rid of the upper textures if there should be sky
		if (gl_frontsector->ceilingpic == skyflatnum
			&& gl_backsector->ceilingpic  == skyflatnum)
		{
			bothceilingssky = true;
		}

		// likewise, but for floors and upper textures
		if (gl_frontsector->floorpic == skyflatnum
			&& gl_backsector->floorpic == skyflatnum)
		{
			bothfloorssky = true;
		}

		if (!bothceilingssky)
			gl_toptexture = R_GetTextureNum(gl_sidedef->toptexture);
		if (!bothfloorssky)
			gl_bottomtexture = R_GetTextureNum(gl_sidedef->bottomtexture);

		// check TOP TEXTURE
		if ((worldhighslope < worldtopslope || worldhigh < worldtop) && gl_toptexture)
		{
			{
				fixed_t texturevpegtop; // top

				grTex = HWR_GetTexture(gl_toptexture);

				// PEGGING
				if (gl_linedef->flags & ML_DONTPEGTOP)
					texturevpegtop = 0;
				else if (gl_linedef->flags & ML_EFFECT1)
					texturevpegtop = worldhigh + textureheight[gl_sidedef->toptexture] - worldtop;
				else
					texturevpegtop = gl_backsector->ceilingheight + textureheight[gl_sidedef->toptexture] - gl_frontsector->ceilingheight;

				texturevpegtop += gl_sidedef->rowoffset;

				// This is so that it doesn't overflow and screw up the wall, it doesn't need to go higher than the texture's height anyway
				texturevpegtop %= (textures[gl_toptexture]->height)<<FRACBITS;

				wallVerts[3].t = wallVerts[2].t = texturevpegtop * grTex->scaleY;
				wallVerts[0].t = wallVerts[1].t = (texturevpegtop + gl_frontsector->ceilingheight - gl_backsector->ceilingheight) * grTex->scaleY;
				wallVerts[0].s = wallVerts[3].s = cliplow * grTex->scaleX;
				wallVerts[2].s = wallVerts[1].s = cliphigh * grTex->scaleX;

				// Adjust t value for sloped walls
				if (!(gl_linedef->flags & ML_EFFECT1))
				{
					// Unskewed
					wallVerts[3].t -= (worldtop - gl_frontsector->ceilingheight) * grTex->scaleY;
					wallVerts[2].t -= (worldtopslope - gl_frontsector->ceilingheight) * grTex->scaleY;
					wallVerts[0].t -= (worldhigh - gl_backsector->ceilingheight) * grTex->scaleY;
					wallVerts[1].t -= (worldhighslope - gl_backsector->ceilingheight) * grTex->scaleY;
				}
				else if (gl_linedef->flags & ML_DONTPEGTOP)
				{
					// Skewed by top
					wallVerts[0].t = (texturevpegtop + worldtop - worldhigh) * grTex->scaleY;
					wallVerts[1].t = (texturevpegtop + worldtopslope - worldhighslope) * grTex->scaleY;
				}
				else
				{
					// Skewed by bottom
					wallVerts[0].t = wallVerts[1].t = (texturevpegtop + worldtop - worldhigh) * grTex->scaleY;
					wallVerts[3].t = wallVerts[0].t - (worldtop - worldhigh) * grTex->scaleY;
					wallVerts[2].t = wallVerts[1].t - (worldtopslope - worldhighslope) * grTex->scaleY;
				}
			}

			// set top/bottom coords
			wallVerts[3].y = FIXED_TO_FLOAT(worldtop);
			wallVerts[0].y = FIXED_TO_FLOAT(worldhigh);
			wallVerts[2].y = FIXED_TO_FLOAT(worldtopslope);
			wallVerts[1].y = FIXED_TO_FLOAT(worldhighslope);

			if (gl_frontsector->numlights)
				HWR_SplitWall(gl_frontsector, wallVerts, gl_toptexture, &Surf, FF_CUTLEVEL, NULL, 0);
			else if (grTex->mipmap.flags & TF_TRANSPARENT)
				HWR_AddTransparentWall(wallVerts, &Surf, gl_toptexture, PF_Environment, false, lightnum, colormap);
			else
				HWR_ProjectWall(wallVerts, &Surf, PF_Masked, lightnum, colormap);
		}

		// check BOTTOM TEXTURE
		if ((
			worldlowslope > worldbottomslope ||
            worldlow > worldbottom) && gl_bottomtexture) //only if VISIBLE!!!
		{
			{
				fixed_t texturevpegbottom = 0; // bottom

				grTex = HWR_GetTexture(gl_bottomtexture);

				// PEGGING
				if (!(gl_linedef->flags & ML_DONTPEGBOTTOM))
					texturevpegbottom = 0;
				else if (gl_linedef->flags & ML_EFFECT1)
					texturevpegbottom = worldbottom - worldlow;
				else
					texturevpegbottom = gl_frontsector->floorheight - gl_backsector->floorheight;

				texturevpegbottom += gl_sidedef->rowoffset;

				// This is so that it doesn't overflow and screw up the wall, it doesn't need to go higher than the texture's height anyway
				texturevpegbottom %= (textures[gl_bottomtexture]->height)<<FRACBITS;

				wallVerts[3].t = wallVerts[2].t = texturevpegbottom * grTex->scaleY;
				wallVerts[0].t = wallVerts[1].t = (texturevpegbottom + gl_backsector->floorheight - gl_frontsector->floorheight) * grTex->scaleY;
				wallVerts[0].s = wallVerts[3].s = cliplow * grTex->scaleX;
				wallVerts[2].s = wallVerts[1].s = cliphigh * grTex->scaleX;

				// Adjust t value for sloped walls
				if (!(gl_linedef->flags & ML_EFFECT1))
				{
					// Unskewed
					wallVerts[0].t -= (worldbottom - gl_frontsector->floorheight) * grTex->scaleY;
					wallVerts[1].t -= (worldbottomslope - gl_frontsector->floorheight) * grTex->scaleY;
					wallVerts[3].t -= (worldlow - gl_backsector->floorheight) * grTex->scaleY;
					wallVerts[2].t -= (worldlowslope - gl_backsector->floorheight) * grTex->scaleY;
				}
				else if (gl_linedef->flags & ML_DONTPEGBOTTOM)
				{
					// Skewed by bottom
					wallVerts[0].t = wallVerts[1].t = (texturevpegbottom + worldlow - worldbottom) * grTex->scaleY;
					//wallVerts[3].t = wallVerts[0].t - (worldlow - worldbottom) * grTex->scaleY; // no need, [3] is already this
					wallVerts[2].t = wallVerts[1].t - (worldlowslope - worldbottomslope) * grTex->scaleY;
				}
				else
				{
					// Skewed by top
					wallVerts[0].t = (texturevpegbottom + worldlow - worldbottom) * grTex->scaleY;
					wallVerts[1].t = (texturevpegbottom + worldlowslope - worldbottomslope) * grTex->scaleY;
				}
			}

			// set top/bottom coords
			wallVerts[3].y = FIXED_TO_FLOAT(worldlow);
			wallVerts[0].y = FIXED_TO_FLOAT(worldbottom);
			wallVerts[2].y = FIXED_TO_FLOAT(worldlowslope);
			wallVerts[1].y = FIXED_TO_FLOAT(worldbottomslope);

			if (gl_frontsector->numlights)
				HWR_SplitWall(gl_frontsector, wallVerts, gl_bottomtexture, &Surf, FF_CUTLEVEL, NULL, 0);
			else if (grTex->mipmap.flags & TF_TRANSPARENT)
				HWR_AddTransparentWall(wallVerts, &Surf, gl_bottomtexture, PF_Environment, false, lightnum, colormap);
			else
				HWR_ProjectWall(wallVerts, &Surf, PF_Masked, lightnum, colormap);
		}

		gl_midtexture = R_GetTextureNum(gl_sidedef->midtexture);
		if (gl_midtexture)
		{
			FBITFIELD blendmode;
			sector_t *front, *back;
			fixed_t  popentop, popenbottom, polytop, polybottom, lowcut, highcut;
			fixed_t     texturevpeg = 0;
			INT32 repeats;

			if (gl_linedef->frontsector->heightsec != -1)
				front = &sectors[gl_linedef->frontsector->heightsec];
			else
				front = gl_linedef->frontsector;

			if (gl_linedef->backsector->heightsec != -1)
				back = &sectors[gl_linedef->backsector->heightsec];
			else
				back = gl_linedef->backsector;

			if (gl_sidedef->repeatcnt)
				repeats = 1 + gl_sidedef->repeatcnt;
			else if (gl_linedef->flags & ML_EFFECT5)
			{
				fixed_t high, low;

				if (front->ceilingheight > back->ceilingheight)
					high = back->ceilingheight;
				else
					high = front->ceilingheight;

				if (front->floorheight > back->floorheight)
					low = front->floorheight;
				else
					low = back->floorheight;

				repeats = (high - low)/textureheight[gl_sidedef->midtexture];
				if ((high-low)%textureheight[gl_sidedef->midtexture])
					repeats++; // tile an extra time to fill the gap -- Monster Iestyn
			}
			else
				repeats = 1;

			// SoM: a little note: This code re-arranging will
			// fix the bug in Nimrod map02. popentop and popenbottom
			// record the limits the texture can be displayed in.
			// polytop and polybottom, are the ideal (i.e. unclipped)
			// heights of the polygon, and h & l, are the final (clipped)
			// poly coords.

			// NOTE: With polyobjects, whenever you need to check the properties of the polyobject sector it belongs to,
			// you must use the linedef's backsector to be correct
			// From CB
			if (gl_curline->polyseg)
			{
				popentop = back->ceilingheight;
				popenbottom = back->floorheight;
			}
			else
            {
				popentop = min(worldtop, worldhigh);
				popenbottom = max(worldbottom, worldlow);
			}

			if (gl_linedef->flags & ML_EFFECT2)
			{
				if (!!(gl_linedef->flags & ML_DONTPEGBOTTOM) ^ !!(gl_linedef->flags & ML_EFFECT3))
				{
					polybottom = max(front->floorheight, back->floorheight) + gl_sidedef->rowoffset;
					polytop = polybottom + textureheight[gl_midtexture]*repeats;
				}
				else
				{
					polytop = min(front->ceilingheight, back->ceilingheight) + gl_sidedef->rowoffset;
					polybottom = polytop - textureheight[gl_midtexture]*repeats;
				}
			}
			else if (!!(gl_linedef->flags & ML_DONTPEGBOTTOM) ^ !!(gl_linedef->flags & ML_EFFECT3))
			{
				polybottom = popenbottom + gl_sidedef->rowoffset;
				polytop = polybottom + textureheight[gl_midtexture]*repeats;
			}
			else
			{
				polytop = popentop + gl_sidedef->rowoffset;
				polybottom = polytop - textureheight[gl_midtexture]*repeats;
			}
			// CB
			// NOTE: With polyobjects, whenever you need to check the properties of the polyobject sector it belongs to,
			// you must use the linedef's backsector to be correct
			if (gl_curline->polyseg)
			{
				lowcut = polybottom;
				highcut = polytop;
			}
			else
			{
				// The cut-off values of a linedef can always be constant, since every line has an absoulute front and or back sector
				lowcut = popenbottom;
				highcut = popentop;
			}

			h = min(highcut, polytop);
			l = max(polybottom, lowcut);

			{
				// PEGGING
				if (!!(gl_linedef->flags & ML_DONTPEGBOTTOM) ^ !!(gl_linedef->flags & ML_EFFECT3))
					texturevpeg = textureheight[gl_sidedef->midtexture]*repeats - h + polybottom;
				else
					texturevpeg = polytop - h;

				grTex = HWR_GetTexture(gl_midtexture);

				wallVerts[3].t = wallVerts[2].t = texturevpeg * grTex->scaleY;
				wallVerts[0].t = wallVerts[1].t = (h - l + texturevpeg) * grTex->scaleY;
				wallVerts[0].s = wallVerts[3].s = cliplow * grTex->scaleX;
				wallVerts[2].s = wallVerts[1].s = cliphigh * grTex->scaleX;
			}

			// set top/bottom coords
			// Take the texture peg into account, rather than changing the offsets past
			// where the polygon might not be.
			wallVerts[2].y = wallVerts[3].y = FIXED_TO_FLOAT(h);
			wallVerts[0].y = wallVerts[1].y = FIXED_TO_FLOAT(l);

			// Correct to account for slopes
			{
				fixed_t midtextureslant;

				if (gl_linedef->flags & ML_EFFECT2)
					midtextureslant = 0;
				else if (!!(gl_linedef->flags & ML_DONTPEGBOTTOM) ^ !!(gl_linedef->flags & ML_EFFECT3))
					midtextureslant = worldlow < worldbottom
							  ? worldbottomslope-worldbottom
							  : worldlowslope-worldlow;
				else
					midtextureslant = worldtop < worldhigh
							  ? worldtopslope-worldtop
							  : worldhighslope-worldhigh;

				polytop += midtextureslant;
				polybottom += midtextureslant;

				highcut += worldtop < worldhigh
						 ? worldtopslope-worldtop
						 : worldhighslope-worldhigh;
				lowcut += worldlow < worldbottom
						? worldbottomslope-worldbottom
						: worldlowslope-worldlow;

				// Texture stuff
				h = min(highcut, polytop);
				l = max(polybottom, lowcut);

				{
					// PEGGING
					if (!!(gl_linedef->flags & ML_DONTPEGBOTTOM) ^ !!(gl_linedef->flags & ML_EFFECT3))
						texturevpeg = textureheight[gl_sidedef->midtexture]*repeats - h + polybottom;
					else
						texturevpeg = polytop - h;
					wallVerts[2].t = texturevpeg * grTex->scaleY;
					wallVerts[1].t = (h - l + texturevpeg) * grTex->scaleY;
				}

				wallVerts[2].y = FIXED_TO_FLOAT(h);
				wallVerts[1].y = FIXED_TO_FLOAT(l);
			}

			// set alpha for transparent walls
			// ooops ! this do not work at all because render order we should render it in backtofront order
			switch (gl_linedef->special)
			{
				//  Translucent
				case 102:
				case 121:
				case 123:
				case 124:
				case 125:
				case 141:
				case 142:
				case 144:
				case 145:
				case 174:
				case 175:
				case 192:
				case 195:
				case 221:
				case 253:
				case 256:
					if (gl_linedef->blendmode && gl_linedef->blendmode != AST_FOG)
						blendmode = HWR_SurfaceBlend(gl_linedef->blendmode, R_GetLinedefTransTable(gl_linedef->alpha), &Surf);
					else
						blendmode = PF_Translucent;
					break;
				default:
					if (gl_linedef->blendmode && gl_linedef->blendmode != AST_FOG)
					{
						if (gl_linedef->alpha >= 0 && gl_linedef->alpha < FRACUNIT)
							blendmode = HWR_SurfaceBlend(gl_linedef->blendmode, R_GetLinedefTransTable(gl_linedef->alpha), &Surf);
						else
							blendmode = HWR_GetBlendModeFlag(gl_linedef->blendmode);
					}
					else if (gl_linedef->alpha >= 0 && gl_linedef->alpha < FRACUNIT)
						blendmode = HWR_TranstableToAlpha(R_GetLinedefTransTable(gl_linedef->alpha), &Surf);
					else
						blendmode = PF_Masked;
					break;
			}

			if (gl_curline->polyseg && gl_curline->polyseg->translucency > 0)
			{
				if (gl_curline->polyseg->translucency >= NUMTRANSMAPS) // wall not drawn
				{
					Surf.PolyColor.s.alpha = 0x00; // This shouldn't draw anything regardless of blendmode
					blendmode = PF_Masked;
				}
				else
					blendmode = HWR_TranstableToAlpha(gl_curline->polyseg->translucency, &Surf);
			}

			// Render midtextures on two-sided lines with a z-buffer offset.
			// This will cause the midtexture appear on top, if a FOF overlaps with it.
			blendmode |= PF_Decal;

			if (gl_frontsector->numlights)
			{
				if (!(blendmode & PF_Masked))
					HWR_SplitWall(gl_frontsector, wallVerts, gl_midtexture, &Surf, FF_TRANSLUCENT, NULL, blendmode);
				else
					HWR_SplitWall(gl_frontsector, wallVerts, gl_midtexture, &Surf, FF_CUTLEVEL, NULL, blendmode);
			}
			else if (!(blendmode & PF_Masked))
				HWR_AddTransparentWall(wallVerts, &Surf, gl_midtexture, blendmode, false, lightnum, colormap);
			else
				HWR_ProjectWall(wallVerts, &Surf, blendmode, lightnum, colormap);
		}

		// Sky culling
		// No longer so much a mess as before!
		if (!gl_curline->polyseg) // Don't do it for polyobjects
		{
			if (gl_frontsector->ceilingpic == skyflatnum)
			{
				if (gl_backsector->ceilingpic != skyflatnum) // don't cull if back sector is also sky
				{
					wallVerts[2].y = wallVerts[3].y = FIXED_TO_FLOAT(INT32_MAX); // draw to top of map space
					wallVerts[0].y = FIXED_TO_FLOAT(worldtop);
					wallVerts[1].y = FIXED_TO_FLOAT(worldtopslope);
					HWR_DrawSkyWall(wallVerts, &Surf);
				}
			}

			if (gl_frontsector->floorpic == skyflatnum)
			{
				if (gl_backsector->floorpic != skyflatnum) // don't cull if back sector is also sky
				{
					wallVerts[3].y = FIXED_TO_FLOAT(worldbottom);
					wallVerts[2].y = FIXED_TO_FLOAT(worldbottomslope);
					wallVerts[0].y = wallVerts[1].y = FIXED_TO_FLOAT(INT32_MIN); // draw to bottom of map space
					HWR_DrawSkyWall(wallVerts, &Surf);
				}
			}
		}
	}
	else
	{
		// Single sided line... Deal only with the middletexture (if one exists)
		gl_midtexture = R_GetTextureNum(gl_sidedef->midtexture);
		if (gl_midtexture && gl_linedef->special != 41) // (Ignore horizon line for OGL)
		{
			{
				fixed_t     texturevpeg;
				// PEGGING
				if ((gl_linedef->flags & (ML_DONTPEGBOTTOM|ML_EFFECT2)) == (ML_DONTPEGBOTTOM|ML_EFFECT2))
					texturevpeg = gl_frontsector->floorheight + textureheight[gl_sidedef->midtexture] - gl_frontsector->ceilingheight + gl_sidedef->rowoffset;
				else if (gl_linedef->flags & ML_DONTPEGBOTTOM)
					texturevpeg = worldbottom + textureheight[gl_sidedef->midtexture] - worldtop + gl_sidedef->rowoffset;
				else
					// top of texture at top
					texturevpeg = gl_sidedef->rowoffset;

				grTex = HWR_GetTexture(gl_midtexture);

				wallVerts[3].t = wallVerts[2].t = texturevpeg * grTex->scaleY;
				wallVerts[0].t = wallVerts[1].t = (texturevpeg + gl_frontsector->ceilingheight - gl_frontsector->floorheight) * grTex->scaleY;
				wallVerts[0].s = wallVerts[3].s = cliplow * grTex->scaleX;
				wallVerts[2].s = wallVerts[1].s = cliphigh * grTex->scaleX;

				// Texture correction for slopes
				if (gl_linedef->flags & ML_EFFECT2) {
					wallVerts[3].t += (gl_frontsector->ceilingheight - worldtop) * grTex->scaleY;
					wallVerts[2].t += (gl_frontsector->ceilingheight - worldtopslope) * grTex->scaleY;
					wallVerts[0].t += (gl_frontsector->floorheight - worldbottom) * grTex->scaleY;
					wallVerts[1].t += (gl_frontsector->floorheight - worldbottomslope) * grTex->scaleY;
				} else if (gl_linedef->flags & ML_DONTPEGBOTTOM) {
					wallVerts[3].t = wallVerts[0].t + (worldbottom-worldtop) * grTex->scaleY;
					wallVerts[2].t = wallVerts[1].t + (worldbottomslope-worldtopslope) * grTex->scaleY;
				} else {
					wallVerts[0].t = wallVerts[3].t - (worldbottom-worldtop) * grTex->scaleY;
					wallVerts[1].t = wallVerts[2].t - (worldbottomslope-worldtopslope) * grTex->scaleY;
				}
			}

			//Set textures properly on single sided walls that are sloped
			wallVerts[3].y = FIXED_TO_FLOAT(worldtop);
			wallVerts[0].y = FIXED_TO_FLOAT(worldbottom);
			wallVerts[2].y = FIXED_TO_FLOAT(worldtopslope);
			wallVerts[1].y = FIXED_TO_FLOAT(worldbottomslope);

			// I don't think that solid walls can use translucent linedef types...
			if (gl_frontsector->numlights)
				HWR_SplitWall(gl_frontsector, wallVerts, gl_midtexture, &Surf, FF_CUTLEVEL, NULL, 0);
			else
			{
				if (grTex->mipmap.flags & TF_TRANSPARENT)
					HWR_AddTransparentWall(wallVerts, &Surf, gl_midtexture, PF_Environment, false, lightnum, colormap);
				else
					HWR_ProjectWall(wallVerts, &Surf, PF_Masked, lightnum, colormap);
			}
		}

		if (!gl_curline->polyseg)
		{
			if (gl_frontsector->ceilingpic == skyflatnum) // It's a single-sided line with sky for its sector
			{
				wallVerts[2].y = wallVerts[3].y = FIXED_TO_FLOAT(INT32_MAX); // draw to top of map space
				wallVerts[0].y = FIXED_TO_FLOAT(worldtop);
				wallVerts[1].y = FIXED_TO_FLOAT(worldtopslope);
				HWR_DrawSkyWall(wallVerts, &Surf);
			}
			if (gl_frontsector->floorpic == skyflatnum)
			{
				wallVerts[3].y = FIXED_TO_FLOAT(worldbottom);
				wallVerts[2].y = FIXED_TO_FLOAT(worldbottomslope);
				wallVerts[0].y = wallVerts[1].y = FIXED_TO_FLOAT(INT32_MIN); // draw to bottom of map space
				HWR_DrawSkyWall(wallVerts, &Surf);
			}
		}
	}


	//Hurdler: 3d-floors test
	if (gl_frontsector && gl_backsector && !Tag_Compare(&gl_frontsector->tags, &gl_backsector->tags) && (gl_backsector->ffloors || gl_frontsector->ffloors))
	{
		ffloor_t * rover;
		fixed_t    highcut = 0, lowcut = 0;
		fixed_t lowcutslope, highcutslope;

		// Used for height comparisons and etc across FOFs and slopes
		fixed_t high1, highslope1, low1, lowslope1;

		INT32 texnum;
		line_t * newline = NULL; // Multi-Property FOF

		lowcut = max(worldbottom, worldlow);
		highcut = min(worldtop, worldhigh);
		lowcutslope = max(worldbottomslope, worldlowslope);
		highcutslope = min(worldtopslope, worldhighslope);

		if (gl_backsector->ffloors)
		{
			for (rover = gl_backsector->ffloors; rover; rover = rover->next)
			{
				boolean bothsides = false;
				// Skip if it exists on both sectors.
				ffloor_t * r2;
				for (r2 = gl_frontsector->ffloors; r2; r2 = r2->next)
					if (rover->master == r2->master)
					{
						bothsides = true;
						break;
					}

				if (bothsides) continue;

				if (!(rover->flags & FF_EXISTS) || !(rover->flags & FF_RENDERSIDES))
					continue;
				if (!(rover->flags & FF_ALLSIDES) && rover->flags & FF_INVERTSIDES)
					continue;

				SLOPEPARAMS(*rover->t_slope, high1, highslope1, *rover->topheight)
				SLOPEPARAMS(*rover->b_slope, low1,  lowslope1,  *rover->bottomheight)

				if ((high1 < lowcut && highslope1 < lowcutslope) || (low1 > highcut && lowslope1 > highcutslope))
					continue;

				texnum = R_GetTextureNum(sides[rover->master->sidenum[0]].midtexture);

				if (rover->master->flags & ML_TFERLINE)
				{
					size_t linenum = gl_curline->linedef-gl_backsector->lines[0];
					newline = rover->master->frontsector->lines[0] + linenum;
					texnum = R_GetTextureNum(sides[newline->sidenum[0]].midtexture);
				}

				h  = P_GetFFloorTopZAt   (rover, v1x, v1y);
				hS = P_GetFFloorTopZAt   (rover, v2x, v2y);
				l  = P_GetFFloorBottomZAt(rover, v1x, v1y);
				lS = P_GetFFloorBottomZAt(rover, v2x, v2y);
				// Adjust the heights so the FOF does not overlap with top and bottom textures.
				if (h >= highcut && hS >= highcutslope)
				{
					h = highcut;
					hS = highcutslope;
				}
				if (l <= lowcut && lS <= lowcutslope)
				{
					l = lowcut;
					lS = lowcutslope;
				}
				//Hurdler: HW code starts here
				//FIXME: check if peging is correct
				// set top/bottom coords

				wallVerts[3].y = FIXED_TO_FLOAT(h);
				wallVerts[2].y = FIXED_TO_FLOAT(hS);
				wallVerts[0].y = FIXED_TO_FLOAT(l);
				wallVerts[1].y = FIXED_TO_FLOAT(lS);
				if (rover->flags & FF_FOG)
				{
					wallVerts[3].t = wallVerts[2].t = 0;
					wallVerts[0].t = wallVerts[1].t = 0;
					wallVerts[0].s = wallVerts[3].s = 0;
					wallVerts[2].s = wallVerts[1].s = 0;
				}
				else
				{
					fixed_t texturevpeg;
					boolean attachtobottom = false;
					boolean slopeskew = false; // skew FOF walls with slopes?

					// Wow, how was this missing from OpenGL for so long?
					// ...Oh well, anyway, Lower Unpegged now changes pegging of FOFs like in software
					// -- Monster Iestyn 26/06/18
					if (newline)
					{
						texturevpeg = sides[newline->sidenum[0]].rowoffset;
						attachtobottom = !!(newline->flags & ML_DONTPEGBOTTOM);
						slopeskew = !!(newline->flags & ML_DONTPEGTOP);
					}
					else
					{
						texturevpeg = sides[rover->master->sidenum[0]].rowoffset;
						attachtobottom = !!(gl_linedef->flags & ML_DONTPEGBOTTOM);
						slopeskew = !!(rover->master->flags & ML_DONTPEGTOP);
					}

					grTex = HWR_GetTexture(texnum);

					if (!slopeskew) // no skewing
					{
						if (attachtobottom)
							texturevpeg -= *rover->topheight - *rover->bottomheight;
						wallVerts[3].t = (*rover->topheight - h + texturevpeg) * grTex->scaleY;
						wallVerts[2].t = (*rover->topheight - hS + texturevpeg) * grTex->scaleY;
						wallVerts[0].t = (*rover->topheight - l + texturevpeg) * grTex->scaleY;
						wallVerts[1].t = (*rover->topheight - lS + texturevpeg) * grTex->scaleY;
					}
					else
					{
						if (!attachtobottom) // skew by top
						{
							wallVerts[3].t = wallVerts[2].t = texturevpeg * grTex->scaleY;
							wallVerts[0].t = (h - l + texturevpeg) * grTex->scaleY;
							wallVerts[1].t = (hS - lS + texturevpeg) * grTex->scaleY;
						}
						else // skew by bottom
						{
							wallVerts[0].t = wallVerts[1].t = texturevpeg * grTex->scaleY;
							wallVerts[3].t = wallVerts[0].t - (h - l) * grTex->scaleY;
							wallVerts[2].t = wallVerts[1].t - (hS - lS) * grTex->scaleY;
						}
					}

					wallVerts[0].s = wallVerts[3].s = cliplow * grTex->scaleX;
					wallVerts[2].s = wallVerts[1].s = cliphigh * grTex->scaleX;
				}
				if (rover->flags & FF_FOG)
				{
					FBITFIELD blendmode;

					blendmode = PF_Fog|PF_NoTexture;

					lightnum = rover->master->frontsector->lightlevel;
					colormap = rover->master->frontsector->extra_colormap;
					lightnum = colormap ? lightnum : HWR_CalcWallLight(lightnum, vs.x, vs.y, ve.x, ve.y);

					Surf.PolyColor.s.alpha = HWR_FogBlockAlpha(rover->master->frontsector->lightlevel, rover->master->frontsector->extra_colormap);

					if (gl_frontsector->numlights)
						HWR_SplitWall(gl_frontsector, wallVerts, 0, &Surf, rover->flags, rover, blendmode);
					else
						HWR_AddTransparentWall(wallVerts, &Surf, 0, blendmode, true, lightnum, colormap);
				}
				else
				{
					FBITFIELD blendmode = PF_Masked;

					if ((rover->flags & FF_TRANSLUCENT && rover->alpha < 256) || rover->blend)
					{
						blendmode = rover->blend ? HWR_GetBlendModeFlag(rover->blend) : PF_Translucent;
						Surf.PolyColor.s.alpha = (UINT8)rover->alpha-1 > 255 ? 255 : rover->alpha-1;
					}

					if (gl_frontsector->numlights)
						HWR_SplitWall(gl_frontsector, wallVerts, texnum, &Surf, rover->flags, rover, blendmode);
					else
					{
						if (blendmode != PF_Masked)
							HWR_AddTransparentWall(wallVerts, &Surf, texnum, blendmode, false, lightnum, colormap);
						else
							HWR_ProjectWall(wallVerts, &Surf, PF_Masked, lightnum, colormap);
					}
				}
			}
		}

		if (gl_frontsector->ffloors) // Putting this seperate should allow 2 FOF sectors to be connected without too many errors? I think?
		{
			for (rover = gl_frontsector->ffloors; rover; rover = rover->next)
			{
				boolean bothsides = false;
				// Skip if it exists on both sectors.
				ffloor_t * r2;
				for (r2 = gl_backsector->ffloors; r2; r2 = r2->next)
					if (rover->master == r2->master)
					{
						bothsides = true;
						break;
					}

				if (bothsides) continue;

				if (!(rover->flags & FF_EXISTS) || !(rover->flags & FF_RENDERSIDES))
					continue;
				if (!(rover->flags & FF_ALLSIDES || rover->flags & FF_INVERTSIDES))
					continue;

				SLOPEPARAMS(*rover->t_slope, high1, highslope1, *rover->topheight)
				SLOPEPARAMS(*rover->b_slope, low1,  lowslope1,  *rover->bottomheight)

				if ((high1 < lowcut && highslope1 < lowcutslope) || (low1 > highcut && lowslope1 > highcutslope))
					continue;

				texnum = R_GetTextureNum(sides[rover->master->sidenum[0]].midtexture);

				if (rover->master->flags & ML_TFERLINE)
				{
					size_t linenum = gl_curline->linedef-gl_backsector->lines[0];
					newline = rover->master->frontsector->lines[0] + linenum;
					texnum = R_GetTextureNum(sides[newline->sidenum[0]].midtexture);
				}
				h  = P_GetFFloorTopZAt   (rover, v1x, v1y);
				hS = P_GetFFloorTopZAt   (rover, v2x, v2y);
				l  = P_GetFFloorBottomZAt(rover, v1x, v1y);
				lS = P_GetFFloorBottomZAt(rover, v2x, v2y);
				// Adjust the heights so the FOF does not overlap with top and bottom textures.
				if (h >= highcut && hS >= highcutslope)
				{
					h = highcut;
					hS = highcutslope;
				}
				if (l <= lowcut && lS <= lowcutslope)
				{
					l = lowcut;
					lS = lowcutslope;
				}
				//Hurdler: HW code starts here
				//FIXME: check if peging is correct
				// set top/bottom coords

				wallVerts[3].y = FIXED_TO_FLOAT(h);
				wallVerts[2].y = FIXED_TO_FLOAT(hS);
				wallVerts[0].y = FIXED_TO_FLOAT(l);
				wallVerts[1].y = FIXED_TO_FLOAT(lS);
				if (rover->flags & FF_FOG)
				{
					wallVerts[3].t = wallVerts[2].t = 0;
					wallVerts[0].t = wallVerts[1].t = 0;
					wallVerts[0].s = wallVerts[3].s = 0;
					wallVerts[2].s = wallVerts[1].s = 0;
				}
				else
				{
					grTex = HWR_GetTexture(texnum);

					if (newline)
					{
						wallVerts[3].t = wallVerts[2].t = (*rover->topheight - h + sides[newline->sidenum[0]].rowoffset) * grTex->scaleY;
						wallVerts[0].t = wallVerts[1].t = (h - l + (*rover->topheight - h + sides[newline->sidenum[0]].rowoffset)) * grTex->scaleY;
					}
					else
					{
						wallVerts[3].t = wallVerts[2].t = (*rover->topheight - h + sides[rover->master->sidenum[0]].rowoffset) * grTex->scaleY;
						wallVerts[0].t = wallVerts[1].t = (h - l + (*rover->topheight - h + sides[rover->master->sidenum[0]].rowoffset)) * grTex->scaleY;
					}

					wallVerts[0].s = wallVerts[3].s = cliplow * grTex->scaleX;
					wallVerts[2].s = wallVerts[1].s = cliphigh * grTex->scaleX;
				}

				if (rover->flags & FF_FOG)
				{
					FBITFIELD blendmode;

					blendmode = PF_Fog|PF_NoTexture;

					lightnum = rover->master->frontsector->lightlevel;
					colormap = rover->master->frontsector->extra_colormap;
					lightnum = colormap ? lightnum : HWR_CalcWallLight(lightnum, vs.x, vs.y, ve.x, ve.y);

					Surf.PolyColor.s.alpha = HWR_FogBlockAlpha(rover->master->frontsector->lightlevel, rover->master->frontsector->extra_colormap);

					if (gl_backsector->numlights)
						HWR_SplitWall(gl_backsector, wallVerts, 0, &Surf, rover->flags, rover, blendmode);
					else
						HWR_AddTransparentWall(wallVerts, &Surf, 0, blendmode, true, lightnum, colormap);
				}
				else
				{
					FBITFIELD blendmode = PF_Masked;

					if ((rover->flags & FF_TRANSLUCENT && rover->alpha < 256) || rover->blend)
					{
						blendmode = rover->blend ? HWR_GetBlendModeFlag(rover->blend) : PF_Translucent;
						Surf.PolyColor.s.alpha = (UINT8)rover->alpha-1 > 255 ? 255 : rover->alpha-1;
					}

					if (gl_backsector->numlights)
						HWR_SplitWall(gl_backsector, wallVerts, texnum, &Surf, rover->flags, rover, blendmode);
					else
					{
						if (blendmode != PF_Masked)
							HWR_AddTransparentWall(wallVerts, &Surf, texnum, blendmode, false, lightnum, colormap);
						else
							HWR_ProjectWall(wallVerts, &Surf, PF_Masked, lightnum, colormap);
					}
				}
			}
		}
	}
#undef SLOPEPARAMS
//Hurdler: end of 3d-floors test
}

// From PrBoom:
//
// e6y: Check whether the player can look beyond this line
//
boolean checkforemptylines = true;
// Don't modify anything here, just check
// Kalaron: Modified for sloped linedefs
static boolean CheckClip(seg_t * seg, sector_t * afrontsector, sector_t * abacksector)
{
	fixed_t frontf1,frontf2, frontc1, frontc2; // front floor/ceiling ends
	fixed_t backf1, backf2, backc1, backc2; // back floor ceiling ends
	boolean bothceilingssky = false, bothfloorssky = false;

	if (abacksector->ceilingpic == skyflatnum && afrontsector->ceilingpic == skyflatnum)
		bothceilingssky = true;
	if (abacksector->floorpic == skyflatnum && afrontsector->floorpic == skyflatnum)
		bothfloorssky = true;

	// GZDoom method of sloped line clipping

	if (afrontsector->f_slope || afrontsector->c_slope || abacksector->f_slope || abacksector->c_slope)
	{
		fixed_t v1x, v1y, v2x, v2y; // the seg's vertexes as fixed_t
		v1x = FLOAT_TO_FIXED(((polyvertex_t *)gl_curline->pv1)->x);
		v1y = FLOAT_TO_FIXED(((polyvertex_t *)gl_curline->pv1)->y);
		v2x = FLOAT_TO_FIXED(((polyvertex_t *)gl_curline->pv2)->x);
		v2y = FLOAT_TO_FIXED(((polyvertex_t *)gl_curline->pv2)->y);
#define SLOPEPARAMS(slope, end1, end2, normalheight) \
		end1 = P_GetZAt(slope, v1x, v1y, normalheight); \
		end2 = P_GetZAt(slope, v2x, v2y, normalheight);

		SLOPEPARAMS(afrontsector->f_slope, frontf1, frontf2, afrontsector->  floorheight)
		SLOPEPARAMS(afrontsector->c_slope, frontc1, frontc2, afrontsector->ceilingheight)
		SLOPEPARAMS( abacksector->f_slope,  backf1,  backf2,  abacksector->  floorheight)
		SLOPEPARAMS( abacksector->c_slope,  backc1,  backc2,  abacksector->ceilingheight)
#undef SLOPEPARAMS
	}
	else
	{
		frontf1 = frontf2 = afrontsector->  floorheight;
		frontc1 = frontc2 = afrontsector->ceilingheight;
		backf1  =  backf2 =  abacksector->  floorheight;
		backc1  =  backc2 =  abacksector->ceilingheight;
	}
	// properly render skies (consider door "open" if both ceilings are sky)
	// same for floors
	if (!bothceilingssky && !bothfloorssky)
	{
		// now check for closed sectors!
		if ((backc1 <= frontf1 && backc2 <= frontf2)
			|| (backf1 >= frontc1 && backf2 >= frontc2))
		{
			checkforemptylines = false;
			return true;
		}

		if (backc1 <= backf1 && backc2 <= backf2)
		{
			// preserve a kind of transparent door/lift special effect:
			if (((backc1 >= frontc1 && backc2 >= frontc2) || seg->sidedef->toptexture)
			&& ((backf1 <= frontf1 && backf2 <= frontf2) || seg->sidedef->bottomtexture))
			{
				checkforemptylines = false;
				return true;
			}
		}
	}

	if (!bothceilingssky) {
		if (backc1 != frontc1 || backc2 != frontc2)
		{
			checkforemptylines = false;
			return false;
		}
	}

	if (!bothfloorssky) {
		if (backf1 != frontf1 || backf2 != frontf2)
		{
			checkforemptylines = false;
			return false;
		}
	}

	return false;
}

// -----------------+
// HWR_AddLine      : Clips the given segment and adds any visible pieces to the line list.
// Notes            : gl_cursectorlight is set to the current subsector -> sector -> light value
//                  : (it may be mixed with the wall's own flat colour in the future ...)
// -----------------+
static void HWR_AddLine(seg_t * line)
{
	angle_t angle1, angle2;

	// SoM: Backsector needs to be run through R_FakeFlat
	static sector_t tempsec;

	fixed_t v1x, v1y, v2x, v2y; // the seg's vertexes as fixed_t
	if (line->polyseg && !(line->polyseg->flags & POF_RENDERSIDES))
		return;

	gl_curline = line;

	v1x = FLOAT_TO_FIXED(((polyvertex_t *)gl_curline->pv1)->x);
	v1y = FLOAT_TO_FIXED(((polyvertex_t *)gl_curline->pv1)->y);
	v2x = FLOAT_TO_FIXED(((polyvertex_t *)gl_curline->pv2)->x);
	v2y = FLOAT_TO_FIXED(((polyvertex_t *)gl_curline->pv2)->y);

	// OPTIMIZE: quickly reject orthogonal back sides.
	angle1 = R_PointToAngle64(v1x, v1y);
	angle2 = R_PointToAngle64(v2x, v2y);

	// PrBoom: Back side, i.e. backface culling - read: endAngle >= startAngle!
	if (angle2 - angle1 < ANGLE_180)
		return;

	// PrBoom: use REAL clipping math YAYYYYYYY!!!

	if (!gld_clipper_SafeCheckRange(angle2, angle1))
	{
		return;
	}

	checkforemptylines = true;

	gl_backsector = line->backsector;

	if (!line->backsector)
	{
		gld_clipper_SafeAddClipRange(angle2, angle1);
	}
	else
	{
		boolean bothceilingssky = false, bothfloorssky = false;

		gl_backsector = R_FakeFlat(gl_backsector, &tempsec, NULL, NULL, true);

		if (gl_backsector->ceilingpic == skyflatnum && gl_frontsector->ceilingpic == skyflatnum)
			bothceilingssky = true;
		if (gl_backsector->floorpic == skyflatnum && gl_frontsector->floorpic == skyflatnum)
			bothfloorssky = true;

		if (bothceilingssky && bothfloorssky) // everything's sky? let's save us a bit of time then
		{
			if (!line->polyseg &&
				!line->sidedef->midtexture
				&& ((!gl_frontsector->ffloors && !gl_backsector->ffloors)
					|| Tag_Compare(&gl_frontsector->tags, &gl_backsector->tags)))
				return; // line is empty, don't even bother
			// treat like wide open window instead
			HWR_ProcessSeg(); // Doesn't need arguments because they're defined globally :D
			return;
		}

		if (CheckClip(line, gl_frontsector, gl_backsector))
		{
			gld_clipper_SafeAddClipRange(angle2, angle1);
			checkforemptylines = false;
		}
		// Reject empty lines used for triggers and special events.
		// Identical floor and ceiling on both sides,
		//  identical light levels on both sides,
		//  and no middle texture.
		if (checkforemptylines && R_IsEmptyLine(line, gl_frontsector, gl_backsector))
			return;
	}

	HWR_ProcessSeg(); // Doesn't need arguments because they're defined globally :D
}

// HWR_CheckBBox
// Checks BSP node/subtree bounding box.
// Returns true
//  if some part of the bbox might be visible.
//
// modified to use local variables

static boolean HWR_CheckBBox(fixed_t *bspcoord)
{
	INT32 boxpos;
	fixed_t px1, py1, px2, py2;
	angle_t angle1, angle2;

	// Find the corners of the box
	// that define the edges from current viewpoint.
	if (dup_viewx <= bspcoord[BOXLEFT])
		boxpos = 0;
	else if (dup_viewx < bspcoord[BOXRIGHT])
		boxpos = 1;
	else
		boxpos = 2;

	if (dup_viewy >= bspcoord[BOXTOP])
		boxpos |= 0;
	else if (dup_viewy > bspcoord[BOXBOTTOM])
		boxpos |= 1<<2;
	else
		boxpos |= 2<<2;

	if (boxpos == 5)
		return true;

	px1 = bspcoord[checkcoord[boxpos][0]];
	py1 = bspcoord[checkcoord[boxpos][1]];
	px2 = bspcoord[checkcoord[boxpos][2]];
	py2 = bspcoord[checkcoord[boxpos][3]];

	angle1 = R_PointToAngle64(px1, py1);
	angle2 = R_PointToAngle64(px2, py2);
	return gld_clipper_SafeCheckRange(angle2, angle1);
}

//
// HWR_AddPolyObjectSegs
//
// haleyjd 02/19/06
// Adds all segs in all polyobjects in the given subsector.
// Modified for hardware rendering.
//
static inline void HWR_AddPolyObjectSegs(void)
{
	size_t i, j;
	seg_t *gl_fakeline = Z_Calloc(sizeof(seg_t), PU_STATIC, NULL);
	polyvertex_t *pv1 = Z_Calloc(sizeof(polyvertex_t), PU_STATIC, NULL);
	polyvertex_t *pv2 = Z_Calloc(sizeof(polyvertex_t), PU_STATIC, NULL);

	// Sort through all the polyobjects
	for (i = 0; i < numpolys; ++i)
	{
		// Render the polyobject's lines
		for (j = 0; j < po_ptrs[i]->segCount; ++j)
		{
			// Copy the info of a polyobject's seg, then convert it to OpenGL floating point
			M_Memcpy(gl_fakeline, po_ptrs[i]->segs[j], sizeof(seg_t));

			// Now convert the line to float and add it to be rendered
			pv1->x = FIXED_TO_FLOAT(gl_fakeline->v1->x);
			pv1->y = FIXED_TO_FLOAT(gl_fakeline->v1->y);
			pv2->x = FIXED_TO_FLOAT(gl_fakeline->v2->x);
			pv2->y = FIXED_TO_FLOAT(gl_fakeline->v2->y);

			gl_fakeline->pv1 = pv1;
			gl_fakeline->pv2 = pv2;

			HWR_AddLine(gl_fakeline);
		}
	}

	// Free temporary data no longer needed
	Z_Free(pv2);
	Z_Free(pv1);
	Z_Free(gl_fakeline);
}

static void HWR_AddPolyObjectPlanes(void)
{
	size_t i;
	sector_t *polyobjsector;
	INT32 light = 0;

	// Polyobject Planes need their own function for drawing because they don't have extrasubsectors by themselves
	// It should be okay because polyobjects should always be convex anyway

	for (i  = 0; i < numpolys; i++)
	{
		polyobjsector = po_ptrs[i]->lines[0]->backsector; // the in-level polyobject sector

		if (!(po_ptrs[i]->flags & POF_RENDERPLANES)) // Only render planes when you should
			continue;

		if (po_ptrs[i]->translucency >= NUMTRANSMAPS)
			continue;

		if (polyobjsector->floorheight <= gl_frontsector->ceilingheight
			&& polyobjsector->floorheight >= gl_frontsector->floorheight
			&& (viewz < polyobjsector->floorheight))
		{
			light = R_GetPlaneLight(gl_frontsector, polyobjsector->floorheight, true);
			if (po_ptrs[i]->translucency > 0)
			{
				FSurfaceInfo Surf;
				FBITFIELD blendmode;
				memset(&Surf, 0x00, sizeof(Surf));
				blendmode = HWR_TranstableToAlpha(po_ptrs[i]->translucency, &Surf);
				HWR_AddTransparentPolyobjectFloor(&levelflats[polyobjsector->floorpic], po_ptrs[i], false, polyobjsector->floorheight,
													(light == -1 ? gl_frontsector->lightlevel : *gl_frontsector->lightlist[light].lightlevel), Surf.PolyColor.s.alpha, polyobjsector, blendmode, (light == -1 ? gl_frontsector->extra_colormap : *gl_frontsector->lightlist[light].extra_colormap));
			}
			else
			{
				HWR_GetLevelFlat(&levelflats[polyobjsector->floorpic]);
				HWR_RenderPolyObjectPlane(po_ptrs[i], false, polyobjsector->floorheight, PF_Occlude,
										(light == -1 ? gl_frontsector->lightlevel : *gl_frontsector->lightlist[light].lightlevel), &levelflats[polyobjsector->floorpic],
										polyobjsector, 255, (light == -1 ? gl_frontsector->extra_colormap : *gl_frontsector->lightlist[light].extra_colormap));
			}
		}

		if (polyobjsector->ceilingheight >= gl_frontsector->floorheight
			&& polyobjsector->ceilingheight <= gl_frontsector->ceilingheight
			&& (viewz > polyobjsector->ceilingheight))
		{
			light = R_GetPlaneLight(gl_frontsector, polyobjsector->ceilingheight, true);
			if (po_ptrs[i]->translucency > 0)
			{
				FSurfaceInfo Surf;
				FBITFIELD blendmode;
				memset(&Surf, 0x00, sizeof(Surf));
				blendmode = HWR_TranstableToAlpha(po_ptrs[i]->translucency, &Surf);
				HWR_AddTransparentPolyobjectFloor(&levelflats[polyobjsector->ceilingpic], po_ptrs[i], true, polyobjsector->ceilingheight,
				                                  (light == -1 ? gl_frontsector->lightlevel : *gl_frontsector->lightlist[light].lightlevel), Surf.PolyColor.s.alpha, polyobjsector, blendmode, (light == -1 ? gl_frontsector->extra_colormap : *gl_frontsector->lightlist[light].extra_colormap));
			}
			else
			{
				HWR_GetLevelFlat(&levelflats[polyobjsector->ceilingpic]);
				HWR_RenderPolyObjectPlane(po_ptrs[i], true, polyobjsector->ceilingheight, PF_Occlude,
				                          (light == -1 ? gl_frontsector->lightlevel : *gl_frontsector->lightlist[light].lightlevel), &levelflats[polyobjsector->ceilingpic],
				                          polyobjsector, 255, (light == -1 ? gl_frontsector->extra_colormap : *gl_frontsector->lightlist[light].extra_colormap));
			}
		}
	}
}

static FBITFIELD HWR_RippleBlend(sector_t *sector, ffloor_t *rover, boolean ceiling)
{
	(void)sector;
	(void)ceiling;
	return /*R_IsRipplePlane(sector, rover, ceiling)*/ (rover->flags & FF_RIPPLE) ? PF_Ripple : 0;
}

// -----------------+
// HWR_Subsector    : Determine floor/ceiling planes.
//                  : Add sprites of things in sector.
//                  : Draw one or more line segments.
// Notes            : Sets gl_cursectorlight to the light of the parent sector, to modulate wall textures
// -----------------+
static void HWR_Subsector(size_t num)
{
	INT16 count;
	seg_t *line;
	subsector_t *sub;
	static sector_t tempsec; //SoM: 4/7/2000
	INT32 floorlightlevel;
	INT32 ceilinglightlevel;
	INT32 locFloorHeight, locCeilingHeight;
	INT32 cullFloorHeight, cullCeilingHeight;
	INT32 light = 0;
	extracolormap_t *floorcolormap;
	extracolormap_t *ceilingcolormap;

#ifdef PARANOIA //no risk while developing, enough debugging nights!
	if (num >= addsubsector)
		I_Error("HWR_Subsector: ss %s with numss = %s, addss = %s\n",
			sizeu1(num), sizeu2(numsubsectors), sizeu3(addsubsector));

	/*if (num >= numsubsectors)
		I_Error("HWR_Subsector: ss %i with numss = %i",
		        num,
		        numsubsectors);*/
#endif

	if (num < numsubsectors)
	{
		// subsector
		sub = &subsectors[num];
		// sector
		gl_frontsector = sub->sector;
		// how many linedefs
		count = sub->numlines;
		// first line seg
		line = &segs[sub->firstline];
	}
	else
	{
		// there are no segs but only planes
		sub = &subsectors[0];
		gl_frontsector = sub->sector;
		count = 0;
		line = NULL;
	}

	//SoM: 4/7/2000: Test to make Boom water work in Hardware mode.
	gl_frontsector = R_FakeFlat(gl_frontsector, &tempsec, &floorlightlevel,
								&ceilinglightlevel, false);
	//FIXME: Use floorlightlevel and ceilinglightlevel insted of lightlevel.

	floorcolormap = ceilingcolormap = gl_frontsector->extra_colormap;

	// ------------------------------------------------------------------------
	// sector lighting, DISABLED because it's done in HWR_StoreWallRange
	// ------------------------------------------------------------------------
	/// \todo store a RGBA instead of just intensity, allow coloured sector lighting
	//light = (FUBYTE)(sub->sector->lightlevel & 0xFF) / 255.0f;
	//gl_cursectorlight.red   = light;
	//gl_cursectorlight.green = light;
	//gl_cursectorlight.blue  = light;
	//gl_cursectorlight.alpha = light;

// ----- end special tricks -----
	cullFloorHeight   = P_GetSectorFloorZAt  (gl_frontsector, viewx, viewy);
	cullCeilingHeight = P_GetSectorCeilingZAt(gl_frontsector, viewx, viewy);
	locFloorHeight    = P_GetSectorFloorZAt  (gl_frontsector, gl_frontsector->soundorg.x, gl_frontsector->soundorg.y);
	locCeilingHeight  = P_GetSectorCeilingZAt(gl_frontsector, gl_frontsector->soundorg.x, gl_frontsector->soundorg.y);

	if (gl_frontsector->ffloors)
	{
		if (gl_frontsector->moved)
		{
			gl_frontsector->numlights = sub->sector->numlights = 0;
			R_Prep3DFloors(gl_frontsector);
			sub->sector->lightlist = gl_frontsector->lightlist;
			sub->sector->numlights = gl_frontsector->numlights;
			sub->sector->moved = gl_frontsector->moved = false;
		}

		light = R_GetPlaneLight(gl_frontsector, locFloorHeight, false);
		if (gl_frontsector->floorlightsec == -1)
			floorlightlevel = *gl_frontsector->lightlist[light].lightlevel;
		floorcolormap = *gl_frontsector->lightlist[light].extra_colormap;

		light = R_GetPlaneLight(gl_frontsector, locCeilingHeight, false);
		if (gl_frontsector->ceilinglightsec == -1)
			ceilinglightlevel = *gl_frontsector->lightlist[light].lightlevel;
		ceilingcolormap = *gl_frontsector->lightlist[light].extra_colormap;
	}

	sub->sector->extra_colormap = gl_frontsector->extra_colormap;

	// render floor ?
	// yeah, easy backface cull! :)
	if (cullFloorHeight < dup_viewz)
	{
		if (gl_frontsector->floorpic != skyflatnum)
		{
			if (sub->validcount != validcount)
			{
				HWR_GetLevelFlat(&levelflats[gl_frontsector->floorpic]);
				HWR_RenderPlane(sub, &extrasubsectors[num], false,
					// Hack to make things continue to work around slopes.
					locFloorHeight == cullFloorHeight ? locFloorHeight : gl_frontsector->floorheight,
					// We now return you to your regularly scheduled rendering.
					PF_Occlude, floorlightlevel, &levelflats[gl_frontsector->floorpic], NULL, 255, floorcolormap);
			}
		}
	}

	if (cullCeilingHeight > dup_viewz)
	{
		if (gl_frontsector->ceilingpic != skyflatnum)
		{
			if (sub->validcount != validcount)
			{
				HWR_GetLevelFlat(&levelflats[gl_frontsector->ceilingpic]);
				HWR_RenderPlane(sub, &extrasubsectors[num], true,
					// Hack to make things continue to work around slopes.
					locCeilingHeight == cullCeilingHeight ? locCeilingHeight : gl_frontsector->ceilingheight,
					// We now return you to your regularly scheduled rendering.
					PF_Occlude, ceilinglightlevel, &levelflats[gl_frontsector->ceilingpic], NULL, 255, ceilingcolormap);
			}
		}
	}

	// Moved here because before, when above the ceiling and the floor does not have the sky flat, it doesn't draw the sky
	if (gl_frontsector->ceilingpic == skyflatnum || gl_frontsector->floorpic == skyflatnum)
		drawsky = true;

	if (gl_frontsector->ffloors)
	{
		/// \todo fix light, xoffs, yoffs, extracolormap ?
		ffloor_t * rover;
		for (rover = gl_frontsector->ffloors;
			rover; rover = rover->next)
		{
			fixed_t cullHeight, centerHeight;

            // bottom plane
			cullHeight   = P_GetFFloorBottomZAt(rover, viewx, viewy);
			centerHeight = P_GetFFloorBottomZAt(rover, gl_frontsector->soundorg.x, gl_frontsector->soundorg.y);

			if (!(rover->flags & FF_EXISTS) || !(rover->flags & FF_RENDERPLANES))
				continue;
			if (sub->validcount == validcount)
				continue;

			if (centerHeight <= locCeilingHeight &&
			    centerHeight >= locFloorHeight &&
			    ((dup_viewz < cullHeight && (rover->flags & FF_BOTHPLANES || !(rover->flags & FF_INVERTPLANES))) ||
			     (dup_viewz > cullHeight && (rover->flags & FF_BOTHPLANES || rover->flags & FF_INVERTPLANES))))
			{
				if (rover->flags & FF_FOG)
				{
					UINT8 alpha;

					light = R_GetPlaneLight(gl_frontsector, centerHeight, dup_viewz < cullHeight ? true : false);
					alpha = HWR_FogBlockAlpha(*gl_frontsector->lightlist[light].lightlevel, rover->master->frontsector->extra_colormap);

					HWR_AddTransparentFloor(0,
					                       &extrasubsectors[num],
										   false,
					                       *rover->bottomheight,
					                       *gl_frontsector->lightlist[light].lightlevel,
					                       alpha, rover->master->frontsector, PF_Fog|PF_NoTexture,
										   true, rover->master->frontsector->extra_colormap);
				}
				else if ((rover->flags & FF_TRANSLUCENT && rover->alpha < 256) || rover->blend) // SoM: Flags are more efficient
				{
					light = R_GetPlaneLight(gl_frontsector, centerHeight, dup_viewz < cullHeight ? true : false);

					HWR_AddTransparentFloor(&levelflats[*rover->bottompic],
					                       &extrasubsectors[num],
										   false,
					                       *rover->bottomheight,
					                       *gl_frontsector->lightlist[light].lightlevel,
					                       rover->alpha-1 > 255 ? 255 : rover->alpha-1, rover->master->frontsector,
					                       HWR_RippleBlend(gl_frontsector, rover, false) | (rover->blend ? HWR_GetBlendModeFlag(rover->blend) : PF_Translucent),
					                       false, *gl_frontsector->lightlist[light].extra_colormap);
				}
				else
				{
					HWR_GetLevelFlat(&levelflats[*rover->bottompic]);
					light = R_GetPlaneLight(gl_frontsector, centerHeight, dup_viewz < cullHeight ? true : false);
					HWR_RenderPlane(sub, &extrasubsectors[num], false, *rover->bottomheight, HWR_RippleBlend(gl_frontsector, rover, false)|PF_Occlude, *gl_frontsector->lightlist[light].lightlevel, &levelflats[*rover->bottompic],
					                rover->master->frontsector, 255, *gl_frontsector->lightlist[light].extra_colormap);
				}
			}

			// top plane
			cullHeight   = P_GetFFloorTopZAt(rover, viewx, viewy);
			centerHeight = P_GetFFloorTopZAt(rover, gl_frontsector->soundorg.x, gl_frontsector->soundorg.y);

			if (centerHeight >= locFloorHeight &&
			    centerHeight <= locCeilingHeight &&
			    ((dup_viewz > cullHeight && (rover->flags & FF_BOTHPLANES || !(rover->flags & FF_INVERTPLANES))) ||
			     (dup_viewz < cullHeight && (rover->flags & FF_BOTHPLANES || rover->flags & FF_INVERTPLANES))))
			{
				if (rover->flags & FF_FOG)
				{
					UINT8 alpha;

					light = R_GetPlaneLight(gl_frontsector, centerHeight, dup_viewz < cullHeight ? true : false);
					alpha = HWR_FogBlockAlpha(*gl_frontsector->lightlist[light].lightlevel, rover->master->frontsector->extra_colormap);

					HWR_AddTransparentFloor(0,
					                       &extrasubsectors[num],
										   true,
					                       *rover->topheight,
					                       *gl_frontsector->lightlist[light].lightlevel,
					                       alpha, rover->master->frontsector, PF_Fog|PF_NoTexture,
										   true, rover->master->frontsector->extra_colormap);
				}
				else if ((rover->flags & FF_TRANSLUCENT && rover->alpha < 256) || rover->blend)
				{
					light = R_GetPlaneLight(gl_frontsector, centerHeight, dup_viewz < cullHeight ? true : false);

					HWR_AddTransparentFloor(&levelflats[*rover->toppic],
					                        &extrasubsectors[num],
											true,
					                        *rover->topheight,
					                        *gl_frontsector->lightlist[light].lightlevel,
					                        rover->alpha-1 > 255 ? 255 : rover->alpha-1, rover->master->frontsector,
 					                        HWR_RippleBlend(gl_frontsector, rover, false) | (rover->blend ? HWR_GetBlendModeFlag(rover->blend) : PF_Translucent),
					                        false, *gl_frontsector->lightlist[light].extra_colormap);
				}
				else
				{
					HWR_GetLevelFlat(&levelflats[*rover->toppic]);
					light = R_GetPlaneLight(gl_frontsector, centerHeight, dup_viewz < cullHeight ? true : false);
					HWR_RenderPlane(sub, &extrasubsectors[num], true, *rover->topheight, HWR_RippleBlend(gl_frontsector, rover, false)|PF_Occlude, *gl_frontsector->lightlist[light].lightlevel, &levelflats[*rover->toppic],
					                  rover->master->frontsector, 255, *gl_frontsector->lightlist[light].extra_colormap);
				}
			}
		}
	}

	// Draw all the polyobjects in this subsector
	if (sub->polyList)
	{
		polyobj_t *po = sub->polyList;

		numpolys = 0;

		// Count all the polyobjects, reset the list, and recount them
		while (po)
		{
			++numpolys;
			po = (polyobj_t *)(po->link.next);
		}

		// for render stats
		ps_numpolyobjects.value.i += numpolys;

		// Sort polyobjects
		R_SortPolyObjects(sub);

		// Draw polyobject lines.
		HWR_AddPolyObjectSegs();

		if (sub->validcount != validcount) // This validcount situation seems to let us know that the floors have already been drawn.
		{
			// Draw polyobject planes
			HWR_AddPolyObjectPlanes();
		}
	}

// Hurder ici se passe les choses INT32�essantes!
// on vient de tracer le sol et le plafond
// on trace �pr�ent d'abord les sprites et ensuite les murs
// hurdler: faux: on ajoute seulement les sprites, le murs sont trac� d'abord
	if (line)
	{
		// draw sprites first, coz they are clipped to the solidsegs of
		// subsectors more 'in front'
		HWR_AddSprites(gl_frontsector);

		//Hurdler: at this point validcount must be the same, but is not because
		//         gl_frontsector doesn't point anymore to sub->sector due to
		//         the call gl_frontsector = R_FakeFlat(...)
		//         if it's not done, the sprite is drawn more than once,
		//         what looks really bad with translucency or dynamic light,
		//         without talking about the overdraw of course.
		sub->sector->validcount = validcount;/// \todo fix that in a better way

		while (count--)
		{

			if (!line->glseg && !line->polyseg) // ignore segs that belong to polyobjects
				HWR_AddLine(line);
			line++;
		}
	}

	sub->validcount = validcount;
}

//
// Renders all subsectors below a given node,
//  traversing subtree recursively.
// Just call with BSP root.

// BP: big hack for a test in lighning ref : 1249753487AB
fixed_t *hwbbox;

static void HWR_RenderBSPNode(INT32 bspnum)
{
	/*//GZDoom code
	if(bspnum == -1)
	{
		HWR_Subsector(subsectors);
		return;
	}
	while(!((size_t)bspnum&(~NF_SUBSECTOR))) // Keep going until found a subsector
	{
		node_t *bsp = &nodes[bspnum];

		// Decide which side the view point is on
		INT32 side = R_PointOnSide(dup_viewx, dup_viewy, bsp);

		// Recursively divide front space (toward the viewer)
		HWR_RenderBSPNode(bsp->children[side]);

		// Possibly divide back space (away from viewer)
		side ^= 1;

		if (!HWR_CheckBBox(bsp->bbox[side]))
			return;

		bspnum = bsp->children[side];
	}

	HWR_Subsector(bspnum-1);
*/
	node_t *bsp = &nodes[bspnum];

	// Decide which side the view point is on
	INT32 side;

	ps_numbspcalls.value.i++;

	// Found a subsector?
	if (bspnum & NF_SUBSECTOR)
	{
		if (bspnum == -1)
		{
			//*(gl_drawsubsector_p++) = 0;
			HWR_Subsector(0);
		}
		else
		{
			//*(gl_drawsubsector_p++) = bspnum&(~NF_SUBSECTOR);
			HWR_Subsector(bspnum&(~NF_SUBSECTOR));
		}
		return;
	}

	// Decide which side the view point is on.
	side = R_PointOnSide(dup_viewx, dup_viewy, bsp);

	// BP: big hack for a test in lighning ref : 1249753487AB
	hwbbox = bsp->bbox[side];

	// Recursively divide front space.
	HWR_RenderBSPNode(bsp->children[side]);

	// Possibly divide back space.
	if (HWR_CheckBBox(bsp->bbox[side^1]))
	{
		// BP: big hack for a test in lighning ref : 1249753487AB
		hwbbox = bsp->bbox[side^1];
		HWR_RenderBSPNode(bsp->children[side^1]);
	}
}

/*
//
// Clear 'stack' of subsectors to draw
//
static void HWR_ClearDrawSubsectors(void)
{
	gl_drawsubsector_p = gl_drawsubsectors;
}

//
// Draw subsectors pushed on the drawsubsectors 'stack', back to front
//
static void HWR_RenderSubsectors(void)
{
	while (gl_drawsubsector_p > gl_drawsubsectors)
	{
		HWR_RenderBSPNode(
		lastsubsec->nextsubsec = bspnum & (~NF_SUBSECTOR);
	}
}
*/

// A drawnode is something that points to a 3D floor, 3D side, or masked
// middle texture. This is used for sorting with sprites.
typedef struct
{
	FOutVector    wallVerts[4];
	FSurfaceInfo  Surf;
	INT32         texnum;
	FBITFIELD     blend;
	INT32         drawcount;
	boolean fogwall;
	INT32 lightlevel;
	extracolormap_t *wallcolormap; // Doing the lighting in HWR_RenderWall now for correct fog after sorting
} wallinfo_t;

static wallinfo_t *wallinfo = NULL;
static size_t numwalls = 0; // a list of transparent walls to be drawn

void HWR_RenderWall(FOutVector *wallVerts, FSurfaceInfo *pSurf, FBITFIELD blend, boolean fogwall, INT32 lightlevel, extracolormap_t *wallcolormap);

#define MAX_TRANSPARENTWALL 256

typedef struct
{
	extrasubsector_t *xsub;
	boolean isceiling;
	fixed_t fixedheight;
	INT32 lightlevel;
	levelflat_t *levelflat;
	INT32 alpha;
	sector_t *FOFSector;
	FBITFIELD blend;
	boolean fogplane;
	extracolormap_t *planecolormap;
	INT32 drawcount;
} planeinfo_t;

static size_t numplanes = 0; // a list of transparent floors to be drawn
static planeinfo_t *planeinfo = NULL;

typedef struct
{
	polyobj_t *polysector;
	boolean isceiling;
	fixed_t fixedheight;
	INT32 lightlevel;
	levelflat_t *levelflat;
	INT32 alpha;
	sector_t *FOFSector;
	FBITFIELD blend;
	extracolormap_t *planecolormap;
	INT32 drawcount;
} polyplaneinfo_t;

static size_t numpolyplanes = 0; // a list of transparent poyobject floors to be drawn
static polyplaneinfo_t *polyplaneinfo = NULL;

//Hurdler: 3D water sutffs
typedef struct gl_drawnode_s
{
	planeinfo_t *plane;
	polyplaneinfo_t *polyplane;
	wallinfo_t *wall;
	gl_vissprite_t *sprite;

//	struct gl_drawnode_s *next;
//	struct gl_drawnode_s *prev;
} gl_drawnode_t;

static INT32 drawcount = 0;

#define MAX_TRANSPARENTFLOOR 512

// This will likely turn into a copy of HWR_Add3DWater and replace it.
void HWR_AddTransparentFloor(levelflat_t *levelflat, extrasubsector_t *xsub, boolean isceiling, fixed_t fixedheight, INT32 lightlevel, INT32 alpha, sector_t *FOFSector, FBITFIELD blend, boolean fogplane, extracolormap_t *planecolormap)
{
	static size_t allocedplanes = 0;

	// Force realloc if buffer has been freed
	if (!planeinfo)
		allocedplanes = 0;

	if (allocedplanes < numplanes + 1)
	{
		allocedplanes += MAX_TRANSPARENTFLOOR;
		Z_Realloc(planeinfo, allocedplanes * sizeof (*planeinfo), PU_LEVEL, &planeinfo);
	}

	planeinfo[numplanes].isceiling = isceiling;
	planeinfo[numplanes].fixedheight = fixedheight;
	planeinfo[numplanes].lightlevel = (planecolormap && (planecolormap->flags & CMF_FOG)) ? lightlevel : 255;
	planeinfo[numplanes].levelflat = levelflat;
	planeinfo[numplanes].xsub = xsub;
	planeinfo[numplanes].alpha = alpha;
	planeinfo[numplanes].FOFSector = FOFSector;
	planeinfo[numplanes].blend = blend;
	planeinfo[numplanes].fogplane = fogplane;
	planeinfo[numplanes].planecolormap = planecolormap;
	planeinfo[numplanes].drawcount = drawcount++;

	numplanes++;
}

// Adding this for now until I can create extrasubsector info for polyobjects
// When that happens it'll just be done through HWR_AddTransparentFloor and HWR_RenderPlane
void HWR_AddTransparentPolyobjectFloor(levelflat_t *levelflat, polyobj_t *polysector, boolean isceiling, fixed_t fixedheight, INT32 lightlevel, INT32 alpha, sector_t *FOFSector, FBITFIELD blend, extracolormap_t *planecolormap)
{
	static size_t allocedpolyplanes = 0;

	// Force realloc if buffer has been freed
	if (!polyplaneinfo)
		allocedpolyplanes = 0;

	if (allocedpolyplanes < numpolyplanes + 1)
	{
		allocedpolyplanes += MAX_TRANSPARENTFLOOR;
		Z_Realloc(polyplaneinfo, allocedpolyplanes * sizeof (*polyplaneinfo), PU_LEVEL, &polyplaneinfo);
	}

	polyplaneinfo[numpolyplanes].isceiling = isceiling;
	polyplaneinfo[numpolyplanes].fixedheight = fixedheight;
	polyplaneinfo[numpolyplanes].lightlevel = (planecolormap && (planecolormap->flags & CMF_FOG)) ? lightlevel : 255;
	polyplaneinfo[numpolyplanes].levelflat = levelflat;
	polyplaneinfo[numpolyplanes].polysector = polysector;
	polyplaneinfo[numpolyplanes].alpha = alpha;
	polyplaneinfo[numpolyplanes].FOFSector = FOFSector;
	polyplaneinfo[numpolyplanes].blend = blend;
	polyplaneinfo[numpolyplanes].planecolormap = planecolormap;
	polyplaneinfo[numpolyplanes].drawcount = drawcount++;
	numpolyplanes++;
}

// putting sortindex and sortnode here so the comparator function can see them
gl_drawnode_t *sortnode;
size_t *sortindex;

static int CompareDrawNodes(const void *p1, const void *p2)
{
	size_t n1 = *(const size_t*)p1;
	size_t n2 = *(const size_t*)p2;
	INT32 v1 = 0;
	INT32 v2 = 0;
	INT32 diff;
	if (sortnode[n1].plane)
		v1 = sortnode[n1].plane->drawcount;
	else if (sortnode[n1].polyplane)
		v1 = sortnode[n1].polyplane->drawcount;
	else if (sortnode[n1].wall)
		v1 = sortnode[n1].wall->drawcount;
	else I_Error("CompareDrawNodes: n1 unknown");

	if (sortnode[n2].plane)
		v2 = sortnode[n2].plane->drawcount;
	else if (sortnode[n2].polyplane)
		v2 = sortnode[n2].polyplane->drawcount;
	else if (sortnode[n2].wall)
		v2 = sortnode[n2].wall->drawcount;
	else I_Error("CompareDrawNodes: n2 unknown");

	diff = v2 - v1;
	if (diff == 0) I_Error("CompareDrawNodes: diff is zero");
	return diff;
}

static int CompareDrawNodePlanes(const void *p1, const void *p2)
{
	size_t n1 = *(const size_t*)p1;
	size_t n2 = *(const size_t*)p2;
	if (!sortnode[n1].plane) I_Error("CompareDrawNodePlanes: Uh.. This isn't a plane! (n1)");
	if (!sortnode[n2].plane) I_Error("CompareDrawNodePlanes: Uh.. This isn't a plane! (n2)");
	return ABS(sortnode[n2].plane->fixedheight - viewz) - ABS(sortnode[n1].plane->fixedheight - viewz);
}

//
// HWR_CreateDrawNodes
// Creates and sorts a list of drawnodes for the scene being rendered.
static void HWR_CreateDrawNodes(void)
{
	UINT32 i = 0, p = 0;
	size_t run_start = 0;

	// Dump EVERYTHING into a huge drawnode list. Then we'll sort it!
	// Could this be optimized into _AddTransparentWall/_AddTransparentPlane?
	// Hell yes! But sort algorithm must be modified to use a linked list.
	sortnode = Z_Calloc((sizeof(planeinfo_t)*numplanes)
					+ (sizeof(polyplaneinfo_t)*numpolyplanes)
					+ (sizeof(wallinfo_t)*numwalls)
					,PU_STATIC, NULL);
	// todo:
	// However, in reality we shouldn't be re-copying and shifting all this information
	// that is already lying around. This should all be in some sort of linked list or lists.
	sortindex = Z_Calloc(sizeof(size_t) * (numplanes + numpolyplanes + numwalls), PU_STATIC, NULL);

	PS_START_TIMING(ps_hw_nodesorttime);

	for (i = 0; i < numplanes; i++, p++)
	{
		sortnode[p].plane = &planeinfo[i];
		sortindex[p] = p;
	}

	for (i = 0; i < numpolyplanes; i++, p++)
	{
		sortnode[p].polyplane = &polyplaneinfo[i];
		sortindex[p] = p;
	}

	for (i = 0; i < numwalls; i++, p++)
	{
		sortnode[p].wall = &wallinfo[i];
		sortindex[p] = p;
	}

	ps_numdrawnodes.value.i = p;

	// p is the number of stuff to sort

	// sort the list based on the value of the 'drawcount' member of the drawnodes.
	qsort(sortindex, p, sizeof(size_t), CompareDrawNodes);

	// an additional pass is needed to correct the order of consecutive planes in the list.
	// for each consecutive run of planes in the list, sort that run based on plane height and view height.
	while (run_start < p-1)// p-1 because a 1 plane run at the end of the list does not count
	{
		// locate run start
		if (sortnode[sortindex[run_start]].plane)
		{
			// found it, now look for run end
			size_t run_end;// (inclusive)
			for (i = run_start+1; i < p; i++)// size_t and UINT32 being used mixed here... shouldnt break anything though..
			{
				if (!sortnode[sortindex[i]].plane) break;
			}
			run_end = i-1;
			if (run_end > run_start)// if there are multiple consecutive planes, not just one
			{
				// consecutive run of planes found, now sort it
				qsort(sortindex + run_start, run_end - run_start + 1, sizeof(size_t), CompareDrawNodePlanes);
			}
			run_start = run_end + 1;// continue looking for runs coming right after this one
		}
		else
		{
			// this wasnt the run start, try next one
			run_start++;
		}
	}

	PS_STOP_TIMING(ps_hw_nodesorttime);

	PS_START_TIMING(ps_hw_nodedrawtime);

	// Okay! Let's draw it all! Woo!
	HWD.pfnSetTransform(&atransform);

	for (i = 0; i < p; i++)
	{
		if (sortnode[sortindex[i]].plane)
		{
			// We aren't traversing the BSP tree, so make gl_frontsector null to avoid crashes.
			gl_frontsector = NULL;

			if (!(sortnode[sortindex[i]].plane->blend & PF_NoTexture))
				HWR_GetLevelFlat(sortnode[sortindex[i]].plane->levelflat);
			HWR_RenderPlane(NULL, sortnode[sortindex[i]].plane->xsub, sortnode[sortindex[i]].plane->isceiling, sortnode[sortindex[i]].plane->fixedheight, sortnode[sortindex[i]].plane->blend, sortnode[sortindex[i]].plane->lightlevel,
				sortnode[sortindex[i]].plane->levelflat, sortnode[sortindex[i]].plane->FOFSector, sortnode[sortindex[i]].plane->alpha, sortnode[sortindex[i]].plane->planecolormap);
		}
		else if (sortnode[sortindex[i]].polyplane)
		{
			// We aren't traversing the BSP tree, so make gl_frontsector null to avoid crashes.
			gl_frontsector = NULL;

			if (!(sortnode[sortindex[i]].polyplane->blend & PF_NoTexture))
				HWR_GetLevelFlat(sortnode[sortindex[i]].polyplane->levelflat);
			HWR_RenderPolyObjectPlane(sortnode[sortindex[i]].polyplane->polysector, sortnode[sortindex[i]].polyplane->isceiling, sortnode[sortindex[i]].polyplane->fixedheight, sortnode[sortindex[i]].polyplane->blend, sortnode[sortindex[i]].polyplane->lightlevel,
				sortnode[sortindex[i]].polyplane->levelflat, sortnode[sortindex[i]].polyplane->FOFSector, sortnode[sortindex[i]].polyplane->alpha, sortnode[sortindex[i]].polyplane->planecolormap);
		}
		else if (sortnode[sortindex[i]].wall)
		{
			if (!(sortnode[sortindex[i]].wall->blend & PF_NoTexture))
				HWR_GetTexture(sortnode[sortindex[i]].wall->texnum);
			HWR_RenderWall(sortnode[sortindex[i]].wall->wallVerts, &sortnode[sortindex[i]].wall->Surf, sortnode[sortindex[i]].wall->blend, sortnode[sortindex[i]].wall->fogwall,
				sortnode[sortindex[i]].wall->lightlevel, sortnode[sortindex[i]].wall->wallcolormap);
		}
	}

	PS_STOP_TIMING(ps_hw_nodedrawtime);

	numwalls = 0;
	numplanes = 0;
	numpolyplanes = 0;

	// No mem leaks, please.
	Z_Free(sortnode);
	Z_Free(sortindex);
}

// -----------------+
// HWR_ClearView : clear the viewwindow, with maximum z value
// -----------------+
static inline void HWR_ClearView(void)
{
	//  3--2
	//  | /|
	//  |/ |
	//  0--1

	/// \bug faB - enable depth mask, disable color mask

	HWD.pfnGClipRect((INT32)gl_viewwindowx,
	                 (INT32)gl_viewwindowy,
	                 (INT32)(gl_viewwindowx + gl_viewwidth),
	                 (INT32)(gl_viewwindowy + gl_viewheight),
	                 ZCLIP_PLANE);
	HWD.pfnClearBuffer(false, true, 0);

	//disable clip window - set to full size
	// rem by Hurdler
	// HWD.pfnGClipRect(0, 0, vid.width, vid.height);
}


// -----------------+
// HWR_SetViewSize  : set projection and scaling values
// -----------------+
void HWR_SetViewSize(void)
{
	// setup view size
	gl_viewwidth = (float)vid.width;
	gl_viewheight = (float)vid.height;

	if (splitscreen)
		gl_viewheight /= 2;

	gl_centerx = gl_viewwidth / 2;
	gl_basecentery = gl_viewheight / 2; //note: this is (gl_centerx * gl_viewheight / gl_viewwidth)

	gl_viewwindowx = (vid.width - gl_viewwidth) / 2;
	gl_windowcenterx = (float)(vid.width / 2);
	if (fabsf(gl_viewwidth - vid.width) < 1.0E-36f)
	{
		gl_baseviewwindowy = 0;
		gl_basewindowcentery = gl_viewheight / 2;               // window top left corner at 0,0
	}
	else
	{
		gl_baseviewwindowy = (vid.height-gl_viewheight) / 2;
		gl_basewindowcentery = (float)(vid.height / 2);
	}

	gl_pspritexscale = gl_viewwidth / BASEVIDWIDTH;
	gl_pspriteyscale = ((vid.height*gl_pspritexscale*BASEVIDWIDTH)/BASEVIDHEIGHT)/vid.width;

	HWD.pfnFlushScreenTextures();
}

// Set view aiming, for the sky dome, the skybox,
// and the normal view, all with a single function.
void HWR_SetTransformAiming(FTransform *trans, player_t *player, boolean skybox)
{
	// 1 = always on
	// 2 = chasecam only
	if (cv_glshearing.value == 1 || (cv_glshearing.value == 2 && R_IsViewpointThirdPerson(player, skybox)))
	{
		fixed_t fixedaiming = AIMINGTODY(aimingangle);
		trans->viewaiming = FIXED_TO_FLOAT(fixedaiming);
		trans->shearing = true;
		gl_aimingangle = 0;
	}
	else
	{
		trans->shearing = false;
		gl_aimingangle = aimingangle;
	}

	trans->anglex = (float)(gl_aimingangle>>ANGLETOFINESHIFT)*(360.0f/(float)FINEANGLES);
}

//
// Sets the shader state.
//
static void HWR_SetShaderState(void)
{
	HWD.pfnSetSpecialState(HWD_SET_SHADERS, (INT32)HWR_UseShader());
}

// ==========================================================================
// Same as rendering the player view, but from the skybox object
// ==========================================================================
void HWR_RenderSkyboxView(INT32 viewnumber, player_t *player)
{
	const float fpov = FIXED_TO_FLOAT(cv_fov.value+player->fovadd);
	postimg_t *type;

	if (splitscreen && player == &players[secondarydisplayplayer])
		type = &postimgtype2;
	else
		type = &postimgtype;

	if (!HWR_ShouldUsePaletteRendering())
	{
		// do we really need to save player (is it not the same)?
		player_t *saved_player = stplyr;
		stplyr = player;
		ST_doPaletteStuff();
		stplyr = saved_player;
#ifdef ALAM_LIGHTING
		HWR_SetLights(viewnumber);
#endif
	}

	// note: sets viewangle, viewx, viewy, viewz
	R_SkyboxFrame(player);

	// copy view cam position for local use
	dup_viewx = viewx;
	dup_viewy = viewy;
	dup_viewz = viewz;
	dup_viewangle = viewangle;

	// set window position
	gl_centery = gl_basecentery;
	gl_viewwindowy = gl_baseviewwindowy;
	gl_windowcentery = gl_basewindowcentery;
	if (splitscreen && viewnumber == 1)
	{
		gl_viewwindowy += (vid.height/2);
		gl_windowcentery += (vid.height/2);
	}

	// check for new console commands.
	NetUpdate();

	gl_viewx = FIXED_TO_FLOAT(dup_viewx);
	gl_viewy = FIXED_TO_FLOAT(dup_viewy);
	gl_viewz = FIXED_TO_FLOAT(dup_viewz);
	gl_viewsin = FIXED_TO_FLOAT(viewsin);
	gl_viewcos = FIXED_TO_FLOAT(viewcos);

	//04/01/2000: Hurdler: added for T&L
	//                     It should replace all other gl_viewxxx when finished
	memset(&atransform, 0x00, sizeof(FTransform));

	HWR_SetTransformAiming(&atransform, player, true);
	atransform.angley = (float)(viewangle>>ANGLETOFINESHIFT)*(360.0f/(float)FINEANGLES);

	gl_viewludsin = FIXED_TO_FLOAT(FINECOSINE(gl_aimingangle>>ANGLETOFINESHIFT));
	gl_viewludcos = FIXED_TO_FLOAT(-FINESINE(gl_aimingangle>>ANGLETOFINESHIFT));

	if (*type == postimg_flip)
		atransform.flip = true;
	else
		atransform.flip = false;

	atransform.x      = gl_viewx;  // FIXED_TO_FLOAT(viewx)
	atransform.y      = gl_viewy;  // FIXED_TO_FLOAT(viewy)
	atransform.z      = gl_viewz;  // FIXED_TO_FLOAT(viewz)
	atransform.scalex = 1;
	atransform.scaley = (float)vid.width/vid.height;
	atransform.scalez = 1;

	atransform.fovxangle = fpov; // Tails
	atransform.fovyangle = fpov; // Tails
	if (player->viewrollangle != 0)
	{
		fixed_t rol = AngleFixed(player->viewrollangle);
		atransform.rollangle = FIXED_TO_FLOAT(rol);
		atransform.roll = true;
	}
	atransform.splitscreen = splitscreen;

	gl_fovlud = (float)(1.0l/tan((double)(fpov*M_PIl/360l)));

	//------------------------------------------------------------------------
	HWR_ClearView();

	if (drawsky)
		HWR_DrawSkyBackground(player);

	//Hurdler: it doesn't work in splitscreen mode
	drawsky = splitscreen;

	HWR_ClearSprites();

	drawcount = 0;

	if (rendermode == render_opengl)
	{
		angle_t a1 = gld_FrustumAngle(gl_aimingangle);
		gld_clipper_Clear();
		gld_clipper_SafeAddClipRange(viewangle + a1, viewangle - a1);
#ifdef HAVE_SPHEREFRUSTRUM
		gld_FrustrumSetup();
#endif
	}

	//04/01/2000: Hurdler: added for T&L
	//                     Actually it only works on Walls and Planes
	HWD.pfnSetTransform(&atransform);

	// Reset the shader state.
	HWR_SetShaderState();

	validcount++;

	if (cv_glbatching.value)
		HWR_StartBatching();

	HWR_RenderBSPNode((INT32)numnodes-1);

	if (cv_glbatching.value)
		HWR_RenderBatches();

	// Check for new console commands.
	NetUpdate();

#ifdef ALAM_LIGHTING
	//14/11/99: Hurdler: moved here because it doesn't work with
	// subsector, see other comments;
	HWR_ResetLights();
#endif

	// Draw MD2 and sprites
	HWR_SortVisSprites();
	HWR_DrawSprites();

#ifdef NEWCORONAS
	//Hurdler: they must be drawn before translucent planes, what about gl fog?
	HWR_DrawCoronas();
#endif

	if (numplanes || numpolyplanes || numwalls) //Hurdler: render 3D water and transparent walls after everything
	{
		HWR_CreateDrawNodes();
	}

	HWD.pfnSetTransform(NULL);
	HWD.pfnUnSetShader();

	// Check for new console commands.
	NetUpdate();

	// added by Hurdler for correct splitscreen
	// moved here by hurdler so it works with the new near clipping plane
	HWD.pfnGClipRect(0, 0, vid.width, vid.height, NZCLIP_PLANE);
}

// ==========================================================================
//
// ==========================================================================
void HWR_RenderPlayerView(INT32 viewnumber, player_t *player)
{
	const float fpov = FIXED_TO_FLOAT(cv_fov.value+player->fovadd);
	postimg_t *type;

	const boolean skybox = (skyboxmo[0] && cv_skybox.value); // True if there's a skybox object and skyboxes are on

	FRGBAFloat ClearColor;

	if (splitscreen && player == &players[secondarydisplayplayer])
		type = &postimgtype2;
	else
		type = &postimgtype;

	ClearColor.red = 0.0f;
	ClearColor.green = 0.0f;
	ClearColor.blue = 0.0f;
	ClearColor.alpha = 1.0f;

	if (cv_glshaders.value)
		HWD.pfnSetShaderInfo(HWD_SHADERINFO_LEVELTIME, (INT32)leveltime); // The water surface shader needs the leveltime.

	if (viewnumber == 0) // Only do it if it's the first screen being rendered
		HWD.pfnClearBuffer(true, false, &ClearColor); // Clear the Color Buffer, stops HOMs. Also seems to fix the skybox issue on Intel GPUs.

	PS_START_TIMING(ps_hw_skyboxtime);
	if (skybox && drawsky) // If there's a skybox and we should be drawing the sky, draw the skybox
		HWR_RenderSkyboxView(viewnumber, player); // This is drawn before everything else so it is placed behind
	PS_STOP_TIMING(ps_hw_skyboxtime);

	if (!HWR_ShouldUsePaletteRendering())
	{
		// do we really need to save player (is it not the same)?
		player_t *saved_player = stplyr;
		stplyr = player;
		ST_doPaletteStuff();
		stplyr = saved_player;
#ifdef ALAM_LIGHTING
		HWR_SetLights(viewnumber);
#endif
	}

	// note: sets viewangle, viewx, viewy, viewz
	R_SetupFrame(player);
	framecount++; // timedemo

	// copy view cam position for local use
	dup_viewx = viewx;
	dup_viewy = viewy;
	dup_viewz = viewz;
	dup_viewangle = viewangle;

	// set window position
	gl_centery = gl_basecentery;
	gl_viewwindowy = gl_baseviewwindowy;
	gl_windowcentery = gl_basewindowcentery;
	if (splitscreen && viewnumber == 1)
	{
		gl_viewwindowy += (vid.height/2);
		gl_windowcentery += (vid.height/2);
	}

	// check for new console commands.
	NetUpdate();

	gl_viewx = FIXED_TO_FLOAT(dup_viewx);
	gl_viewy = FIXED_TO_FLOAT(dup_viewy);
	gl_viewz = FIXED_TO_FLOAT(dup_viewz);
	gl_viewsin = FIXED_TO_FLOAT(viewsin);
	gl_viewcos = FIXED_TO_FLOAT(viewcos);

	//04/01/2000: Hurdler: added for T&L
	//                     It should replace all other gl_viewxxx when finished
	memset(&atransform, 0x00, sizeof(FTransform));

	HWR_SetTransformAiming(&atransform, player, false);
	atransform.angley = (float)(viewangle>>ANGLETOFINESHIFT)*(360.0f/(float)FINEANGLES);

	gl_viewludsin = FIXED_TO_FLOAT(FINECOSINE(gl_aimingangle>>ANGLETOFINESHIFT));
	gl_viewludcos = FIXED_TO_FLOAT(-FINESINE(gl_aimingangle>>ANGLETOFINESHIFT));

	if (*type == postimg_flip)
		atransform.flip = true;
	else
		atransform.flip = false;

	atransform.x      = gl_viewx;  // FIXED_TO_FLOAT(viewx)
	atransform.y      = gl_viewy;  // FIXED_TO_FLOAT(viewy)
	atransform.z      = gl_viewz;  // FIXED_TO_FLOAT(viewz)
	atransform.scalex = 1;
	atransform.scaley = (float)vid.width/vid.height;
	atransform.scalez = 1;

	atransform.fovxangle = fpov; // Tails
	atransform.fovyangle = fpov; // Tails
	if (player->viewrollangle != 0)
	{
		fixed_t rol = AngleFixed(player->viewrollangle);
		atransform.rollangle = FIXED_TO_FLOAT(rol);
		atransform.roll = true;
	}
	atransform.splitscreen = splitscreen;

	gl_fovlud = (float)(1.0l/tan((double)(fpov*M_PIl/360l)));

	//------------------------------------------------------------------------
	HWR_ClearView(); // Clears the depth buffer and resets the view I believe

	if (!skybox && drawsky) // Don't draw the regular sky if there's a skybox
		HWR_DrawSkyBackground(player);

	//Hurdler: it doesn't work in splitscreen mode
	drawsky = splitscreen;

	HWR_ClearSprites();

	drawcount = 0;

	if (rendermode == render_opengl)
	{
		angle_t a1 = gld_FrustumAngle(gl_aimingangle);
		gld_clipper_Clear();
		gld_clipper_SafeAddClipRange(viewangle + a1, viewangle - a1);
#ifdef HAVE_SPHEREFRUSTRUM
		gld_FrustrumSetup();
#endif
	}

	//04/01/2000: Hurdler: added for T&L
	//                     Actually it only works on Walls and Planes
	HWD.pfnSetTransform(&atransform);

	// Reset the shader state.
	HWR_SetShaderState();

	ps_numbspcalls.value.i = 0;
	ps_numpolyobjects.value.i = 0;
	PS_START_TIMING(ps_bsptime);

	validcount++;

	if (cv_glbatching.value)
		HWR_StartBatching();

	HWR_RenderBSPNode((INT32)numnodes-1);

	PS_STOP_TIMING(ps_bsptime);

	if (cv_glbatching.value)
		HWR_RenderBatches();

	// Check for new console commands.
	NetUpdate();

#ifdef ALAM_LIGHTING
	//14/11/99: Hurdler: moved here because it doesn't work with
	// subsector, see other comments;
	HWR_ResetLights();
#endif

	// Draw MD2 and sprites
	ps_numsprites.value.i = gl_visspritecount;
	PS_START_TIMING(ps_hw_spritesorttime);
	HWR_SortVisSprites();
	PS_STOP_TIMING(ps_hw_spritesorttime);
	PS_START_TIMING(ps_hw_spritedrawtime);
	HWR_DrawSprites();
	PS_STOP_TIMING(ps_hw_spritedrawtime);

#ifdef NEWCORONAS
	//Hurdler: they must be drawn before translucent planes, what about gl fog?
	HWR_DrawCoronas();
#endif

	ps_numdrawnodes.value.i = 0;
	ps_hw_nodesorttime.value.p = 0;
	ps_hw_nodedrawtime.value.p = 0;
	if (numplanes || numpolyplanes || numwalls) //Hurdler: render 3D water and transparent walls after everything
	{
		HWR_CreateDrawNodes();
	}

	HWD.pfnSetTransform(NULL);
	HWD.pfnUnSetShader();

	HWR_DoPostProcessor(player);

	// Check for new console commands.
	NetUpdate();

	// added by Hurdler for correct splitscreen
	// moved here by hurdler so it works with the new near clipping plane
	HWD.pfnGClipRect(0, 0, vid.width, vid.height, NZCLIP_PLANE);
}

// Returns whether palette rendering is "actually enabled."
// Can't have palette rendering if shaders are disabled.
boolean HWR_ShouldUsePaletteRendering(void)
{
	return (cv_glpaletterendering.value && HWR_UseShader());
}

// enable or disable palette rendering state depending on settings and availability
// called when relevant settings change
// shader recompilation is done in the cvar callback
static void HWR_TogglePaletteRendering(void)
{
	// which state should we go to?
	if (HWR_ShouldUsePaletteRendering())
	{
		// are we not in that state already?
		if (!gl_palette_rendering_state)
		{
			gl_palette_rendering_state = true;

			// The textures will still be converted to RGBA by r_opengl.
			// This however makes hw_cache use paletted blending for composite textures!
			// (patchformat is not touched)
			textureformat = GL_TEXFMT_P_8;

			HWR_SetMapPalette();
			HWR_SetPalette(pLocalPalette);

			// If the r_opengl "texture palette" stays the same during this switch, these textures
			// will not be cleared out. However they are still out of date since the
			// composite texture blending method has changed. Therefore they need to be cleared.
			HWR_LoadMapTextures(numtextures);
		}
	}
	else
	{
		// are we not in that state already?
		if (gl_palette_rendering_state)
		{
			gl_palette_rendering_state = false;
			textureformat = GL_TEXFMT_RGBA;
			HWR_SetPalette(pLocalPalette);
			// If the r_opengl "texture palette" stays the same during this switch, these textures
			// will not be cleared out. However they are still out of date since the
			// composite texture blending method has changed. Therefore they need to be cleared.
			HWR_LoadMapTextures(numtextures);
		}
	}
}

void HWR_LoadLevel(void)
{
#ifdef ALAM_LIGHTING
	// BP: reset light between levels (we draw preview frame lights on current frame)
	HWR_ResetLights();
#endif

	HWR_CreatePlanePolygons((INT32)numnodes - 1);

	// Build the sky dome
	HWR_ClearSkyDome();
	HWR_BuildSkyDome();

	if (HWR_ShouldUsePaletteRendering())
		HWR_SetMapPalette();

	gl_maploaded = true;
}

// ==========================================================================
//                                                         3D ENGINE COMMANDS
// ==========================================================================

static CV_PossibleValue_t glshaders_cons_t[] = {{0, "Off"}, {1, "On"}, {2, "Ignore custom shaders"}, {0, NULL}};
static CV_PossibleValue_t glmodelinterpolation_cons_t[] = {{0, "Off"}, {1, "Sometimes"}, {2, "Always"}, {0, NULL}};
static CV_PossibleValue_t glfakecontrast_cons_t[] = {{0, "Off"}, {1, "On"}, {2, "Smooth"}, {0, NULL}};
static CV_PossibleValue_t glshearing_cons_t[] = {{0, "Off"}, {1, "On"}, {2, "Third-person"}, {0, NULL}};

static void CV_glfiltermode_OnChange(void);
static void CV_glanisotropic_OnChange(void);
static void CV_glmodellighting_OnChange(void);
static void CV_glpaletterendering_OnChange(void);
static void CV_glpalettedepth_OnChange(void);
static void CV_glshaders_OnChange(void);

static CV_PossibleValue_t glfiltermode_cons_t[]= {{HWD_SET_TEXTUREFILTER_POINTSAMPLED, "Nearest"},
	{HWD_SET_TEXTUREFILTER_BILINEAR, "Bilinear"}, {HWD_SET_TEXTUREFILTER_TRILINEAR, "Trilinear"},
	{HWD_SET_TEXTUREFILTER_MIXED1, "Linear_Nearest"},
	{HWD_SET_TEXTUREFILTER_MIXED2, "Nearest_Linear"},
	{HWD_SET_TEXTUREFILTER_MIXED3, "Nearest_Mipmap"},
	{0, NULL}};
CV_PossibleValue_t glanisotropicmode_cons_t[] = {{1, "MIN"}, {16, "MAX"}, {0, NULL}};

consvar_t cv_glshaders = CVAR_INIT ("gr_shaders", "On", CV_SAVE|CV_CALL, glshaders_cons_t, CV_glshaders_OnChange);
consvar_t cv_glallowshaders = CVAR_INIT ("gr_allowclientshaders", "On", CV_NETVAR, CV_OnOff, NULL);
consvar_t cv_fovchange = CVAR_INIT ("gr_fovchange", "Off", CV_SAVE, CV_OnOff, NULL);

#ifdef ALAM_LIGHTING
consvar_t cv_gldynamiclighting = CVAR_INIT ("gr_dynamiclighting", "On", CV_SAVE, CV_OnOff, NULL);
consvar_t cv_glstaticlighting  = CVAR_INIT ("gr_staticlighting", "On", CV_SAVE, CV_OnOff, NULL);
consvar_t cv_glcoronas = CVAR_INIT ("gr_coronas", "On", CV_SAVE, CV_OnOff, NULL);
consvar_t cv_glcoronasize = CVAR_INIT ("gr_coronasize", "1", CV_SAVE|CV_FLOAT, 0, NULL);
#endif

consvar_t cv_glmodels = CVAR_INIT ("gr_models", "Off", CV_SAVE, CV_OnOff, NULL);
consvar_t cv_glmodelinterpolation = CVAR_INIT ("gr_modelinterpolation", "Sometimes", CV_SAVE, glmodelinterpolation_cons_t, NULL);
consvar_t cv_glmodellighting = CVAR_INIT ("gr_modellighting", "Off", CV_SAVE|CV_CALL, CV_OnOff, CV_glmodellighting_OnChange);

consvar_t cv_glshearing = CVAR_INIT ("gr_shearing", "Off", CV_SAVE, glshearing_cons_t, NULL);
consvar_t cv_glspritebillboarding = CVAR_INIT ("gr_spritebillboarding", "Off", CV_SAVE, CV_OnOff, NULL);
consvar_t cv_glskydome = CVAR_INIT ("gr_skydome", "On", CV_SAVE, CV_OnOff, NULL);
consvar_t cv_glfakecontrast = CVAR_INIT ("gr_fakecontrast", "Smooth", CV_SAVE, glfakecontrast_cons_t, NULL);
consvar_t cv_glslopecontrast = CVAR_INIT ("gr_slopecontrast", "Off", CV_SAVE, CV_OnOff, NULL);

consvar_t cv_glfiltermode = CVAR_INIT ("gr_filtermode", "Nearest", CV_SAVE|CV_CALL, glfiltermode_cons_t, CV_glfiltermode_OnChange);
consvar_t cv_glanisotropicmode = CVAR_INIT ("gr_anisotropicmode", "1", CV_CALL, glanisotropicmode_cons_t, CV_glanisotropic_OnChange);

consvar_t cv_glsolvetjoin = CVAR_INIT ("gr_solvetjoin", "On", 0, CV_OnOff, NULL);

consvar_t cv_glbatching = CVAR_INIT ("gr_batching", "On", 0, CV_OnOff, NULL);

static CV_PossibleValue_t glpalettedepth_cons_t[] = {{16, "16 bits"}, {24, "24 bits"}, {0, NULL}};

consvar_t cv_glpaletterendering = CVAR_INIT ("gr_paletterendering", "Off", CV_SAVE|CV_CALL, CV_OnOff, CV_glpaletterendering_OnChange);
consvar_t cv_glpalettedepth = CVAR_INIT ("gr_palettedepth", "16 bits", CV_SAVE|CV_CALL, glpalettedepth_cons_t, CV_glpalettedepth_OnChange);

static void CV_glfiltermode_OnChange(void)
{
	if (rendermode == render_opengl)
		HWD.pfnSetSpecialState(HWD_SET_TEXTUREFILTERMODE, cv_glfiltermode.value);
}

static void CV_glanisotropic_OnChange(void)
{
	if (rendermode == render_opengl)
		HWD.pfnSetSpecialState(HWD_SET_TEXTUREANISOTROPICMODE, cv_glanisotropicmode.value);
}

static void CV_glmodellighting_OnChange(void)
{
	// if shaders have been compiled, then they now need to be recompiled.
	if (gl_shadersavailable)
		HWR_CompileShaders();
}

static void CV_glpaletterendering_OnChange(void)
{
	if (gl_shadersavailable)
	{
		HWR_CompileShaders();
		HWR_TogglePaletteRendering();
	}
}

static void CV_glpalettedepth_OnChange(void)
{
	// refresh the screen palette
	if (HWR_ShouldUsePaletteRendering())
		HWR_SetPalette(pLocalPalette);
}

static void CV_glshaders_OnChange(void)
{
	if (cv_glpaletterendering.value)
	{
		// can't do palette rendering without shaders, so update the state if needed
		HWR_TogglePaletteRendering();
	}
}

//added by Hurdler: console varibale that are saved
void HWR_AddCommands(void)
{
	CV_RegisterVar(&cv_fovchange);

#ifdef ALAM_LIGHTING
	CV_RegisterVar(&cv_glstaticlighting);
	CV_RegisterVar(&cv_gldynamiclighting);
	CV_RegisterVar(&cv_glcoronasize);
	CV_RegisterVar(&cv_glcoronas);
#endif

	CV_RegisterVar(&cv_glmodellighting);
	CV_RegisterVar(&cv_glmodelinterpolation);
	CV_RegisterVar(&cv_glmodels);

	CV_RegisterVar(&cv_glskydome);
	CV_RegisterVar(&cv_glspritebillboarding);
	CV_RegisterVar(&cv_glfakecontrast);
	CV_RegisterVar(&cv_glshearing);
	CV_RegisterVar(&cv_glshaders);
	CV_RegisterVar(&cv_glallowshaders);

	CV_RegisterVar(&cv_glfiltermode);
	CV_RegisterVar(&cv_glsolvetjoin);

	CV_RegisterVar(&cv_glbatching);

	CV_RegisterVar(&cv_glpaletterendering);
	CV_RegisterVar(&cv_glpalettedepth);
}

void HWR_AddSessionCommands(void)
{
	if (gl_sessioncommandsadded)
		return;
	CV_RegisterVar(&cv_glanisotropicmode);
	gl_sessioncommandsadded = true;
}

// --------------------------------------------------------------------------
// Setup the hardware renderer
// --------------------------------------------------------------------------
void HWR_Startup(void)
{
	if (!gl_init)
	{
		CONS_Printf("HWR_Startup()...\n");

		textureformat = patchformat = GL_TEXFMT_RGBA;

		HWR_InitPolyPool();
		HWR_AddSessionCommands();
		HWR_InitMapTextures();
		HWR_InitModels();
#ifdef ALAM_LIGHTING
		HWR_InitLight();
#endif

		gl_shadersavailable = HWR_InitShaders();
		HWR_LoadAllCustomShaders();
		HWR_TogglePaletteRendering();
	}

	gl_init = true;
}

// --------------------------------------------------------------------------
// Called after switching to the hardware renderer
// --------------------------------------------------------------------------
void HWR_Switch(void)
{
	// Add session commands
	if (!gl_sessioncommandsadded)
		HWR_AddSessionCommands();

	// Set special states from CVARs
	HWD.pfnSetSpecialState(HWD_SET_TEXTUREFILTERMODE, cv_glfiltermode.value);
	HWD.pfnSetSpecialState(HWD_SET_TEXTUREANISOTROPICMODE, cv_glanisotropicmode.value);

	// Load textures
	if (!gl_maptexturesloaded)
		HWR_LoadMapTextures(numtextures);

	// Create plane polygons
	if (!gl_maploaded && (gamestate == GS_LEVEL || (gamestate == GS_TITLESCREEN && titlemapinaction)))
	{
		HWR_ClearAllTextures();
		HWR_LoadLevel();
	}
}

// --------------------------------------------------------------------------
// Free resources allocated by the hardware renderer
// --------------------------------------------------------------------------
void HWR_Shutdown(void)
{
	CONS_Printf("HWR_Shutdown()\n");
	HWR_FreeExtraSubsectors();
	HWR_FreePolyPool();
	HWR_FreeMapTextures();
	HWD.pfnFlushScreenTextures();
}

void transform(float *cx, float *cy, float *cz)
{
	float tr_x,tr_y;
	// translation
	tr_x = *cx - gl_viewx;
	tr_y = *cz - gl_viewy;
//	*cy = *cy;

	// rotation around vertical y axis
	*cx = (tr_x * gl_viewsin) - (tr_y * gl_viewcos);
	tr_x = (tr_x * gl_viewcos) + (tr_y * gl_viewsin);

	//look up/down ----TOTAL SUCKS!!!--- do the 2 in one!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	tr_y = *cy - gl_viewz;

	*cy = (tr_x * gl_viewludcos) + (tr_y * gl_viewludsin);
	*cz = (tr_x * gl_viewludsin) - (tr_y * gl_viewludcos);

	//scale y before frustum so that frustum can be scaled to screen height
	*cy *= ORIGINAL_ASPECT * gl_fovlud;
	*cx *= gl_fovlud;
}

void HWR_AddTransparentWall(FOutVector *wallVerts, FSurfaceInfo *pSurf, INT32 texnum, FBITFIELD blend, boolean fogwall, INT32 lightlevel, extracolormap_t *wallcolormap)
{
	static size_t allocedwalls = 0;

	// Force realloc if buffer has been freed
	if (!wallinfo)
		allocedwalls = 0;

	if (allocedwalls < numwalls + 1)
	{
		allocedwalls += MAX_TRANSPARENTWALL;
		Z_Realloc(wallinfo, allocedwalls * sizeof (*wallinfo), PU_LEVEL, &wallinfo);
	}

	M_Memcpy(wallinfo[numwalls].wallVerts, wallVerts, sizeof (wallinfo[numwalls].wallVerts));
	M_Memcpy(&wallinfo[numwalls].Surf, pSurf, sizeof (FSurfaceInfo));
	wallinfo[numwalls].texnum = texnum;
	wallinfo[numwalls].blend = blend;
	wallinfo[numwalls].drawcount = drawcount++;
	wallinfo[numwalls].fogwall = fogwall;
	wallinfo[numwalls].lightlevel = lightlevel;
	wallinfo[numwalls].wallcolormap = wallcolormap;
	numwalls++;
}

void HWR_RenderWall(FOutVector *wallVerts, FSurfaceInfo *pSurf, FBITFIELD blend, boolean fogwall, INT32 lightlevel, extracolormap_t *wallcolormap)
{
	FBITFIELD blendmode = blend;
	UINT8 alpha = pSurf->PolyColor.s.alpha; // retain the alpha

	INT32 shader = -1;

	// Lighting is done here instead so that fog isn't drawn incorrectly on transparent walls after sorting
	HWR_Lighting(pSurf, lightlevel, wallcolormap);

	pSurf->PolyColor.s.alpha = alpha; // put the alpha back after lighting

	if (blend & PF_Environment)
		blendmode |= PF_Occlude;	// PF_Occlude must be used for solid objects

	if (HWR_UseShader())
	{
		if (fogwall)
			shader = SHADER_FOG;
		else
			shader = SHADER_WALL;

		blendmode |= PF_ColorMapped;
	}

	if (fogwall)
		blendmode |= PF_Fog;

	blendmode |= PF_Modulated;	// No PF_Occlude means overlapping (incorrect) transparency
	HWR_ProcessPolygon(pSurf, wallVerts, 4, blendmode, shader, false);
}

INT32 HWR_GetTextureUsed(void)
{
	return HWD.pfnGetTextureUsed();
}

void HWR_DoPostProcessor(player_t *player)
{
	postimg_t *type;

	HWD.pfnUnSetShader();

	if (splitscreen && player == &players[secondarydisplayplayer])
		type = &postimgtype2;
	else
		type = &postimgtype;

	// Armageddon Blast Flash!
	// Could this even be considered postprocessor?
	if (player->flashcount && !HWR_ShouldUsePaletteRendering())
	{
		FOutVector      v[4];
		FSurfaceInfo Surf;

		v[0].x = v[2].y = v[3].x = v[3].y = -4.0f;
		v[0].y = v[1].x = v[1].y = v[2].x = 4.0f;
		v[0].z = v[1].z = v[2].z = v[3].z = 4.0f; // 4.0 because of the same reason as with the sky, just after the screen is cleared so near clipping plane is 3.99

		// This won't change if the flash palettes are changed unfortunately, but it works for its purpose
		if (player->flashpal == PAL_NUKE)
		{
			Surf.PolyColor.s.red = 0xff;
			Surf.PolyColor.s.green = Surf.PolyColor.s.blue = 0x7F; // The nuke palette is kind of pink-ish
		}
		else
			Surf.PolyColor.s.red = Surf.PolyColor.s.green = Surf.PolyColor.s.blue = 0xff;

		Surf.PolyColor.s.alpha = 0xc0; // match software mode

		HWD.pfnDrawPolygon(&Surf, v, 4, PF_Modulated|PF_Additive|PF_NoTexture|PF_NoDepthTest);
	}

	// Capture the screen for intermission and screen waving
	if(gamestate != GS_INTERMISSION)
		HWD.pfnMakeScreenTexture(HWD_SCREENTEXTURE_GENERIC1);

	if (splitscreen) // Not supported in splitscreen - someone want to add support?
		return;

	// Drunken vision! WooOOooo~
	if (*type == postimg_water || *type == postimg_heat)
	{
		// 10 by 10 grid. 2 coordinates (xy)
		float v[SCREENVERTS][SCREENVERTS][2];
		static double disStart = 0;
		UINT8 x, y;
		INT32 WAVELENGTH;
		INT32 AMPLITUDE;
		INT32 FREQUENCY;

		// Modifies the wave.
		if (*type == postimg_water)
		{
			WAVELENGTH = 20; // Lower is longer
			AMPLITUDE = 20; // Lower is bigger
			FREQUENCY = 16; // Lower is faster
		}
		else
		{
			WAVELENGTH = 10; // Lower is longer
			AMPLITUDE = 30; // Lower is bigger
			FREQUENCY = 4; // Lower is faster
		}

		for (x = 0; x < SCREENVERTS; x++)
		{
			for (y = 0; y < SCREENVERTS; y++)
			{
				// Change X position based on its Y position.
				v[x][y][0] = (x/((float)(SCREENVERTS-1.0f)/9.0f))-4.5f + (float)sin((disStart+(y*WAVELENGTH))/FREQUENCY)/AMPLITUDE;
				v[x][y][1] = (y/((float)(SCREENVERTS-1.0f)/9.0f))-4.5f;
			}
		}
		HWD.pfnPostImgRedraw(v);
		if (!(paused || P_AutoPause()))
			disStart += 1;

		// Capture the screen again for screen waving on the intermission
		if(gamestate != GS_INTERMISSION)
			HWD.pfnMakeScreenTexture(HWD_SCREENTEXTURE_GENERIC1);
	}
	// Flipping of the screen isn't done here anymore
}

void HWR_StartScreenWipe(void)
{
	//CONS_Debug(DBG_RENDER, "In HWR_StartScreenWipe()\n");
	HWD.pfnMakeScreenTexture(HWD_SCREENTEXTURE_WIPE_START);
}

void HWR_EndScreenWipe(void)
{
	//CONS_Debug(DBG_RENDER, "In HWR_EndScreenWipe()\n");
	HWD.pfnMakeScreenTexture(HWD_SCREENTEXTURE_WIPE_END);
}

void HWR_DrawIntermissionBG(void)
{
	HWD.pfnDrawScreenTexture(HWD_SCREENTEXTURE_GENERIC1);
}

//
// hwr mode wipes
//
static lumpnum_t wipelumpnum;

// puts wipe lumpname in wipename[9]
static boolean HWR_WipeCheck(UINT8 wipenum, UINT8 scrnnum)
{
	static char lumpname[9] = "FADEmmss";
	size_t lsize;

	// not a valid wipe number
	if (wipenum > 99 || scrnnum > 99)
		return false; // shouldn't end up here really, the loop should've stopped running beforehand

	// puts the numbers into the wipename
	lumpname[4] = '0'+(wipenum/10);
	lumpname[5] = '0'+(wipenum%10);
	lumpname[6] = '0'+(scrnnum/10);
	lumpname[7] = '0'+(scrnnum%10);
	wipelumpnum = W_CheckNumForName(lumpname);

	// again, shouldn't be here really
	if (wipelumpnum == LUMPERROR)
		return false;

	lsize = W_LumpLength(wipelumpnum);
	if (!(lsize == 256000 || lsize == 64000 || lsize == 16000 || lsize == 4000))
	{
		CONS_Alert(CONS_WARNING, "Fade mask lump %s of incorrect size, ignored\n", lumpname);
		return false; // again, shouldn't get here if it is a bad size
	}

	return true;
}

void HWR_DoWipe(UINT8 wipenum, UINT8 scrnnum)
{
	if (!HWR_WipeCheck(wipenum, scrnnum))
		return;

	HWR_GetFadeMask(wipelumpnum);
	HWD.pfnDoScreenWipe(HWD_SCREENTEXTURE_WIPE_START, HWD_SCREENTEXTURE_WIPE_END);
}

void HWR_DoTintedWipe(UINT8 wipenum, UINT8 scrnnum)
{
	// It does the same thing
	HWR_DoWipe(wipenum, scrnnum);
}

void HWR_MakeScreenFinalTexture(void)
{
	int tex = HWR_ShouldUsePaletteRendering() ? HWD_SCREENTEXTURE_GENERIC3 : HWD_SCREENTEXTURE_GENERIC2;
	HWD.pfnMakeScreenTexture(tex);
}

void HWR_DrawScreenFinalTexture(int width, int height)
{
	int tex = HWR_ShouldUsePaletteRendering() ? HWD_SCREENTEXTURE_GENERIC3 : HWD_SCREENTEXTURE_GENERIC2;
	HWD.pfnDrawScreenFinalTexture(tex, width, height);
}

#endif // HWRENDER
