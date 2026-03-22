/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/
//gl_fog.c -- global fog (shader-based, no fixed-function GL)

#include "quakedef.h"
#include "mathlib.h"
#include "gl_shader.h"

#define CLAMP(min,val,max) ((val) < (min) ? (min) : ((val) > (max) ? (max) : (val)))

//==============================================================================
//
//  GLOBAL FOG
//
//==============================================================================

#define DEFAULT_DENSITY 0.0
#define DEFAULT_GRAY 0.3

float fog_density;
float fog_red;
float fog_green;
float fog_blue;

float old_density;
float old_red;
float old_green;
float old_blue;

float fade_time; //duration of fade
float fade_done; //time when fade will be done

/* Current frame fog parameters — set by Fog_SetupFrame, read by shaders */
float	r_fog_density;
float	r_fog_color[3];

/*
=============
Fog_Update

update internal variables
=============
*/
void Fog_Update (float density, float red, float green, float blue, float time)
{
	//save previous settings for fade
	if (time > 0)
	{
		//check for a fade in progress
		if (fade_done > cl.time)
		{
			float f;

			f = (fade_done - cl.time) / fade_time;
			old_density = f * old_density + (1.0 - f) * fog_density;
			old_red = f * old_red + (1.0 - f) * fog_red;
			old_green = f * old_green + (1.0 - f) * fog_green;
			old_blue = f * old_blue + (1.0 - f) * fog_blue;
		}
		else
		{
			old_density = fog_density;
			old_red = fog_red;
			old_green = fog_green;
			old_blue = fog_blue;
		}
	}

	fog_density = density;
	fog_red = red;
	fog_green = green;
	fog_blue = blue;
	fade_time = time;
	fade_done = cl.time + time;
}

/*
=============
Fog_ParseServerMessage

handle an SVC_FOG message from server
=============
*/
void Fog_ParseServerMessage (void)
{
	float density, red, green, blue, time;

	density = MSG_ReadByte() / 255.0;
	red = MSG_ReadByte() / 255.0;
	green = MSG_ReadByte() / 255.0;
	blue = MSG_ReadByte() / 255.0;
	time = q_max(0.0, MSG_ReadShort() / 100.0);

	Con_Printf("Fog: density=%.3f rgb=(%.2f, %.2f, %.2f) fade=%.1fs\n",
		density, red, green, blue, time);
	Fog_Update (density, red, green, blue, time);
}

/*
=============
Fog_FogCommand_f

handle the 'fog' console command
=============
*/
void Fog_FogCommand_f (void)
{
	switch (Cmd_Argc())
	{
	default:
	case 1:
		Con_Printf("usage:\n");
		Con_Printf("   fog <density>\n");
		Con_Printf("   fog <red> <green> <blue>\n");
		Con_Printf("   fog <density> <red> <green> <blue>\n");
		Con_Printf("current values:\n");
		Con_Printf("   \"density\" is \"%f\"\n", fog_density);
		Con_Printf("   \"red\" is \"%f\"\n", fog_red);
		Con_Printf("   \"green\" is \"%f\"\n", fog_green);
		Con_Printf("   \"blue\" is \"%f\"\n", fog_blue);
		break;
	case 2:
		Fog_Update(q_max(0.0, atof(Cmd_Argv(1))),
				   fog_red,
				   fog_green,
				   fog_blue,
				   0.0);
		break;
	case 3: //TEST
		Fog_Update(q_max(0.0, atof(Cmd_Argv(1))),
				   fog_red,
				   fog_green,
				   fog_blue,
				   atof(Cmd_Argv(2)));
		break;
	case 4:
		Fog_Update(fog_density,
				   CLAMP(0.0, atof(Cmd_Argv(1)), 1.0),
				   CLAMP(0.0, atof(Cmd_Argv(2)), 1.0),
				   CLAMP(0.0, atof(Cmd_Argv(3)), 1.0),
				   0.0);
		break;
	case 5:
		Fog_Update(q_max(0.0, atof(Cmd_Argv(1))),
				   CLAMP(0.0, atof(Cmd_Argv(2)), 1.0),
				   CLAMP(0.0, atof(Cmd_Argv(3)), 1.0),
				   CLAMP(0.0, atof(Cmd_Argv(4)), 1.0),
				   0.0);
		break;
	case 6: //TEST
		Fog_Update(q_max(0.0, atof(Cmd_Argv(1))),
				   CLAMP(0.0, atof(Cmd_Argv(2)), 1.0),
				   CLAMP(0.0, atof(Cmd_Argv(3)), 1.0),
				   CLAMP(0.0, atof(Cmd_Argv(4)), 1.0),
				   atof(Cmd_Argv(5)));
		break;
	}
}

/*
=============
Fog_ParseWorldspawn

called at map load
=============
*/
void Fog_ParseWorldspawn (void)
{
	char key[128], value[4096];
	const char *data;

	//initially no fog
	fog_density = DEFAULT_DENSITY;
	fog_red = DEFAULT_GRAY;
	fog_green = DEFAULT_GRAY;
	fog_blue = DEFAULT_GRAY;

	old_density = DEFAULT_DENSITY;
	old_red = DEFAULT_GRAY;
	old_green = DEFAULT_GRAY;
	old_blue = DEFAULT_GRAY;

	fade_time = 0.0;
	fade_done = 0.0;

	data = COM_Parse(cl.worldmodel->entities);
	if (!data)
		return; // error
	if (com_token[0] != '{')
		return; // error
	while (1)
	{
		data = COM_Parse(data);
		if (!data)
			return; // error
		if (com_token[0] == '}')
			break; // end of worldspawn
		if (com_token[0] == '_')
			strcpy(key, com_token + 1);
		else
			strcpy(key, com_token);
		while (key[strlen(key)-1] == ' ') // remove trailing spaces
			key[strlen(key)-1] = 0;
		data = COM_Parse(data);
		if (!data)
			return; // error
		strcpy(value, com_token);

		if (!strcmp("fog", key))
		{
			sscanf(value, "%f %f %f %f", &fog_density, &fog_red, &fog_green, &fog_blue);
			Con_Printf("Fog: worldspawn density=%.3f rgb=(%.2f, %.2f, %.2f)\n",
				fog_density, fog_red, fog_green, fog_blue);
		}
	}
}

/*
=============
Fog_GetColor

calculates fog color for this frame, taking into account fade times
=============
*/
float *Fog_GetColor (void)
{
	static float c[4];
	float f;
	int i;

	if (fade_done > cl.time)
	{
		f = (fade_done - cl.time) / fade_time;
		c[0] = f * old_red + (1.0 - f) * fog_red;
		c[1] = f * old_green + (1.0 - f) * fog_green;
		c[2] = f * old_blue + (1.0 - f) * fog_blue;
		c[3] = 1.0;
	}
	else
	{
		c[0] = fog_red;
		c[1] = fog_green;
		c[2] = fog_blue;
		c[3] = 1.0;
	}

	//find closest 24-bit RGB value, so solid-colored sky can match the fog perfectly
	for (i=0;i<3;i++)
		c[i] = (float)(Q_rint(c[i] * 255)) / 255.0f;

	return c;
}

/*
=============
Fog_GetDensity

returns current density of fog
=============
*/
float Fog_GetDensity (void)
{
	float f;

	if (fade_done > cl.time)
	{
		f = (fade_done - cl.time) / fade_time;
		return f * old_density + (1.0 - f) * fog_density;
	}
	else
		return fog_density;
}

/*
=============
Fog_SetupFrame

called at the beginning of each frame — updates globals read by shaders
=============
*/
void Fog_SetupFrame (void)
{
	extern mleaf_t *r_viewleaf;
	float *c = Fog_GetColor();
	r_fog_density = Fog_GetDensity() / 512.0;
	r_fog_color[0] = c[0];
	r_fog_color[1] = c[1];
	r_fog_color[2] = c[2];

	/* Disable fog when underwater — liquid surfaces have their own
	 * tinting and fog looks wrong submerged */
	if (r_viewleaf && r_viewleaf->contents <= CONTENTS_WATER)
		r_fog_density = 0;
}

/*
=============
Fog_EnableGFog / Fog_DisableGFog

No-ops — fog is now handled entirely in shaders via u_fog_density/u_fog_color
uniforms set by GL_ImmEnd from the r_fog_density/r_fog_color globals.
=============
*/
void Fog_EnableGFog (void)
{
}

void Fog_DisableGFog (void)
{
}

/*
=============
Fog_StartAdditive / Fog_StopAdditive

For additive blended draws, fog color should be black so fog
fades to black instead of the fog color. We just set r_fog_color
to black temporarily.
=============
*/
void Fog_StartAdditive (void)
{
	if (Fog_GetDensity() > 0)
	{
		r_fog_color[0] = 0;
		r_fog_color[1] = 0;
		r_fog_color[2] = 0;
	}
}

void Fog_StartAdditiveDouble(void)
{
	if (Fog_GetDensity() > 0)
	{
		r_fog_density *= 10.0;
		r_fog_color[0] = 0;
		r_fog_color[1] = 0;
		r_fog_color[2] = 0;
	}
}

void Fog_StopAdditive (void)
{
	if (Fog_GetDensity() > 0)
	{
		float *c = Fog_GetColor();
		r_fog_color[0] = c[0];
		r_fog_color[1] = c[1];
		r_fog_color[2] = c[2];
	}
}

void Fog_StopAdditiveDouble(void)
{
	if (Fog_GetDensity() > 0)
	{
		r_fog_density /= 10.0;
		float *c = Fog_GetColor();
		r_fog_color[0] = c[0];
		r_fog_color[1] = c[1];
		r_fog_color[2] = c[2];
	}
}

//==============================================================================
//
//  VOLUMETRIC FOG
//
//==============================================================================

cvar_t r_vfog = {"r_vfog", "1", CVAR_NONE};

void Fog_DrawVFog (void){}
void Fog_MarkModels (void){}

//==============================================================================
//
//  INIT
//
//==============================================================================

void Fog_NewMap (void)
{
	Fog_ParseWorldspawn ();
	Fog_MarkModels ();
}

void Fog_Init (void)
{
	Cmd_AddCommand ("fog",Fog_FogCommand_f);

	fog_density = DEFAULT_DENSITY;
	fog_red = DEFAULT_GRAY;
	fog_green = DEFAULT_GRAY;
	fog_blue = DEFAULT_GRAY;
}
