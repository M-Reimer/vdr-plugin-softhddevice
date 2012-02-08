///
///	@file softhddevice.cpp	@brief A software HD device plugin for VDR.
///
///	Copyright (c) 2011, 2012 by Johns.  All Rights Reserved.
///
///	Contributor(s):
///
///	License: AGPLv3
///
///	This program is free software: you can redistribute it and/or modify
///	it under the terms of the GNU Affero General Public License as
///	published by the Free Software Foundation, either version 3 of the
///	License.
///
///	This program is distributed in the hope that it will be useful,
///	but WITHOUT ANY WARRANTY; without even the implied warranty of
///	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
///	GNU Affero General Public License for more details.
///
///	$Id$
//////////////////////////////////////////////////////////////////////////////

#include <vdr/interface.h>
#include <vdr/plugin.h>
#include <vdr/player.h>
#include <vdr/osd.h>
#include <vdr/dvbspu.h>
#include <vdr/shutdown.h>

#ifdef HAVE_CONFIG
#include "config.h"
#endif

#include "softhddev.h"
#include "softhddevice.h"
extern "C"
{
#include "video.h"
    extern void AudioPoller(void);
    extern void CodecSetAudioPassthrough(int);
}

//////////////////////////////////////////////////////////////////////////////

static const char *const VERSION = "0.4.7";
static const char *const DESCRIPTION =
trNOOP("A software and GPU emulated HD device");

static const char *MAINMENUENTRY = trNOOP("Suspend Soft-HD-Device");
static class cSoftHdDevice *MyDevice;

//////////////////////////////////////////////////////////////////////////////

#define RESOLUTIONS 4			///< number of resolutions

    /// resolutions names
static const char *const Resolution[RESOLUTIONS] = {
    "576i", "720p", "1080i_fake", "1080i"
};

static char ConfigMakePrimary;		///< config primary wanted
static char ConfigHideMainMenuEntry;	///< config hide main menu entry

static int ConfigVideoSkipLines;	///< config skip lines top/bottom

    /// config deinterlace
static int ConfigVideoDeinterlace[RESOLUTIONS];

    /// config skip chroma
static int ConfigVideoSkipChromaDeinterlace[RESOLUTIONS];

    /// config denoise
static int ConfigVideoDenoise[RESOLUTIONS];

    /// config sharpen
static int ConfigVideoSharpen[RESOLUTIONS];

    /// config scaling
static int ConfigVideoScaling[RESOLUTIONS];

static int ConfigVideoAudioDelay;	///< config audio delay
static int ConfigAudioPassthrough;	///< config audio pass-through

static int ConfigAutoCropInterval;	///< auto crop detection interval
static int ConfigAutoCropDelay;		///< auto crop detection delay
static int ConfigAutoCropTolerance;	///< auto crop detection tolerance

static char ConfigSuspendClose;		///< suspend should close devices
static char ConfigSuspendX11;		///< suspend should stop x11

static volatile char DoMakePrimary;	///< flag switch primary

//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////
//	C Callbacks
//////////////////////////////////////////////////////////////////////////////

class cSoftRemote:public cRemote
{
  public:
    cSoftRemote(const char *name):cRemote(name)
    {
    }

    bool Put(const char *code, bool repeat = false, bool release = false) {
	return cRemote::Put(code, repeat, release);
    }
};

extern "C" void FeedKeyPress(const char *keymap, const char *key, int repeat,
    int release)
{
    cRemote *remote;
    cSoftRemote *csoft;

    if (!keymap || !key) {
	return;
    }
    // find remote
    for (remote = Remotes.First(); remote; remote = Remotes.Next(remote)) {
	if (!strcmp(remote->Name(), keymap)) {
	    break;
	}
    }

    if (remote) {
	csoft = (cSoftRemote *) remote;
    } else {
	dsyslog("[softhddev]%s: remote '%s' not found\n", __FUNCTION__,
	    keymap);
	csoft = new cSoftRemote(keymap);
    }

    //dsyslog("[softhddev]%s %s, %s\n", __FUNCTION__, keymap, key);
    csoft->Put(key, repeat, release);
}

//////////////////////////////////////////////////////////////////////////////
//	OSD
//////////////////////////////////////////////////////////////////////////////

class cSoftOsd:public cOsd
{
    int Level;				///< level: subtitle

  public:
     cSoftOsd(int, int, uint);
     virtual ~ cSoftOsd(void);
    virtual void Flush(void);
    virtual void SetActive(bool);
};

static volatile char OsdDirty;		///< flag force redraw everything

/**
**	Sets this OSD to be the active one.
**
**	@param on	true on, false off
**
**	@note only needed as workaround for text2skin plugin with
**	undrawn areas.
*/
void cSoftOsd::SetActive(bool on)
{
    dsyslog("[softhddev]%s: %d\n", __FUNCTION__, on);

    if (Active() == on) {
	return;				// already active, no action
    }
    cOsd::SetActive(on);
    if (on) {
	OsdDirty = 1;
    } else {
	OsdClose();
    }
}

cSoftOsd::cSoftOsd(int left, int top, uint level)
:cOsd(left, top, level)
{
    /* FIXME: OsdWidth/OsdHeight not correct!
       dsyslog("[softhddev]%s: %dx%d+%d+%d, %d\n", __FUNCTION__, OsdWidth(),
       OsdHeight(), left, top, level);
     */

    this->Level = level;
    SetActive(true);
}

cSoftOsd::~cSoftOsd(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);
    SetActive(false);
    // done by SetActive: OsdClose();

#ifdef USE_YAEPG
    // support yaepghd, video window
    if (vidWin.bpp) {			// restore fullsized video
	int width;
	int height;
	double video_aspect;

	::GetOsdSize(&width, &height, &video_aspect);
	// works osd relative
	VideoSetOutputPosition(0, 0, width, height);
    }
#endif
}

/**
**	Actually commits all data to the OSD hardware.
*/
void cSoftOsd::Flush(void)
{
    cPixmapMemory *pm;

    if (!Active()) {
	return;
    }
#ifdef USE_YAEPG
    // support yaepghd, video window
    if (vidWin.bpp) {
	dsyslog("[softhddev]%s: %dx%d+%d+%d\n", __FUNCTION__, vidWin.Width(),
	    vidWin.Height(), vidWin.x1, vidWin.y2);

	// FIXME: vidWin is OSD relative not video window.
	VideoSetOutputPosition(Left() + vidWin.x1, Top() + vidWin.y1,
	    vidWin.Width(), vidWin.Height());
    }
#endif

    if (!IsTrueColor()) {
	static char warned;
	cBitmap *bitmap;
	int i;

	if (!warned) {
	    dsyslog("[softhddev]%s: FIXME: should be truecolor\n",
		__FUNCTION__);
	    warned = 1;
	}
	// draw all bitmaps
	for (i = 0; (bitmap = GetBitmap(i)); ++i) {
	    uint8_t *argb;
	    int x;
	    int y;
	    int w;
	    int h;
	    int x1;
	    int y1;
	    int x2;
	    int y2;

	    // get dirty bounding box
	    if (OsdDirty) {		// forced complete update
		x1 = 0;
		y1 = 0;
		x2 = bitmap->Width() - 1;
		y2 = bitmap->Height() - 1;
	    } else if (!bitmap->Dirty(x1, y1, x2, y2)) {
		continue;		// nothing dirty continue
	    }
	    // convert and upload only dirty areas
	    w = x2 - x1 + 1;
	    h = y2 - y1 + 1;
	    if (1) {			// just for the case it makes trouble
		int width;
		int height;
		double video_aspect;

		::GetOsdSize(&width, &height, &video_aspect);
		if (w > width) {
		    w = width;
		    x2 = x1 + width - 1;
		}
		if (h > height) {
		    h = height;
		    y2 = y1 + height - 1;
		}
	    }
#ifdef DEBUG
	    if (w > bitmap->Width() || h > bitmap->Height()) {
		esyslog(tr("softhdev: dirty area too big\n"));
		abort();
	    }
#endif
	    argb = (uint8_t *) malloc(w * h * sizeof(uint32_t));
	    for (y = y1; y <= y2; ++y) {
		for (x = x1; x <= x2; ++x) {
		    ((uint32_t *) argb)[x - x1 + (y - y1) * w] =
			bitmap->GetColor(x, y);
		}
	    }
	    OsdDrawARGB(Left() + bitmap->X0() + x1, Top() + bitmap->Y0() + y1,
		w, h, argb);

	    bitmap->Clean();
	    // FIXME: reuse argb
	    free(argb);
	}
	OsdDirty = 0;
	return;
    }

    LOCK_PIXMAPS;
    while ((pm = RenderPixmaps())) {
	int x;
	int y;
	int w;
	int h;

	x = Left() + pm->ViewPort().X();
	y = Top() + pm->ViewPort().Y();
	w = pm->ViewPort().Width();
	h = pm->ViewPort().Height();

	/*
	   dsyslog("[softhddev]%s: draw %dx%d+%d+%d %p\n", __FUNCTION__, w, h,
	   x, y, pm->Data());
	 */

	OsdDrawARGB(x, y, w, h, pm->Data());

	delete pm;
    }
}

//////////////////////////////////////////////////////////////////////////////
//	OSD provider
//////////////////////////////////////////////////////////////////////////////

class cSoftOsdProvider:public cOsdProvider
{
  private:
    static cOsd *Osd;
  public:
    virtual cOsd * CreateOsd(int, int, uint);
    virtual bool ProvidesTrueColor(void);
    cSoftOsdProvider(void);
};

cOsd *cSoftOsdProvider::Osd;		///< single osd

/**
**	Create a new OSD.
**
**	@param left	x-coordinate of OSD
**	@param top	y-coordinate of OSD
**	@param level	layer level of OSD
*/
cOsd *cSoftOsdProvider::CreateOsd(int left, int top, uint level)
{
    //dsyslog("[softhddev]%s: %d, %d, %d\n", __FUNCTION__, left, top, level);

    Osd = new cSoftOsd(left, top, level);
    return Osd;
}

/**
**	 Returns true if this OSD provider is able to handle a true color OSD.
*/
bool cSoftOsdProvider::ProvidesTrueColor(void)
{
    return true;
}

/**
**	Create cOsdProvider class.
*/
cSoftOsdProvider::cSoftOsdProvider(void)
:  cOsdProvider()
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);
}

//////////////////////////////////////////////////////////////////////////////
//	cMenuSetupPage
//////////////////////////////////////////////////////////////////////////////

class cMenuSetupSoft:public cMenuSetupPage
{
  protected:
    int MakePrimary;
    int HideMainMenuEntry;
    int SkipLines;
    int Scaling[RESOLUTIONS];
    int Deinterlace[RESOLUTIONS];
    int SkipChromaDeinterlace[RESOLUTIONS];
    int Denoise[RESOLUTIONS];
    int Sharpen[RESOLUTIONS];
    int AudioDelay;
    int AudioPassthrough;
    int AutoCropInterval;
    int AutoCropDelay;
    int AutoCropTolerance;
    int SuspendClose;
    int SuspendX11;
  protected:
     virtual void Store(void);
  public:
     cMenuSetupSoft(void);
};

/**
**	Create a seperator item.
**
**	@param label	text inside separator
*/
static inline cOsdItem *SeparatorItem(const char *label)
{
    cOsdItem *item;

    item = new cOsdItem(cString::sprintf("* %s: ", label));
    item->SetSelectable(false);

    return item;
}

/**
**	Constructor setup menu.
*/
cMenuSetupSoft::cMenuSetupSoft(void)
{
    static const char *const deinterlace[] = {
	"Bob", "Weave/None", "Temporal", "TemporalSpatial", "Software"
    };
    static const char *const scaling[] = {
	"Normal", "Fast", "HQ", "Anamorphic"
    };
    static const char *const passthrough[] = {
	"None", "AC-3"
    };
    static const char *const resolution[RESOLUTIONS] = {
	"576i", "720p", "fake 1080i", "1080i"
    };
    int i;

    // cMenuEditBoolItem cMenuEditBitItem cMenuEditNumItem
    // cMenuEditStrItem cMenuEditStraItem cMenuEditIntItem
    MakePrimary = ConfigMakePrimary;
    Add(new cMenuEditBoolItem(tr("Make primary device"), &MakePrimary,
	    trVDR("no"), trVDR("yes")));
    HideMainMenuEntry = ConfigHideMainMenuEntry;
    Add(new cMenuEditBoolItem(tr("Hide main menu entry"), &HideMainMenuEntry,
	    trVDR("no"), trVDR("yes")));
    //
    //	video
    //
    Add(SeparatorItem(tr("Video")));

    SkipLines = ConfigVideoSkipLines;
    Add(new cMenuEditIntItem(tr("Skip lines top+bot (pixel)"), &SkipLines, 0,
	    64));

    for (i = 0; i < RESOLUTIONS; ++i) {
	Add(SeparatorItem(resolution[i]));
	Scaling[i] = ConfigVideoScaling[i];
	Add(new cMenuEditStraItem(tr("Scaling"), &Scaling[i], 4, scaling));
	Deinterlace[i] = ConfigVideoDeinterlace[i];
	Add(new cMenuEditStraItem(tr("Deinterlace"), &Deinterlace[i], 5,
		deinterlace));
	SkipChromaDeinterlace[i] = ConfigVideoSkipChromaDeinterlace[i];
	Add(new cMenuEditBoolItem(tr("SkipChromaDeinterlace (vdpau)"),
		&SkipChromaDeinterlace[i], trVDR("no"), trVDR("yes")));
	Denoise[i] = ConfigVideoDenoise[i];
	Add(new cMenuEditIntItem(tr("Denoise (0..1000) (vdpau)"), &Denoise[i],
		0, 1000));
	Sharpen[i] = ConfigVideoSharpen[i];
	Add(new cMenuEditIntItem(tr("Sharpen (-1000..1000) (vdpau)"),
		&Sharpen[i], -1000, 1000));
    }
    //
    //	audio
    //
    Add(SeparatorItem(tr("Audio")));
    AudioDelay = ConfigVideoAudioDelay;
    Add(new cMenuEditIntItem(tr("Audio delay (ms)"), &AudioDelay, -1000,
	    1000));
    AudioPassthrough = ConfigAudioPassthrough;
    Add(new cMenuEditStraItem(tr("Audio pass-through"), &AudioPassthrough, 2,
	    passthrough));
    //
    //	auto-crop
    //
    Add(SeparatorItem(tr("Auto-crop")));
    AutoCropInterval = ConfigAutoCropInterval;
    Add(new cMenuEditIntItem(tr("autocrop interval (frames)"),
	    &AutoCropInterval, 0, 200));
    AutoCropDelay = ConfigAutoCropDelay;
    Add(new cMenuEditIntItem(tr("autocrop delay (n * interval)"),
	    &AutoCropDelay, 0, 200));
    AutoCropTolerance = ConfigAutoCropTolerance;
    Add(new cMenuEditIntItem(tr("autocrop tolerance (pixel)"),
	    &AutoCropTolerance, 0, 32));
    //
    //	suspend
    //
    Add(SeparatorItem(tr("Suspend")));
    SuspendClose = ConfigSuspendClose;
    Add(new cMenuEditBoolItem(tr("suspend closes video+audio"), &SuspendClose,
	    trVDR("no"), trVDR("yes")));
    SuspendX11 = ConfigSuspendX11;
    Add(new cMenuEditBoolItem(tr("suspend stops x11"), &SuspendX11,
	    trVDR("no"), trVDR("yes")));
}

/**
**	Store setup.
*/
void cMenuSetupSoft::Store(void)
{
    int i;

    SetupStore("MakePrimary", ConfigMakePrimary = MakePrimary);
    SetupStore("HideMainMenuEntry", ConfigHideMainMenuEntry =
	HideMainMenuEntry);

    SetupStore("SkipLines", ConfigVideoSkipLines = SkipLines);
    VideoSetSkipLines(ConfigVideoSkipLines);

    for (i = 0; i < RESOLUTIONS; ++i) {
	char buf[128];

	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "Scaling");
	SetupStore(buf, ConfigVideoScaling[i] = Scaling[i]);
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "Deinterlace");
	SetupStore(buf, ConfigVideoDeinterlace[i] = Deinterlace[i]);
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i],
	    "SkipChromaDeinterlace");
	SetupStore(buf, ConfigVideoSkipChromaDeinterlace[i] =
	    SkipChromaDeinterlace[i]);
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "Denoise");
	SetupStore(buf, ConfigVideoDenoise[i] = Denoise[i]);
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "Sharpen");
	SetupStore(buf, ConfigVideoSharpen[i] = Sharpen[i]);
    }
    VideoSetScaling(ConfigVideoScaling);
    VideoSetDeinterlace(ConfigVideoDeinterlace);
    VideoSetSkipChromaDeinterlace(ConfigVideoSkipChromaDeinterlace);
    VideoSetDenoise(ConfigVideoDenoise);
    VideoSetSharpen(ConfigVideoSharpen);

    SetupStore("AudioDelay", ConfigVideoAudioDelay = AudioDelay);
    VideoSetAudioDelay(ConfigVideoAudioDelay);
    SetupStore("AudioPassthrough", ConfigAudioPassthrough = AudioPassthrough);
    CodecSetAudioPassthrough(ConfigAudioPassthrough);

    SetupStore("AutoCrop.Interval", ConfigAutoCropInterval = AutoCropInterval);
    SetupStore("AutoCrop.Delay", ConfigAutoCropDelay = AutoCropDelay);
    SetupStore("AutoCrop.Tolerance", ConfigAutoCropTolerance =
	AutoCropTolerance);
    VideoSetAutoCrop(ConfigAutoCropInterval, ConfigAutoCropDelay,
	ConfigAutoCropTolerance);

    SetupStore("Suspend.Close", ConfigSuspendClose = SuspendClose);
    SetupStore("Suspend.X11", ConfigSuspendX11 = SuspendX11);
}

//////////////////////////////////////////////////////////////////////////////
//	cPlayer
//////////////////////////////////////////////////////////////////////////////

/**
**	Dummy player for suspend mode.
*/
class cSoftHdPlayer:public cPlayer
{
  protected:
  public:
    cSoftHdPlayer(void);
    virtual ~ cSoftHdPlayer();
};

cSoftHdPlayer::cSoftHdPlayer(void)
{
}

cSoftHdPlayer::~cSoftHdPlayer()
{
    Detach();
}

//////////////////////////////////////////////////////////////////////////////
//	cControl
//////////////////////////////////////////////////////////////////////////////

/**
**	Dummy control for suspend mode.
*/
class cSoftHdControl:public cControl
{
  public:
    static cSoftHdPlayer *Player;	///< dummy player
    virtual void Hide(void)
    {
    }
    virtual eOSState ProcessKey(eKeys);

    cSoftHdControl(void);

    virtual ~ cSoftHdControl();
};

cSoftHdPlayer *cSoftHdControl::Player;

/**
**	Handle a key event.
**
**	@param key	key pressed
*/
eOSState cSoftHdControl::ProcessKey(eKeys key)
{
    if (!ISMODELESSKEY(key) || key == kBack || key == kStop) {
	if (Player) {
	    delete Player;

	    Player = NULL;
	}
	Resume();
	return osEnd;
    }
    return osContinue;
}

cSoftHdControl::cSoftHdControl(void)
:  cControl(Player = new cSoftHdPlayer)
{
}

cSoftHdControl::~cSoftHdControl()
{
    if (Player) {
	delete Player;

	Player = NULL;
    }
    Resume();
}

//////////////////////////////////////////////////////////////////////////////
//	cDevice
//////////////////////////////////////////////////////////////////////////////

class cSoftHdDevice:public cDevice
{
  public:
    cSoftHdDevice(void);
     virtual ~ cSoftHdDevice(void);

    virtual bool HasDecoder(void) const;
    virtual bool CanReplay(void) const;
    virtual bool SetPlayMode(ePlayMode);
    virtual void TrickSpeed(int);
    virtual void Clear(void);
    virtual void Play(void);
    virtual void Freeze(void);
    virtual void Mute(void);
    virtual void SetVolumeDevice(int);
    virtual void StillPicture(const uchar *, int);
    virtual bool Poll(cPoller &, int = 0);
    virtual bool Flush(int = 0);
    virtual int64_t GetSTC(void);
    virtual void SetVideoDisplayFormat(eVideoDisplayFormat);
    virtual void GetVideoSize(int &, int &, double &);
    virtual void GetOsdSize(int &, int &, double &);
    virtual int PlayVideo(const uchar *, int);

    //virtual int PlayTsVideo(const uchar *, int);
#ifndef USE_AUDIO_THREAD		// FIXME: testing none threaded
    virtual int PlayTsAudio(const uchar *, int);
#endif
    virtual void SetAudioChannelDevice(int);
    virtual int GetAudioChannelDevice(void);
    virtual void SetDigitalAudioDevice(bool);
    virtual void SetAudioTrackDevice(eTrackType);
    virtual int PlayAudio(const uchar *, int, uchar);

// Image Grab facilities

    virtual uchar *GrabImage(int &, bool, int, int, int);

#if 0
// SPU facilities
  private:
     cDvbSpuDecoder * spuDecoder;
  public:
     virtual cSpuDecoder * GetSpuDecoder(void);
#endif

  protected:
     virtual void MakePrimaryDevice(bool);
};

cSoftHdDevice::cSoftHdDevice(void)
{
    //dsyslog("[softhddev]%s\n", __FUNCTION__);

#if 0
    spuDecoder = NULL;
#endif
}

cSoftHdDevice::~cSoftHdDevice(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);
}

/**
**	Informs a device that it will be the primary device.
**
**	@param on	flag if becoming or loosing primary
*/
void cSoftHdDevice::MakePrimaryDevice(bool on)
{
    dsyslog("[softhddev]%s: %d\n", __FUNCTION__, on);

    cDevice::MakePrimaryDevice(on);
    if (on) {
	new cSoftOsdProvider();
    }
}

#if 0

cSpuDecoder *cSoftHdDevice::GetSpuDecoder(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    if (IsPrimaryDevice() && !spuDecoder) {
	spuDecoder = new cDvbSpuDecoder();
    }
    return spuDecoder;
}

#endif

bool cSoftHdDevice::HasDecoder(void) const
{
    return true;
}

/**
**	Returns true if this device can currently start a replay session.
*/
bool cSoftHdDevice::CanReplay(void) const
{
    return true;
}

/**
**	 Sets the device into the given play mode.
*/
bool cSoftHdDevice::SetPlayMode(ePlayMode play_mode)
{
    dsyslog("[softhddev]%s: %d\n", __FUNCTION__, play_mode);

    switch (play_mode) {
	case pmAudioVideo:
	    break;
	case pmAudioOnly:
	case pmAudioOnlyBlack:
	    break;
	case pmVideoOnly:
	    break;
	case pmNone:
	    return true;
	case pmExtern_THIS_SHOULD_BE_AVOIDED:
	    dsyslog("[softhddev] play mode external\n");
	    Suspend(1, 1, 0);
	    return true;
	default:
	    dsyslog("[softhddev]playmode not implemented... %d\n", play_mode);
	    break;
    }
    ::SetPlayMode();
    return true;
}

/**
**	Gets the current System Time Counter, which can be used to
**	synchronize audio, video and subtitles.
*/
int64_t cSoftHdDevice::GetSTC(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    return::VideoGetClock();
}

/**
**	Set trick play speed.
**
**	@param speed	trick speed
*/
void cSoftHdDevice::TrickSpeed(int speed)
{
    dsyslog("[softhddev]%s: %d\n", __FUNCTION__, speed);
}

void cSoftHdDevice::Clear(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    cDevice::Clear();
    ::Clear();
}

void cSoftHdDevice::Play(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    cDevice::Play();
    ::Play();
}

/**
**	Puts the device into "freeze frame" mode.
*/
void cSoftHdDevice::Freeze(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    cDevice::Freeze();
    ::Freeze();
}

void cSoftHdDevice::Mute(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    cDevice::Mute();
    ::Mute();
}

void cSoftHdDevice::SetVolumeDevice(int volume)
{
    dsyslog("[softhddev]%s: %d\n", __FUNCTION__, volume);

    ::SetVolumeDevice(volume);
}

/**
**	Display the given I-frame as a still picture.
*/
void cSoftHdDevice::StillPicture(const uchar * data, int length)
{
    dsyslog("[softhddev]%s: %s %p %d\n", __FUNCTION__,
	data[0] == 0x47 ? "ts" : "pes", data, length);

    if (data[0] == 0x47) {		// ts sync
	cDevice::StillPicture(data, length);
	return;
    }

    ::StillPicture(data, length);
}

/**
**	Check if the device is ready for further action.
**
**	@param poller		file handles (unused)
**	@param timeout_ms	timeout in ms to become ready
*/
bool cSoftHdDevice::Poll(
    __attribute__ ((unused)) cPoller & poller, int timeout_ms)
{
    //dsyslog("[softhddev]%s: %d\n", __FUNCTION__, timeout_ms);

    return::Poll(timeout_ms);
}

/**
**	Flush the device output buffers.
**
**	@param timeout_ms	timeout in ms to become ready
*/
bool cSoftHdDevice::Flush(int timeout_ms)
{
    dsyslog("[softhddev]%s: %d ms\n", __FUNCTION__, timeout_ms);

    return::Flush(timeout_ms);
}

// ----------------------------------------------------------------------------

/**
**	Sets the video display format to the given one (only useful if this
**	device has an MPEG decoder).
**
**	@note FIXME: this function isn't called on the initial channel
*/
void cSoftHdDevice:: SetVideoDisplayFormat(eVideoDisplayFormat
    video_display_format)
{
    static int last = -1;

    cDevice::SetVideoDisplayFormat(video_display_format);

    dsyslog("[softhddev]%s: %d\n", __FUNCTION__, video_display_format);

    // called on every channel switch, no need to kill osd...
    if (last != video_display_format) {
	last = video_display_format;

	::VideoSetDisplayFormat(video_display_format);
	OsdDirty = 1;
    }
}

/**
**	Returns the width, height and video_aspect ratio of the currently
**	displayed video material.
**
**	@note the size is used to scale the subtitle.
*/
void cSoftHdDevice::GetVideoSize(int &width, int &height, double &video_aspect)
{
    ::GetOsdSize(&width, &height, &video_aspect);
}

/**
**	Returns the width, height and pixel_aspect ratio the OSD.
**
**	FIXME: Called every second, for nothing (no OSD displayed)?
*/
void cSoftHdDevice::GetOsdSize(int &width, int &height, double &pixel_aspect)
{
    ::GetOsdSize(&width, &height, &pixel_aspect);
}

// ----------------------------------------------------------------------------

/**
**	Play a audio packet.
*/
int cSoftHdDevice::PlayAudio(const uchar * data, int length, uchar id)
{
    //dsyslog("[softhddev]%s: %p %p %d %d\n", __FUNCTION__, this, data, length, id);

    return::PlayAudio(data, length, id);
}

void cSoftHdDevice::SetAudioTrackDevice(
    __attribute__ ((unused)) eTrackType type)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);
}

void cSoftHdDevice::SetDigitalAudioDevice( __attribute__ ((unused)) bool on)
{
    //dsyslog("[softhddev]%s: %s\n", __FUNCTION__, on ? "true" : "false");
}

void cSoftHdDevice::SetAudioChannelDevice( __attribute__ ((unused))
    int audio_channel)
{
    //dsyslog("[softhddev]%s: %d\n", __FUNCTION__, audio_channel);
}

int cSoftHdDevice::GetAudioChannelDevice(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);
    return 0;
}

// ----------------------------------------------------------------------------

/**
**	Play a video packet.
*/
int cSoftHdDevice::PlayVideo(const uchar * data, int length)
{
    //dsyslog("[softhddev]%s: %p %d\n", __FUNCTION__, data, length);

    return::PlayVideo(data, length);
}

#if 0
///
///	Play a TS video packet.
///
int cSoftHdDevice::PlayTsVideo(const uchar * data, int length)
{
    // many code to repeat
}
#endif

#ifndef USE_AUDIO_THREAD		// FIXME: testing none threaded

///
///	Play a TS audio packet.
///
///	misuse this function as audio poller
///
///	@param data	ts data buffer
///	@param length	ts packet length
///
int cSoftHdDevice::PlayTsAudio(const uchar * data, int length)
{
    AudioPoller();

    return cDevice::PlayTsAudio(data, length);
}

#endif

/**
**	Grabs the currently visible screen image.
**
**	@param size	size of the returned data
**	@param jpeg	flag true, create JPEG data
**	@param quality	JPEG quality
**	@param width	number of horizontal pixels in the frame
**	@param height	number of vertical pixels in the frame
*/
uchar *cSoftHdDevice::GrabImage(int &size, bool jpeg, int quality, int width,
    int height)
{
    dsyslog("[softhddev]%s: %d, %d, %d, %dx%d\n", __FUNCTION__, size, jpeg,
	quality, width, height);

    return::GrabImage(&size, jpeg, quality, width, height);
}

//////////////////////////////////////////////////////////////////////////////
//	cPlugin
//////////////////////////////////////////////////////////////////////////////

class cPluginSoftHdDevice:public cPlugin
{
  public:
    cPluginSoftHdDevice(void);
    virtual ~ cPluginSoftHdDevice(void);
    virtual const char *Version(void);
    virtual const char *Description(void);
    virtual const char *CommandLineHelp(void);
    virtual bool ProcessArgs(int, char *[]);
    virtual bool Initialize(void);
    virtual bool Start(void);
    virtual void Stop(void);
    // virtual void Housekeeping(void);
    virtual void MainThreadHook(void);
    virtual const char *MainMenuEntry(void);
    virtual cOsdObject *MainMenuAction(void);
    virtual cMenuSetupPage *SetupMenu(void);
    virtual bool SetupParse(const char *, const char *);
//    virtual bool Service(const char *, void * = NULL);
    virtual const char **SVDRPHelpPages(void);
    virtual cString SVDRPCommand(const char *, const char *, int &);
};

cPluginSoftHdDevice::cPluginSoftHdDevice(void)
{
    // Initialize any member variables here.
    // DON'T DO ANYTHING ELSE THAT MAY HAVE SIDE EFFECTS, REQUIRE GLOBAL
    // VDR OBJECTS TO EXIST OR PRODUCE ANY OUTPUT!
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);
}

cPluginSoftHdDevice::~cPluginSoftHdDevice(void)
{
    // Clean up after yourself!
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    ::SoftHdDeviceExit();
}

const char *cPluginSoftHdDevice::Version(void)
{
    return VERSION;
}

const char *cPluginSoftHdDevice::Description(void)
{
    return tr(DESCRIPTION);
}

/**
**	Return a string that describes all known command line options.
*/
const char *cPluginSoftHdDevice::CommandLineHelp(void)
{
    return::CommandLineHelp();
}

/**
**	Process the command line arguments.
*/
bool cPluginSoftHdDevice::ProcessArgs(int argc, char *argv[])
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    return::ProcessArgs(argc, argv);
}

bool cPluginSoftHdDevice::Initialize(void)
{
    // Start any background activities the plugin shall perform.
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    MyDevice = new cSoftHdDevice();

    return true;
}

/**
**	 Start any background activities the plugin shall perform.
*/
bool cPluginSoftHdDevice::Start(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    if (!MyDevice->IsPrimaryDevice()) {
	isyslog("[softhddev] softhddevice is not the primary device!");
	if (ConfigMakePrimary) {
	    // Must be done in the main thread
	    dsyslog("[softhddev] makeing softhddevice %d the primary device!",
		MyDevice->DeviceNumber());
	    DoMakePrimary = 1;
	} else {
	    isyslog("[softhddev] softhddevice %d is not the primary device!",
		MyDevice->DeviceNumber());
	}
    }

    ::Start();

    return true;
}

void cPluginSoftHdDevice::Stop(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    ::Stop();
}

#if 0

/**
**	Perform any cleanup or other regular tasks.
*/
void cPluginSoftHdDevice::Housekeeping(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    // ::Housekeeping();
}

#endif

/**
**	Create main menu entry.
*/
const char *cPluginSoftHdDevice::MainMenuEntry(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    return ConfigHideMainMenuEntry ? NULL : tr(MAINMENUENTRY);
}

/**
**	Perform the action when selected from the main VDR menu.
*/
cOsdObject *cPluginSoftHdDevice::MainMenuAction(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    //MyDevice->StopReplay();
    if (!cSoftHdControl::Player) {	// not already suspended
	cControl::Launch(new cSoftHdControl);
	cControl::Attach();
	Suspend(ConfigSuspendClose, ConfigSuspendClose, ConfigSuspendX11);
	if (ShutdownHandler.GetUserInactiveTime()) {
	    dsyslog("[softhddev]%s: set user inactive\n", __FUNCTION__);
	    ShutdownHandler.SetUserInactive();
	}
    }

    return NULL;
}

/**
**	Called for every plugin once during every cycle of VDR's main program
**	loop.
*/
void cPluginSoftHdDevice::MainThreadHook(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    if (DoMakePrimary && MyDevice) {
	dsyslog("[softhddev]%s: switching primary device\n", __FUNCTION__);
	cDevice::SetPrimaryDevice(MyDevice->DeviceNumber() + 1);
	DoMakePrimary = 0;
    }
    // check if user is inactive, automatic enter suspend mode
    if (ShutdownHandler.IsUserInactive()) {
	// this is regular called, but guarded against double calls
	Suspend(ConfigSuspendClose, ConfigSuspendClose, ConfigSuspendX11);
    }

    ::MainThreadHook();
}

/**
**	Return our setup menu.
*/
cMenuSetupPage *cPluginSoftHdDevice::SetupMenu(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    return new cMenuSetupSoft;
}

/**
**	Parse setup parameters
**
**	@param name	paramter name (case sensetive)
**	@param value	value as string
**
**	@returns true if the parameter is supported.
*/
bool cPluginSoftHdDevice::SetupParse(const char *name, const char *value)
{
    int i;

    //dsyslog("[softhddev]%s: '%s' = '%s'\n", __FUNCTION__, name, value);

    if (!strcmp(name, "MakePrimary")) {
	ConfigMakePrimary = atoi(value);
	return true;
    }
    if (!strcmp(name, "HideMainMenuEntry")) {
	ConfigHideMainMenuEntry = atoi(value);
	return true;
    }
    if (!strcmp(name, "SkipLines")) {
	VideoSetSkipLines(ConfigVideoSkipLines = atoi(value));
	return true;
    }
    for (i = 0; i < RESOLUTIONS; ++i) {
	char buf[128];

	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "Scaling");
	if (!strcmp(name, buf)) {
	    ConfigVideoScaling[i] = atoi(value);
	    VideoSetScaling(ConfigVideoScaling);
	    return true;
	}
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "Deinterlace");
	if (!strcmp(name, buf)) {
	    ConfigVideoDeinterlace[i] = atoi(value);
	    VideoSetDeinterlace(ConfigVideoDeinterlace);
	    return true;
	}
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i],
	    "SkipChromaDeinterlace");
	if (!strcmp(name, buf)) {
	    ConfigVideoSkipChromaDeinterlace[i] = atoi(value);
	    VideoSetSkipChromaDeinterlace(ConfigVideoSkipChromaDeinterlace);
	    return true;
	}
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "Denoise");
	if (!strcmp(name, buf)) {
	    ConfigVideoDenoise[i] = atoi(value);
	    VideoSetDenoise(ConfigVideoDenoise);
	    return true;
	}
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "Sharpen");
	if (!strcmp(name, buf)) {
	    ConfigVideoSharpen[i] = atoi(value);
	    VideoSetSharpen(ConfigVideoSharpen);
	    return true;
	}
    }

    if (!strcmp(name, "AudioDelay")) {
	VideoSetAudioDelay(ConfigVideoAudioDelay = atoi(value));
	return true;
    }
    if (!strcmp(name, "AudioPassthrough")) {
	CodecSetAudioPassthrough(ConfigAudioPassthrough = atoi(value));
	return true;
    }

    if (!strcmp(name, "AutoCrop.Interval")) {
	VideoSetAutoCrop(ConfigAutoCropInterval =
	    atoi(value), ConfigAutoCropDelay, ConfigAutoCropTolerance);
	return true;
    }
    if (!strcmp(name, "AutoCrop.Delay")) {
	VideoSetAutoCrop(ConfigAutoCropInterval, ConfigAutoCropDelay =
	    atoi(value), ConfigAutoCropTolerance);
	return true;
    }
    if (!strcmp(name, "AutoCrop.Tolerance")) {
	VideoSetAutoCrop(ConfigAutoCropInterval, ConfigAutoCropDelay,
	    ConfigAutoCropTolerance = atoi(value));
	return true;
    }

    if (!strcmp(name, "Suspend.Close")) {
	ConfigSuspendClose = atoi(value);
	return true;
    }
    if (!strcmp(name, "Suspend.X11")) {
	ConfigSuspendX11 = atoi(value);
	return true;
    }
    return false;
}

#if 0

bool cPluginSoftHdDevice::Service(const char *Id, void *Data)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    return false;
}

#endif

//----------------------------------------------------------------------------
//	cPlugin SVDRP
//----------------------------------------------------------------------------

/**
**	Return SVDRP commands help pages.
**
**	return a pointer to a list of help strings for all of the plugin's
**	SVDRP commands.
*/
const char **cPluginSoftHdDevice::SVDRPHelpPages(void)
{
    // FIXME: translation?
    static const char *text[] = {
	"SUSP\n" "    Suspend plugin.\n",
	"RESU\n" "    Resume plugin.\n",
	NULL
    };

    return text;
}

/**
**	Handle SVDRP commands.
*/
cString cPluginSoftHdDevice::SVDRPCommand(const char *command,
    __attribute__ ((unused)) const char *option,
    __attribute__ ((unused)) int &reply_code)
{
    if (!strcasecmp(command, "SUSP")) {
	if (cSoftHdControl::Player) {	// already suspended
	    return "SoftHdDevice already suspended";
	}
	// should be after suspend, but SetPlayMode resumes
	cControl::Launch(new cSoftHdControl);
	cControl::Attach();
	Suspend(ConfigSuspendClose, ConfigSuspendClose, ConfigSuspendX11);
	return "SoftHdDevice is suspended";
    }
    if (!strcasecmp(command, "RESU")) {
	if (ShutdownHandler.GetUserInactiveTime()) {
	    ShutdownHandler.SetUserInactiveTimeout();
	}
	if (cSoftHdControl::Player) {	// suspended
	    cControl::Shutdown();	// not need, if not suspended
	}
	Resume();
	return "SoftHdDevice is resumed";
    }
    return NULL;
}

VDRPLUGINCREATOR(cPluginSoftHdDevice);	// Don't touch this!
