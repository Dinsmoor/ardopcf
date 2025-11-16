// PipeWire audio backend for Linux
// Experimental alternative to ALSA.c that allows audio device sharing

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <pipewire/pipewire.h>
#include <pipewire/keys.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/hook.h>

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

// Capture accumulation buffer - PipeWire callbacks are variable size
// but ARDOP expects exactly ReceiveSize (240) samples at a time
static short capture_accumulator[ReceiveSize];
static int capture_accum_count = 0;

// Playback queue for bridging SendtoCard() to on_playback_process()
// ALSA uses blocking writes, PipeWire uses pull callbacks
// Longest frame is <6 seconds, so buffer ~8 seconds for headroom
#define PLAYBACK_QUEUE_SIZE (SendSize * 80)  // ~8 seconds at 12kHz (96000 samples)
static short playback_queue[PLAYBACK_QUEUE_SIZE];
static int playback_queue_head = 0;  // write position
static int playback_queue_tail = 0;  // read position
static int playback_queue_count = 0;  // samples currently queued

// Drain state for SoundFlush()
static bool drain_requested = false;
static bool drain_completed = false;

bool AudioInit = false;

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
	uint32_t samples_to_copy;
	uint32_t i;

	if ((b = pw_stream_dequeue_buffer(playback_stream)) == NULL) {
		ZF_LOGW("PipeWire playback: out of buffers");
		return;
	}

	buf = b->buffer;
	dest = buf->datas[0].data;
	n_frames = buf->datas[0].maxsize / BYTES_PER_SAMPLE;

	// Consume samples from playback queue
	samples_to_copy = (playback_queue_count < n_frames) ? playback_queue_count : n_frames;

	for (i = 0; i < samples_to_copy; i++) {
		dest[i] = playback_queue[playback_queue_tail];
		playback_queue_tail = (playback_queue_tail + 1) % PLAYBACK_QUEUE_SIZE;
	}
	playback_queue_count -= samples_to_copy;

	// Fill remainder with silence if queue underrun
	if (samples_to_copy < n_frames) {
		memset(&dest[samples_to_copy], 0, (n_frames - samples_to_copy) * BYTES_PER_SAMPLE);
	}

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
	uint32_t i, samples_available, samples_needed;

	if ((b = pw_stream_dequeue_buffer(capture_stream)) == NULL) {
		ZF_LOGW("PipeWire capture: out of buffers");
		return;
	}

	buf = b->buffer;
	src = buf->datas[0].data;
	n_frames = buf->datas[0].chunk->size / BYTES_PER_SAMPLE;

	// PipeWire callbacks are variable size, but ARDOP expects exactly
	// ReceiveSize (240) samples at a time. Accumulate samples until we
	// have a full buffer, then process it (matching ALSA behavior).

	samples_available = n_frames;
	i = 0;

	while (samples_available > 0) {
		samples_needed = ReceiveSize - capture_accum_count;

		if (samples_available >= samples_needed) {
			// We have enough to fill the buffer
			memcpy(&capture_accumulator[capture_accum_count], &src[i],
			       samples_needed * sizeof(int16_t));
			i += samples_needed;
			samples_available -= samples_needed;

			// Copy to inbuffer[0] and process (matching ALSA.c:1702-1716)
			memcpy(&inbuffer[0][0], capture_accumulator, ReceiveSize * sizeof(int16_t));

			if (Capturing) {
				ProcessNewSamples(&inbuffer[0][0], ReceiveSize);
			} else {
				// Still preprocess even when not Capturing (for WAV recording, etc)
				PreprocessNewSamples(&inbuffer[0][0], ReceiveSize);
			}

			capture_accum_count = 0;  // Reset for next batch
		} else {
			// Not enough samples yet, accumulate what we have
			memcpy(&capture_accumulator[capture_accum_count], &src[i],
			       samples_available * sizeof(int16_t));
			capture_accum_count += samples_available;
			samples_available = 0;
		}
	}

	pw_stream_queue_buffer(capture_stream, b);
}

// Drained callback - called when playback stream has finished playing all data
static void on_playback_drained(void *userdata) {
	ZF_LOGD("Playback stream drained");
	drain_completed = true;
}

static const struct pw_stream_events playback_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = on_playback_process,
	.drained = on_playback_drained,
};

static const struct pw_stream_events capture_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = on_capture_process,
};

// ============================================================================
// Audio Device Management
// ============================================================================

// Temporary structure for collecting devices during enumeration
struct device_list_data {
	struct pw_registry *registry;
	struct spa_hook registry_listener;
	int sync_seq;
	bool done;
};

// Registry event: new global object
static void registry_event_global(void *data, uint32_t id, uint32_t permissions,
				   const char *type, uint32_t version,
				   const struct spa_dict *props)
{
	const char *media_class, *node_desc, *node_name;
	int devindex;
	bool is_source = false, is_sink = false;

	// We only care about Node objects
	if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
		return;

	if (!props)
		return;

	// Get properties
	media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
	node_desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
	node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);

	if (!media_class || !node_name)
		return;

	// Filter for audio devices
	if (strcmp(media_class, "Audio/Source") == 0) {
		is_source = true;
	} else if (strcmp(media_class, "Audio/Sink") == 0) {
		is_sink = true;
	} else {
		return;  // Not an audio device
	}

	// Add to AudioDevices array
	devindex = ExtendDevices(&AudioDevices);
	AudioDevices[devindex]->name = strdup(node_name);
	AudioDevices[devindex]->alias = NULL;  // PipeWire doesn't have aliases like ALSA
	AudioDevices[devindex]->desc = node_desc ? strdup(node_desc) : strdup(node_name);
	AudioDevices[devindex]->capture = is_source;
	AudioDevices[devindex]->playback = is_sink;
	AudioDevices[devindex]->capturebusy = false;
	AudioDevices[devindex]->playbackbusy = false;

	ZF_LOGV("Found audio device: %s (%s) - %s", node_name,
		is_source ? "capture" : "playback", node_desc ? node_desc : "");
}

// Registry event: global object removed
static void registry_event_global_remove(void *data, uint32_t id)
{
	// We don't track removals during enumeration
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_event_global,
	.global_remove = registry_event_global_remove,
};

// Core event: done - signals that initial enumeration is complete
static void core_event_done(void *data, uint32_t id, int seq)
{
	struct device_list_data *d = data;
	if (id == PW_ID_CORE && seq == d->sync_seq) {
		d->done = true;
	}
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = core_event_done,
};

void GetDevices() {
	struct pw_registry *registry;
	struct device_list_data data = { 0 };
	struct spa_hook core_listener;
	int res;
	int iterations = 0;
	const int MAX_ITERATIONS = 100;  // Safety limit

	// Initialize the AudioDevices array
	InitDevices(&AudioDevices);

	if (!core || !loop) {
		ZF_LOGW("PipeWire not initialized, cannot enumerate devices");
		// Add fallback default device
		int devindex = ExtendDevices(&AudioDevices);
		AudioDevices[devindex]->name = strdup("pipewire");
		AudioDevices[devindex]->alias = strdup("default");
		AudioDevices[devindex]->desc = strdup("PipeWire Default Audio");
		AudioDevices[devindex]->capture = true;
		AudioDevices[devindex]->playback = true;
		return;
	}

	// Get the registry to enumerate objects
	registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
	data.registry = registry;
	data.done = false;

	// Listen for core events (done signal)
	pw_core_add_listener(core, &core_listener, &core_events, &data);

	// Listen for registry events (global objects)
	pw_registry_add_listener(registry, &data.registry_listener,
				 &registry_events, NULL);

	// Trigger sync to get a done event when initial enumeration completes
	data.sync_seq = pw_core_sync(core, PW_ID_CORE, 0);

	// Run loop until done or timeout
	while (!data.done && iterations < MAX_ITERATIONS) {
		struct pw_loop *pwloop = pw_main_loop_get_loop(loop);
		pw_loop_iterate(pwloop, 10);  // 10ms timeout per iteration
		iterations++;
	}

	if (!data.done) {
		ZF_LOGW("Device enumeration timed out after %d iterations", iterations);
	}

	// Cleanup
	spa_hook_remove(&data.registry_listener);
	spa_hook_remove(&core_listener);
	pw_proxy_destroy((struct pw_proxy*)registry);

	ZF_LOGI("Enumerated %d PipeWire audio devices", AudioDevices ?
		(AudioDevices[0] ? 1 : 0) : 0);  // Rough count

	// Always add NOSOUND as last device (matching ALSA behavior)
	int devindex = ExtendDevices(&AudioDevices);
	AudioDevices[devindex]->name = strdup("NOSOUND");
	AudioDevices[devindex]->desc = strdup("A dummy audio device for diagnostic use.");
	AudioDevices[devindex]->capture = true;
	AudioDevices[devindex]->playback = true;
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

	// Log available devices (matching ALSA.c:1684-1691)
	if (ZF_LOG_ON_VERBOSE && !quiet) {
		LogDevices(AudioDevices, "All audio devices", false, false);
	} else if (!quiet) {
		LogDevices(AudioDevices, "Capture (input) Devices", true, false);
		LogDevices(AudioDevices, "Playback (output) Devices", false, true);
	}

	if (!quiet) {
		ZF_LOGI("PipeWire audio initialized successfully");
	}
	AudioInit = true;
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

	// Handle NOSOUND device for testing/debugging (matching ALSA.c:1118)
	if (strcmp(devstr, "NOSOUND") == 0 || strcmp(devstr, "-1") == 0) {
		strncpy(PlaybackDevice, "NOSOUND", sizeof(PlaybackDevice) - 1);
		strncpy(LastGoodPlaybackDevice, "NOSOUND", sizeof(LastGoodPlaybackDevice) - 1);
		TXEnabled = true;
		ZF_LOGI("NOSOUND playback device selected (no audio output)");
		return true;
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

	// Activate the stream so it starts processing
	pw_stream_set_active(playback_stream, true);

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

	// Handle NOSOUND device for testing/debugging (matching ALSA.c:1279)
	if (strcmp(devstr, "NOSOUND") == 0 || strcmp(devstr, "-1") == 0) {
		strncpy(CaptureDevice, "NOSOUND", sizeof(CaptureDevice) - 1);
		strncpy(LastGoodCaptureDevice, "NOSOUND", sizeof(LastGoodCaptureDevice) - 1);
		RXEnabled = true;
		ZF_LOGI("NOSOUND capture device selected (no audio input)");
		return true;
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

	// Activate the stream so it starts processing
	pw_stream_set_active(capture_stream, true);

	strncpy(CaptureDevice, devstr, sizeof(CaptureDevice) - 1);
	strncpy(LastGoodCaptureDevice, devstr, sizeof(LastGoodCaptureDevice) - 1);
	RXEnabled = true;
	capture_active = true;

	ZF_LOGI("PipeWire capture opened successfully");

	// Start capture if not currently transmitting (matching ALSA.c:1356-1357)
	if (!SoundIsPlaying) {
		StartCapture();
	}

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
	int i;

	if (!TXEnabled) {
		ZF_LOGW("SendtoCard() called when not TXEnabled. Ignoring.");
		return false;
	}

	// Handle NOSOUND device - just discard samples
	if (strcmp(PlaybackDevice, "NOSOUND") == 0) {
		// Record to WAV if enabled (matching ALSA behavior)
		if (txwff != NULL)
			WriteWav(&txbuffer[TxIndex][0], n, txwff);
		return true;
	}

	// Block if queue is too full (mimics ALSA blocking behavior)
	// Wait for callbacks to drain the queue before adding more samples
	int wait_iterations = 0;
	const int MAX_WAIT_ITERATIONS = 500;  // 5 seconds timeout (500 * 10ms)

	while (playback_queue_count + n > PLAYBACK_QUEUE_SIZE && TXEnabled && wait_iterations < MAX_WAIT_ITERATIONS) {
		if (loop) {
			struct pw_loop *pwloop = pw_main_loop_get_loop(loop);
			pw_loop_iterate(pwloop, 10);  // Pump event loop to trigger callbacks
		}
		txSleep(10);  // Sleep 10ms
		wait_iterations++;
	}

	// Check if we timed out
	if (wait_iterations >= MAX_WAIT_ITERATIONS) {
		ZF_LOGE("SendtoCard() timeout waiting for queue space! count=%d n=%d (queue not draining)",
			playback_queue_count, n);
		return false;
	}

	// Check if we were disabled while waiting
	if (!TXEnabled) {
		ZF_LOGW("TXEnabled became false while waiting for queue space");
		return false;
	}

	// Queue samples from txbuffer[TxIndex] for playback callback to consume
	for (i = 0; i < n; i++) {
		playback_queue[playback_queue_head] = txbuffer[TxIndex][i];
		playback_queue_head = (playback_queue_head + 1) % PLAYBACK_QUEUE_SIZE;
	}
	playback_queue_count += n;

	ZF_LOGD("SendtoCard: queued %d samples (total queued: %d)", n, playback_queue_count);

	// Record to WAV if enabled (matching ALSA behavior)
	if (txwff != NULL)
		WriteWav(&txbuffer[TxIndex][0], n, txwff);

	return true;
}

void PollReceivedSamples() {
	// Handle NOSOUND device (matching ALSA.c:1698)
	if (strcmp(CaptureDevice, "NOSOUND") == 0)
		return;

	// Process PipeWire events non-blocking (required for modem timing)
	// This must NOT block or it will break the ~100ms polling requirement
	if (loop) {
		// Iterate once with no blocking (timeout=0)
		// This processes callbacks and events without blocking
		struct pw_loop *pwloop = pw_main_loop_get_loop(loop);
		pw_loop_iterate(pwloop, 0);
	}

	// The actual sample processing happens in on_capture_process() callback
	// which calls ProcessNewSamples() when Capturing == true
}

void StopCapture() {
	Capturing = false;
}

bool SoundFlush() {
	int txlenMs = 0;
	unsigned int drain_start_time;
	const unsigned int MAX_DRAIN_WAIT_MS = 5000;  // Safety timeout

	// Append Trailer then send remaining samples
	// if AddTrailer() or SendtoCard() fail, TXEnabled will be set to false
	if (TXEnabled && AddTrailer() && SendtoCard(Number)) {
		// Calculate expected transmission duration (matching ALSA.c:1744)
		txlenMs = SampleNo / 12 + 20;  // 12000 samples per sec. 20 mS TXTAIL
		ZF_LOGD("Tx Time %d ms, queued samples: %d", txlenMs, playback_queue_count);

		// Handle NOSOUND device - just sleep for timing (matching ALSA.c behavior)
		if (strcmp(PlaybackDevice, "NOSOUND") != 0) {
			// Only do drain logic for real PipeWire streams
			drain_requested = true;
			drain_completed = false;

			if (playback_stream && pw_stream_flush(playback_stream, true) < 0) {
				ZF_LOGW("pw_stream_flush() failed");
			} else if (loop) {
				// Wait for drain to complete or timeout
				struct pw_loop *pwloop = pw_main_loop_get_loop(loop);
				drain_start_time = Now;
				while (!drain_completed && (Now - drain_start_time) < MAX_DRAIN_WAIT_MS) {
					// Pump event loop to process drained callback
					pw_loop_iterate(pwloop, 0);
					txSleep(10);  // Small sleep to avoid busy-wait
				}

				if (!drain_completed) {
					ZF_LOGW("Drain timeout after %u ms", Now - drain_start_time);
				} else {
					ZF_LOGD("Drain completed after %u ms", Now - drain_start_time);
				}

				// Reactivate stream after drain (per PipeWire docs)
				// pw_stream_flush with drain=true pauses the stream
				if (playback_stream) {
					pw_stream_set_active(playback_stream, true);
					ZF_LOGD("Reactivated playback stream after drain");
				}
			}

			drain_requested = false;
		}

		// Additional timing sleep if needed (matching ALSA behavior)
		if (pttOnTime + txlenMs > Now) {
			txSleep((pttOnTime + txlenMs) - Now);
		}
	} else {
		ZF_LOGW("SoundFlush() called when not TXEnabled or trailer/send failed");
	}

	SoundIsPlaying = false;

	if (blnEnbARQRpt > 0 || blnDISCRepeating)  // Start Repeat Timer if frame should be repeated
		dttNextPlay = Now + intFrameRepeatInterval + extraDelay;

	KeyPTT(false);  // Unkey the Transmitter

	if (txwff != NULL) {
		CloseWav(txwff);
		txwff = NULL;
	}

	StartCapture();

	if (WriteRxWav && !HWriteRxWav) {
		// Start recording if not already recording, else extend the recording time.
		StartRxWav();
	}

	return TXEnabled;
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
