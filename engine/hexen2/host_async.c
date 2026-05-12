/*
 * host_async.c -- asynchronous main-thread task queue and background save thread
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 1997-1998  Raven Software Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "quakedef.h"
#include "sdl_inc.h"

#ifdef __EMSCRIPTEN__
/* No pthreads in Emscripten build — APC is no-op, save is synchronous. */
void AsyncQueue_Init(void) {}
void AsyncQueue_Destroy(void) {}
void AsyncQueue_Drain(void) {}
void Host_InvokeOnMainThread(void (*func)(void *p), void *p) { func(p); }
void Host_InitSave(void) {}
void Host_WaitForSaveThread(void) {}
qboolean Host_IsSaving(void) { return false; }
void Host_SubmitSave(const savedata_t *sd) {
	/* Synchronous fallback: do what the worker thread would have done */
	char path[MAX_OSPATH];
	FILE *f;
	int i, err;
	Host_RemoveGIPFiles(sd->savedest);
	err = Host_CopyFiles(sd->userdir, "*.gip", sd->savedest);
	if (err) { Con_Printf("Warning: save copy failed.\n"); return; }
	if (q_snprintf(path, sizeof(path), "%s/info.dat", sd->savedest) >= (int)sizeof(path)) return;
	f = fopen(path, "w");
	if (!f) { Con_Printf("Warning: could not write info.dat.\n"); return; }
	fprintf(f, "%i\n", sd->version);
	fprintf(f, "%s\n", sd->comment);
	for (i = 0; i < NUM_SPAWN_PARMS; i++) fprintf(f, "%f\n", sd->spawn_parms[i]);
	fprintf(f, "%d\n", sd->current_skill);
	fprintf(f, "%s\n", sd->mapname);
	fprintf(f, "%f\n", sd->sv_time);
	fprintf(f, "%d\n", sd->maxclients);
	fprintf(f, "%f\n", sd->deathmatch_val);
	fprintf(f, "%f\n", sd->coop_val);
	fprintf(f, "%f\n", sd->teamplay_val);
	fprintf(f, "%f\n", sd->randomclass_val);
	fprintf(f, "%f\n", sd->playerclass_val);
	fprintf(f, "%u\n", sd->info_mask);
	fprintf(f, "%u\n", sd->info_mask2);
	if (ferror(f)) Con_Printf("Warning: save write error.\n");
	fclose(f);
}
void Host_ShutdownSave(void) {}

#else  /* !__EMSCRIPTEN__ */

#define ASYNC_QUEUE_CAPACITY  64   /* power of two */

typedef struct {
	void (*func)(void *param);
	void  *param;
} asyncproc_t;

typedef struct {
	size_t         head;
	size_t         tail;
	SDL_Mutex     *mutex;
	SDL_Condition *notfull;
	SDL_AtomicInt  teardown;
	asyncproc_t    procs[ASYNC_QUEUE_CAPACITY];
} asyncqueue_t;

static asyncqueue_t async_queue;

void AsyncQueue_Init(void) {
	async_queue.mutex   = SDL_CreateMutex();
	async_queue.notfull = SDL_CreateCondition();
	SDL_SetAtomicInt(&async_queue.teardown, 0);
}

void AsyncQueue_Destroy(void) {
	SDL_SetAtomicInt(&async_queue.teardown, 1);
	SDL_BroadcastCondition(async_queue.notfull);
	SDL_DestroyCondition(async_queue.notfull);
	SDL_DestroyMutex(async_queue.mutex);
	memset(&async_queue, 0, sizeof(async_queue));
}

void Host_InvokeOnMainThread(void (*func)(void *p), void *p) {
	SDL_LockMutex(async_queue.mutex);
	while (((async_queue.tail - async_queue.head) >= ASYNC_QUEUE_CAPACITY)
		   && !SDL_GetAtomicInt(&async_queue.teardown))
		SDL_WaitCondition(async_queue.notfull, async_queue.mutex);
	if (!SDL_GetAtomicInt(&async_queue.teardown)) {
		size_t slot = async_queue.tail % ASYNC_QUEUE_CAPACITY;
		async_queue.procs[slot].func  = func;
		async_queue.procs[slot].param = p;
		async_queue.tail++;
	}
	SDL_UnlockMutex(async_queue.mutex);
}

void AsyncQueue_Drain(void) {
	for (;;) {
		asyncproc_t proc;
		SDL_LockMutex(async_queue.mutex);
		if (async_queue.head == async_queue.tail) {
			SDL_UnlockMutex(async_queue.mutex);
			return;
		}
		proc = async_queue.procs[async_queue.head % ASYNC_QUEUE_CAPACITY];
		async_queue.head++;
		SDL_SignalCondition(async_queue.notfull);
		SDL_UnlockMutex(async_queue.mutex);
		proc.func(proc.param);
	}
}

static savedata_t      save_pending;
static SDL_Mutex      *save_mutex;
static SDL_Condition  *save_pending_cond;
static SDL_Condition  *save_finished_cond;
static SDL_AtomicInt   save_in_progress;
static SDL_AtomicInt   save_shutdown;
static SDL_Thread     *save_thread;

static void SaveError_CB(void *unused) {
	Con_Printf("Warning: background save failed.\n");
}

static int SDLCALL SaveThread_f(void *unused) {
	savedata_t sd;
	char path[MAX_OSPATH];
	FILE *f;
	int i, err;

	for (;;) {
		SDL_LockMutex(save_mutex);
		while (!SDL_GetAtomicInt(&save_in_progress)
			   && !SDL_GetAtomicInt(&save_shutdown))
			SDL_WaitCondition(save_pending_cond, save_mutex);
		if (SDL_GetAtomicInt(&save_shutdown)) {
			SDL_UnlockMutex(save_mutex);
			return 0;
		}
		sd = save_pending;
		SDL_UnlockMutex(save_mutex);

		err = 0;
		Host_RemoveGIPFiles(sd.savedest);
		err = Host_CopyFiles(sd.userdir, "*.gip", sd.savedest);
		if (err) goto done;

		if (q_snprintf(path, sizeof(path), "%s/info.dat", sd.savedest)
			>= (int)sizeof(path)) { err = 1; goto done; }
		f = fopen(path, "w");
		if (!f) { err = 1; goto done; }
		fprintf(f, "%i\n", sd.version);
		fprintf(f, "%s\n", sd.comment);
		for (i = 0; i < NUM_SPAWN_PARMS; i++)
			fprintf(f, "%f\n", sd.spawn_parms[i]);
		fprintf(f, "%d\n", sd.current_skill);
		fprintf(f, "%s\n", sd.mapname);
		fprintf(f, "%f\n", sd.sv_time);
		fprintf(f, "%d\n", sd.maxclients);
		fprintf(f, "%f\n", sd.deathmatch_val);
		fprintf(f, "%f\n", sd.coop_val);
		fprintf(f, "%f\n", sd.teamplay_val);
		fprintf(f, "%f\n", sd.randomclass_val);
		fprintf(f, "%f\n", sd.playerclass_val);
		fprintf(f, "%u\n", sd.info_mask);
		fprintf(f, "%u\n", sd.info_mask2);
		err = ferror(f);
		fclose(f);

	done:
		SDL_LockMutex(save_mutex);
		SDL_SetAtomicInt(&save_in_progress, 0);
		SDL_SignalCondition(save_finished_cond);
		SDL_UnlockMutex(save_mutex);
		if (err)
			Host_InvokeOnMainThread(SaveError_CB, NULL);
	}
}

void Host_InitSave(void) {
	save_mutex         = SDL_CreateMutex();
	save_pending_cond  = SDL_CreateCondition();
	save_finished_cond = SDL_CreateCondition();
	SDL_SetAtomicInt(&save_in_progress, 0);
	SDL_SetAtomicInt(&save_shutdown, 0);
	save_thread = SDL_CreateThread(SaveThread_f, "SaveThread", NULL);
	if (!save_thread)
		Sys_Error("Host_InitSave: SDL_CreateThread failed: %s", SDL_GetError());
}

qboolean Host_IsSaving(void) {
	return SDL_GetAtomicInt(&save_in_progress) != 0;
}

void Host_WaitForSaveThread(void) {
	SDL_LockMutex(save_mutex);
	while (SDL_GetAtomicInt(&save_in_progress))
		SDL_WaitCondition(save_finished_cond, save_mutex);
	SDL_UnlockMutex(save_mutex);
}

void Host_ShutdownSave(void) {
	SDL_SetAtomicInt(&save_shutdown, 1);
	SDL_LockMutex(save_mutex);
	SDL_SignalCondition(save_pending_cond);
	SDL_UnlockMutex(save_mutex);
	SDL_WaitThread(save_thread, NULL);
	save_thread = NULL;
	SDL_DestroyCondition(save_finished_cond);
	SDL_DestroyCondition(save_pending_cond);
	SDL_DestroyMutex(save_mutex);
}

void Host_SubmitSave(const savedata_t *data) {
	SDL_LockMutex(save_mutex);
	save_pending = *data;
	SDL_SetAtomicInt(&save_in_progress, 1);
	SDL_SignalCondition(save_pending_cond);
	SDL_UnlockMutex(save_mutex);
}

#endif  /* !__EMSCRIPTEN__ */
