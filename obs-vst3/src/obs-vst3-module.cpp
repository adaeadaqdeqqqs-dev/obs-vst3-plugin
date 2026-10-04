/******************************************************************************
    Copyright (C) 2025-2026 pkv <pkv@obsproject.com>
    This file is part of obs-vst3.
    It uses the Steinberg VST3 SDK, which is licensed under MIT license.
    See https://github.com/steinbergmedia/vst3sdk for details.
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

// Qt must be included before Steinberg SDK headers (which define Status macro).
#include <QCoreApplication>

#include <util/platform.h>

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <thread>
#include <unordered_set>

// Steinberg headers must come after Qt but before obs-vst3 headers that use them.
#include "pluginterfaces/vst/ivstaudioprocessor.h"

#include "obs-vst3.h"
#include "VST3HostApp.h"
#include "VST3Scanner.h"
#include "vst3-filter.h"

#ifdef __linux__
#include "editor/linux/RunLoopImpl.h"
#endif

/* ----------------- global host & runloop ---------------- */
// As in DAWS there is a single global host. The linux runloop is also global in the SDK.
VST3HostApp *g_host_app_ = nullptr;
#ifdef __linux__
RunLoopImpl *g_run_loop_ = nullptr;
Display *g_display_ = nullptr;
#endif

bool load_host()
{
    g_host_app_ = new VST3HostApp();
    if (!g_host_app_) {
        return false;
    }

#ifdef __linux__
    g_display_ = XOpenDisplay(nullptr);
    if (!g_display_) {
        blog(LOG_WARNING,
             "[obs-vst3] No X11/XWayland display available; "
             "disabling VST3 host");
        delete g_host_app_;
        g_host_app_ = nullptr;
        return false;
    }
    g_run_loop_ = new RunLoopImpl(g_display_);
    g_host_app_->setRunLoop(g_run_loop_);
#endif
    return true;
}

void unload_host()
{
#ifdef __linux__
    if (g_run_loop_) {
        g_run_loop_->stop();
    }
    delete g_run_loop_;
#endif
    delete g_host_app_;
}

/* -------------------- VST3 list (scan/cache) ------------------ */

// global VST3Scanner
VST3Scanner *g_scanner_list_ = nullptr;
std::atomic<bool> g_vst3_scan_done_{false};

static void vst3_cache_save()
{
    if (!g_scanner_list_) {
        return;
    }

    char *path = obs_module_config_path(nullptr);
    os_mkdirs(path);
    bfree(path);

    char *filepath = obs_module_config_path("vst3list.json");

    obs_data_t *root = obs_data_create();
    obs_data_array_t *arr = obs_data_array_create();

    for (const auto &p : g_scanner_list_->pluginList) {
        obs_data_t *obj = obs_data_create();
        obs_data_set_string(obj, "name", p.name.c_str());
        obs_data_set_string(obj, "id", p.id.c_str());
        obs_data_set_string(obj, "path", p.path.c_str());
        obs_data_set_string(obj, "pluginName", p.pluginName.c_str());
        obs_data_set_bool(obj, "discardable", p.discardable);
        obs_data_array_push_back(arr, obj);
        obs_data_release(obj);
    }

    obs_data_set_int(root, "version", 1);
    obs_data_set_array(root, "plugins", arr);
    obs_data_array_release(arr);

    obs_data_save_json_safe(root, filepath, "tmp", "bak");
    obs_data_release(root);
    bfree(filepath);
}

static bool vst3_cache_load()
{
    char *path = obs_module_config_path("vst3list.json");
    if (!path) {
        return false;
    }

    obs_data_t *root = obs_data_create_from_json_file_safe(path, "bak");
    bfree(path);
    if (!root) {
        return false;
    }

    obs_data_array_t *arr = obs_data_get_array(root, "plugins");
    if (!arr) {
        obs_data_release(root);
        return false;
    }

    g_scanner_list_->pluginList.clear();
    g_scanner_list_->classCount.clear();

    std::unordered_map<std::string, ModuleCache> modules;

    size_t count = obs_data_array_count(arr);
    for (size_t i = 0; i < count; ++i) {
        obs_data_t *obj = obs_data_array_item(arr, i);
        VST3ClassInfo ci;

        ci.name = obs_data_get_string(obj, "name");
        ci.id = obs_data_get_string(obj, "id");
        ci.path = obs_data_get_string(obj, "path");
        ci.pluginName = obs_data_get_string(obj, "pluginName");
        ci.discardable = obs_data_get_bool(obj, "discardable");

        obs_data_release(obj);

        if (ci.path.empty() || !std::filesystem::exists(ci.path)) {
            continue;
        }

        auto &m = modules[ci.path];
        if (ci.discardable) {
            m.discardable = true;
        }

        m.classes.push_back(std::move(ci));
    }

    obs_data_array_release(arr);
    obs_data_release(root);

    // we need to update the json list in case VST3s have been removed
    std::unordered_set<std::string> cachedPaths;
    cachedPaths.reserve(modules.size());

    for (auto &kv : modules) {
        cachedPaths.insert(kv.first);
    }

    g_scanner_list_->updateModulesList(modules, cachedPaths);

    for (auto &[modulePath, m] : modules) {
        if (m.discardable) {
            g_scanner_list_->addModuleClasses(modulePath);
        } else {
            for (auto &ci : m.classes) {
                g_scanner_list_->pluginList.push_back(ci);
                ++g_scanner_list_->classCount[modulePath];
            }
        }
    }

    g_scanner_list_->sort();

    return !g_scanner_list_->pluginList.empty();
}

bool retrieve_vst3_list()
{
    g_vst3_scan_done_.store(false, std::memory_order_relaxed);
    g_scanner_list_ = new VST3Scanner();
    if (!g_scanner_list_->hasVST3()) {
        blog(LOG_INFO, "[VST3 Scanner] No VST3 were found");
        return false;
    }

    // Scan in background thread to not slow down OBS startup
    std::thread([] {
        using clock = std::chrono::steady_clock;
        auto start = clock::now();

        bool loaded_from_cache = vst3_cache_load();
        if (!loaded_from_cache) {
            if (!g_scanner_list_->scanForVST3Plugins()) {
                blog(LOG_INFO,
                     "[VST3 Scanner] Error when scanning for VST3. "
                     "Module will be unloaded.");
            }
        }

        blog(LOG_INFO, "[VST3 Scanner] Available plugins:");
        for (const auto &plugin : g_scanner_list_->pluginList) {
            blog(LOG_INFO, "[VST3 Scanner]   %s", plugin.name.c_str());
        }

        auto end = clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      end - start).count();
        blog(LOG_INFO,
             "[VST3 Scanner] %s in %lld ms, found %zu plugins",
             loaded_from_cache ? "Loaded cache & non-cacheable VST3s"
                               : "Completed scan",
             (long long)ms, g_scanner_list_->pluginList.size());

        vst3_cache_save();
        g_vst3_scan_done_.store(true, std::memory_order_release);
    }).detach();

    return true;
}

void free_vst3_list()
{
    delete g_scanner_list_;
    g_scanner_list_ = nullptr;
}

/* --------------------- utilities ----------------------- */

static bool is_valid_hex(const std::string &hex)
{
    if (hex.empty()) {
        return false;
    }
    if ((hex.size() & 1) != 0) {
        return false;
    }
    for (char c : hex) {
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

std::string toHex(const std::vector<uint8_t> &data)
{
    std::ostringstream oss;
    for (auto b : data) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
    }
    return oss.str();
}

std::vector<uint8_t> fromHex(const std::string &hex)
{
    if (!is_valid_hex(hex)) {
        blog(LOG_INFO, "Corrupted VST3 settings.");
        return {};
    }
    std::vector<uint8_t> data;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        data.push_back(
            (uint8_t)std::stoi(hex.substr(i, 2), nullptr, 16));
    }
    return data;
}

uint64_t
obs_to_vst3_speaker_arrangement(speaker_layout layout)
{
    using namespace Steinberg::Vst;
    switch (layout) {
    case SPEAKERS_MONO:
        return static_cast<uint64_t>(SpeakerArr::kMono);
    case SPEAKERS_STEREO:
        return static_cast<uint64_t>(SpeakerArr::kStereo);
    case SPEAKERS_2POINT1:
        return static_cast<uint64_t>(SpeakerArr::k30Cine);
    case SPEAKERS_4POINT0:
        return static_cast<uint64_t>(SpeakerArr::k40Cine);
    case SPEAKERS_4POINT1:
        return static_cast<uint64_t>(SpeakerArr::k41Cine);
    case SPEAKERS_5POINT1:
        return static_cast<uint64_t>(SpeakerArr::k51);
    case SPEAKERS_7POINT1:
        return static_cast<uint64_t>(SpeakerArr::k71Music);
    case SPEAKERS_UNKNOWN:
    default:
        return static_cast<uint64_t>(SpeakerArr::kEmpty);
    }
}

enum speaker_layout convert_speaker_layout(uint8_t channels)
{
    switch (channels) {
    case 0:
        return SPEAKERS_UNKNOWN;
    case 1:
        return SPEAKERS_MONO;
    case 2:
        return SPEAKERS_STEREO;
    case 3:
        return SPEAKERS_2POINT1;
    case 4:
        return SPEAKERS_4POINT0;
    case 5:
        return SPEAKERS_4POINT1;
    case 6:
        return SPEAKERS_5POINT1;
    case 8:
        return SPEAKERS_7POINT1;
    default:
        return SPEAKERS_UNKNOWN;
    }
}
