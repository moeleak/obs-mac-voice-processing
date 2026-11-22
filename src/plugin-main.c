// SPDX-License-Identifier: GPL-2.0-or-later

#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-mac-voice-processing", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Echo cancellation and noise suppression through macOS's voice processing";
}

extern struct obs_source_info voice_processing_filter_info;

bool obs_module_load(void)
{
	obs_register_source(&voice_processing_filter_info);
	return true;
}
