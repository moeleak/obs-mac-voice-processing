// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Puts the voice processing filter on a microphone source without OBS's
 * frontend, with the libobs, the mac-capture plugin (for the source) and this
 * plugin of the OBS.app given, ROUNDS times: it changes the filter's settings, disables and enables it,
 * takes it off and releases it. Then it leaves a filter on for LONG seconds,
 * for the filter to log how its buffer went. Fails if the plugin or the
 * filter is missing.
 */
#include <obs.h>
#include <util/bmem.h>

#include <stdio.h>
#include <unistd.h>

#define ROUNDS 3
#define LONG 25

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s OBS.app\n", argv[0]);
		return 2;
	}

	if (!obs_startup("en-US", NULL, NULL))
		return 1;

	struct obs_audio_info audio = {.samples_per_sec = 48000, .speakers = SPEAKERS_STEREO};
	if (!obs_reset_audio(&audio)) {
		fprintf(stderr, "test: no audio\n");
		return 1;
	}

	const char *plugins[] = {"mac-capture", "obs-mac-voice-processing"};
	for (size_t i = 0; i < sizeof(plugins) / sizeof(*plugins); i++) {
		char binary[4096], data[4096];
		snprintf(binary, sizeof(binary), "%s/Contents/PlugIns/%s.plugin/Contents/MacOS/%s", argv[1], plugins[i],
			 plugins[i]);
		snprintf(data, sizeof(data), "%s/Contents/PlugIns/%s.plugin/Contents/Resources", argv[1], plugins[i]);

		obs_module_t *module = NULL;
		if (obs_open_module(&module, binary, data) != MODULE_SUCCESS || !obs_init_module(module)) {
			fprintf(stderr, "test: %s did not load\n", plugins[i]);
			return 1;
		}
	}

	const char *name = obs_source_get_display_name("mac_voice_processing_filter");
	printf("test: filter '%s'\n", name ? name : "");
	if (!name)
		return 1;

	obs_source_t *mic = obs_source_create("coreaudio_input_capture", "microphone", NULL, NULL);
	if (!mic) {
		fprintf(stderr, "test: no source\n");
		return 1;
	}

	/* A new filter each round, as a user adds and removes one. */
	for (int round = 1; round <= ROUNDS; round++) {
		obs_source_t *filter =
			obs_source_create("mac_voice_processing_filter", "echo cancellation", NULL, NULL);
		if (!filter) {
			fprintf(stderr, "test: no filter\n");
			return 1;
		}

		obs_source_filter_add(mic, filter);
		printf("test: %d: added\n", round);
		sleep(3);

		obs_data_t *settings = obs_data_create();
		obs_data_set_bool(settings, "agc", round % 2 == 1);
		obs_data_set_bool(settings, "bypass", round == 2);
		obs_source_update(filter, settings);
		obs_data_release(settings);
		printf("test: %d: updated\n", round);
		sleep(2);

		obs_source_set_enabled(filter, false);
		printf("test: %d: disabled\n", round);
		sleep(1);
		obs_source_set_enabled(filter, true);
		printf("test: %d: enabled\n", round);
		sleep(2);

		obs_source_filter_remove(mic, filter);
		obs_source_release(filter);
		printf("test: %d: released\n", round);
	}

	obs_source_t *filter = obs_source_create("mac_voice_processing_filter", "echo cancellation", NULL, NULL);
	obs_source_filter_add(mic, filter);
	printf("test: on for %d s\n", LONG);
	sleep(LONG);
	obs_source_filter_remove(mic, filter);
	obs_source_release(filter);

	obs_source_release(mic);
	printf("test: released\n");

	obs_shutdown();
	printf("test: memory leaks: %ld\n", bnum_allocs());
	return 0;
}
