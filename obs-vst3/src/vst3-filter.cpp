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

#include <thread>
#include <chrono>
#include "vst3-filter.h"
#include "vst3-filter-audio.h"
#include "vst3-filter-sidechain.h"
#include "vst3-filter-properties.h"
#include "VST3Plugin.h"
#include "VST3Scanner.h"

#include <obs-module.h>
#include <util/platform.h>

/* -------------------------------------------------------- */
#define do_log(level, format, ...)                             \
    blog(level, "[VST3 filter ('%s')]: " format,              \
         obs_source_get_name(vd->context), ##__VA_ARGS__)

#define warnvst3(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define infovst3(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)
/* -------------------------------------------------------- */

// External globals
extern VST3Scanner *g_scanner_list_;
extern std::atomic<bool> g_vst3_scan_done_;

// --- stability fix: wait until the background VST3 scan has finished before
// resolving a plugin by id/path. At OBS startup saved filters are loaded while
// the scanner thread is still running; without this wait getPathById() returns
// empty and the plugin silently fails to load until OBS is restarted. ---
static bool wait_for_vst3_scan(int timeout_ms = 8000)
{
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + milliseconds(timeout_ms);
    while (!g_vst3_scan_done_.load(std::memory_order_acquire)) {
        if (steady_clock::now() >= deadline) {
            blog(LOG_WARNING,
                 "[VST3 filter] scan still running after %d ms; "
                 "proceeding without waiting further",
                 timeout_ms);
            return false;
        }
        std::this_thread::sleep_for(milliseconds(10));
    }
    return true;
}

static const char *vst3_filter_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return obs_module_text("VST3.Plugin");
}

void *vst3_create(obs_data_t *settings, obs_source_t *filter)
{
    auto *vd = new vst3_audio_data();

    if (!vd) {
        return nullptr;
    }

    vd->context = filter;
    vd->vst3_id = {};
    vd->vst3_name = {};
    vd->vst3_path = {};

    audio_t *audio = obs_get_audio();
    const struct audio_output_info *aoi = audio_output_get_info(audio);

    size_t frames = (size_t)FRAME_SIZE;
    vd->frames = frames;
    size_t channels = audio_output_get_channels(audio);
    vd->channels = channels;
    vd->sample_rate = audio_output_get_sample_rate(audio);
    vd->layout = aoi->speakers;
    vd->has_sidechain.store(false, std::memory_order_relaxed);
    vd->sidechain_enabled.store(false, std::memory_order_relaxed);
    vd->noview.store(true, std::memory_order_relaxed);

    vd->latency = 1000000000LL / (1000 / BUFFER_SIZE_MSEC);

    // allocate copy buffers (contiguous for the channels)
    vd->copy_buffers[0] = (float *)bmalloc(
        (size_t)FRAME_SIZE * channels * sizeof(float));
    vd->sc_copy_buffers[0] = (float *)bmalloc(
        FRAME_SIZE * channels * sizeof(float));

    for (size_t c = 1; c < channels; ++c) {
        vd->copy_buffers[c] = vd->copy_buffers[c - 1] + frames;
        vd->sc_copy_buffers[c] = vd->sc_copy_buffers[c - 1] + frames;
    }

    // reserve deques (about 4 ticks)
    for (size_t i = 0; i < channels; i++) {
        deque_reserve(&vd->input_buffers[i], 8 * frames * sizeof(float));
        deque_reserve(&vd->output_buffers[i], 8 * frames * sizeof(float));
        deque_reserve(&vd->sc_input_buffers[i], 8 * frames * sizeof(float));
    }

    vd->bypass.store(true, std::memory_order_relaxed);

    vst3_update(vd, settings);
    return vd;
}

void vst3_destroy(void *data)
{
    struct vst3_audio_data *vd =
        static_cast<struct vst3_audio_data *>(data);
    vd->bypass.store(true, std::memory_order_relaxed);
    vd->sidechain_enabled.store(false, std::memory_order_relaxed);

    if (vd->weak_sidechain) {
        obs_source_t *sidechain =
            obs_weak_source_get_source(vd->weak_sidechain);
        if (sidechain) {
            obs_source_remove_audio_capture_callback(
                sidechain, sidechain_capture, vd);
            obs_source_release(sidechain);
        }
        obs_weak_source_release(vd->weak_sidechain);
    }

    std::atomic_store(&vd->sc_resampler,
                      std::shared_ptr<audio_resampler>{});
    vd->sc_last_timestamp = 0;

    for (size_t i = 0; i < vd->channels; i++) {
        deque_free(&vd->input_buffers[i]);
        deque_free(&vd->output_buffers[i]);
        {
            std::lock_guard<std::mutex> lock(vd->sidechain_mutex);
            deque_free(&vd->sc_input_buffers[i]);
        }
    }
    bfree(vd->copy_buffers[0]);
    bfree(vd->sc_copy_buffers[0]);
    deque_free(&vd->info_buffer);
    da_free(vd->output_data);

    auto plugin = std::atomic_load(&vd->plugin);
    if (plugin) {
        plugin->setProcessing(false);
        std::atomic_store(&vd->plugin, std::shared_ptr<VST3Plugin>{});
    }

    delete vd;
}

void destroy_current_VST3Plugin(vst3_audio_data *vd, obs_data *settings)
{
    if (!vd) {
        return;
    }

    vd->bypass.store(true, std::memory_order_relaxed);
    auto plugin = std::atomic_load(&vd->plugin);
    if (!plugin) {
        return;
    }

    std::atomic_store(&vd->plugin, std::shared_ptr<VST3Plugin>{});

    plugin->setProcessing(false);
    plugin->hideEditor();
    plugin->deactivateComponent();
    vd->noview.store(true, std::memory_order_relaxed);

    if (vd->weak_sidechain) {
        teardown_sidechain(vd, settings);
    }
}

bool create_VST3Plugin(vst3_audio_data *vd)
{
    if (!vd) {
        return false;
    }

    if (vd->vst3_id.empty() || vd->vst3_path.empty()) {
        return true;
    }

    const std::string class_id = vd->vst3_id;
    const std::string vst3_path = vd->vst3_path;
    const int sample_rate = vd->sample_rate;
    const int max_block = FRAME_SIZE;

    auto arr =
        obs_to_vst3_speaker_arrangement(vd->layout);

    VST3Plugin *raw = new VST3Plugin();
    raw->obsVst3Data = vd;

    if (!raw->init(class_id, vst3_path, sample_rate, max_block, arr)) {
        infovst3("Failed to initialize VST3 plugin %s",
                 raw->name.c_str());
        vd->last_init_failed = true;
        delete raw;
        return false;
    } else {
        infovst3("Plugin %s was successfully initialized.",
                 raw->name.c_str());
    }

    if (!raw->createView()) {
        infovst3("Failed to create editor view for plugin at: %s",
                 vst3_path.c_str());
        vd->noview.store(true, std::memory_order_relaxed);
    } else {
        vd->noview.store(false, std::memory_order_relaxed);
        infovst3("Plugin %s has a GUI.", raw->name.c_str());
    }

    auto plugin = std::shared_ptr<VST3Plugin>(raw,
        [](VST3Plugin *p) {
            if (p) {
                p->deleteLater();
            }
        });

    std::atomic_store(&vd->plugin, plugin);
    vd->bypass.store(false, std::memory_order_relaxed);

    return true;
}

bool init_VST3Plugin(void *data, obs_data *settings)
{
    auto *vd = static_cast<vst3_audio_data *>(data);
    if (!vd) {
        return false;
    }

    if (vd->init_in_progress.test_and_set()) {
        return false;
    }

    struct ClearFlag {
        std::atomic_flag &f_;
        ~ClearFlag() { f_.clear(); }
    } guard{vd->init_in_progress};

    destroy_current_VST3Plugin(vd, settings);
    return create_VST3Plugin(vd);
}

void vst3_update(void *data, obs_data_t *settings)
{
    auto *vd = static_cast<struct vst3_audio_data *>(data);
    if (!vd) {
        return;
    }

    std::string vst3_plugin_id(
        obs_data_get_string(settings, "vst3_plugin"));

    if (vst3_plugin_id.empty()) {
        vd->bypass.store(true, std::memory_order_relaxed);
        vd->vst3_id.clear();
        vd->vst3_path.clear();
        vd->vst3_name.clear();
        vd->has_sidechain.store(false, std::memory_order_relaxed);
        destroy_current_VST3Plugin(vd, settings);
        return;
    }

    auto plugin = std::atomic_load(&vd->plugin);
    bool initial_load = vd->vst3_id.empty() && !plugin;
    bool is_swap = (vd->vst3_id != vst3_plugin_id);

    if (is_swap) {
        if (!initial_load) {
            destroy_current_VST3Plugin(vd, settings);
        }

        if (vd->output_data.array) {
            da_free(vd->output_data);
        }
        vd->vst3_id = vst3_plugin_id;
        vd->last_init_failed = false;

        // stability fix: make sure the scan is complete before we look up the
        // plugin path, otherwise startup loads hit an empty list and fail.
        wait_for_vst3_scan();

        if (!g_scanner_list_->getPathById(vst3_plugin_id).empty()) {
            vd->vst3_path =
                g_scanner_list_->getPathById(vst3_plugin_id);
            vd->vst3_name =
                g_scanner_list_->getNameById(vst3_plugin_id);
        } else {
            vd->vst3_path = obs_data_get_string(settings, "vst3_path");
            vd->vst3_name = obs_data_get_string(settings, "vst3_name");
        }

        infovst3("filter applied: %s, path: %s",
                 vd->vst3_name.c_str(), vd->vst3_path.c_str());

        if (init_VST3Plugin(vd, settings)) {
            auto plugin2 = std::atomic_load(&vd->plugin);
            if (plugin2) {
                plugin2->setProcessing(true);
                vd->sc_channels = plugin2->sidechainNumChannels;
            }
            vd->has_sidechain.store(
                vd->sc_channels == 1 || vd->sc_channels == 2,
                std::memory_order_relaxed);
            vd->bypass.store(false, std::memory_order_relaxed);
            plugin = plugin2;
        } else {
            infovst3("VST3 failure; plugin deactivated.");
            vd->bypass.store(true, std::memory_order_relaxed);
            vd->has_sidechain.store(false, std::memory_order_relaxed);
            vd->sidechain_enabled.store(false, std::memory_order_relaxed);
        }
    }

    // Only load the state the first time the filter is loaded
    if (plugin && initial_load) {
        const char *hexComp =
            obs_data_get_string(settings, "vst3_state");
        const char *hexCtrl =
            obs_data_get_string(settings, "vst3_ctrl_state");
        if (hexComp && *hexComp) {
            std::vector<uint8_t> comp = fromHex(hexComp);
            std::vector<uint8_t> ctrl;
            if (hexCtrl && *hexCtrl) {
                ctrl = fromHex(hexCtrl);
            }
            plugin->loadStates(comp, ctrl);
        }
    }

    if (vd->has_sidechain.load(std::memory_order_relaxed)) {
        sidechain_swap(vd, settings);
    }
}

void register_vst3_source()
{
    struct obs_source_info vst3_filter = {};
    vst3_filter.id = "vst3_filter";
    vst3_filter.type = OBS_SOURCE_TYPE_FILTER;
    vst3_filter.output_flags = OBS_SOURCE_AUDIO;
    vst3_filter.get_name = vst3_filter_name;
    vst3_filter.create = vst3_create;
    vst3_filter.destroy = vst3_destroy;
    vst3_filter.update = vst3_update;
    vst3_filter.filter_audio = vst3_filter_audio;
    vst3_filter.get_properties = vst3_properties;
    vst3_filter.save = vst3_save;
    vst3_filter.video_tick = vst3_tick;
    obs_register_source(&vst3_filter);
}
