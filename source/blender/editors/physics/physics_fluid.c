/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) Blender Foundation
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/editors/physics/physics_fluid.c
 *  \ingroup edphys
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "MEM_guardedalloc.h"

/* types */
#include "DNA_action_types.h"
#include "DNA_object_types.h"
#include "DNA_object_fluidsim_types.h"

#include "BLI_blenlib.h"
#include "BLI_math.h"
#include "BLI_utildefines.h"

#include "BLT_translation.h"

#include "BKE_context.h"
#include "BKE_customdata.h"
#include "BKE_fluidsim.h"
#include "BKE_main.h"
#include "BKE_modifier.h"
#include "BKE_object.h"
#include "BKE_report.h"
#include "BKE_scene.h"
#include "BKE_screen.h"
#include "BKE_smoke.h"
#include "BKE_depsgraph.h"

#include "LBM_fluidsim.h"

#include "ED_screen.h"
#include "PIL_time.h"

#include "WM_types.h"
#include "WM_api.h"

#include "physics_intern.h" // own include

/* enable/disable overall compilation */
/* mantaflow include */
#ifdef WITH_MANTA
#	include "manta_fluid_API.h"
#endif
#include "DNA_smoke_types.h"

#ifdef WITH_MOD_FLUID

#include "BKE_global.h"

#include "WM_api.h"

#include "DNA_scene_types.h"
#include "DNA_mesh_types.h"

static float get_fluid_viscosity(FluidsimSettings *settings)
{
	return (1.0f/powf(10.0f, settings->viscosityExponent)) * settings->viscosityValue;
}

static float get_fluid_rate(FluidsimSettings *settings)
{
	float rate = 1.0f; /* default rate if not animated... */

	rate = settings->animRate;

	if (rate < 0.0f)
		rate = 0.0f;

	return rate;
}

static void get_fluid_gravity(float *gravity, Scene *scene, FluidsimSettings *fss)
{
	if (scene->physics_settings.flag & PHYS_GLOBAL_GRAVITY) {
		copy_v3_v3(gravity, scene->physics_settings.gravity);
	}
	else {
		copy_v3_v3(gravity, fss->grav);
	}
}

static float get_fluid_size_m(Scene *scene, Object *domainob, FluidsimSettings *fss)
{
	if (!scene->unit.system) {
		return fss->realsize;
	}
	else {
		float dim[3];
		float longest_axis;

		BKE_object_dimensions_get(domainob, dim);
		longest_axis = max_fff(dim[0], dim[1], dim[2]);

		return longest_axis * scene->unit.scale_length;
	}
}

static bool fluid_is_animated_mesh(FluidsimSettings *fss)
{
	return ((fss->type == OB_FLUIDSIM_CONTROL) || fss->domainNovecgen);
}

/* ********************** fluid sim settings struct functions ********************** */

#if 0
/* helper function */
void fluidsimGetGeometryObjFilename(Object *ob, char *dst)  //, char *srcname)
{
	//BLI_snprintf(dst, FILE_MAXFILE, "%s_cfgdata_%s.bobj.gz", srcname, ob->id.name);
	BLI_snprintf(dst, FILE_MAXFILE, "fluidcfgdata_%s.bobj.gz", ob->id.name);
}
#endif


/* ********************** fluid sim channel helper functions ********************** */

typedef struct FluidAnimChannels {
	int length;

	double aniFrameTime;

	float *timeAtFrame;
	float *DomainTime;
	float *DomainGravity;
	float *DomainViscosity;
} FluidAnimChannels;

typedef struct FluidObject {
	struct FluidObject *next, *prev;

	struct Object *object;

	float *Translation;
	float *Rotation;
	float *Scale;
	float *Active;

	float *InitialVelocity;

	float *AttractforceStrength;
	float *AttractforceRadius;
	float *VelocityforceStrength;
	float *VelocityforceRadius;

	float *VertexCache;
	int numVerts, numTris;
} FluidObject;

// no. of entries for the two channel sizes
#define CHANNEL_FLOAT 1
#define CHANNEL_VEC   3

// simplify channels before printing
// for API this is done anyway upon init
#if 0
static void fluidsimPrintChannel(FILE *file, float *channel, int paramsize, char *str, int entries)
{
	int i, j;
	int channelSize = paramsize;

	if (entries == 3) {
		elbeemSimplifyChannelVec3(channel, &channelSize);
	}
	else if (entries == 1) {
		elbeemSimplifyChannelFloat(channel, &channelSize);
	}
	else {
		/* invalid, cant happen? */
	}

	fprintf(file, "      CHANNEL %s =\n", str);
	for (i=0; i < channelSize; i++) {
		fprintf(file, "        ");
		for (j=0;j <= entries;j++) {  // also print time value
			fprintf(file, " %f ", channel[i*(entries + 1) + j]);
			if (j == entries-1) { fprintf(file, "  "); }
		}
		fprintf(file, "\n");
	}

	fprintf(file,  "      ;\n");
}
#endif


/* Note: fluid anim channel data layout
 * ------------------------------------
 * CHANNEL_FLOAT:
 * frame 1     |frame 2
 * [dataF][time][dataF][time]
 *
 * CHANNEL_VEC:
 * frame 1                   |frame 2
 * [dataX][dataY][dataZ][time][dataX][dataY][dataZ][time]
 *
 */

static void init_time(FluidsimSettings *domainSettings, FluidAnimChannels *channels)
{
	int i;

	channels->timeAtFrame = MEM_callocN((channels->length + 1) * sizeof(float), "timeAtFrame channel");

	channels->timeAtFrame[0] = channels->timeAtFrame[1] = domainSettings->animStart; // start at index 1

	for (i=2; i <= channels->length; i++) {
		channels->timeAtFrame[i] = channels->timeAtFrame[i - 1] + (float)channels->aniFrameTime;
	}
}

/* if this is slow, can replace with faster, less readable code */
static void set_channel(float *channel, float time, float *value, int i, int size)
{
	if (size == CHANNEL_FLOAT) {
		channel[(i * 2) + 0] = value[0];
		channel[(i * 2) + 1] = time;
	}
	else if (size == CHANNEL_VEC) {
		channel[(i * 4) + 0] = value[0];
		channel[(i * 4) + 1] = value[1];
		channel[(i * 4) + 2] = value[2];
		channel[(i * 4) + 3] = time;
	}
}

static void set_vertex_channel(float *channel, float time, struct Scene *scene, struct FluidObject *fobj, int i)
{
	Object *ob = fobj->object;
	FluidsimModifierData *fluidmd = (FluidsimModifierData *)modifiers_findByType(ob, eModifierType_Fluidsim);
	float *verts;
	int *tris=NULL, numVerts=0, numTris=0;
	int modifierIndex = BLI_findindex(&ob->modifiers, fluidmd);
	int framesize = (3*fobj->numVerts) + 1;
	int j;

	if (channel == NULL)
		return;

	initElbeemMesh(scene, ob, &numVerts, &verts, &numTris, &tris, 1, modifierIndex);

	/* don't allow mesh to change number of verts in anim sequence */
	if (numVerts != fobj->numVerts) {
		MEM_freeN(channel);
		channel = NULL;
		return;
	}

	/* fill frame of channel with vertex locations */
	for (j=0; j < (3*numVerts); j++) {
		channel[i*framesize + j] = verts[j];
	}
	channel[i*framesize + framesize-1] = time;

	MEM_freeN(verts);
	MEM_freeN(tris);
}

static void free_domain_channels(FluidAnimChannels *channels)
{
	if (!channels->timeAtFrame)
		return;
	MEM_freeN(channels->timeAtFrame);
	channels->timeAtFrame = NULL;
	MEM_freeN(channels->DomainGravity);
	channels->DomainGravity = NULL;
	MEM_freeN(channels->DomainViscosity);
	channels->DomainViscosity = NULL;
	MEM_freeN(channels->DomainTime);
	channels->DomainTime = NULL;
}

static void free_all_fluidobject_channels(ListBase *fobjects)
{
	FluidObject *fobj;

	for (fobj=fobjects->first; fobj; fobj=fobj->next) {
		if (fobj->Translation) {
			MEM_freeN(fobj->Translation);
			fobj->Translation = NULL;
			MEM_freeN(fobj->Rotation);
			fobj->Rotation = NULL;
			MEM_freeN(fobj->Scale);
			fobj->Scale = NULL;
			MEM_freeN(fobj->Active);
			fobj->Active = NULL;
			MEM_freeN(fobj->InitialVelocity);
			fobj->InitialVelocity = NULL;
		}

		if (fobj->AttractforceStrength) {
			MEM_freeN(fobj->AttractforceStrength);
			fobj->AttractforceStrength = NULL;
			MEM_freeN(fobj->AttractforceRadius);
			fobj->AttractforceRadius = NULL;
			MEM_freeN(fobj->VelocityforceStrength);
			fobj->VelocityforceStrength = NULL;
			MEM_freeN(fobj->VelocityforceRadius);
			fobj->VelocityforceRadius = NULL;
		}

		if (fobj->VertexCache) {
			MEM_freeN(fobj->VertexCache);
			fobj->VertexCache = NULL;
		}
	}
}

static void fluid_init_all_channels(bContext *C, Object *UNUSED(fsDomain), FluidsimSettings *domainSettings, FluidAnimChannels *channels, ListBase *fobjects)
{
	Scene *scene = CTX_data_scene(C);
	Base *base;
	int i;
	int length = channels->length;
	float eval_time;

	/* init time values (assuming that time moves at a constant speed; may be overridden later) */
	init_time(domainSettings, channels);

	/* allocate domain animation channels */
	channels->DomainGravity = MEM_callocN(length * (CHANNEL_VEC+1) * sizeof(float), "channel DomainGravity");
	channels->DomainViscosity = MEM_callocN(length * (CHANNEL_FLOAT+1) * sizeof(float), "channel DomainViscosity");
	channels->DomainTime = MEM_callocN(length * (CHANNEL_FLOAT+1) * sizeof(float), "channel DomainTime");

	/* allocate fluid objects */
	for (base=scene->base.first; base; base= base->next) {
		Object *ob = base->object;
		FluidsimModifierData *fluidmd = (FluidsimModifierData *)modifiers_findByType(ob, eModifierType_Fluidsim);

		if (fluidmd) {
			FluidObject *fobj = MEM_callocN(sizeof(FluidObject), "Fluid Object");
			fobj->object = ob;

			if (ELEM(fluidmd->fss->type, OB_FLUIDSIM_DOMAIN, OB_FLUIDSIM_PARTICLE)) {
				BLI_addtail(fobjects, fobj);
				continue;
			}

			fobj->Translation = MEM_callocN(length * (CHANNEL_VEC+1) * sizeof(float), "fluidobject Translation");
			fobj->Rotation = MEM_callocN(length * (CHANNEL_VEC+1) * sizeof(float), "fluidobject Rotation");
			fobj->Scale = MEM_callocN(length * (CHANNEL_VEC+1) * sizeof(float), "fluidobject Scale");
			fobj->Active = MEM_callocN(length * (CHANNEL_FLOAT+1) * sizeof(float), "fluidobject Active");
			fobj->InitialVelocity = MEM_callocN(length * (CHANNEL_VEC+1) * sizeof(float), "fluidobject InitialVelocity");

			if (fluidmd->fss->type == OB_FLUIDSIM_CONTROL) {
				fobj->AttractforceStrength = MEM_callocN(length * (CHANNEL_FLOAT+1) * sizeof(float), "fluidobject AttractforceStrength");
				fobj->AttractforceRadius = MEM_callocN(length * (CHANNEL_FLOAT+1) * sizeof(float), "fluidobject AttractforceRadius");
				fobj->VelocityforceStrength = MEM_callocN(length * (CHANNEL_FLOAT+1) * sizeof(float), "fluidobject VelocityforceStrength");
				fobj->VelocityforceRadius = MEM_callocN(length * (CHANNEL_FLOAT+1) * sizeof(float), "fluidobject VelocityforceRadius");
			}

			if (fluid_is_animated_mesh(fluidmd->fss)) {
				float *verts=NULL;
				int *tris=NULL, modifierIndex = BLI_findindex(&ob->modifiers, (ModifierData *)fluidmd);

				initElbeemMesh(scene, ob, &fobj->numVerts, &verts, &fobj->numTris, &tris, 0, modifierIndex);
				fobj->VertexCache = MEM_callocN(length *((fobj->numVerts*CHANNEL_VEC)+1) * sizeof(float), "fluidobject VertexCache");

				MEM_freeN(verts);
				MEM_freeN(tris);
			}

			BLI_addtail(fobjects, fobj);
		}
	}

	/* now we loop over the frames and fill the allocated channels with data */
	for (i=0; i < channels->length; i++) {
		FluidObject *fobj;
		float viscosity, gravity[3];
		float timeAtFrame, time;

		eval_time = domainSettings->bakeStart + i;

		/* XXX: This can't be used due to an anim sys optimization that ignores recalc object animation,
		 * leaving it for the depgraph (this ignores object animation such as modifier properties though... :/ )
		 * --> BKE_animsys_evaluate_all_animation(CTX_data_main(C), eval_time);
		 * This doesn't work with drivers:
		 * --> BKE_animsys_evaluate_animdata(&fsDomain->id, fsDomain->adt, eval_time, ADT_RECALC_ALL);
		 */

		/* Modifying the global scene isn't nice, but we can do it in
		 * this part of the process before a threaded job is created */
		scene->r.cfra = (int)eval_time;
		ED_update_for_newframe(CTX_data_main(C), scene, 1);

		/* now scene data should be current according to animation system, so we fill the channels */

		/* Domain time */
		// TODO: have option for not running sim, time mangling, in which case second case comes in handy
		if (channels->DomainTime) {
			time = get_fluid_rate(domainSettings) * (float)channels->aniFrameTime;
			timeAtFrame = channels->timeAtFrame[i] + time;

			channels->timeAtFrame[i+1] = timeAtFrame;
			set_channel(channels->DomainTime, i, &time, i, CHANNEL_FLOAT);
		}
		else {
			timeAtFrame = channels->timeAtFrame[i+1];
		}

		/* Domain properties - gravity/viscosity */
		get_fluid_gravity(gravity, scene, domainSettings);
		set_channel(channels->DomainGravity, timeAtFrame, gravity, i, CHANNEL_VEC);
		viscosity = get_fluid_viscosity(domainSettings);
		set_channel(channels->DomainViscosity, timeAtFrame, &viscosity, i, CHANNEL_FLOAT);

		/* object movement */
		for (fobj=fobjects->first; fobj; fobj=fobj->next) {
			Object *ob = fobj->object;
			FluidsimModifierData *fluidmd = (FluidsimModifierData *)modifiers_findByType(ob, eModifierType_Fluidsim);
			float active = (float) ((fluidmd->fss->flag & OB_FLUIDSIM_ACTIVE) ? 1 : 0);
			float rot_d[3] = {0.f, 0.f, 0.f}, old_rot[3] = {0.f, 0.f, 0.f};

			if (ELEM(fluidmd->fss->type, OB_FLUIDSIM_DOMAIN, OB_FLUIDSIM_PARTICLE))
				continue;

			/* init euler rotation values and convert to elbeem format */
			/* get the rotation from ob->obmat rather than ob->rot to account for parent animations */
			if (i) {
				copy_v3_v3(old_rot, fobj->Rotation + 4*(i-1));
				mul_v3_fl(old_rot, (float)-M_PI / 180.f);
			}

			mat4_to_compatible_eulO(rot_d, old_rot, 0, ob->obmat);
			mul_v3_fl(rot_d, -180.0f / (float)M_PI);

			set_channel(fobj->Translation, timeAtFrame, ob->loc, i, CHANNEL_VEC);
			set_channel(fobj->Rotation, timeAtFrame, rot_d, i, CHANNEL_VEC);
			set_channel(fobj->Scale, timeAtFrame, ob->size, i, CHANNEL_VEC);
			set_channel(fobj->Active, timeAtFrame, &active, i, CHANNEL_FLOAT);
			set_channel(fobj->InitialVelocity, timeAtFrame, &fluidmd->fss->iniVelx, i, CHANNEL_VEC);

			// printf("Active: %f, Frame: %f\n", active, timeAtFrame);

			if (fluidmd->fss->type == OB_FLUIDSIM_CONTROL) {
				set_channel(fobj->AttractforceStrength, timeAtFrame, &fluidmd->fss->attractforceStrength, i, CHANNEL_FLOAT);
				set_channel(fobj->AttractforceRadius, timeAtFrame, &fluidmd->fss->attractforceRadius, i, CHANNEL_FLOAT);
				set_channel(fobj->VelocityforceStrength, timeAtFrame, &fluidmd->fss->velocityforceStrength, i, CHANNEL_FLOAT);
				set_channel(fobj->VelocityforceRadius, timeAtFrame, &fluidmd->fss->velocityforceRadius, i, CHANNEL_FLOAT);
			}

			if (fluid_is_animated_mesh(fluidmd->fss)) {
				set_vertex_channel(fobj->VertexCache, timeAtFrame, scene, fobj, i);
			}
		}
	}
}

static void export_fluid_objects(ListBase *fobjects, Scene *scene, int length)
{
	FluidObject *fobj;

	for (fobj=fobjects->first; fobj; fobj=fobj->next) {
		Object *ob = fobj->object;
		FluidsimModifierData *fluidmd = (FluidsimModifierData *)modifiers_findByType(ob, eModifierType_Fluidsim);
		int modifierIndex = BLI_findindex(&ob->modifiers, fluidmd);

		float *verts=NULL;
		int *tris=NULL;
		int numVerts=0, numTris=0;
		bool deform = fluid_is_animated_mesh(fluidmd->fss);

		elbeemMesh fsmesh;

		if (ELEM(fluidmd->fss->type, OB_FLUIDSIM_DOMAIN, OB_FLUIDSIM_PARTICLE))
			continue;

		elbeemResetMesh(&fsmesh);

		fsmesh.type = fluidmd->fss->type;
		fsmesh.name = ob->id.name;

		initElbeemMesh(scene, ob, &numVerts, &verts, &numTris, &tris, 0, modifierIndex);

		fsmesh.numVertices   = numVerts;
		fsmesh.numTriangles  = numTris;
		fsmesh.vertices      = verts;
		fsmesh.triangles     = tris;

		fsmesh.channelSizeTranslation  =
		fsmesh.channelSizeRotation     =
		fsmesh.channelSizeScale        =
		fsmesh.channelSizeInitialVel   =
		fsmesh.channelSizeActive       = length;

		fsmesh.channelTranslation      = fobj->Translation;
		fsmesh.channelRotation         = fobj->Rotation;
		fsmesh.channelScale            = fobj->Scale;
		fsmesh.channelActive           = fobj->Active;

		if ( ELEM(fsmesh.type, OB_FLUIDSIM_FLUID, OB_FLUIDSIM_INFLOW)) {
			fsmesh.channelInitialVel = fobj->InitialVelocity;
			fsmesh.localInivelCoords = ((fluidmd->fss->typeFlags & OB_FSINFLOW_LOCALCOORD) ? 1 : 0);
		}

		if (fluidmd->fss->typeFlags & OB_FSBND_NOSLIP)
			fsmesh.obstacleType = FLUIDSIM_OBSTACLE_NOSLIP;
		else if (fluidmd->fss->typeFlags & OB_FSBND_PARTSLIP)
			fsmesh.obstacleType = FLUIDSIM_OBSTACLE_PARTSLIP;
		else if (fluidmd->fss->typeFlags & OB_FSBND_FREESLIP)
			fsmesh.obstacleType = FLUIDSIM_OBSTACLE_FREESLIP;

		fsmesh.obstaclePartslip = fluidmd->fss->partSlipValue;
		fsmesh.volumeInitType = fluidmd->fss->volumeInitType;
		fsmesh.obstacleImpactFactor = fluidmd->fss->surfaceSmoothing; // misused value

		if (fsmesh.type == OB_FLUIDSIM_CONTROL) {
			fsmesh.cpsTimeStart = fluidmd->fss->cpsTimeStart;
			fsmesh.cpsTimeEnd = fluidmd->fss->cpsTimeEnd;
			fsmesh.cpsQuality = fluidmd->fss->cpsQuality;
			fsmesh.obstacleType = (fluidmd->fss->flag & OB_FLUIDSIM_REVERSE);

			fsmesh.channelSizeAttractforceRadius =
			fsmesh.channelSizeVelocityforceStrength =
			fsmesh.channelSizeVelocityforceRadius =
			fsmesh.channelSizeAttractforceStrength = length;

			fsmesh.channelAttractforceStrength = fobj->AttractforceStrength;
			fsmesh.channelAttractforceRadius = fobj->AttractforceRadius;
			fsmesh.channelVelocityforceStrength = fobj->VelocityforceStrength;
			fsmesh.channelVelocityforceRadius = fobj->VelocityforceRadius;
		}
		else {
			fsmesh.channelAttractforceStrength =
			fsmesh.channelAttractforceRadius =
			fsmesh.channelVelocityforceStrength =
			fsmesh.channelVelocityforceRadius = NULL;
		}

		/* animated meshes */
		if (deform) {
			fsmesh.channelSizeVertices = length;
			fsmesh.channelVertices = fobj->VertexCache;

			/* remove channels */
			fsmesh.channelTranslation      =
			fsmesh.channelRotation         =
			fsmesh.channelScale            = NULL;

			/* Override user settings, only noslip is supported here! */
			if (fsmesh.type != OB_FLUIDSIM_CONTROL)
				fsmesh.obstacleType = FLUIDSIM_OBSTACLE_NOSLIP;
		}

		elbeemAddMesh(&fsmesh);

		if (verts) MEM_freeN(verts);
		if (tris) MEM_freeN(tris);
	}
}

static int fluid_validate_scene(ReportList *reports, Scene *scene, Object *fsDomain)
{
	Base *base;
	Object *newdomain = NULL;
	int channelObjCount = 0;
	int fluidInputCount = 0;

	for (base=scene->base.first; base; base= base->next) {
		Object *ob = base->object;
		FluidsimModifierData *fluidmdtmp = (FluidsimModifierData *)modifiers_findByType(ob, eModifierType_Fluidsim);

		/* only find objects with fluid modifiers */
		if (!fluidmdtmp || ob->type != OB_MESH) continue;

		if (fluidmdtmp->fss->type == OB_FLUIDSIM_DOMAIN) {
			/* if no initial domain object given, find another potential domain */
			if (!fsDomain) {
				newdomain = ob;
			}
			/* if there's more than one domain, cancel */
			else if (fsDomain && ob != fsDomain) {
				BKE_report(reports, RPT_ERROR, "There should be only one domain object");
				return 0;
			}
		}

		/* count number of objects needed for animation channels */
		if ( !ELEM(fluidmdtmp->fss->type, OB_FLUIDSIM_DOMAIN, OB_FLUIDSIM_PARTICLE) )
			channelObjCount++;

		/* count number of fluid input objects */
		if (ELEM(fluidmdtmp->fss->type, OB_FLUIDSIM_FLUID, OB_FLUIDSIM_INFLOW))
			fluidInputCount++;
	}

	if (newdomain)
		fsDomain = newdomain;

	if (!fsDomain) {
		BKE_report(reports, RPT_ERROR, "No domain object found");
		return 0;
	}

	if (channelObjCount >= 255) {
		BKE_report(reports, RPT_ERROR, "Cannot bake with more than 256 objects");
		return 0;
	}

	if (fluidInputCount == 0) {
		BKE_report(reports, RPT_ERROR, "No fluid input objects in the scene");
		return 0;
	}

	return 1;
}


#define FLUID_SUFFIX_CONFIG		"fluidsim.cfg"
#define FLUID_SUFFIX_CONFIG_TMP	(FLUID_SUFFIX_CONFIG ".tmp")
#define FLUID_SUFFIX_SURFACE	"fluidsurface"

static bool fluid_init_filepaths(
        Main *bmain, ReportList *reports, FluidsimSettings *domainSettings, Object *fsDomain,
        char *targetDir, char *targetFile)
{
	const char *suffixConfigTmp = FLUID_SUFFIX_CONFIG_TMP;

	/* prepare names... */
	const char *relbase = modifier_path_relbase(bmain, fsDomain);

	/* We do not accept empty paths, they can end in random places silently, see T51176. */
	if (domainSettings->surfdataPath[0] == '\0') {
		modifier_path_init(domainSettings->surfdataPath, sizeof(domainSettings->surfdataPath),
		                   OB_FLUIDSIM_SURF_DIR_DEFAULT);
		BKE_reportf(reports, RPT_WARNING, "Fluidsim: empty cache path, reset to default '%s'",
		            domainSettings->surfdataPath);
	}

	BLI_strncpy(targetDir, domainSettings->surfdataPath, FILE_MAXDIR);
	BLI_path_abs(targetDir, relbase);

	/* .tmp: don't overwrite/delete original file */
	BLI_join_dirfile(targetFile, FILE_MAX, targetDir, suffixConfigTmp);

	/* Ensure whole path exists and is wirtable. */
	const bool dir_exists = BLI_dir_create_recursive(targetDir);
	const bool is_writable = BLI_file_is_writable(targetFile);

	/* We change path to some presumably valid default value, but do not allow bake process to continue,
	 * this gives user chance to set manually another path. */
	if (!dir_exists || !is_writable) {
		modifier_path_init(domainSettings->surfdataPath, sizeof(domainSettings->surfdataPath),
		                   OB_FLUIDSIM_SURF_DIR_DEFAULT);

		if (!dir_exists) {
			BKE_reportf(reports, RPT_ERROR, "Fluidsim: could not create cache directory '%s', reset to default '%s'",
			            targetDir, domainSettings->surfdataPath);
		}
		else {
			BKE_reportf(reports, RPT_ERROR, "Fluidsim: cache directory '%s' is not writable, reset to default '%s'",
			            targetDir, domainSettings->surfdataPath);
		}

		BLI_strncpy(targetDir, domainSettings->surfdataPath, FILE_MAXDIR);
		BLI_path_abs(targetDir, relbase);

		/* .tmp: don't overwrite/delete original file */
		BLI_join_dirfile(targetFile, FILE_MAX, targetDir, suffixConfigTmp);

		/* Ensure whole path exists and is wirtable. */
		if (!BLI_dir_create_recursive(targetDir) || !BLI_file_is_writable(targetFile)) {
			BKE_reportf(reports, RPT_ERROR, "Fluidsim: could not use default cache directory '%s', "
			                                "please define a valid cache path manually", targetDir);
		}
		return false;
	}

	return true;
}

/* ******************************************************************************** */
/* ********************** write fluidsim config to file ************************* */
/* ******************************************************************************** */

typedef struct FluidBakeJob {
	/* from wmJob */
	void *owner;
	short *stop, *do_update;
	float *progress;
	int current_frame;
	elbeemSimulationSettings *settings;
} FluidBakeJob;

static void fluidbake_free(void *customdata)
{
	FluidBakeJob *fb= (FluidBakeJob *)customdata;
	MEM_freeN(fb);
}

/* called by fluidbake, only to check job 'stop' value */
static int fluidbake_breakjob(void *customdata)
{
	FluidBakeJob *fb= (FluidBakeJob *)customdata;

	if (fb->stop && *(fb->stop))
		return 1;

	/* this is not nice yet, need to make the jobs list template better
	 * for identifying/acting upon various different jobs */
	/* but for now we'll reuse the render break... */
	return (G.is_break);
}

/* called by fluidbake, wmJob sends notifier */
static void fluidbake_updatejob(void *customdata, float progress)
{
	FluidBakeJob *fb= (FluidBakeJob *)customdata;

	*(fb->do_update) = true;
	*(fb->progress) = progress;
}

static void fluidbake_startjob(void *customdata, short *stop, short *do_update, float *progress)
{
	FluidBakeJob *fb= (FluidBakeJob *)customdata;

	fb->stop= stop;
	fb->do_update = do_update;
	fb->progress = progress;

	G.is_break = false;  /* XXX shared with render - replace with job 'stop' switch */

	elbeemSimulate();
	*do_update = true;
	*stop = 0;
}

static void fluidbake_endjob(void *customdata)
{
	FluidBakeJob *fb= (FluidBakeJob *)customdata;

	if (fb->settings) {
		MEM_freeN(fb->settings);
		fb->settings = NULL;
	}
}

static int runSimulationCallback(void *data, int status, int frame)
{
	FluidBakeJob *fb = (FluidBakeJob *)data;
	elbeemSimulationSettings *settings = fb->settings;

	if (status == FLUIDSIM_CBSTATUS_NEWFRAME) {
		fluidbake_updatejob(fb, frame / (float)settings->noOfFrames);
		//printf("elbeem blender cb s%d, f%d, domainid:%d noOfFrames: %d\n", status, frame, settings->domainId, settings->noOfFrames ); // DEBUG
	}

	if (fluidbake_breakjob(fb)) {
		return FLUIDSIM_CBRET_ABORT;
	}

	return FLUIDSIM_CBRET_CONTINUE;
}

static void fluidbake_free_data(FluidAnimChannels *channels, ListBase *fobjects, elbeemSimulationSettings *fsset, FluidBakeJob *fb)
{
	free_domain_channels(channels);
	MEM_freeN(channels);
	channels = NULL;

	free_all_fluidobject_channels(fobjects);
	BLI_freelistN(fobjects);
	MEM_freeN(fobjects);
	fobjects = NULL;

	if (fsset) {
		MEM_freeN(fsset);
		fsset = NULL;
	}

	if (fb) {
		MEM_freeN(fb);
		fb = NULL;
	}
}

/* copied from rna_fluidsim.c: fluidsim_find_lastframe() */
static void fluidsim_delete_until_lastframe(FluidsimSettings *fss, const char *relbase)
{
	char targetDir[FILE_MAX], targetFile[FILE_MAX];
	char targetDirVel[FILE_MAX], targetFileVel[FILE_MAX];
	char previewDir[FILE_MAX], previewFile[FILE_MAX];
	int curFrame = 1, exists = 0;

	BLI_join_dirfile(targetDir,    sizeof(targetDir),    fss->surfdataPath, OB_FLUIDSIM_SURF_FINAL_OBJ_FNAME);
	BLI_join_dirfile(targetDirVel, sizeof(targetDirVel), fss->surfdataPath, OB_FLUIDSIM_SURF_FINAL_VEL_FNAME);
	BLI_join_dirfile(previewDir,   sizeof(previewDir),   fss->surfdataPath, OB_FLUIDSIM_SURF_PREVIEW_OBJ_FNAME);

	BLI_path_abs(targetDir,    relbase);
	BLI_path_abs(targetDirVel, relbase);
	BLI_path_abs(previewDir,   relbase);

	do {
		BLI_strncpy(targetFile, targetDir, sizeof(targetFile));
		BLI_strncpy(targetFileVel, targetDirVel, sizeof(targetFileVel));
		BLI_strncpy(previewFile, previewDir, sizeof(previewFile));

		BLI_path_frame(targetFile, curFrame, 0);
		BLI_path_frame(targetFileVel, curFrame, 0);
		BLI_path_frame(previewFile, curFrame, 0);

		curFrame++;

		if ((exists = BLI_exists(targetFile))) {
			BLI_delete(targetFile, false, false);
			BLI_delete(targetFileVel, false, false);
			BLI_delete(previewFile, false, false);
		}
	} while (exists);

	return;
}

static int fluidsimBake(bContext *C, ReportList *reports, Object *fsDomain, short do_job)
{
	Main *bmain = CTX_data_main(C);
	Scene *scene= CTX_data_scene(C);
	int i;
	FluidsimSettings *domainSettings;

	char debugStrBuffer[256];

	int gridlevels = 0;
	const char *relbase= modifier_path_relbase(bmain, fsDomain);
	const char *strEnvName = "BLENDER_ELBEEMDEBUG"; // from blendercall.cpp
	const char *suffixConfigTmp = FLUID_SUFFIX_CONFIG_TMP;
	const char *suffixSurface = FLUID_SUFFIX_SURFACE;

	char targetDir[FILE_MAX];  // store & modify output settings
	char targetFile[FILE_MAX]; // temp. store filename from targetDir for access

	float domainMat[4][4];
	float invDomMat[4][4];

	int noFrames;
	int origFrame = scene->r.cfra;

	FluidAnimChannels *channels = MEM_callocN(sizeof(FluidAnimChannels), "fluid domain animation channels");
	ListBase *fobjects = MEM_callocN(sizeof(ListBase), "fluid objects");
	FluidsimModifierData *fluidmd = NULL;
	Mesh *mesh = NULL;

	FluidBakeJob *fb;
	elbeemSimulationSettings *fsset= MEM_callocN(sizeof(elbeemSimulationSettings), "Fluid sim settings");

	fb= MEM_callocN(sizeof(FluidBakeJob), "fluid bake job");

	if (getenv(strEnvName)) {
		int dlevel = atoi(getenv(strEnvName));
		elbeemSetDebugLevel(dlevel);
		BLI_snprintf(debugStrBuffer, sizeof(debugStrBuffer), "fluidsimBake::msg: Debug messages activated due to envvar '%s'\n", strEnvName);
		elbeemDebugOut(debugStrBuffer);
	}

	/* make sure it corresponds to startFrame setting (old: noFrames = scene->r.efra - scene->r.sfra +1) */;
	noFrames = scene->r.efra - 0;
	if (noFrames<=0) {
		BKE_report(reports, RPT_ERROR, "No frames to export (check your animation range settings)");
		fluidbake_free_data(channels, fobjects, fsset, fb);
		return 0;
	}

	/* check scene for sane object/modifier settings */
	if (!fluid_validate_scene(reports, scene, fsDomain)) {
		fluidbake_free_data(channels, fobjects, fsset, fb);
		return 0;
	}

	/* these both have to be valid, otherwise we wouldn't be here */
	fluidmd = (FluidsimModifierData *)modifiers_findByType(fsDomain, eModifierType_Fluidsim);
	domainSettings = fluidmd->fss;
	mesh = fsDomain->data;

	domainSettings->bakeStart = 1;
	domainSettings->bakeEnd = scene->r.efra;

	// calculate bounding box
	fluid_get_bb(mesh->mvert, mesh->totvert, fsDomain->obmat, domainSettings->bbStart, domainSettings->bbSize);

	// reset last valid frame
	domainSettings->lastgoodframe = -1;

	/* delete old baked files */
	fluidsim_delete_until_lastframe(domainSettings, relbase);

	/* rough check of settings... */
	if (domainSettings->previewresxyz > domainSettings->resolutionxyz) {
		BLI_snprintf(debugStrBuffer, sizeof(debugStrBuffer), "fluidsimBake::warning - Preview (%d) >= Resolution (%d)... setting equal.\n", domainSettings->previewresxyz,  domainSettings->resolutionxyz);
		elbeemDebugOut(debugStrBuffer);
		domainSettings->previewresxyz = domainSettings->resolutionxyz;
	}
	// set adaptive coarsening according to resolutionxyz
	// this should do as an approximation, with in/outflow
	// doing this more accurate would be overkill
	// perhaps add manual setting?
	if (domainSettings->maxRefine <0) {
		if (domainSettings->resolutionxyz>128) {
			gridlevels = 2;
		}
		else if (domainSettings->resolutionxyz > 64) {
			gridlevels = 1;
		}
		else {
			gridlevels = 0;
		}
	}
	else {
		gridlevels = domainSettings->maxRefine;
	}
	BLI_snprintf(debugStrBuffer, sizeof(debugStrBuffer), "fluidsimBake::msg: Baking %s, refine: %d\n", fsDomain->id.name, gridlevels);
	elbeemDebugOut(debugStrBuffer);



	/* ******** prepare output file paths ******** */
	if (!fluid_init_filepaths(bmain, reports, domainSettings, fsDomain, targetDir, targetFile)) {
		fluidbake_free_data(channels, fobjects, fsset, fb);
		return false;
	}

	channels->length = scene->r.efra; // DG TODO: why using endframe and not "noFrames" here? .. because "noFrames" is buggy too? (not using sfra)
	channels->aniFrameTime = (double)((double)domainSettings->animEnd - (double)domainSettings->animStart) / (double)noFrames;

	/* ******** initialize and allocate animation channels ******** */
	fluid_init_all_channels(C, fsDomain, domainSettings, channels, fobjects);

	/* reset to original current frame */
	scene->r.cfra = origFrame;
	ED_update_for_newframe(CTX_data_main(C), scene, 1);

	/* ******** init domain object's matrix ******** */
	copy_m4_m4(domainMat, fsDomain->obmat);
	if (!invert_m4_m4(invDomMat, domainMat)) {
		BLI_snprintf(debugStrBuffer, sizeof(debugStrBuffer), "fluidsimBake::error - Invalid obj matrix?\n");
		elbeemDebugOut(debugStrBuffer);
		BKE_report(reports, RPT_ERROR, "Invalid object matrix");

		fluidbake_free_data(channels, fobjects, fsset, fb);
		return 0;
	}

	/* ********  start writing / exporting ******** */
	// use .tmp, don't overwrite/delete original file
	BLI_join_dirfile(targetFile, sizeof(targetFile), targetDir, suffixConfigTmp);

	/* ******** export domain to elbeem ******** */
	elbeemResetSettings(fsset);
	fsset->version = 1;
	fsset->threads = (domainSettings->threads == 0) ? BKE_scene_num_threads(scene) : domainSettings->threads;
	// setup global settings
	copy_v3_v3(fsset->geoStart, domainSettings->bbStart);
	copy_v3_v3(fsset->geoSize, domainSettings->bbSize);

	// simulate with 50^3
	fsset->resolutionxyz = (int)domainSettings->resolutionxyz;
	fsset->previewresxyz = (int)domainSettings->previewresxyz;

	fsset->realsize = get_fluid_size_m(scene, fsDomain, domainSettings);
	fsset->viscosity = get_fluid_viscosity(domainSettings);
	get_fluid_gravity(fsset->gravity, scene, domainSettings);

	// simulate 5 frames, each 0.03 seconds, output to ./apitest_XXX.bobj.gz
	fsset->animStart = domainSettings->animStart;
	fsset->aniFrameTime = channels->aniFrameTime;
	fsset->noOfFrames = noFrames; // is otherwise subtracted in parser

	BLI_join_dirfile(targetFile, sizeof(targetFile), targetDir, suffixSurface);

	// defaults for compressibility and adaptive grids
	fsset->gstar = domainSettings->gstar;
	fsset->maxRefine = domainSettings->maxRefine; // check <-> gridlevels
	fsset->generateParticles = domainSettings->generateParticles;
	fsset->numTracerParticles = domainSettings->generateTracers;
	fsset->surfaceSmoothing = domainSettings->surfaceSmoothing;
	fsset->surfaceSubdivs = domainSettings->surfaceSubdivs;
	fsset->farFieldSize = domainSettings->farFieldSize;
	BLI_strncpy(fsset->outputPath, targetFile, sizeof(fsset->outputPath));

	// domain channels
	fsset->channelSizeFrameTime =
	fsset->channelSizeViscosity =
	fsset->channelSizeGravity = channels->length;
	fsset->channelFrameTime = channels->DomainTime;
	fsset->channelViscosity = channels->DomainViscosity;
	fsset->channelGravity = channels->DomainGravity;

	fsset->runsimCallback = &runSimulationCallback;
	fsset->runsimUserData = fb;

	if (domainSettings->typeFlags & OB_FSBND_NOSLIP)		fsset->domainobsType = FLUIDSIM_OBSTACLE_NOSLIP;
	else if (domainSettings->typeFlags&OB_FSBND_PARTSLIP)	fsset->domainobsType = FLUIDSIM_OBSTACLE_PARTSLIP;
	else if (domainSettings->typeFlags&OB_FSBND_FREESLIP)	fsset->domainobsType = FLUIDSIM_OBSTACLE_FREESLIP;
	fsset->domainobsPartslip = domainSettings->partSlipValue;

	/* use domainobsType also for surface generation flag (bit: >=64) */
	if (domainSettings->typeFlags & OB_FSSG_NOOBS)
		fsset->mFsSurfGenSetting = FLUIDSIM_FSSG_NOOBS;
	else
		fsset->mFsSurfGenSetting = 0; // "normal" mode

	fsset->generateVertexVectors = (domainSettings->domainNovecgen==0);

	// init blender domain transform matrix
	{ int j;
	for (i=0; i<4; i++) {
		for (j=0; j<4; j++) {
			fsset->surfaceTrafo[i*4+j] = invDomMat[j][i];
		}
	} }

	/* ******** init solver with settings ******** */
	elbeemInit();
	elbeemAddDomain(fsset);

	/* ******** export all fluid objects to elbeem ******** */
	export_fluid_objects(fobjects, scene, channels->length);

	/* custom data for fluid bake job */
	fb->settings = fsset;

	if (do_job) {
		wmJob *wm_job = WM_jobs_get(CTX_wm_manager(C), CTX_wm_window(C), scene, "Fluid Simulation",
		                            WM_JOB_PROGRESS, WM_JOB_TYPE_OBJECT_SIM_FLUID);

		/* setup job */
		WM_jobs_customdata_set(wm_job, fb, fluidbake_free);
		WM_jobs_timer(wm_job, 0.1, NC_SCENE|ND_FRAME, NC_SCENE|ND_FRAME);
		WM_jobs_callbacks(wm_job, fluidbake_startjob, NULL, NULL, fluidbake_endjob);

		WM_jobs_start(CTX_wm_manager(C), wm_job);
	}
	else {
		short dummy_stop = 0, dummy_do_update = 0;
		float dummy_progress = 0.0f;

		/* blocking, use with exec() */
		fluidbake_startjob((void *)fb, &dummy_stop, &dummy_do_update, &dummy_progress);
		fluidbake_endjob((void *)fb);
		fluidbake_free((void *)fb);
	}

	/* ******** free stored animation data ******** */
	fluidbake_free_data(channels, fobjects, NULL, NULL);

	// elbeemFree();
	return 1;
}

static void UNUSED_FUNCTION(fluidsimFreeBake)(Object *UNUSED(ob))
{
	/* not implemented yet */
}

#else /* WITH_MOD_FLUID */

/* only compile dummy functions */
static int fluidsimBake(bContext *UNUSED(C), ReportList *UNUSED(reports), Object *UNUSED(ob), short UNUSED(do_job))
{
	return 0;
}

#endif /* WITH_MOD_FLUID */

/***************************** Operators ******************************/

static int fluid_bake_invoke(bContext *C, wmOperator *op, const wmEvent *UNUSED(event))
{
	/* only one bake job at a time */
	if (WM_jobs_test(CTX_wm_manager(C), CTX_data_scene(C), WM_JOB_TYPE_OBJECT_SIM_FLUID))
		return OPERATOR_CANCELLED;

	if (!fluidsimBake(C, op->reports, CTX_data_active_object(C), true))
		return OPERATOR_CANCELLED;

	return OPERATOR_FINISHED;
}

static int fluid_bake_exec(bContext *C, wmOperator *op)
{
	if (!fluidsimBake(C, op->reports, CTX_data_active_object(C), false))
		return OPERATOR_CANCELLED;

	return OPERATOR_FINISHED;
}

void FLUID_OT_bake(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Fluid Simulation Bake";
	ot->description = "Bake fluid simulation";
	ot->idname = "FLUID_OT_bake";

	/* api callbacks */
	ot->invoke = fluid_bake_invoke;
	ot->exec = fluid_bake_exec;
	ot->poll = ED_operator_object_active_editable;
}

/***************************** Bake Fluid Mantaflow ******************************/

typedef struct FluidMantaflowJob {
	/* from wmJob */
	void *owner;
	short *stop, *do_update;
	float *progress;
	const char *type;
	const char *name;

	struct Main *bmain;
	Scene *scene;
	Object *ob;

	SmokeModifierData *smd;

	int success;
	double start;

	int* pause_frame;
} FluidMantaflowJob;

static bool fluid_manta_initjob(bContext *C, FluidMantaflowJob *job, wmOperator *op, char *error_msg, int error_size)
{
	SmokeModifierData *smd = NULL;
	SmokeDomainSettings *sds;
	Object *ob = CTX_data_active_object(C);

	smd = (SmokeModifierData *)modifiers_findByType(ob, eModifierType_Smoke);
	if (!smd) {
		BLI_strncpy(error_msg, N_("Bake failed: no Fluid modifier found"), error_size);
		return false;
	}
	sds = smd->domain;
	if (!sds) {
		BLI_strncpy(error_msg, N_("Bake failed: invalid domain"), error_size);
		return false;
	}

	job->bmain = CTX_data_main(C);
	job->scene = CTX_data_scene(C);
	job->ob = CTX_data_active_object(C);
	job->smd = smd;
	job->type = op->type->idname;
	job->name = op->type->name;

	return true;
}

static bool fluid_manta_initpaths(FluidMantaflowJob *job, ReportList *reports)
{
	SmokeDomainSettings *sds = job->smd->domain;
	char tmpDir[FILE_MAX];
	tmpDir[0] = '\0';

	const char *relbase = modifier_path_relbase(job->bmain, job->ob);

	/* We do not accept empty paths, they can end in random places silently, see T51176. */
	if (sds->cache_directory[0] == '\0') {
		modifier_path_init(sds->cache_directory, sizeof(sds->cache_directory), FLUID_DOMAIN_DIR_DEFAULT);
		BKE_reportf(reports, RPT_WARNING, "Fluid Mantaflow: Empty cache path, reset to default '%s'", sds->cache_directory);
	}

	BLI_strncpy(tmpDir, sds->cache_directory, FILE_MAXDIR);
	BLI_path_abs(tmpDir, relbase);

	/* Ensure whole path exists */
	const bool dir_exists = BLI_dir_create_recursive(tmpDir);

	/* We change path to some presumably valid default value, but do not allow bake process to continue,
	 * this gives user chance to set manually another path. */
	if (!dir_exists) {
		modifier_path_init(sds->cache_directory, sizeof(sds->cache_directory), FLUID_DOMAIN_DIR_DEFAULT);

		BKE_reportf(reports, RPT_ERROR, "Fluid Mantaflow: Could not create cache directory '%s', reset to default '%s'",
			            tmpDir, sds->cache_directory);

		BLI_strncpy(tmpDir, sds->cache_directory, FILE_MAXDIR);
		BLI_path_abs(tmpDir, relbase);

		/* Ensure whole path exists and is wirtable. */
		if (!BLI_dir_create_recursive(tmpDir)) {
			BKE_reportf(reports, RPT_ERROR, "Fluid Mantaflow: Could not use default cache directory '%s', "
			                                "please define a valid cache path manually", tmpDir);
		}
		return false;
	}

	/* Copy final dir back into domain settings */
	BLI_strncpy(sds->cache_directory, tmpDir, FILE_MAXDIR);
	return true;
}

static void fluid_manta_bake_free(void *customdata)
{
	FluidMantaflowJob *job = customdata;
	MEM_freeN(job);
}

static void fluid_manta_bake_sequence(FluidMantaflowJob *job)
{
	SmokeDomainSettings *sds = job->smd->domain;
	Scene *scene = job->scene;
	int frame = 1, orig_frame;
	int frames;
	int *pause_frame = NULL;
	bool is_first_frame;

	frames = sds->cache_frame_end - sds->cache_frame_start + 1;

	if (frames <= 0) {
		BLI_strncpy(sds->error, N_("No frames to bake"), sizeof(sds->error));
		return;
	}

	/* Show progress bar. */
	if (job->do_update)
		*(job->do_update) = true;

	/* Get current pause frame (pointer) - depending on bake type */
	pause_frame = job->pause_frame;

	/* Set frame to start point (depending on current pause frame value) */
	is_first_frame = ((*pause_frame) == 0);
	frame = is_first_frame ? sds->cache_frame_start : (*pause_frame);

	/* Save orig frame and update scene frame */
	orig_frame = scene->r.cfra;
	CFRA = frame;

	/* Loop through selected frames */
	for ( ; frame <= sds->cache_frame_end; frame++) {
		const float progress = (frame - sds->cache_frame_start) / (float)frames;

		/* Keep track of pause frame - needed to init future loop */
		(*pause_frame) = frame;

		/* If user requested stop, quit baking */
		if (G.is_break) {

			job->success = 0;
			return;
		}

		/* Update progress bar */
		if (job->do_update)
			*(job->do_update) = true;
		if (job->progress)
			*(job->progress) = progress;

		CFRA = frame;

		/* Update animation system */
		ED_update_for_newframe(job->bmain, scene, 1);
	}

	/* Restore frame position that we were on before bake */
	CFRA = orig_frame;
}

static int fluid_manta_bake_modal(bContext *C, wmOperator *UNUSED(op), const wmEvent *event)
{
	/* no running blender, remove handler and pass through */
	if (0 == WM_jobs_test(CTX_wm_manager(C), CTX_data_scene(C), WM_JOB_TYPE_OBJECT_SIM_MANTA))
		return OPERATOR_FINISHED | OPERATOR_PASS_THROUGH;

	switch (event->type) {
		case ESCKEY:
			return OPERATOR_RUNNING_MODAL;
	}
	return OPERATOR_PASS_THROUGH;
}

static void fluid_manta_bake_endjob(void *customdata)
{
	FluidMantaflowJob *job = customdata;
	SmokeDomainSettings *sds = job->smd->domain;

	G.is_rendering = false;
	BKE_spacedata_draw_locks(false);

	if (STREQ(job->type, "MANTA_OT_bake_data"))
	{
		sds->cache_flag &= ~FLUID_DOMAIN_BAKING_DATA;
		sds->cache_flag |= FLUID_DOMAIN_BAKED_DATA;
	}
	else if (STREQ(job->type, "MANTA_OT_bake_noise"))
	{
		sds->cache_flag &= ~FLUID_DOMAIN_BAKING_NOISE;
		sds->cache_flag |= FLUID_DOMAIN_BAKED_NOISE;
	}
	else if (STREQ(job->type, "MANTA_OT_bake_mesh"))
	{
		sds->cache_flag &= ~FLUID_DOMAIN_BAKING_MESH;
		sds->cache_flag |= FLUID_DOMAIN_BAKED_MESH;
	}
	else if (STREQ(job->type, "MANTA_OT_bake_particles"))
	{
		sds->cache_flag &= ~FLUID_DOMAIN_BAKING_PARTICLES;
		sds->cache_flag |= FLUID_DOMAIN_BAKED_PARTICLES;
	}
	else if (STREQ(job->type, "MANTA_OT_bake_guiding"))
	{
		sds->cache_flag &= ~FLUID_DOMAIN_BAKING_GUIDING;
		sds->cache_flag |= FLUID_DOMAIN_BAKED_GUIDING;
	}
	DAG_id_tag_update(&job->ob->id, OB_RECALC_DATA);

	/* Bake was successful:
	 *  Report for ended bake and how long it took */
	if (job->success) {
		/* Show bake info */
		WM_reportf(RPT_INFO, "Fluid Mantaflow: %s complete! (%.2f)", job->name, PIL_check_seconds_timer() - job->start);
	}
	else {
		if (strlen(sds->error)) { /* If an error occurred */
			WM_reportf(RPT_ERROR, "Fluid Mantaflow: %s failed: %s", job->name, sds->error);
		}
		else { /* User canceled the bake */
			WM_reportf(RPT_WARNING, "Fluid Mantaflow: %s canceled!", job->name);
		}
	}
}

static void fluid_manta_bake_startjob(void *customdata, short *stop, short *do_update, float *progress)
{
	FluidMantaflowJob *job = customdata;
	SmokeDomainSettings *sds = job->smd->domain;

	char tmpDir[FILE_MAX];
	tmpDir[0] = '\0';

	job->stop = stop;
	job->do_update = do_update;
	job->progress = progress;
	job->start = PIL_check_seconds_timer();
	job->success = 1;

	G.is_break = false;

	/* same annoying hack as in physics_pointcache.c and dynamicpaint_ops.c to prevent data corruption*/
	G.is_rendering = true;
	BKE_spacedata_draw_locks(true);

	if (STREQ(job->type, "MANTA_OT_bake_data"))
	{
		BLI_path_join(tmpDir, sizeof(tmpDir), sds->cache_directory, FLUID_DOMAIN_DIR_DATA, NULL);
		BLI_dir_create_recursive(tmpDir); /* Create 'data' subdir if it does not exist already */
		sds->cache_flag &= ~FLUID_DOMAIN_BAKED_DATA;
		sds->cache_flag |= FLUID_DOMAIN_BAKING_DATA;
		job->pause_frame = &sds->cache_frame_pause_data;
	}
	else if (STREQ(job->type, "MANTA_OT_bake_noise"))
	{
		BLI_path_join(tmpDir, sizeof(tmpDir), sds->cache_directory, FLUID_DOMAIN_DIR_NOISE, NULL);
		BLI_dir_create_recursive(tmpDir); /* Create 'noise' subdir if it does not exist already */
		sds->cache_flag &= ~FLUID_DOMAIN_BAKED_NOISE;
		sds->cache_flag |= FLUID_DOMAIN_BAKING_NOISE;
		job->pause_frame = &sds->cache_frame_pause_noise;
	}
	else if (STREQ(job->type, "MANTA_OT_bake_mesh"))
	{
		BLI_path_join(tmpDir, sizeof(tmpDir), sds->cache_directory, FLUID_DOMAIN_DIR_MESH, NULL);
		BLI_dir_create_recursive(tmpDir); /* Create 'mesh' subdir if it does not exist already */
		sds->cache_flag &= ~FLUID_DOMAIN_BAKED_MESH;
		sds->cache_flag |= FLUID_DOMAIN_BAKING_MESH;
		job->pause_frame = &sds->cache_frame_pause_mesh;
	}
	else if (STREQ(job->type, "MANTA_OT_bake_particles"))
	{
		BLI_path_join(tmpDir, sizeof(tmpDir), sds->cache_directory, FLUID_DOMAIN_DIR_PARTICLES, NULL);
		BLI_dir_create_recursive(tmpDir); /* Create 'particles' subdir if it does not exist already */
		sds->cache_flag &= ~FLUID_DOMAIN_BAKED_PARTICLES;
		sds->cache_flag |= FLUID_DOMAIN_BAKING_PARTICLES;
		job->pause_frame = &sds->cache_frame_pause_particles;
	}
	else if (STREQ(job->type, "MANTA_OT_bake_guiding"))
	{
		BLI_path_join(tmpDir, sizeof(tmpDir), sds->cache_directory, FLUID_DOMAIN_DIR_GUIDING, NULL);
		BLI_dir_create_recursive(tmpDir); /* Create 'particles' subdir if it does not exist already */
		sds->cache_flag &= ~FLUID_DOMAIN_BAKED_GUIDING;
		sds->cache_flag |= FLUID_DOMAIN_BAKING_GUIDING;
		job->pause_frame = &sds->cache_frame_pause_guiding;
	}
	DAG_id_tag_update(&job->ob->id, OB_RECALC_DATA);

	fluid_manta_bake_sequence(job);

	if (do_update)
		*do_update = true;
	if (stop)
		*stop = 0;
}

static int fluid_manta_bake_exec(bContext *C, wmOperator *op)
{
	FluidMantaflowJob *job = MEM_mallocN(sizeof(FluidMantaflowJob), "FluidMantaflowJob");
	char error_msg[256] = "\0";

	if (!fluid_manta_initjob(C, job, op, error_msg, sizeof(error_msg))) {
		if (error_msg[0]) {
			BKE_report(op->reports, RPT_ERROR, error_msg);
		}
		fluid_manta_bake_free(job);
		return OPERATOR_CANCELLED;
	}
	fluid_manta_initpaths(job, op->reports);
	fluid_manta_bake_startjob(job, NULL, NULL, NULL);
	fluid_manta_bake_endjob(job);
	fluid_manta_bake_free(job);

	return OPERATOR_FINISHED;
}

static int fluid_manta_bake_invoke(struct bContext *C, struct wmOperator *op, const wmEvent *UNUSED(_event))
{
	Scene *scene = CTX_data_scene(C);
	FluidMantaflowJob *job = MEM_mallocN(sizeof(FluidMantaflowJob), "FluidMantaflowJob");
	char error_msg[256] = "\0";

	if (!fluid_manta_initjob(C, job, op, error_msg, sizeof(error_msg))) {
		if (error_msg[0]) {
			BKE_report(op->reports, RPT_ERROR, error_msg);
		}
		fluid_manta_bake_free(job);
		return OPERATOR_CANCELLED;
	}

	fluid_manta_initpaths(job, op->reports);

	wmJob *wm_job = WM_jobs_get(CTX_wm_manager(C), CTX_wm_window(C), scene,
								"Fluid Mantaflow Bake", WM_JOB_PROGRESS,
								WM_JOB_TYPE_OBJECT_SIM_MANTA);

	WM_jobs_customdata_set(wm_job, job, fluid_manta_bake_free);
	WM_jobs_timer(wm_job, 0.1, NC_OBJECT | ND_MODIFIER, NC_OBJECT | ND_MODIFIER);
	WM_jobs_callbacks(wm_job, fluid_manta_bake_startjob, NULL, NULL, fluid_manta_bake_endjob);

	WM_jobs_start(CTX_wm_manager(C), wm_job);
	WM_event_add_modal_handler(C, op);

	return OPERATOR_RUNNING_MODAL;
}

static void fluid_manta_free_endjob(void *customdata)
{
	FluidMantaflowJob *job = customdata;
	SmokeDomainSettings *sds = job->smd->domain;

	G.is_rendering = false;
	BKE_spacedata_draw_locks(false);

	/* Free was successful:
	 *  Report for ended free job and how long it took */
	if (job->success) {
		/* Show free job info */
		WM_reportf(RPT_INFO, "Fluid Mantaflow: %s complete! (%.2f)", job->name, PIL_check_seconds_timer() - job->start);
	}
	else {
		if (strlen(sds->error)) { /* If an error occurred */
			WM_reportf(RPT_ERROR, "Fluid Mantaflow: %s failed: %s", job->name, sds->error);
		}
		else { /* User canceled the free job */
			WM_reportf(RPT_WARNING, "Fluid Mantaflow: %s canceled!", job->name);
		}
	}
}

static void fluid_manta_free_startjob(void *customdata, short *stop, short *do_update, float *progress)
{
	FluidMantaflowJob *job = customdata;
	SmokeDomainSettings *sds = job->smd->domain;
	Scene *scene = job->scene;

	char tmpDir[FILE_MAX];
	tmpDir[0] = '\0';

	job->stop = stop;
	job->do_update = do_update;
	job->progress = progress;
	job->start = PIL_check_seconds_timer();
	job->success = 1;

	G.is_break = false;

	G.is_rendering = true;
	BKE_spacedata_draw_locks(true);

	if (STREQ(job->type, "MANTA_OT_free_data"))
	{
		sds->cache_flag &= ~(FLUID_DOMAIN_BAKING_DATA|FLUID_DOMAIN_BAKED_DATA|
							 FLUID_DOMAIN_BAKING_NOISE|FLUID_DOMAIN_BAKED_NOISE|
							 FLUID_DOMAIN_BAKING_MESH|FLUID_DOMAIN_BAKED_MESH|
							 FLUID_DOMAIN_BAKING_PARTICLES|FLUID_DOMAIN_BAKED_PARTICLES);

		BLI_path_join(tmpDir, sizeof(tmpDir), sds->cache_directory, FLUID_DOMAIN_DIR_DATA, NULL);
		if (BLI_exists(tmpDir)) BLI_delete(tmpDir, true, true);
		BLI_path_join(tmpDir, sizeof(tmpDir), sds->cache_directory, FLUID_DOMAIN_DIR_NOISE, NULL);
		if (BLI_exists(tmpDir)) BLI_delete(tmpDir, true, true);

		/* Free optional mesh and particles as well - otherwise they would not be in sync with data cache */
		BLI_path_join(tmpDir, sizeof(tmpDir), sds->cache_directory, FLUID_DOMAIN_DIR_MESH, NULL);
		if (BLI_exists(tmpDir)) BLI_delete(tmpDir, true, true);
		BLI_path_join(tmpDir, sizeof(tmpDir), sds->cache_directory, FLUID_DOMAIN_DIR_PARTICLES, NULL);
		if (BLI_exists(tmpDir)) BLI_delete(tmpDir, true, true);

		/* Reset pause frame */
		sds->cache_frame_pause_data = 0;
	}
	else if (STREQ(job->type, "MANTA_OT_free_noise"))
	{
		sds->cache_flag &= ~(FLUID_DOMAIN_BAKING_NOISE|FLUID_DOMAIN_BAKED_NOISE);

		BLI_path_join(tmpDir, sizeof(tmpDir), sds->cache_directory, FLUID_DOMAIN_DIR_NOISE, NULL);
		if (BLI_exists(tmpDir)) BLI_delete(tmpDir, true, true);

		/* Reset pause frame */
		sds->cache_frame_pause_noise = 0;
	}
	else if (STREQ(job->type, "MANTA_OT_free_mesh"))
	{
		sds->cache_flag &= ~(FLUID_DOMAIN_BAKING_MESH|FLUID_DOMAIN_BAKED_MESH);

		BLI_path_join(tmpDir, sizeof(tmpDir), sds->cache_directory, FLUID_DOMAIN_DIR_MESH, NULL);
		if (BLI_exists(tmpDir)) BLI_delete(tmpDir, true, true);

		/* Reset pause frame */
		sds->cache_frame_pause_mesh = 0;
	}
	else if (STREQ(job->type, "MANTA_OT_free_particles"))
	{
		sds->cache_flag &= ~(FLUID_DOMAIN_BAKING_PARTICLES|FLUID_DOMAIN_BAKED_PARTICLES);

		BLI_path_join(tmpDir, sizeof(tmpDir), sds->cache_directory, FLUID_DOMAIN_DIR_PARTICLES, NULL);
		if (BLI_exists(tmpDir)) BLI_delete(tmpDir, true, true);

		/* Reset pause frame */
		sds->cache_frame_pause_particles = 0;
	}
	else if (STREQ(job->type, "MANTA_OT_free_guiding"))
	{
		sds->cache_flag &= ~(FLUID_DOMAIN_BAKING_GUIDING|FLUID_DOMAIN_BAKED_GUIDING);

		BLI_path_join(tmpDir, sizeof(tmpDir), sds->cache_directory, FLUID_DOMAIN_DIR_GUIDING, NULL);
		if (BLI_exists(tmpDir)) BLI_delete(tmpDir, true, true);

		/* Reset pause frame */
		sds->cache_frame_pause_guiding = 0;
	}
	DAG_id_tag_update(&job->ob->id, OB_RECALC_DATA);

	*do_update = true;
	*stop = 0;

	/* Reset scene frame to cache frame start */
	scene->r.cfra = sds->cache_frame_start;

	/* Update scene so that viewport shows freed up scene */
	ED_update_for_newframe(job->bmain, job->scene, 1);
}

static int fluid_manta_free_exec(struct bContext *C, struct wmOperator *op)
{
	SmokeModifierData *smd = NULL;
	SmokeDomainSettings *sds;
	Object *ob = CTX_data_active_object(C);
	Scene *scene = CTX_data_scene(C);

	/*
	 * Get modifier data
	 */
	smd = (SmokeModifierData *)modifiers_findByType(ob, eModifierType_Smoke);
	if (!smd) {
		BKE_report(op->reports, RPT_ERROR, "Bake free failed: no Fluid modifier found");
		return OPERATOR_CANCELLED;
	}
	sds = smd->domain;
	if (!sds) {
		BKE_report(op->reports, RPT_ERROR, "Bake free failed: invalid domain");
		return OPERATOR_CANCELLED;
	}

	/* Cannot free data if other bakes currently working */
	if (smd->domain->cache_flag & (FLUID_DOMAIN_BAKING_DATA|FLUID_DOMAIN_BAKING_NOISE|
								   FLUID_DOMAIN_BAKING_MESH|FLUID_DOMAIN_BAKING_PARTICLES))
	{
		BKE_report(op->reports, RPT_ERROR, "Bake free failed: pending bake jobs found");
		return OPERATOR_CANCELLED;
	}

	FluidMantaflowJob *job = MEM_mallocN(sizeof(FluidMantaflowJob), "FluidMantaflowJob");
	job->bmain = CTX_data_main(C);
	job->scene = scene;
	job->ob = ob;
	job->smd = smd;
	job->type = op->type->idname;
	job->name = op->type->name;

	fluid_manta_initpaths(job, op->reports);

	wmJob *wm_job = WM_jobs_get(CTX_wm_manager(C), CTX_wm_window(C), scene,
								"Fluid Mantaflow Free", WM_JOB_PROGRESS,
								WM_JOB_TYPE_OBJECT_SIM_MANTA);

	WM_jobs_customdata_set(wm_job, job, fluid_manta_bake_free);
	WM_jobs_timer(wm_job, 0.1, NC_OBJECT | ND_MODIFIER, NC_OBJECT | ND_MODIFIER);
	WM_jobs_callbacks(wm_job, fluid_manta_free_startjob, NULL, NULL, fluid_manta_free_endjob);

	/*  Free Fluid Geometry	*/
	WM_jobs_start(CTX_wm_manager(C), wm_job);

	return OPERATOR_FINISHED;
}

static int fluid_manta_pause_exec(struct bContext *C, struct wmOperator *op)
{
	SmokeModifierData *smd = NULL;
	SmokeDomainSettings *sds;
	Object *ob = CTX_data_active_object(C);

	/*
	 * Get modifier data
	 */
	smd = (SmokeModifierData *)modifiers_findByType(ob, eModifierType_Smoke);
	if (!smd) {
		BKE_report(op->reports, RPT_ERROR, "Bake free failed: no Fluid modifier found");
		return OPERATOR_CANCELLED;
	}
	sds = smd->domain;
	if (!sds) {
		BKE_report(op->reports, RPT_ERROR, "Bake free failed: invalid domain");
		return OPERATOR_CANCELLED;
	}

	G.is_break = true;

	return OPERATOR_FINISHED;
}

void MANTA_OT_bake_data(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Bake Data";
	ot->description = "Bake Fluid Data";
	ot->idname = "MANTA_OT_bake_data";

	/* api callbacks */
	ot->exec = fluid_manta_bake_exec;
	ot->invoke = fluid_manta_bake_invoke;
	ot->modal = fluid_manta_bake_modal;
	ot->poll = ED_operator_object_active_editable;
}

void MANTA_OT_free_data(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Free Data";
	ot->description = "Free Fluid Data";
	ot->idname = "MANTA_OT_free_data";

	/* api callbacks */
	ot->exec = fluid_manta_free_exec;
	ot->poll = ED_operator_object_active_editable;
}

void MANTA_OT_bake_noise(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Bake Noise";
	ot->description = "Bake Fluid Noise";
	ot->idname = "MANTA_OT_bake_noise";

	/* api callbacks */
	ot->exec = fluid_manta_bake_exec;
	ot->invoke = fluid_manta_bake_invoke;
	ot->modal = fluid_manta_bake_modal;
	ot->poll = ED_operator_object_active_editable;
}

void MANTA_OT_free_noise(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Free Noise";
	ot->description = "Free Fluid Noise";
	ot->idname = "MANTA_OT_free_noise";

	/* api callbacks */
	ot->exec = fluid_manta_free_exec;
	ot->poll = ED_operator_object_active_editable;
}

void MANTA_OT_bake_mesh(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Bake Mesh";
	ot->description = "Bake Fluid Mesh";
	ot->idname = "MANTA_OT_bake_mesh";

	/* api callbacks */
	ot->exec = fluid_manta_bake_exec;
	ot->invoke = fluid_manta_bake_invoke;
	ot->modal = fluid_manta_bake_modal;
	ot->poll = ED_operator_object_active_editable;
}

void MANTA_OT_free_mesh(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Free Mesh";
	ot->description = "Free Fluid Mesh";
	ot->idname = "MANTA_OT_free_mesh";

	/* api callbacks */
	ot->exec = fluid_manta_free_exec;
	ot->poll = ED_operator_object_active_editable;
}

void MANTA_OT_bake_particles(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Bake Particles";
	ot->description = "Bake Fluid Particles";
	ot->idname = "MANTA_OT_bake_particles";

	/* api callbacks */
	ot->exec = fluid_manta_bake_exec;
	ot->invoke = fluid_manta_bake_invoke;
	ot->modal = fluid_manta_bake_modal;
	ot->poll = ED_operator_object_active_editable;
}

void MANTA_OT_free_particles(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Free Particles";
	ot->description = "Free Fluid Particles";
	ot->idname = "MANTA_OT_free_particles";

	/* api callbacks */
	ot->exec = fluid_manta_free_exec;
	ot->poll = ED_operator_object_active_editable;
}

void MANTA_OT_bake_guiding(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Bake Guiding";
	ot->description = "Bake Fluid Guiding";
	ot->idname = "MANTA_OT_bake_guiding";

	/* api callbacks */
	ot->exec = fluid_manta_bake_exec;
	ot->invoke = fluid_manta_bake_invoke;
	ot->modal = fluid_manta_bake_modal;
	ot->poll = ED_operator_object_active_editable;
}

void MANTA_OT_free_guiding(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Free Guiding";
	ot->description = "Free Fluid Guiding";
	ot->idname = "MANTA_OT_free_guiding";

	/* api callbacks */
	ot->exec = fluid_manta_free_exec;
	ot->poll = ED_operator_object_active_editable;
}

void MANTA_OT_pause_bake(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Pause Bake";
	ot->description = "Pause Bake";
	ot->idname = "MANTA_OT_pause_bake";

	/* api callbacks */
	ot->exec = fluid_manta_pause_exec;
	ot->poll = ED_operator_object_active_editable;
}

static int manta_make_file_exec(bContext *C, wmOperator *UNUSED(op))
{
	SmokeModifierData *smd;
	Object * smokeDomain = CTX_data_active_object(C);
	smd = (SmokeModifierData *)modifiers_findByType(smokeDomain, eModifierType_Smoke);

	char tmpDir[FILE_MAX];
	tmpDir[0] = '\0';

	if (smd->domain->fluid == NULL)
		smoke_reallocate_fluid(smd->domain, smd->domain->res, 1);

	BLI_path_join(tmpDir, sizeof(tmpDir), smd->domain->cache_directory, FLUID_DOMAIN_DIR_SCRIPT, NULL);
	BLI_dir_create_recursive(tmpDir); /* Create 'script' subdir if it does not exist already */

	if (smd->domain->fluid && smd->domain->type == FLUID_DOMAIN_TYPE_GAS)
		smoke_manta_export(smd->domain->fluid, smd);

	if (smd->domain->fluid && smd->domain->type == FLUID_DOMAIN_TYPE_LIQUID)
		liquid_manta_export(smd->domain->fluid, smd);

	return OPERATOR_FINISHED;
}

void MANTA_OT_make_file(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Create Mantaflow File";
	ot->description = "Generate Python script (only needed for external simulation with standalone Mantaflow)";
	ot->idname = "MANTA_OT_make_file";

	/* api callbacks */
	ot->exec = manta_make_file_exec;
	ot->poll = ED_operator_object_active_editable;
}

