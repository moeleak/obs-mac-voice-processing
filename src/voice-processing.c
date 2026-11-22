// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Voice processing filter, through Apple's voice processing
 * (kAudioUnitSubType_VoiceProcessingIO), as calls on macOS have it.
 *
 * The voice processing unit is an I/O unit: it captures a microphone itself
 * and cancels from it the echo of what the output device plays, whichever
 * application plays it, suppresses noise and, optionally, controls the gain.
 * It cannot process audio that it is handed, so this filter runs one on the
 * microphone that its source captures (or another one chosen in its
 * properties) and replaces the source's audio with what the unit captures,
 * frame for frame. The two capture on two clocks, which drift apart, so the
 * buffer between them is read a little faster or slower to stay level (see
 * vp_filter_audio).
 *
 * The unit only runs while the filter is enabled and attached to a source, as
 * macOS lowers other audio while one runs.
 */

#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <inttypes.h>
#include <math.h>

#include <obs-module.h>
#include <util/deque.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/task.h>
#include <util/threading.h>

#include "audio-device-enum.h"

#define do_log(level, format, ...) \
	blog(level, "[voice processing: '%s'] " format, obs_source_get_name(vp->context), ##__VA_ARGS__)

#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)

#define S_DEVICE "device_id"
#define S_OUTPUT_DEVICE "output_device_id"
#define S_AGC "agc"
#define S_BYPASS "bypass"
#define S_DUCKING "ducking"
#define S_INFO "info"

/* The microphone of the source the filter is on. */
#define DEVICE_SOURCE ""
#define DEVICE_DEFAULT "default"

#define MT_ obs_module_text
#define TEXT_NAME MT_("VoiceProcessing")
#define TEXT_DEVICE MT_("VoiceProcessing.Device")
#define TEXT_DEVICE_SOURCE MT_("VoiceProcessing.Device.Source")
#define TEXT_DEVICE_DEFAULT MT_("VoiceProcessing.Device.Default")
#define TEXT_OUTPUT_DEVICE MT_("VoiceProcessing.OutputDevice")
#define TEXT_AGC MT_("VoiceProcessing.AGC")
#define TEXT_BYPASS MT_("VoiceProcessing.Bypass")
#define TEXT_DUCKING MT_("VoiceProcessing.Ducking")
#define TEXT_DUCKING_DEFAULT MT_("VoiceProcessing.Ducking.Default")
#define TEXT_DUCKING_MIN MT_("VoiceProcessing.Ducking.Min")
#define TEXT_DUCKING_MID MT_("VoiceProcessing.Ducking.Mid")
#define TEXT_DUCKING_MAX MT_("VoiceProcessing.Ducking.Max")
#define TEXT_INFO MT_("VoiceProcessing.Info")

#define BUS_OUTPUT 0
#define BUS_INPUT 1

/* What the ducking levels are, as AUVoiceIOOtherAudioDuckingLevel has them
 * (macOS 14). */
#define DUCKING_DEFAULT 0
#define DUCKING_MIN 10
#define DUCKING_MID 20
#define DUCKING_MAX 30

struct voice_processing {
	obs_source_t *context;
	obs_source_t *parent;

	/* Starts and stops the unit, in order, off the threads that ask. */
	os_task_queue_t *tasks;

	/* The unit, only touched by the tasks, and its input callback's
	 * buffer. */
	AudioUnit unit;
	float *capture;
	UInt32 capture_frames;

	/* Guards everything below. */
	pthread_mutex_t mutex;

	/* What the next start uses. */
	char *device_uid;
	char *parent_device_uid;
	char *output_uid;
	bool agc;
	bool bypass;
	long long ducking;

	uint32_t sample_rate;
	size_t channels;

	/* What the unit captured, mono float, until the source's audio
	 * comes. */
	struct deque buffer;
	bool running;
	bool primed;
	/* How many frames the buffer keeps between the two captures: it grows
	 * each time the unit falls behind. */
	size_t target;
	/* Where in the buffer the next frame is read, between two of its
	 * frames, and how full it is, smoothed. */
	double position;
	double level;
	double ratio;
	/* How it went, since the last report. */
	uint64_t behind;
	uint64_t dropped;
	uint64_t reported_frames;
	uint64_t reported_ns;
	bool reported;
	/* What the unit captured since, in how many frames at a time, and
	 * what of it was lost: not rendered, and with what error, or more
	 * than the capture buffer holds. */
	uint64_t unit_frames;
	UInt32 unit_slice;
	uint64_t unit_failed;
	OSStatus unit_error;
	uint64_t unit_oversized;
	/* The devices it runs on. */
	AudioDeviceID input_device;
	AudioDeviceID output_device;
	/* It could not start, though the filter wanted it. */
	bool failed;

	/* The filter's own, on the audio thread. */
	float *out;
	size_t out_frames;
	float *in;
	size_t in_frames;
	/* The last frame, faded out from when the unit falls behind, and the
	 * gain the unit's frames come back in with after. */
	float last;
	float gain;
};

static const char *vp_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return TEXT_NAME;
}

static bool vp_success(struct voice_processing *vp, OSStatus stat, const char *action)
{
	if (stat != noErr) {
		warn("%s failed: %d", action, (int)stat);
		return false;
	}
	return true;
}

static bool get_device(const char *uid, bool input, AudioDeviceID *id)
{
	if (!uid || !*uid || strcmp(uid, DEVICE_DEFAULT) == 0) {
		AudioObjectPropertyAddress addr = {
			.mSelector = input ? kAudioHardwarePropertyDefaultInputDevice
					   : kAudioHardwarePropertyDefaultOutputDevice,
			.mScope = kAudioObjectPropertyScopeGlobal,
			.mElement = kAudioObjectPropertyElementMain,
		};
		UInt32 size = sizeof(*id);
		return AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &size, id) == noErr &&
		       *id != kAudioObjectUnknown;
	}

	CFStringRef cf_uid = CFStringCreateWithCString(NULL, uid, kCFStringEncodingUTF8);
	bool success = coreaudio_get_device_id(cf_uid, id);
	CFRelease(cf_uid);
	return success;
}

/* ------------------------------------------------------------------------- */
/* The unit                                                                  */

static OSStatus vp_input_callback(void *data, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
				  UInt32 frames, AudioBufferList *unused)
{
	struct voice_processing *vp = data;

	UNUSED_PARAMETER(bus);
	UNUSED_PARAMETER(unused);

	if (frames > vp->capture_frames) {
		pthread_mutex_lock(&vp->mutex);
		vp->unit_slice = frames;
		vp->unit_oversized += frames;
		pthread_mutex_unlock(&vp->mutex);
		return noErr;
	}

	AudioBufferList list = {
		.mNumberBuffers = 1,
		.mBuffers[0] =
			{
				.mNumberChannels = 1,
				.mDataByteSize = frames * sizeof(float),
				.mData = vp->capture,
			},
	};

	OSStatus stat = AudioUnitRender(vp->unit, flags, ts, BUS_INPUT, frames, &list);
	if (stat != noErr) {
		pthread_mutex_lock(&vp->mutex);
		vp->unit_slice = frames;
		vp->unit_failed += frames;
		vp->unit_error = stat;
		pthread_mutex_unlock(&vp->mutex);
		return stat;
	}

	/* As many frames as it rendered, which may be fewer than asked. */
	const size_t bytes = list.mBuffers[0].mDataByteSize < frames * sizeof(float) ? list.mBuffers[0].mDataByteSize
										     : frames * sizeof(float);

	pthread_mutex_lock(&vp->mutex);
	vp->unit_slice = frames;
	vp->unit_frames += bytes / sizeof(float);
	/* At most a second, should the source's audio stop coming. */
	const size_t limit = vp->sample_rate * sizeof(float);
	deque_push_back(&vp->buffer, vp->capture, bytes);
	if (vp->buffer.size > limit)
		deque_pop_front(&vp->buffer, NULL, vp->buffer.size - limit);
	pthread_mutex_unlock(&vp->mutex);

	return noErr;
}

/* The unit plays nothing; the echo it cancels is what others play. */
static OSStatus vp_render_callback(void *data, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
				   UInt32 frames, AudioBufferList *list)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(ts);
	UNUSED_PARAMETER(bus);
	UNUSED_PARAMETER(frames);

	for (UInt32 i = 0; i < list->mNumberBuffers; i++)
		memset(list->mBuffers[i].mData, 0, list->mBuffers[i].mDataByteSize);
	*flags |= kAudioUnitRenderAction_OutputIsSilence;
	return noErr;
}

/* A device for one of the unit's elements: on macOS, the input element (1)
 * takes the microphone and the output element (0) the device whose output is
 * cancelled. */
static bool vp_set_device(struct voice_processing *vp, AudioUnitElement element, AudioDeviceID id)
{
	OSStatus stat = AudioUnitSetProperty(vp->unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global,
					     element, &id, sizeof(id));
	if (stat == noErr)
		return true;

	warn("setting the device of element %u failed: %d", (unsigned)element, (int)stat);
	return false;
}

static void vp_stop(struct voice_processing *vp)
{
	if (!vp->unit)
		return;

	AudioOutputUnitStop(vp->unit);
	AudioUnitUninitialize(vp->unit);
	AudioComponentInstanceDispose(vp->unit);
	vp->unit = NULL;

	pthread_mutex_lock(&vp->mutex);
	vp->running = false;
	vp->input_device = kAudioObjectUnknown;
	vp->output_device = kAudioObjectUnknown;
	vp->primed = false;
	deque_free(&vp->buffer);
	pthread_mutex_unlock(&vp->mutex);

	bfree(vp->capture);
	vp->capture = NULL;
	vp->capture_frames = 0;

	info("stopped");
}

/* How much macOS lowers other audio while the unit runs (macOS 14 and later),
 * and never more while someone speaks (advanced ducking). Set again once the
 * unit is initialized, when it is logged as the unit has it. */
static void vp_set_ducking(struct voice_processing *vp, long long ducking, bool initialized)
{
	if (__builtin_available(macOS 14.0, *)) {
		AUVoiceIOOtherAudioDuckingConfiguration config = {
			.mEnableAdvancedDucking = false,
			.mDuckingLevel = (AUVoiceIOOtherAudioDuckingLevel)ducking,
		};
		OSStatus stat = AudioUnitSetProperty(vp->unit, kAUVoiceIOProperty_OtherAudioDuckingConfiguration,
						     kAudioUnitScope_Global, 0, &config, sizeof(config));
		vp_success(vp, stat, "setting the ducking of other audio");

		UInt32 size = sizeof(config);
		if (initialized && AudioUnitGetProperty(vp->unit, kAUVoiceIOProperty_OtherAudioDuckingConfiguration,
							kAudioUnitScope_Global, 0, &config, &size) == noErr)
			info("lowering other audio at level %u, %s while someone speaks",
			     (unsigned)config.mDuckingLevel, config.mEnableAdvancedDucking ? "more" : "no more");
	}
}

/* The unit, on these devices, until it is stopped. */
static bool vp_open(struct voice_processing *vp, AudioDeviceID input, AudioDeviceID output, bool agc, bool bypass,
		    long long ducking, uint32_t sample_rate)
{
	AudioComponentDescription desc = {
		.componentType = kAudioUnitType_Output,
		.componentSubType = kAudioUnitSubType_VoiceProcessingIO,
		.componentManufacturer = kAudioUnitManufacturer_Apple,
	};
	AudioComponent component = AudioComponentFindNext(NULL, &desc);
	if (!component) {
		warn("voice processing is not available");
		return false;
	}
	OSStatus stat = AudioComponentInstanceNew(component, &vp->unit);
	if (!vp_success(vp, stat, "creating the voice processing unit")) {
		vp->unit = NULL;
		return false;
	}

	UInt32 on = 1;
	stat = AudioUnitSetProperty(vp->unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, BUS_INPUT, &on,
				    sizeof(on));
	if (!vp_success(vp, stat, "enabling input"))
		return false;
	/* The output stays enabled: the unit only cancels echo with it. */
	stat = AudioUnitSetProperty(vp->unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, BUS_OUTPUT,
				    &on, sizeof(on));
	if (!vp_success(vp, stat, "enabling output"))
		return false;

	/* Each element its own device; failing that, the one device of the
	 * unit, as other output units have it. */
	if (vp_set_device(vp, BUS_INPUT, input))
		vp_set_device(vp, BUS_OUTPUT, output);
	else if (!vp_set_device(vp, BUS_OUTPUT, input))
		return false;

	AudioStreamBasicDescription format = {
		.mSampleRate = sample_rate,
		.mFormatID = kAudioFormatLinearPCM,
		.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved,
		.mBytesPerPacket = sizeof(float),
		.mFramesPerPacket = 1,
		.mBytesPerFrame = sizeof(float),
		.mChannelsPerFrame = 1,
		.mBitsPerChannel = 32,
	};
	stat = AudioUnitSetProperty(vp->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, BUS_INPUT,
				    &format, sizeof(format));
	if (!vp_success(vp, stat, "setting the capture format"))
		return false;
	stat = AudioUnitSetProperty(vp->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, BUS_OUTPUT,
				    &format, sizeof(format));
	if (!vp_success(vp, stat, "setting the playback format"))
		return false;

	UInt32 agc_on = agc;
	stat = AudioUnitSetProperty(vp->unit, kAUVoiceIOProperty_VoiceProcessingEnableAGC, kAudioUnitScope_Global, 0,
				    &agc_on, sizeof(agc_on));
	vp_success(vp, stat, "setting automatic gain control");

	UInt32 bypass_on = bypass;
	stat = AudioUnitSetProperty(vp->unit, kAUVoiceIOProperty_BypassVoiceProcessing, kAudioUnitScope_Global, 0,
				    &bypass_on, sizeof(bypass_on));
	vp_success(vp, stat, "setting the bypass of processing");

	vp_set_ducking(vp, ducking, false);

	/* Room for more frames at a time than the unit says it gives at most,
	 * as its own conversions may have it give more. Its maximum is left as
	 * it is: the IO buffer of the devices, which other units of the
	 * process share, follows it. */
	UInt32 max_frames = 0;
	UInt32 size = sizeof(max_frames);
	stat = AudioUnitGetProperty(vp->unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0,
				    &max_frames, &size);
	if (stat != noErr || max_frames < 4096)
		max_frames = 4096;
	vp->capture = bzalloc(max_frames * 4 * sizeof(float));
	vp->capture_frames = max_frames * 4;

	AURenderCallbackStruct capture = {.inputProc = vp_input_callback, .inputProcRefCon = vp};
	stat = AudioUnitSetProperty(vp->unit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0,
				    &capture, sizeof(capture));
	if (!vp_success(vp, stat, "setting the input callback"))
		return false;

	AURenderCallbackStruct playback = {.inputProc = vp_render_callback, .inputProcRefCon = vp};
	stat = AudioUnitSetProperty(vp->unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, BUS_OUTPUT,
				    &playback, sizeof(playback));
	if (!vp_success(vp, stat, "setting the render callback"))
		return false;

	stat = AudioUnitInitialize(vp->unit);
	if (!vp_success(vp, stat, "initializing the voice processing unit"))
		return false;
	vp_set_ducking(vp, ducking, true);

	AudioStreamBasicDescription device;
	size = sizeof(device);
	if (AudioUnitGetProperty(vp->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, BUS_INPUT, &device,
				 &size) == noErr)
		info("the microphone at %.0f Hz, %u channels; processing %s", device.mSampleRate,
		     (unsigned)device.mChannelsPerFrame, bypass ? "bypassed" : "on");

	pthread_mutex_lock(&vp->mutex);
	deque_free(&vp->buffer);
	vp->primed = false;
	vp->target = sample_rate / 50;
	vp->position = 0.0;
	vp->level = 0.0;
	vp->ratio = 1.0;
	vp->behind = 0;
	vp->dropped = 0;
	vp->reported_frames = 0;
	vp->reported_ns = os_gettime_ns();
	vp->reported = false;
	vp->unit_frames = 0;
	vp->unit_slice = 0;
	vp->unit_failed = 0;
	vp->unit_error = noErr;
	vp->unit_oversized = 0;
	vp->running = true;
	vp->input_device = input;
	vp->output_device = output;
	pthread_mutex_unlock(&vp->mutex);

	stat = AudioOutputUnitStart(vp->unit);
	return vp_success(vp, stat, "starting the voice processing unit");
}

/* The unit, if the filter is enabled and on a source, with the settings it
 * last had. */
static void vp_start(struct voice_processing *vp)
{
	pthread_mutex_lock(&vp->mutex);
	/* Read now: a filter is only enabled after its create. */
	const bool wanted = obs_source_enabled(vp->context) && vp->parent;
	const char *device = vp->device_uid && *vp->device_uid ? vp->device_uid : vp->parent_device_uid;
	char *input_uid = bstrdup(device && *device ? device : DEVICE_DEFAULT);
	char *output_uid = bstrdup(vp->output_uid && *vp->output_uid ? vp->output_uid : DEVICE_DEFAULT);
	const bool agc = vp->agc;
	const bool bypass = vp->bypass;
	const long long ducking = vp->ducking;
	const uint32_t sample_rate = vp->sample_rate;
	vp->failed = false;
	pthread_mutex_unlock(&vp->mutex);

	AudioDeviceID input, output;
	bool failed = true;

	if (!wanted) {
		failed = false;
	} else if (!get_device(input_uid, true, &input)) {
		warn("no microphone '%s'", input_uid);
	} else if (!get_device(output_uid, false, &output)) {
		warn("no output device '%s'", output_uid);
	} else if (!vp_open(vp, input, output, agc, bypass, ducking, sample_rate)) {
		vp_stop(vp);
	} else {
		info("started on microphone '%s', cancelling the output of '%s' [%" PRIu32 " Hz]", input_uid,
		     output_uid, sample_rate);
		failed = false;
	}

	pthread_mutex_lock(&vp->mutex);
	vp->failed = failed;
	pthread_mutex_unlock(&vp->mutex);

	bfree(input_uid);
	bfree(output_uid);
}

static void vp_restart_task(void *param)
{
	struct voice_processing *vp = param;
	vp_stop(vp);
	vp_start(vp);
}

static void vp_stop_task(void *param)
{
	vp_stop(param);
}

static inline void vp_restart(struct voice_processing *vp)
{
	os_task_queue_queue_task(vp->tasks, vp_restart_task, vp);
}

/* ------------------------------------------------------------------------- */
/* The filter                                                                */

/* The microphone of the source, for "Same as source". */
static void vp_read_parent_device(struct voice_processing *vp, obs_source_t *parent)
{
	char *uid = NULL;

	if (parent && strcmp(obs_source_get_unversioned_id(parent), "coreaudio_input_capture") == 0) {
		obs_data_t *settings = obs_source_get_settings(parent);
		uid = bstrdup(obs_data_get_string(settings, "device_id"));
		obs_data_release(settings);
	}

	pthread_mutex_lock(&vp->mutex);
	bfree(vp->parent_device_uid);
	vp->parent_device_uid = uid;
	pthread_mutex_unlock(&vp->mutex);
}

static void vp_parent_updated(void *data, calldata_t *cd)
{
	struct voice_processing *vp = data;
	obs_source_t *parent = calldata_ptr(cd, "source");

	pthread_mutex_lock(&vp->mutex);
	const char *old = vp->parent_device_uid;
	char *before = bstrdup(old ? old : "");
	pthread_mutex_unlock(&vp->mutex);

	vp_read_parent_device(vp, parent);

	pthread_mutex_lock(&vp->mutex);
	const char *now = vp->parent_device_uid;
	const bool changed = strcmp(before, now ? now : "") != 0;
	const bool follows = !vp->device_uid || !*vp->device_uid;
	pthread_mutex_unlock(&vp->mutex);
	bfree(before);

	if (changed && follows) {
		info("the source's microphone changed, restarting");
		vp_restart(vp);
	}
}

static void vp_enabled(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	vp_restart(data);
}

/* A default device changed: the unit follows it if it uses it, and starts if
 * it could not before, as the device may only now be there. macOS also tells
 * of a change when the default stays the same, and while a voice processing
 * unit initializes, before this one runs. */
static OSStatus vp_default_device_changed(AudioObjectID object, UInt32 count,
					  const AudioObjectPropertyAddress *addresses, void *data)
{
	struct voice_processing *vp = data;
	AudioDeviceID input = kAudioObjectUnknown;
	AudioDeviceID output = kAudioObjectUnknown;
	AudioDeviceID running_input, running_output;
	bool restart = false;

	UNUSED_PARAMETER(object);

	get_device(NULL, true, &input);
	get_device(NULL, false, &output);

	pthread_mutex_lock(&vp->mutex);
	for (UInt32 i = 0; i < count; i++) {
		if (addresses[i].mSelector == kAudioHardwarePropertyDefaultInputDevice) {
			const char *device = vp->device_uid && *vp->device_uid ? vp->device_uid : vp->parent_device_uid;
			if (!device || !*device || strcmp(device, DEVICE_DEFAULT) == 0)
				restart |= vp->failed || (vp->running && input != vp->input_device);
		} else if (addresses[i].mSelector == kAudioHardwarePropertyDefaultOutputDevice) {
			if (!vp->output_uid || strcmp(vp->output_uid, DEVICE_DEFAULT) == 0)
				restart |= vp->failed || (vp->running && output != vp->output_device);
		}
	}
	restart &= obs_source_enabled(vp->context) && vp->parent;
	running_input = vp->input_device;
	running_output = vp->output_device;
	pthread_mutex_unlock(&vp->mutex);

	if (restart) {
		info("the default devices changed (microphone %u to %u, output %u to %u), restarting",
		     (unsigned)running_input, (unsigned)input, (unsigned)running_output, (unsigned)output);
		vp_restart(vp);
	}
	return noErr;
}

static const AudioObjectPropertyAddress default_input_address = {
	.mSelector = kAudioHardwarePropertyDefaultInputDevice,
	.mScope = kAudioObjectPropertyScopeGlobal,
	.mElement = kAudioObjectPropertyElementMain,
};

static const AudioObjectPropertyAddress default_output_address = {
	.mSelector = kAudioHardwarePropertyDefaultOutputDevice,
	.mScope = kAudioObjectPropertyScopeGlobal,
	.mElement = kAudioObjectPropertyElementMain,
};

static void vp_read_settings(struct voice_processing *vp, obs_data_t *settings)
{
	pthread_mutex_lock(&vp->mutex);
	bfree(vp->device_uid);
	bfree(vp->output_uid);
	vp->device_uid = bstrdup(obs_data_get_string(settings, S_DEVICE));
	vp->output_uid = bstrdup(obs_data_get_string(settings, S_OUTPUT_DEVICE));
	vp->agc = obs_data_get_bool(settings, S_AGC);
	vp->bypass = obs_data_get_bool(settings, S_BYPASS);
	vp->ducking = obs_data_get_int(settings, S_DUCKING);
	vp->sample_rate = audio_output_get_sample_rate(obs_get_audio());
	vp->channels = audio_output_get_channels(obs_get_audio());
	pthread_mutex_unlock(&vp->mutex);
}

static void vp_update(void *data, obs_data_t *settings)
{
	vp_read_settings(data, settings);
	vp_restart(data);
}

static void *vp_create(obs_data_t *settings, obs_source_t *filter)
{
	struct voice_processing *vp = bzalloc(sizeof(*vp));
	vp->context = filter;
	pthread_mutex_init(&vp->mutex, NULL);
	vp->tasks = os_task_queue_create();

	signal_handler_connect(obs_source_get_signal_handler(filter), "enable", vp_enabled, vp);
	AudioObjectAddPropertyListener(kAudioObjectSystemObject, &default_input_address, vp_default_device_changed, vp);
	AudioObjectAddPropertyListener(kAudioObjectSystemObject, &default_output_address, vp_default_device_changed,
				       vp);

	/* It starts once on a source (vp_filter_add): a restart queued now
	 * would run then too. */
	vp_read_settings(vp, settings);
	return vp;
}

static void vp_filter_add(void *data, obs_source_t *parent)
{
	struct voice_processing *vp = data;

	vp_read_parent_device(vp, parent);
	signal_handler_connect(obs_source_get_signal_handler(parent), "update", vp_parent_updated, vp);

	pthread_mutex_lock(&vp->mutex);
	vp->parent = parent;
	pthread_mutex_unlock(&vp->mutex);

	vp_restart(vp);
}

static void vp_filter_remove(void *data, obs_source_t *parent)
{
	struct voice_processing *vp = data;

	signal_handler_disconnect(obs_source_get_signal_handler(parent), "update", vp_parent_updated, vp);

	pthread_mutex_lock(&vp->mutex);
	vp->parent = NULL;
	pthread_mutex_unlock(&vp->mutex);

	os_task_queue_queue_task(vp->tasks, vp_stop_task, vp);
}

static void vp_destroy(void *data)
{
	struct voice_processing *vp = data;

	signal_handler_disconnect(obs_source_get_signal_handler(vp->context), "enable", vp_enabled, vp);
	AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &default_input_address, vp_default_device_changed,
					  vp);
	AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &default_output_address, vp_default_device_changed,
					  vp);

	if (vp->parent)
		signal_handler_disconnect(obs_source_get_signal_handler(vp->parent), "update", vp_parent_updated, vp);

	os_task_queue_queue_task(vp->tasks, vp_stop_task, vp);
	os_task_queue_destroy(vp->tasks);

	pthread_mutex_destroy(&vp->mutex);
	deque_free(&vp->buffer);
	bfree(vp->device_uid);
	bfree(vp->parent_device_uid);
	bfree(vp->output_uid);
	bfree(vp->out);
	bfree(vp->in);
	bfree(vp);
}

/* The source's frames, replaced by as many of the unit's. Some frames more
 * than a packet wait in the buffer, so that the two capture threads may run a
 * little apart: a fiftieth of a second, and half as much again, up to a
 * tenth, each time the unit falls behind. The two captures run on two clocks,
 * the microphone's and the unit's, which drift apart: the unit's frames are
 * read up to 0.2% faster when more than that wait, and slower when fewer,
 * interpolated, so that the buffer neither runs dry nor overflows. Past a
 * tenth of a second more anyway, the oldest go. Until the unit has captured
 * enough, and when it falls behind, what is missing fades out from the last
 * frame rather than breaking off, and what comes after fades back in. Ten seconds in, and then every ten seconds
 * that either happened, how it goes. */
static struct obs_audio_data *vp_filter_audio(void *data, struct obs_audio_data *audio)
{
	struct voice_processing *vp = data;
	const size_t frames = audio->frames;
	const size_t bytes = frames * sizeof(float);

	if (!frames)
		return audio;

	if (vp->out_frames < frames) {
		vp->out = brealloc(vp->out, bytes);
		vp->out_frames = frames;
	}
	/* At most 0.2% more, and the two frames around the last. */
	const size_t most = frames + frames / 500 + 3;
	if (vp->in_frames < most) {
		vp->in = brealloc(vp->in, most * sizeof(float));
		vp->in_frames = most;
	}

	pthread_mutex_lock(&vp->mutex);
	if (!vp->running) {
		pthread_mutex_unlock(&vp->mutex);
		return audio;
	}

	const size_t channels = vp->channels;
	const uint32_t rate = vp->sample_rate;
	const size_t tenth = rate / 10;
	size_t available = vp->buffer.size / sizeof(float);
	size_t done = 0;

	if (!vp->primed && available >= frames + vp->target) {
		vp->primed = true;
		vp->position = 0.0;
		vp->level = (double)vp->target;
	}

	if (vp->primed) {
		vp->level += 0.05 * ((double)available - (double)frames - vp->level);
		/* What is more or less than the target, made up over two
		 * seconds. */
		double ratio = 1.0 + (vp->level - (double)vp->target) / (2.0 * rate);
		ratio = ratio < 0.998 ? 0.998 : ratio > 1.002 ? 1.002 : ratio;
		vp->ratio = ratio;

		const double end = vp->position + (double)frames * ratio;
		const size_t need = (size_t)(vp->position + (double)(frames - 1) * ratio) + 2;

		if (available < need) {
			vp->primed = false;
			vp->behind++;
			vp->target = vp->target * 3 / 2 < tenth ? vp->target * 3 / 2 : tenth;
		} else {
			deque_peek_front(&vp->buffer, vp->in, need * sizeof(float));
			for (size_t i = 0; i < frames; i++) {
				const double x = vp->position + (double)i * ratio;
				const size_t j = (size_t)x;
				const float f = (float)(x - (double)j);
				vp->out[i] = vp->in[j] + (vp->in[j + 1] - vp->in[j]) * f;
			}
			const size_t consumed = (size_t)end;
			vp->position = end - (double)consumed;
			deque_pop_front(&vp->buffer, NULL, consumed * sizeof(float));
			available -= consumed;
			done = frames;

			if (available > vp->target + frames + tenth) {
				vp->dropped += available - vp->target;
				deque_pop_front(&vp->buffer, NULL, (available - vp->target) * sizeof(float));
				vp->level = (double)vp->target;
			}
		}
	}

	bool report = false;
	uint64_t behind = 0, dropped = 0, failed = 0, oversized = 0;
	double ahead_ms = 0.0, drift_ppm = 0.0, unit_rate = 0.0, source_rate = 0.0;
	UInt32 slice = 0;
	OSStatus error = noErr;
	vp->reported_frames += frames;
	if (vp->reported_frames >= (uint64_t)rate * 10) {
		const uint64_t now = os_gettime_ns();
		const double seconds = (double)(now - vp->reported_ns) / 1e9;
		report = !vp->reported || vp->behind || vp->dropped || vp->unit_failed || vp->unit_oversized;
		behind = vp->behind;
		dropped = vp->dropped;
		ahead_ms = 1000.0 * (double)vp->target / rate;
		drift_ppm = (vp->ratio - 1.0) * 1e6;
		unit_rate = (double)vp->unit_frames / seconds;
		source_rate = (double)vp->reported_frames / seconds;
		slice = vp->unit_slice;
		failed = vp->unit_failed;
		error = vp->unit_error;
		oversized = vp->unit_oversized;
		vp->behind = 0;
		vp->dropped = 0;
		vp->reported_frames = 0;
		vp->reported_ns = now;
		vp->unit_frames = 0;
		vp->unit_failed = 0;
		vp->unit_error = noErr;
		vp->unit_oversized = 0;
		vp->reported = true;
	}
	pthread_mutex_unlock(&vp->mutex);

	for (size_t i = 0; i < done && vp->gain < 1.0f; i++) {
		vp->out[i] *= vp->gain;
		vp->gain = fminf(vp->gain + 1.0f / 128.0f, 1.0f);
	}
	if (!done)
		vp->gain = 0.0f;

	float last = done ? vp->out[frames - 1] : vp->last;
	for (size_t i = done; i < frames; i++) {
		last *= 0.99f;
		vp->out[i] = last;
	}
	vp->last = vp->out[frames - 1];

	if (report) {
		info("in the last 10 s, behind %" PRIu64 " times, %" PRIu64
		     " frames dropped; %.0f ms ahead, reading %+.0f ppm faster",
		     behind, dropped, ahead_ms, drift_ppm);
		info("the unit captured %.0f frames a second, %u at a time, the source took %.0f; %" PRIu64
		     " frames not rendered (%d), %" PRIu64 " more than the buffer holds",
		     unit_rate, (unsigned)slice, source_rate, failed, (int)error, oversized);
	}

	for (size_t c = 0; c < channels && c < MAX_AV_PLANES; c++) {
		if (audio->data[c])
			memcpy(audio->data[c], vp->out, bytes);
	}

	return audio;
}

static void vp_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, S_DEVICE, DEVICE_SOURCE);
	obs_data_set_default_string(settings, S_OUTPUT_DEVICE, DEVICE_DEFAULT);
	obs_data_set_default_bool(settings, S_AGC, false);
	obs_data_set_default_bool(settings, S_BYPASS, false);
	obs_data_set_default_int(settings, S_DUCKING, DUCKING_MIN);
}

static void add_devices(obs_property_t *list, bool input)
{
	struct device_list devices = {0};

	coreaudio_enum_devices(&devices, input);
	for (size_t i = 0; i < devices.items.num; i++) {
		struct device_item *item = devices.items.array + i;
		obs_property_list_add_string(list, item->name.array, item->value.array);
	}
	device_list_free(&devices);
}

static obs_properties_t *vp_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	UNUSED_PARAMETER(data);

	p = obs_properties_add_list(props, S_DEVICE, TEXT_DEVICE, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, TEXT_DEVICE_SOURCE, DEVICE_SOURCE);
	obs_property_list_add_string(p, TEXT_DEVICE_DEFAULT, DEVICE_DEFAULT);
	add_devices(p, true);

	p = obs_properties_add_list(props, S_OUTPUT_DEVICE, TEXT_OUTPUT_DEVICE, OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, TEXT_DEVICE_DEFAULT, DEVICE_DEFAULT);
	add_devices(p, false);

	obs_properties_add_bool(props, S_AGC, TEXT_AGC);
	obs_properties_add_bool(props, S_BYPASS, TEXT_BYPASS);

	if (__builtin_available(macOS 14.0, *)) {
		p = obs_properties_add_list(props, S_DUCKING, TEXT_DUCKING, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(p, TEXT_DUCKING_MIN, DUCKING_MIN);
		obs_property_list_add_int(p, TEXT_DUCKING_MID, DUCKING_MID);
		obs_property_list_add_int(p, TEXT_DUCKING_MAX, DUCKING_MAX);
		obs_property_list_add_int(p, TEXT_DUCKING_DEFAULT, DUCKING_DEFAULT);
	}

	obs_properties_add_text(props, S_INFO, TEXT_INFO, OBS_TEXT_INFO);

	return props;
}

struct obs_source_info voice_processing_filter_info = {
	.id = "mac_voice_processing_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_AUDIO,
	.get_name = vp_get_name,
	.create = vp_create,
	.destroy = vp_destroy,
	.update = vp_update,
	.get_defaults = vp_defaults,
	.get_properties = vp_properties,
	.filter_audio = vp_filter_audio,
	.filter_add = vp_filter_add,
	.filter_remove = vp_filter_remove,
};
