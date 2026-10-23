// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.



// Simulation of the hardware video/audio decoders.
// The idea is high level emulation where we simply use FFMPEG.

#pragma once

// An approximation of what the interface will look like. Similar to JPCSP's.

#include <map>
#include <string>
#include "Common/CommonTypes.h"
#include "Core/HLE/sceMpeg.h"
#include "Core/HW/MpegDemux.h"
#include "Core/HW/SimpleAudioDec.h"

class PointerWrap;
class AudioDecoder;

#ifdef USE_FFMPEG
struct SwsContext;
struct AVFrame;
struct AVIOContext;
struct AVFormatContext;
struct AVCodecContext;
#endif

inline s64 getMpegTimeStamp(const u8 *buf) {
	return (s64)buf[5] | ((s64)buf[4] << 8) | ((s64)buf[3] << 16) | ((s64)buf[2] << 24) 
		| ((s64)buf[1] << 32) | ((s64)buf[0] << 36);
}

#ifdef USE_FFMPEG
bool InitFFmpeg();
#endif

// Video cutscene decoding (PS3 setting "videos"): 1 on, 0 off (no video stream is opened: games
// see what they did without FFmpeg, and most skip the video), -1 auto (off for games whose video
// playback is known to hang).
extern int g_mediaVideoDecode;
// Whether this build has FFmpeg (video, MP3/AAC decoding).
bool MediaEngineHasFFmpeg();
// g_mediaVideoDecode for the running game (auto decided by game ID). Also used by sceVideocodec.
bool MediaVideoDecodeEnabled();

// Diagnostics (PS3 stall report): which step of opening or decoding a movie the emulation thread is
// in, e.g. "reading data (12 reads this call)".
std::string MediaDecodeStepText();
// Called before a movie's buffers are allocated, with the bytes about to be needed in one piece, so
// the frontend can free memory first (PS3: little main memory, and it fragments).
extern void (*g_mediaMemoryHook)(const char *what, int bytes);
// H.264 deblocking in videos: 1 on, 0 off (with FFmpeg's fast paths: faster decoding, a little
// blockier), -1 auto (on until a movie falls below full speed, then off for that movie).
extern int g_mediaDeblock;
// Killzone with videos set to On: the built-in sceMpeg instead of its disc's mpeg.prx.
bool MediaGameUsesHLEMpeg();

class MediaEngine {
public:
	MediaEngine();
	~MediaEngine();

	void closeMedia();
	bool loadStream(const u8 *buffer, int readSize, int RingbufferSize);
	bool reloadStream();
	bool addVideoStream(int streamNum, int streamId = -1);
	// open the mpeg context
	bool openContext(bool keepReadPos = false);
	void closeContext();

	// Returns number of packets actually added. I guess the buffer might be full.
	int addStreamData(const u8 *buffer, int addSize);
	bool seekTo(s64 timestamp, int videoPixelMode);

	bool setVideoStream(int streamNum, bool force = false);
	// TODO: Return false if the stream doesn't exist.
	bool setAudioStream(int streamNum);

	u8 *getFrameImage();
	int getRemainSize();
	int getAudioRemainSize();

	bool stepVideo(int videoPixelMode, bool skipFrame = false);
	int writeVideoImage(u32 bufferPtr, int frameWidth = 512, int videoPixelMode = 3);
	int writeVideoImageWithRange(u32 bufferPtr, int frameWidth, int videoPixelMode,
	                             int xpos, int ypos, int width, int height);
	int getAudioSamples(u32 bufferPtr);

	s64 getVideoTimeStamp();
	s64 getAudioTimeStamp();
	s64 getLastTimeStamp();

	bool IsVideoEnd() { return m_isVideoEnd; }
	bool IsNoAudioData();
	bool IsActuallyPlayingAudio();
	int VideoWidth() { return m_desWidth; }
	int VideoHeight() { return m_desHeight; }

	void DoState(PointerWrap &p);

private:
	bool SetupStreams();
	bool setVideoDim(int width = 0, int height = 0);
	void updateSwsFormat(int videoPixelMode);
	int getNextAudioFrame(u8 **buf, int *headerCode1, int *headerCode2);

	static int MpegReadbuffer(void *opaque, uint8_t *buf, int buf_size);

public:  // TODO: Very little of this below should be public.

#ifdef USE_FFMPEG
	std::map<int, AVCodecContext *> m_pCodecCtxs;
	AVFrame *m_pFrame = nullptr;
	AVFrame *m_pFrameRGB = nullptr;
#endif

	u8 *m_buffer = nullptr;

	int m_desWidth = 0;
	int m_desHeight = 0;
	int m_bufSize;  // initialized in constructor
	s64 m_videopts = 0;

	s64 m_firstTimeStamp = 0;
	s64 m_lastTimeStamp = 0;

	bool m_isVideoEnd = false;
	// g_mediaDeblock auto: deblocking switched off for this movie, and the speed measured so far.
	bool m_deblockOff = false;
	double m_speedStart = 0.0, m_speedLast = 0.0;
	u64 m_speedTicks = 0;

private:
#ifdef USE_FFMPEG
	// Video ffmpeg context - not used for audio
	AVFormatContext *m_pFormatCtx = nullptr;
	std::vector<AVCodecContext *> m_codecsToClose;
	AVIOContext *m_pIOContext = nullptr;
	SwsContext *m_sws_ctx = nullptr;
#endif

	int m_sws_fmt = 0;
	int m_videoStream = -1;
	int m_expectedVideoStreams = 0;

	// Used by the demuxer.
	int m_audioStream = -1;

	int m_decodingsize = 0;
	BufferQueue *m_pdata = nullptr;

	s64 m_lastPts = -1;

	MpegDemux *m_demux = nullptr;
	AudioDecoder *m_audioContext = nullptr;
	s64 m_audiopts = 0;

	// used for audio type 
	int m_audioType;

	int m_ringbuffersize;

	u8 m_mpegheader[0x10000];  // TODO: Allocate separately
	int m_mpegheaderReadPos = 0;
	int m_mpegheaderSize = 0;
};

std::string GetFFMPEGVersion();
