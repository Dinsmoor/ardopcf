// PipeWire audio backend for Linux
// Experimental alternative to ALSA.c that allows audio device sharing

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include "common/os_util.h"
#include "common/audio.h"
#include "common/log.h"
#include "common/wav.h"
#include "common/ardopcommon.h"
#include "common/ptt.h"
#include "common/Webgui.h"

// Audio format constants for ardopcf
#define SAMPLE_RATE 12000
#define CHANNELS 1
#define SAMPLE_FORMAT SPA_AUDIO_FORMAT_S16_LE
#define BYTES_PER_SAMPLE 2

// Use LastGoodCaptureDevice and LastGoodPlaybackDevice to store the names of
// the last audio devices that were successfully opened.
char LastGoodCaptureDevice[DEVSTRSZ] = "";
char LastGoodPlaybackDevice[DEVSTRSZ] = "";

extern bool WriteRxWav;  // Record RX controlled by Command line/TX/Timer
extern bool HWriteRxWav;  // Record RX controlled by host command RECRX
extern struct WavFile *txwff;  // For recording of filtered TX audio

void txSleep(unsigned int mS);
void StartRxWav();

// TX and RX audio buffers
extern int SampleNo;
extern int Number;

// txbuffer and TxIndex are globals shared with Modulate.c via audio.h
short txbuffer[2][SendSize];
int TxIndex = 0;

short inbuffer[2][ReceiveSize];
int inIndex = 0;

// PipeWire objects
struct pw_main_loop *loop = NULL;
struct pw_context *context = NULL;
struct pw_core *core = NULL;

struct pw_stream *playback_stream = NULL;
struct pw_stream *capture_stream = NULL;

// Stream state
static bool playback_active = false;
static bool capture_active = false;

void StartCapture() {
	Capturing = true;
	DiscardOldSamples();
	ClearAllMixedSamples();
	State = SearchingForLeader;
}

// ============================================================================
// PipeWire Stream Callbacks
// ============================================================================

// Playback process callback - called when PipeWire needs audio data
static void on_playback_process(void *userdata) {
	struct pw_buffer *b;
	struct spa_buffer *buf;
	int16_t *dest;
	uint32_t n_frames;

	if ((b = pw_stream_dequeue_buffer(playback_stream)) == NULL) {
		ZF_LOGW("PipeWire playback: out of buffers");
		return;
	}

	buf = b->buffer;
	dest = buf->datas[0].data;
	n_frames = buf->datas[0].maxsize / BYTES_PER_SAMPLE;

	// TODO: Implement actual audio output from txbuffer
	// For now, just fill with silence
	memset(dest, 0, n_frames * BYTES_PER_SAMPLE);

	buf->datas[0].chunk->offset = 0;
	buf->datas[0].chunk->stride = BYTES_PER_SAMPLE;
	buf->datas[0].chunk->size = n_frames * BYTES_PER_SAMPLE;

	pw_stream_queue_buffer(playback_stream, b);
}

// Capture process callback - called when PipeWire has audio data available
static void on_capture_process(void *userdata) {
	struct pw_buffer *b;
	struct spa_buffer *buf;
	int16_t *src;
	uint32_t n_frames;

	if ((b = pw_stream_dequeue_buffer(capture_stream)) == NULL) {
		ZF_LOGW("PipeWire capture: out of buffers");
		return;
	}

	buf = b->buffer;
	src = buf->datas[0].data;
	n_frames = buf->datas[0].chunk->size / BYTES_PER_SAMPLE;

	// TODO: Implement actual audio input to inbuffer
	// For now, just acknowledge the buffer

	pw_stream_queue_buffer(capture_stream, b);
}

static const struct pw_stream_events playback_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = on_playback_process,
};

static const struct pw_stream_events capture_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = on_capture_process,
};

// ============================================================================
// Audio Device Management
// ============================================================================

void GetDevices() {
	// TODO: Enumerate PipeWire audio devices
	// For now, just create a default device entry
	int devindex;

	// Initialize the AudioDevices array
	InitDevices(&AudioDevices);

	// Add a default PipeWire device
	devindex = ExtendDevices(&AudioDevices);
	AudioDevices[devindex]->name = strdup("pipewire");
	AudioDevices[devindex]->alias = strdup("default");
	AudioDevices[devindex]->desc = strdup("PipeWire Default Audio");
	AudioDevices[devindex]->capture = true;
	AudioDevices[devindex]->playback = true;
	AudioDevices[devindex]->capturebusy = false;
	AudioDevices[devindex]->playbackbusy = false;
}

void InitAudio(bool quiet) {
	ZF_LOGI("Initializing PipeWire audio backend");

	// Initialize PipeWire library
	pw_init(NULL, NULL);

	// Create main loop
	loop = pw_main_loop_new(NULL);
	if (!loop) {
		ZF_LOGE("Failed to create PipeWire main loop");
		return;
	}

	// Create context
	context = pw_context_new(pw_main_loop_get_loop(loop), NULL, 0);
	if (!context) {
		ZF_LOGE("Failed to create PipeWire context");
		pw_main_loop_destroy(loop);
		loop = NULL;
		return;
	}

	// Connect to PipeWire daemon
	core = pw_context_connect(context, NULL, 0);
	if (!core) {
		ZF_LOGE("Failed to connect to PipeWire");
		pw_context_destroy(context);
		pw_main_loop_destroy(loop);
		context = NULL;
		loop = NULL;
		return;
	}

	// Enumerate devices
	GetDevices();

	if (!quiet) {
		ZF_LOGI("PipeWire audio initialized successfully");
	}
}

// ============================================================================
// Audio Stream Control
// ============================================================================

bool OpenSoundPlayback(char *devstr, int ch) {
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	const struct spa_pod *params[1];
	struct spa_audio_info_raw info;

	// Close existing stream if open
	if (TXEnabled) {
		CloseSoundPlayback(false);
	}

	ZF_LOGI("Opening PipeWire playback: %s", devstr);

	// Create playback stream
	playback_stream = pw_stream_new_simple(
		pw_main_loop_get_loop(loop),
		"ardopcf-playback",
		pw_properties_new(
			PW_KEY_MEDIA_TYPE, "Audio",
			PW_KEY_MEDIA_CATEGORY, "Playback",
			PW_KEY_MEDIA_ROLE, "Communication",
			NULL
		),
		&playback_stream_events,
		NULL
	);

	if (!playback_stream) {
		ZF_LOGE("Failed to create PipeWire playback stream");
		return false;
	}

	// Configure audio format
	spa_zero(info);
	info.format = SAMPLE_FORMAT;
	info.channels = CHANNELS;
	info.rate = SAMPLE_RATE;

	// Build parameters
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

	// Connect stream
	if (pw_stream_connect(
		playback_stream,
		PW_DIRECTION_OUTPUT,
		PW_ID_ANY,
		PW_STREAM_FLAG_AUTOCONNECT |
		PW_STREAM_FLAG_MAP_BUFFERS |
		PW_STREAM_FLAG_RT_PROCESS,
		params, 1
	) < 0) {
		ZF_LOGE("Failed to connect PipeWire playback stream");
		pw_stream_destroy(playback_stream);
		playback_stream = NULL;
		return false;
	}

	strncpy(PlaybackDevice, devstr, sizeof(PlaybackDevice) - 1);
	strncpy(LastGoodPlaybackDevice, devstr, sizeof(LastGoodPlaybackDevice) - 1);
	TXEnabled = true;
	playback_active = true;

	ZF_LOGI("PipeWire playback opened successfully");
	return true;
}

bool OpenSoundCapture(char *devstr, int ch) {
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	const struct spa_pod *params[1];
	struct spa_audio_info_raw info;

	// Close existing stream if open
	if (RXEnabled) {
		CloseSoundCapture(false);
	}

	ZF_LOGI("Opening PipeWire capture: %s", devstr);

	// Create capture stream
	capture_stream = pw_stream_new_simple(
		pw_main_loop_get_loop(loop),
		"ardopcf-capture",
		pw_properties_new(
			PW_KEY_MEDIA_TYPE, "Audio",
			PW_KEY_MEDIA_CATEGORY, "Capture",
			PW_KEY_MEDIA_ROLE, "Communication",
			NULL
		),
		&capture_stream_events,
		NULL
	);

	if (!capture_stream) {
		ZF_LOGE("Failed to create PipeWire capture stream");
		return false;
	}

	// Configure audio format
	spa_zero(info);
	info.format = SAMPLE_FORMAT;
	info.channels = CHANNELS;
	info.rate = SAMPLE_RATE;

	// Build parameters
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

	// Connect stream
	if (pw_stream_connect(
		capture_stream,
		PW_DIRECTION_INPUT,
		PW_ID_ANY,
		PW_STREAM_FLAG_AUTOCONNECT |
		PW_STREAM_FLAG_MAP_BUFFERS |
		PW_STREAM_FLAG_RT_PROCESS,
		params, 1
	) < 0) {
		ZF_LOGE("Failed to connect PipeWire capture stream");
		pw_stream_destroy(capture_stream);
		capture_stream = NULL;
		return false;
	}

	strncpy(CaptureDevice, devstr, sizeof(CaptureDevice) - 1);
	strncpy(LastGoodCaptureDevice, devstr, sizeof(LastGoodCaptureDevice) - 1);
	RXEnabled = true;
	capture_active = true;

	ZF_LOGI("PipeWire capture opened successfully");
	return true;
}

void CloseSoundPlayback(bool do_getdevices) {
	if (playback_stream) {
		pw_stream_destroy(playback_stream);
		playback_stream = NULL;
		playback_active = false;
	}

	TXEnabled = false;
	PlaybackDevice[0] = '\0';

	if (do_getdevices) {
		GetDevices();
	}
}

void CloseSoundCapture(bool do_getdevices) {
	if (capture_stream) {
		pw_stream_destroy(capture_stream);
		capture_stream = NULL;
		capture_active = false;
	}

	RXEnabled = false;
	CaptureDevice[0] = '\0';

	if (do_getdevices) {
		GetDevices();
	}
}

// ============================================================================
// Audio I/O Functions
// ============================================================================

bool SendtoCard(int n) {
	// TODO: Implement sending txbuffer[TxIndex] samples to playback stream
	// This will need to queue data and let the process callback consume it

	ZF_LOGD("SendtoCard: %d samples (stub)", n);
	return true;
}

void PollReceivedSamples() {
	// Process PipeWire events
	if (loop) {
		pw_main_loop_run(loop);
	}

	// TODO: Check if samples are available in capture buffer
	// and call ProcessNewSamples() if Capturing
}

void StopCapture() {
	Capturing = false;
}

bool SoundFlush() {
	// TODO: Drain playback buffer
	// For now, just stop PTT and start capture

	KeyPTT(false);
	StartCapture();

	return true;
}

// ============================================================================
// Device State Queries
// ============================================================================

bool crestorable() {
	return LastGoodCaptureDevice[0] != '\0';
}

bool prestorable() {
	return LastGoodPlaybackDevice[0] != '\0';
}
