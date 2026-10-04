/******************************************************************************
    Copyright (C) 2025-2026 pkv <pkv@obsproject.com>
    This file is part of obs-vst3.
    It uses the Steinberg VST3 SDK, which is licensed under MIT license.
    See https://github.com/steinbergmedia/vst3sdk for details.
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "vst3-filter.h"
#include "VST3Graph.h"

#include <obs-module.h>

// Force export OBS module entry points (MSVC)
#ifdef _MSC_VER
#pragma comment(linker, "/EXPORT:obs_module_load")
#pragma comment(linker, "/EXPORT:obs_module_set_pointer")
#pragma comment(linker, "/EXPORT:obs_module_ver")
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-vst3", "en-US")

MODULE_EXPORT bool obs_module_load(void)
{
    if (!load_host()) {
        blog(LOG_WARNING, "[obs-vst3] Failed to load VST3 host");
        return false;
    }

    retrieve_vst3_list();

    // Register single VST3 filter
    register_vst3_source();

    // NOTE: the VST3 graph (plugin-chain) filter is intentionally NOT registered.
    // Its JSON chain (de)serialization is an unfinished stub in this build, so
    // exposing it risks half-saved chains and instability. Use one single VST3
    // filter per effect instead.
    // register_vst3_graph_source();

    blog(LOG_INFO, "[obs-vst3] Module loaded (single VST3 filter only)");
    return true;
}

MODULE_EXPORT void obs_module_unload(void)
{
    free_vst3_list();
    unload_host();
    blog(LOG_INFO, "[obs-vst3] Module unloaded");
}
