#include <obs-module.h>

#ifndef BETTERFACE_VERSION
#define BETTERFACE_VERSION "dev"
#endif

OBS_DECLARE_MODULE()

MODULE_EXPORT const char *obs_module_name(void)
{
	return "BetterFace for OBS";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Multi-face skin smoothing, face slimming and touch-up (Apple Vision)";
}

void register_betterface_filter();

bool obs_module_load(void)
{
	register_betterface_filter();
	blog(LOG_INFO, "[BetterFace] loaded, version %s", BETTERFACE_VERSION);
	return true;
}
