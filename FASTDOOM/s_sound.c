//
// Copyright (C) 1993-1996 Id Software, Inc.
// Copyright (C) 2016-2017 Alexey Khokholov (Nuke.YKT)
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:  none
//

#include <stdio.h>
#include <stdlib.h>
#include "options.h"
#include "i_system.h"
#include "i_sound.h"
#include "sounds.h"
#include "s_sound.h"

#include "z_zone.h"
#include "m_misc.h"
#include "w_wad.h"

#include "doomdef.h"
#include "doomstat.h"
#include "p_local.h"

#include "doomstat.h"
#include "dmx.h"
#include "ns_music.h"

#include "p_mobj.h"

#include "std_func.h"

#include "ns_cd.h"

// Current music/sfx card - index useless
//  w/o a reference LUT in a sound module.
extern int snd_MusicDevice;
extern int snd_SfxDevice;
// Config file? Same disclaimer as above.
extern int snd_DesiredMusicDevice;
extern int snd_DesiredSfxDevice;

int cdlooping = 0;
int cdtrack = 0;

typedef struct
{
    // sound information (if null, channel avail.)
    sfxinfo_t *sfxinfo;

    // origin of sound
    void *origin;

    // handle of the sound being played
    int handle;

} channel_t;

// the set of channels available
static channel_t *channels;

// These are not used, but should be (menu).
// Maximum volume of a sound effect.
// Internal default is max out of 0-15.
static int snd_SfxVolume;

// Maximum volume of music. Useless so far.
static int snd_MusicVolume;

extern int sfxVolume;
extern int musicVolume;

// whether songs are mus_paused
static byte mus_paused;

// music currently being played
static musicinfo_t *mus_playing = 0;

int snd_clipping = S_CLIPPING_DIST;

//
// Internals.
//
int S_getChannel(void *origin, sfxinfo_t *sfxinfo);
byte S_AdjustSoundParams(mobj_t *source, int *vol, int *sep);
void S_StopChannel(int cnum);

void S_SetMusicVolumeCD(int volume)
{
    CD_SetVolume(volume);
    snd_MusicVolume = volume;
}

void S_SetMusicVolumeMIDI(int volume)
{
    I_SetMusicVolume(volume);
    snd_MusicVolume = volume;
}

void S_SetMusicVolume(int volume)
{
    if (snd_MusicDevice == snd_CD)
        S_SetMusicVolumeCD(volume);
    else
        S_SetMusicVolumeMIDI(volume);
}

void S_StopMusicMIDI(void)
{
    if (mus_playing)
    {
        if (mus_paused)
            MUSIC_Continue();

        MUSIC_StopSong();
        //I_UnRegisterSong(mus_playing->handle);
        Z_Free(mus_playing->data);
        
        mus_playing->data = 0;
        mus_playing = 0;
    }
}

void S_ChangeMusicCD(int musicnum, int looping)
{
    unsigned long tracklength;

    cdtrack = musicnum;    
    cdlooping = looping;

    // Not enough CD tracks. At least not crash.
    if (cdtrack > CD_Cdrom_data.High_audio)
        cdtrack = cdtrack % CD_Cdrom_data.High_audio;

    tracklength = CD_GetTrackLength(cdtrack);
    CD_SetTrack(cdtrack);
    CD_Seek(CD_Cdrom_data.Track_position);
    delay(400);
    CD_PlayAudio(CD_Cdrom_data.Track_position, CD_Cdrom_data.Track_position + tracklength);
}

void S_CheckCD(void)
{
    if (cdlooping)
    {
        CD_Status();
        if (CD_Cdrom_data.Status & 0x100)
            S_ChangeMusicCD(cdtrack, cdlooping);
    }
}

void S_ChangeMusicMIDI(int musicnum, int looping)
{
    musicinfo_t *music;
    char namebuf[9];

    if (snd_MusicDevice == snd_none)
        return;

    if ((snd_MusicDevice == snd_Adlib || snd_MusicDevice == snd_OPL2LPT || snd_MusicDevice == snd_SB || snd_MusicDevice == snd_OPL3LPT || snd_MusicDevice == snd_CMS) && musicnum == mus_intro)
    {
        musicnum = mus_introa;
    }

    music = &S_music[musicnum];

    if (mus_playing == music)
        return;

    // shutdown old music
    S_StopMusicMIDI();

    // get lumpnum if neccessary
    if (!music->lumpnum)
    {
        sprintf(namebuf, "D_%s", music->name);
        music->lumpnum = W_GetNumForName(namebuf);
    }

    // load & register it
    music->data = (void *)W_CacheLumpNum(music->lumpnum, PU_MUSIC);
    music->handle = MUS_RegisterSong(music->data);

    // play it
    MUS_ChainSong(music->handle, looping ? music->handle : -1);
    MUS_PlaySong(music->handle, snd_MusicVolume);

    mus_playing = music;
}

void S_ChangeMusic(int musicnum, int looping)
{
    if (snd_MusicDevice == snd_CD)
        S_ChangeMusicCD(musicnum, looping);
    else
        S_ChangeMusicMIDI(musicnum, looping);
}

void S_StopChannel(int cnum)
{

    int i;
    channel_t *c = &channels[cnum];

    if (c->sfxinfo)
    {
        // stop the sound playing
        if (SFX_Playing(c->handle))
        {
            SFX_StopPatch(c->handle);
        }
        c->sfxinfo = 0;
    }
}

//
// Changes volume, stereo-separation, and pitch variables
//  from the norm of a sound effect to be played.
// If the sound is not audible, returns a 0.
// Otherwise, modifies parameters and returns 1.
//
byte S_AdjustSoundParams(mobj_t *source, int *vol, int *sep)
{
    fixed_t approx_dist;
    fixed_t adx;
    fixed_t ady;
    angle_t angle;

    fixed_t optSine;

    // calculate the distance to sound origin
    //  and clip it if necessary
    adx = abs(players_mo->x - source->x);
    ady = abs(players_mo->y - source->y);

    // From _GG1_ p.428. Appox. eucledian distance fast.
    approx_dist = adx + ady - ((adx < ady ? adx : ady) >> 1);

    if (approx_dist > snd_clipping)
    {
        return 1; // Non audible
    }

    // volume calculation
    if (approx_dist < S_CLOSE_DIST)
    {
        *vol = snd_SfxVolume;
    }
    else
    {
        // distance effect
        *vol = Div1000(snd_SfxVolume * ((snd_clipping - approx_dist) >> FRACBITS));
    }

    if (*vol == 0){
        return 1; // Non audible
    }

    if (monoSound)
    {
        *sep = NORM_SEP;
    }
    else
    {
        // angle of source to listener
        angle = R_PointToAngle2(players_mo->x, players_mo->y, source->x, source->y);

        angle -= players_mo->angle;
        if (angle <= players_mo->angle)
            angle += 0xffffffff;

        angle >>= ANGLETOFINESHIFT;

        // stereo separation (S_STEREO_SWING == 2^21 + 2^22)
        optSine = finesine[angle];
        *sep = 128 - (((optSine << 6) + (optSine << 5)) >> FRACBITS);
    }

    return 0; // Audible
}

void S_SetSfxVolume(int volume)
{
    snd_SfxVolume = volume;
}

//
// Stop and resume music, during game PAUSE.
//
void S_PauseMusic(void)
{
    if (snd_MusicDevice == snd_CD)
    {
        CD_StopAudio();
        mus_paused = 1;
        return;
    }

    if (mus_playing && !mus_paused)
    {
        MUSIC_Pause();
        mus_paused = 1;
    }
}

void S_ResumeMusic(void)
{
    if (snd_MusicDevice == snd_CD)
    {
        CD_ResumeAudio();
        mus_paused = 0;
        return;
    }

    if (mus_playing && mus_paused)
    {
        MUSIC_Continue();
        mus_paused = 0;
    }
}

void S_StopSound(void *origin)
{
    int cnum;

    if (snd_SfxDevice == snd_none)
        return;

    for (cnum = 0; cnum < numChannels; cnum++)
    {
        if (channels[cnum].sfxinfo && channels[cnum].origin == origin)
        {
            S_StopChannel(cnum);
            break;
        }
    }
}

//
// S_getChannel :
//   If none available, return -1.  Otherwise channel #.
//
int S_getChannel(void *origin, sfxinfo_t *sfxinfo)
{
    // channel number to use
    int cnum;

    channel_t *c;

    // Find an open channel
    for (cnum = 0; cnum < numChannels; cnum++)
    {
        if (!channels[cnum].sfxinfo)
            break;
        else if (origin && channels[cnum].origin == origin)
        {
            S_StopChannel(cnum);
            break;
        }
    }

    // None available
    if (cnum == numChannels)
    {
        // Look for lower priority
        for (cnum = 0; cnum < numChannels; cnum++)
            if (channels[cnum].sfxinfo->priority >= sfxinfo->priority)
                break;

        if (cnum == numChannels)
        {
            // FUCK!  No lower priority.  Sorry, Charlie.
            return -1;
        }
        else
        {
            // Otherwise, kick out lower priority.
            S_StopChannel(cnum);
        }
    }

    c = &channels[cnum];

    // channel is decided to be cnum.
    c->sfxinfo = sfxinfo;
    c->origin = origin;

    return cnum;
}

void S_StartSound(mobj_t *origin, byte sfx_id)
{
    int sep;
    sfxinfo_t *sfx;
    int cnum;
    int volume;

    if (snd_SfxDevice == snd_none)
        return;

    volume = snd_SfxVolume;
    
    // Check to see if it is audible,
    //  and if not, modify the params
    if (origin && origin != players_mo)
    {
        byte rc = S_AdjustSoundParams(origin, &volume, &sep);

        if (rc){
            return;
        }

        if (origin->x == players_mo->x && origin->y == players_mo->y)
        {
            sep = NORM_SEP;
        }
    }
    else
    {
        sep = NORM_SEP;
    }

    // kill old sound
    S_StopSound(origin);

    sfx = &S_sfx[sfx_id];

    // try to find a channel
    cnum = S_getChannel(origin, sfx);

    if (cnum < 0)
        return;

    //
    // This is supposed to handle the loading/caching.
    // For some odd reason, the caching is done nearly
    //  each time the sound is needed?
    //

    // get lumpnum if necessary
    if (sfx->lumpnum < 0)
        sfx->lumpnum = I_GetSfxLumpNum(sfx);

    // cache data if necessary
    if (!sfx->data)
    {
        sfx->data = (void *)W_CacheLumpNum(sfx->lumpnum, PU_SOUND);
    }

    // Assigns the handle to one of the channels in the
    //  mix/output buffer.
    channels[cnum].handle = SFX_PlayPatch(sfx->data, sep, volume);
}

//
// Updates music & sounds
//
void S_UpdateSounds(void)
{
    int cnum;
    int volume;
    int sep;

    if (snd_SfxDevice == snd_none)
        return;

    for (cnum = 0; cnum < numChannels; cnum++)
    {
        channel_t *c = &channels[cnum];
        if (c->sfxinfo)
        {
            if (SFX_Playing(c->handle))
            {
                // initialize parameters
                volume = snd_SfxVolume;
                sep = NORM_SEP;

                // check non-local sounds for distance clipping
                //  or modify their params
                if (c->origin && players_mo != c->origin)
                {
                    byte audible = S_AdjustSoundParams(c->origin, &volume, &sep);

                    if (audible){
                        S_StopChannel(cnum);
                    }
                    else
                        SFX_SetOrigin(c->handle, sep, volume);
                }
            }
            else
            {
                // if channel is allocated but sound has stopped,
                //  free it
                S_StopChannel(cnum);
            }
        }
    }
}

//
// Initializes sound stuff, including volume
// Sets channels, SFX and music volume,
//  allocates channel buffer, sets S_sfx lookup.
//
void S_Init(int sfxVolume, int musicVolume)
{
    int i;

    S_SetSfxVolume(sfxVolume);
    // No music with Linux - another dummy.
    S_SetMusicVolume(musicVolume);

    // Allocating the internal channels for mixing
    // (the maximum numer of sounds rendered
    // simultaneously) within zone memory.
    channels = (channel_t *)Z_MallocUnowned(numChannels * sizeof(channel_t), PU_STATIC);

    // Free all channels for use
    for (i = 0; i < numChannels; i++)
        channels[i].sfxinfo = 0;

    // no sounds are playing, and they are not mus_paused
    mus_paused = 0;

    // Note that sounds have not been cached (yet).
    for (i = 1; i < NUMSFX; i++)
        S_sfx[i].lumpnum = -1;
}

//
// Per level startup code.
// Kills playing sounds at start of level,
//  determines music if any, changes music.
//
void S_Start(void)
{
    int cnum;
    int mnum;

    // kill all playing sounds at start of level
    //  (trust me - a good idea)
    for (cnum = 0; cnum < numChannels; cnum++)
        if (channels[cnum].sfxinfo)
            S_StopChannel(cnum);

    // start new music for the level
    mus_paused = 0;

    if (gamemode == commercial)
        mnum = mus_runnin + gamemap - 1;
    else
    {
        int spmus[] =
            {
                // Song - Who? - Where?

                mus_e3m4, // American	e4m1
                mus_e3m2, // Romero	e4m2
                mus_e3m3, // Shawn	e4m3
                mus_e1m5, // American	e4m4
                mus_e2m7, // Tim 	e4m5
                mus_e2m4, // Romero	e4m6
                mus_e2m6, // J.Anderson	e4m7 CHIRON.WAD
                mus_e2m5, // Shawn	e4m8
                mus_e1m9  // Tim		e4m9
            };

        if (gameepisode < 4)
            mnum = mus_e1m1 + (gameepisode - 1) * 9 + gamemap - 1;
        else
            mnum = spmus[gamemap - 1];
    }

    // HACK FOR COMMERCIAL
    //  if (gamemode == commercial && mnum > mus_e3m9)
    //      mnum -= mus_e3m9;

    S_ChangeMusic(mnum, true);
}

void S_ClearSounds(void)
{
    unsigned short i;

    if (snd_SfxDevice == snd_none)
        return;

    // Clean up unused data.
    for (i = 1; i < NUMSFX; i++)
    {
        if (S_sfx[i].data != 0)
        {
            Z_Free(S_sfx[i].data);
            S_sfx[i].data = 0;
        }
    }
}
