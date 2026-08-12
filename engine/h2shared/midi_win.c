/*
 * midi_win.c -- MIDI module for Windows using midiStream API
 *
 * Originally from Hexen II source (C) Raven Software Corp.
 * based on an old DirectX5 sample code.
 * Few bits from Doom Legacy: Copyright (C) 1998-2000 by DooM Legacy Team.
 * Multiple fixes and cleanups and adaptation into new Hammer of Thyrion
 * (uHexen2) code by O.Sezer:
 * Copyright (C) 2006-2012 O.Sezer <sezero@users.sourceforge.net>
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
 *
 */

#include <windows.h>
#include <commctrl.h>
#include <memory.h>
/*#include <mmreg.h>*/
#include <mmsystem.h>

#include "midifile.h"
#include "mid2strm.h"
#include "quakedef.h"
#include "winquake.h"
#include "bgmusic.h"
#include "midi_drv.h"


/* prototypes of functions exported to BGM: */
static void *MIDI_Play (const char *filename);
static void MIDI_Update (void **handle);
static void MIDI_Rewind (void **handle);
static void MIDI_Stop (void **handle);
static void MIDI_Pause (void **handle);
static void MIDI_Resume (void **handle);
static void MIDI_SetVolume (void **handle, float value);

static midi_driver_t midi_win_ms =
{
	false, /* init success */
	"midiStream for Windows",
	MIDI_Init,
	MIDI_Cleanup,
	MIDI_Play,
	MIDI_Update,
	MIDI_Rewind,
	MIDI_Stop,
	MIDI_Pause,
	MIDI_Resume,
	MIDI_SetVolume,
	NULL
};

/* macros to be used with windows MIDIEVENT structure -> dwEvent */
#define MIDIEVENT_CHANNEL(x)	(x & 0x0000000F)
#define MIDIEVENT_TYPE(x)	(x & 0x000000F0)
#define MIDIEVENT_DATA1(x)	((x & 0x0000FF00) >> 8)
#define MIDIEVENT_VOLUME(x)	((x & 0x007F0000) >> 16)

static qboolean	midi_file_open, midi_playing, midi_paused;
static UINT	device_id = MIDI_MAPPER, callback_status;
static int	buf_num, num_empty_bufs;
static DWORD	volume_cache[MIDI_CHANNELS];	/* main thread only, see MidiVolume_CB */

static HMIDISTRM	hStream;
static convert_buf_t	stream_bufs[NUM_STREAM_BUFFERS];

static HANDLE		hBufferReturnEvent;

/* Set by the callback thread when a track has run out of data (or hit an
 * error) and must be torn down.  Polled by MIDI_Update on the main thread.
 * The callback cannot run MIDI_Stop itself -- see MidiProc_EndOfData. */
static LONG volatile	midi_stop_pending;

static void FreeBuffers (void);
static int  StreamBufferSetup (const char *filename);
static void CALLBACK MidiProc (HMIDIIN, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR);

void MIDI_SetAllChannelVolumes (DWORD percent);
void MIDI_SetChannelVolume (DWORD chn, DWORD percent);


#define CHECK_MIDI_ALIVE()		\
do {					\
	if (!midi_playing)		\
	{				\
		if (handle)		\
			*handle = NULL;	\
		return;			\
	}				\
} while (0)

/* Host_PrintAsync, not Con_Printf: this is reached from MidiProc, i.e. from
 * the midiStream callback thread, and the console buffer has no locking.
 * midiOutGetErrorText is a string lookup, not a device call, so it is safe
 * there.  uhexen2-99v0 */
static void MidiErrorMessageBox (MMRESULT mmr)
{
	char temp[1024];

	midiOutGetErrorText(mmr, temp, sizeof(temp));
	Host_PrintAsync("MIDI_DRV: %s\n", temp);
}

static void MIDI_SetVolume (void **handle, float value)
{
	CHECK_MIDI_ALIVE();

	/* No midiOutSetVolume path: on Vista+ it moves the app's slider in the
	 * system mixer instead of the device volume, so upstream disabled it
	 * (892bc9588) and every device is driven through per-channel main
	 * volume controllers instead.  uhexen2-q646 */
	MIDI_SetAllChannelVolumes((DWORD) (value * 1000.0f));
}

static void MIDI_Rewind (void **handle)
{
	CHECK_MIDI_ALIVE();

	/* handled by converter module */
}

static void MIDI_Update (void **handle)
{
	/* End-of-track teardown is requested by the callback thread and run
	 * here, on the main thread: MIDI_Stop calls midiStreamStop /
	 * midiOutReset / midiStreamClose, which MSDN forbids from inside a MIDI
	 * callback, and it also frees zone memory and waits on the very event
	 * the callback is supposed to signal.  BGM_Update calls us once per
	 * frame for as long as it holds a handle, so a polled flag is a
	 * guaranteed hand-off -- the APC queue can refuse work when full.
	 * uhexen2-99v0 */
	if (InterlockedExchange(&midi_stop_pending, 0))
	{
		MIDI_Stop(handle);
		return;
	}

	CHECK_MIDI_ALIVE();

	/* otherwise handled by callback */
}

qboolean MIDI_Init(void)
{
	MMRESULT mmr;
	MIDIOUTCAPS midi_caps;

	if (midi_win_ms.available)
		return true;

	BGM_RegisterMidiDRV(&midi_win_ms);

	if (safemode || COM_CheckParm("-nomidi"))
		return false;

	hBufferReturnEvent = CreateEvent(NULL, FALSE, FALSE, "uHexen2 Midi: Wait For Buffer Return");

	mmr = midiStreamOpen(&hStream, &device_id, (DWORD)1, (DWORD_PTR)MidiProc, (DWORD_PTR)0, CALLBACK_FUNCTION);
	if (mmr != MMSYSERR_NOERROR)
	{
		MidiErrorMessageBox(mmr);
		return false;
	}

	midi_file_open = false;
	midi_playing = false;
	midi_paused = false;
	callback_status = 0;
	midi_win_ms.available = true;

	Con_Printf("%s initialized.\n", midi_win_ms.desc);

	/* try to see if the MIDI device supports midiOutSetVolume */
	if (midiOutGetDevCaps(device_id, &midi_caps, sizeof(midi_caps)) == MMSYSERR_NOERROR)
	{
		if (midi_caps.dwSupport & MIDICAPS_VOLUME)
		{
			if (COM_CheckParm("-nohwmidivol"))
				Con_Printf("Hardware MIDI volume disabled by user\n");
			else
			/* midiOutSetVolume on Vista+ alters the app-specific
			 * volume in the system mixer rather than the device
			 * volume, so hardware MIDI volume is not usable. */
				Con_Printf("Hardware MIDI volume ignored (Vista+)\n");
		}
	}

	return true;
}

static void *MIDI_Play (const char *filename)
{
	MMRESULT mmr;

	if (!midi_win_ms.available)
		return NULL;

	if (!filename || !*filename)
	{
		Con_DPrintf("null music file name\n");
		return NULL;
	}

	if (StreamBufferSetup(filename))
	{
		Con_DPrintf("Couldn't open %s\n", filename);
		return NULL;
	}

	Con_Printf("Started midi music %s\n", filename);
	midi_file_open = true;
	callback_status = 0;

	mmr = midiStreamRestart(hStream);
	if (mmr != MMSYSERR_NOERROR)
	{
		MidiErrorMessageBox(mmr);
		return NULL;
	}

	midi_playing = true;
	midi_paused = false;
	/* NULL, not (void **)&hStream: the parameter is a slot that
	 * CHECK_MIDI_ALIVE zeroes to tell BGM the track is gone, and hStream is
	 * the live HMIDISTRM rather than such a slot -- passing it meant one
	 * tripped check away from nulling the stream handle itself.  Every other
	 * caller passes &midi_handle.handle, which BGM has not been given yet at
	 * this point, so there is nothing to invalidate.  uhexen2-q646 */
	MIDI_SetVolume (NULL, bgmvolume.value);

	return hStream;
}

static void MIDI_Pause (void **handle)
{
	CHECK_MIDI_ALIVE();

	if (!midi_paused)
	{
		midi_paused = true;
		midiStreamPause(hStream);
	}
}

static void MIDI_Resume (void **handle)
{
	CHECK_MIDI_ALIVE();

	if (midi_paused)
	{
		midi_paused = false;
		midiStreamRestart(hStream);
	}
}

static void MIDI_Stop (void **handle)
{
	MMRESULT mmr;

	/*CHECK_MIDI_ALIVE();*/
	if (handle)
		*handle = NULL;

	/* Consume any pending request from the callback thread: we are doing the
	 * teardown right now, and a leftover flag would otherwise kill the next
	 * track on its first MIDI_Update.  uhexen2-99v0 */
	InterlockedExchange(&midi_stop_pending, 0);

	if (midi_file_open || midi_playing)/* || callback_status != STATUS_CALLBACKDEAD)*/
	{
		midi_playing = midi_paused = false;
		if (callback_status != STATUS_CALLBACKDEAD && callback_status != STATUS_WAITINGFOREND)
			callback_status = STATUS_KILLCALLBACK;

		mmr = midiStreamStop(hStream);
		if (mmr != MMSYSERR_NOERROR)
		{
			MidiErrorMessageBox(mmr);
			return;
		}

		mmr = midiOutReset((HMIDIOUT)hStream);
		if (mmr != MMSYSERR_NOERROR)
		{
			MidiErrorMessageBox(mmr);
			return;
		}

		if (WaitForSingleObject(hBufferReturnEvent,DEBUG_CALLBACK_TIMEOUT) == WAIT_TIMEOUT)
		{
			Con_DPrintf("Timed out waiting for MIDI callback\n");
			callback_status = STATUS_CALLBACKDEAD;
		}
	}

	if (callback_status == STATUS_CALLBACKDEAD)
	{
		callback_status = 0;
		if (midi_file_open)
		{
			ConverterCleanup();
			FreeBuffers();
			if (hStream)
			{
				mmr = midiStreamClose(hStream);
				if (mmr != MMSYSERR_NOERROR)
					MidiErrorMessageBox(mmr);
				hStream = NULL;
			}

			midi_file_open = false;
		}
	}
}

void MIDI_Cleanup(void)
{
	MMRESULT mmr;

	if (!midi_win_ms.available)
		return;

	midi_win_ms.available = false;

	CloseHandle(hBufferReturnEvent);

	if (hStream)
	{
		mmr = midiStreamClose(hStream);
		if (mmr != MMSYSERR_NOERROR)
			MidiErrorMessageBox(mmr);
		hStream = NULL;
	}
}

/* FreeBuffers
 *
 * unprepares and frees all our buffers -- something we must do to
 * work around a bug in MMYSYSTEM that prevents a device from playing
 * back properly unless it is closed and reopened after each stop.
 */
static void FreeBuffers(void)
{
	int i;
	MMRESULT mmr;

	for (i = 0; i < NUM_STREAM_BUFFERS; i++)
	{
		if (stream_bufs[i].prepared)
		{
			stream_bufs[i].prepared = FALSE;
			mmr = midiOutUnprepareHeader((HMIDIOUT)hStream, &stream_bufs[i].mh, sizeof(MIDIHDR));
			if (mmr != MMSYSERR_NOERROR)
				MidiErrorMessageBox(mmr);
		}

		if (stream_bufs[i].mh.lpData)
		{
			Z_Free(stream_bufs[i].mh.lpData);
			stream_bufs[i].mh.lpData = NULL;
		}
	}
}

/* StreamBufferSetup
 *
 * Uses the filename to open a MIDI file. Then goes
 * about converting at least the first part of
 * that file into a midiStream buffer for playback.
 */
static int StreamBufferSetup(const char *filename)
{
	int err, i;
	qboolean found_end = false;
	unsigned int flags;
	MMRESULT mmr;
	MIDIPROPTIMEDIV mptd;

	if (!hStream)
	{
		mmr = midiStreamOpen(&hStream, &device_id, (DWORD)1, (DWORD_PTR)MidiProc, (DWORD_PTR)0, CALLBACK_FUNCTION);
		if (mmr != MMSYSERR_NOERROR)
		{
			MidiErrorMessageBox(mmr);
			return 1;
		}
	}

	for (i = 0; i < NUM_STREAM_BUFFERS; i++)
	{
		stream_bufs[i].mh.dwBufferLength = OUT_BUFFER_SIZE;
		/* Reuse a buffer still held from an earlier setup rather than
		 * overwriting the pointer.  This reuse is what fixes the leak:
		 * we used to Z_Malloc a fresh set over the top of a live
		 * lpData and abandon the old one, so NUM_STREAM_BUFFERS *
		 * OUT_BUFFER_SIZE went missing from the main zone for every
		 * track that failed to open -- for a mod with a bad or missing
		 * MIDI track, once per level load.  uhexen2-5rir.
		 *
		 * The error paths below must NOT free these buffers.  By the
		 * time we reach them the device may already own one via
		 * midiStreamOut, and midiOutUnprepareHeader refuses to
		 * unprepare a buffer that is still playing, so freeing here
		 * hands the device and its callback thread a dangling pointer
		 * into the zone.  Only MIDI_Stop's stop-and-wait handshake --
		 * midiStreamStop, midiOutReset, then waiting on
		 * hBufferReturnEvent -- makes freeing safe.  uhexen2-520e. */
		if (!stream_bufs[i].mh.lpData)
			stream_bufs[i].mh.lpData = (LPSTR) Z_Malloc(OUT_BUFFER_SIZE, Z_MAINZONE);
	}

	if (ConverterInit(filename))
		return 1;

	for (i = 0; i < MIDI_CHANNELS; i++)
		volume_cache[i] = VOL_CACHE_INIT;

	mptd.cbStruct = sizeof(mptd);
	mptd.dwTimeDiv = mfs.timediv;

	mmr = midiStreamProperty(hStream, (LPBYTE)&mptd, MIDIPROP_SET | MIDIPROP_TIMEDIV);
	if (mmr != MMSYSERR_NOERROR)
	{
		MidiErrorMessageBox(mmr);
		ConverterCleanup();
		return 1;
	}

	num_empty_bufs = 0;
	flags = CONVERTF_RESET;

	for (buf_num = 0; buf_num < NUM_STREAM_BUFFERS; buf_num++)
	{
	/* Tell the converter to convert up to one entire buffer's length of output
	 * data. Also, set a flag so it knows to reset any saved state variables it
	 * may keep from call to call. */
		stream_bufs[buf_num].start_ofs = 0;
		stream_bufs[buf_num].maxlen = OUT_BUFFER_SIZE;
		stream_bufs[buf_num].starttime = 0;
		stream_bufs[buf_num].times_up = FALSE;

		err = ConvertToBuffer(flags, &stream_bufs[buf_num]);
		if (err != CONVERTERR_NOERROR)
		{
			if (err == CONVERTERR_DONE)
			{
				found_end = true;
			}
			else
			{
				DEBUG_Printf("%s: Initial conversion pass failed\n", __thisfunc__);
				ConverterCleanup();
				return 1;
			}
		}
		stream_bufs[buf_num].mh.dwBytesRecorded = stream_bufs[buf_num].bytes_in;

		if (!stream_bufs[buf_num].prepared)
		{
			mmr = midiOutPrepareHeader((HMIDIOUT)hStream, &stream_bufs[buf_num].mh, sizeof(MIDIHDR));
			if (mmr != MMSYSERR_NOERROR)
			{
				MidiErrorMessageBox(mmr);
				ConverterCleanup();
				return 1;
			}
			stream_bufs[buf_num].prepared = TRUE;
		}

		mmr = midiStreamOut(hStream, &stream_bufs[buf_num].mh, sizeof(MIDIHDR));
		if (mmr != MMSYSERR_NOERROR)
		{
			MidiErrorMessageBox(mmr);
			break;
		}
		flags = 0;

		if (found_end)
			break;
	}

	buf_num = 0;

	return 0;
}

/* MidiProc_EndOfData
 *
 * Called on the callback thread when the converter has no more data for this
 * track, or when refilling a buffer failed.  Waits for the buffers the device
 * still owns to come back, then hands the teardown to the main thread rather
 * than running MIDI_Stop here -- see MIDI_Update for why.
 *
 * The all-buffers-already-back case has to be handled inline: if we merely
 * set STATUS_WAITINGFOREND there, no further MOM_DONE would ever arrive and
 * the track would sit half-stopped with the stream still open.
 */
static void MidiProc_EndOfData (void)
{
	if (num_empty_bufs < NUM_STREAM_BUFFERS)
	{
		callback_status = STATUS_WAITINGFOREND;
		return;
	}

	callback_status = STATUS_CALLBACKDEAD;
	InterlockedExchange(&midi_stop_pending, 1);
	SetEvent(hBufferReturnEvent);
}

/* MidiVolume_CB
 *
 * Main-thread half of the MOM_POSITIONCB main-volume handling.  The channel
 * number and the volume data byte are packed into the APC parameter, so this
 * needs no allocation and volume_cache stays main-thread-only.
 */
static void MidiVolume_CB (void *param)
{
	DWORD packed = (DWORD)(uintptr_t) param;
	DWORD channel = packed & (MIDI_CHANNELS - 1);

	volume_cache[channel] = (packed >> 8) & 0x7F;
	MIDI_SetChannelVolume(channel, (DWORD) (bgmvolume.value * 1000.0f));
}

/* MidiProc
 *
 * the callback handler which continually refills MIDI data buffers
 * as they're returned to us from the audio subsystem.
 *
 * Runs on a Windows-owned callback thread.  Nothing here may call a
 * multimedia function other than midiStreamOut (MSDN: doing so can
 * deadlock), touch the zone allocator, or print to the console.
 */
static void CALLBACK MidiProc(HMIDIIN hMidi, UINT uMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
	MIDIEVENT *me;
	MIDIHDR *mh;
	MMRESULT mmr;
	DWORD packed;
	int err;

	switch (uMsg)
	{
	case MOM_DONE:
		if (callback_status == STATUS_CALLBACKDEAD)
			return;

		num_empty_bufs++;

		if (callback_status == STATUS_WAITINGFOREND)
		{
			if (num_empty_bufs < NUM_STREAM_BUFFERS)
			{
				return;
			}
			else
			{
				callback_status = STATUS_CALLBACKDEAD;
				InterlockedExchange(&midi_stop_pending, 1);
				SetEvent(hBufferReturnEvent);
				return;
			}
		}

		/* this flag is set whenever the callback is waiting for all buffers to
		 * come back. */
		if (callback_status == STATUS_KILLCALLBACK)
		{
			/* count NUM_STREAM_BUFFERS-1 being returned for the last time */
			if (num_empty_bufs < NUM_STREAM_BUFFERS)
			{
				return;
			}
			/* .. then send a stop message when we get the last buffer back */
			else
			{
				callback_status = STATUS_CALLBACKDEAD;
				SetEvent(hBufferReturnEvent);
				return;
			}
		}

		/* fill an available buffer with audio data again */
		if (midi_playing && num_empty_bufs)
		{
			stream_bufs[buf_num].start_ofs = 0;
			stream_bufs[buf_num].maxlen = OUT_BUFFER_SIZE;
			stream_bufs[buf_num].starttime = 0;
			stream_bufs[buf_num].bytes_in = 0;
			stream_bufs[buf_num].times_up = FALSE;

			err = ConvertToBuffer(0, &stream_bufs[buf_num]);
			if (err != CONVERTERR_NOERROR)
			{
				if (err != CONVERTERR_DONE)
					Host_PrintAsync("MidiProc() conversion pass failed!\n");
				/* Both cases end the track.  The error case used to call
				 * ConverterCleanup() here and then keep going, which frees
				 * the track data out from under the next refill on the
				 * wrong thread; let the main thread do the cleanup as part
				 * of the normal stop.  uhexen2-99v0 */
				MidiProc_EndOfData();
				return;
			}

			stream_bufs[buf_num].mh.dwBytesRecorded = stream_bufs[buf_num].bytes_in;

			mmr = midiStreamOut(hStream, &stream_bufs[buf_num].mh, sizeof(MIDIHDR));
			if (mmr != MMSYSERR_NOERROR)
			{
				MidiErrorMessageBox(mmr);
				MidiProc_EndOfData();
				return;
			}
			buf_num = (buf_num + 1) % NUM_STREAM_BUFFERS;
			num_empty_bufs--;
		}
		break;

	case MOM_POSITIONCB:
		mh = (MIDIHDR *)dwParam1;
		me = (MIDIEVENT *)(mh->lpData + mh->dwOffset);
		if (MIDIEVENT_TYPE(me->dwEvent) == MIDICMD_CONTROL)
		{
			if (MIDIEVENT_DATA1(me->dwEvent) != MIDICTL_MSB_MAIN_VOLUME)
				break;

			/* Caching the volume byte and rescaling the channel by
			 * bgmvolume both move to the main thread: midiOutShortMsg from
			 * inside a MIDI callback can deadlock, and it raced the main
			 * thread's own midiOutShortMsg calls on the same stream.  A
			 * dropped APC (queue full) only costs one volume update, so
			 * the non-blocking variant is right here -- blocking would
			 * stall the device's callback thread.  uhexen2-99v0,
			 * uhexen2-q646 */
			packed = MIDIEVENT_CHANNEL(me->dwEvent) | (MIDIEVENT_VOLUME(me->dwEvent) << 8);
			Host_TryInvokeOnMainThread(MidiVolume_CB, (void *)(uintptr_t) packed);
		}
		break;

	default:
		break;
	}
}

/* SetAllChannelVolumes
 *
 * Given a percent in tenths of a percent, sets volume
 * on all channels to reflect the new value.
 */
void MIDI_SetAllChannelVolumes(DWORD volume_percent)
{
	DWORD i;
	DWORD event, status, vol;
	MMRESULT mmr;

	if (!midi_playing)
		return;

	for (i = 0, status = MIDICMD_CONTROL; i < MIDI_CHANNELS; i++, status++)
	{
		vol = (volume_cache[i] * volume_percent) / 1000;
		event = status | ((DWORD)MIDICTL_MSB_MAIN_VOLUME << 8) | ((DWORD)vol << 16);
		mmr = midiOutShortMsg((HMIDIOUT)hStream, event);
		if (mmr != MMSYSERR_NOERROR)
		{
			MidiErrorMessageBox(mmr);
			return;
		}
	}
}

/* SetChannelVolume
 *
 * Given a percent in tenths of a percent, sets volume
 * on a specified channel to reflect the new value.
 */
void MIDI_SetChannelVolume(DWORD channel_num, DWORD volume_percent)
{
	DWORD event, vol;
	MMRESULT mmr;

	if (!midi_playing)
		return;

	vol = (volume_cache[channel_num] * volume_percent) / 1000;
	event = MIDICMD_CONTROL | channel_num | ((DWORD)MIDICTL_MSB_MAIN_VOLUME << 8) | ((DWORD)vol << 16);

	mmr = midiOutShortMsg((HMIDIOUT)hStream, event);
	if (mmr != MMSYSERR_NOERROR)
		MidiErrorMessageBox(mmr);
}

