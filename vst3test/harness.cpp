// Headless libobs harness: loads obs-vst3 as a real OBS module and runs robustness scenarios against the
// instrumented test VST3 plug-in (testfx). One scenario per process: harness <obs-vst3.so> <data dir> <testfx.so>
// <config dir> <scenario>. Prints "RESULT key=value ..." lines and exits 0 when the scenario's checks pass.
#include <obs.h>
#include <util/platform.h>

#include <QCoreApplication>
#include <QThread>

#ifdef _WIN32
#include <windows.h>
#else
#include <obs-nix-platform.h>
#include <dlfcn.h>
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct TestFxStats {
	std::atomic<long> moduleEntry{0}, moduleExit{0};
	std::atomic<long> processors{0}, processorsTerminated{0};
	std::atomic<long> processCalls{0}, violations{0}, processWhileInactive{0};
	std::atomic<long> offMainThreadControl{0}, activations{0}, restartsRequested{0};
	std::atomic<long> initFailuresGiven{0};
	std::atomic<long> lastViolationKind{0};
};

static TestFxStats *stats = nullptr;
static int failures = 0;
static obs_source_t *probeFilter = nullptr; // a VST3 filter without plug-in, used to read the scan state

static void *loadLib(const char *path)
{
#ifdef _WIN32
	return (void *)LoadLibraryA(path);
#else
	return dlopen(path, RTLD_NOW);
#endif
}

static void *getSym(void *lib, const char *name)
{
#ifdef _WIN32
	return (void *)GetProcAddress((HMODULE)lib, name);
#else
	return dlsym(lib, name);
#endif
}

static void check(bool ok, const char *what)
{
	std::printf("CHECK %-58s %s\n", what, ok ? "ok" : "FAILED");
	std::fflush(stdout);
	if (!ok)
		failures++;
}

static void pump(int ms)
{
	auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	while (std::chrono::steady_clock::now() < end) {
		QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
}

// ------------------------------------------------------------------ test audio source + feeder
struct TestSrc {
	obs_source_t *src;
};
static const char *tsrc_name(void *)
{
	return "Test audio";
}
static void *tsrc_create(obs_data_t *, obs_source_t *s)
{
	return new TestSrc{s};
}
static void tsrc_destroy(void *d)
{
	delete static_cast<TestSrc *>(d);
}

struct Feed {
	obs_source_t *src = nullptr;
	std::atomic<bool> run{true};
	std::thread th;
	std::mutex m;
	double sumSq = 0;
	long frames = 0, nonFinite = 0, packets = 0;
	void reset()
	{
		std::lock_guard<std::mutex> l(m);
		sumSq = 0;
		frames = 0;
		packets = 0;
	}
	double rmsRatio()
	{
		std::lock_guard<std::mutex> l(m);
		if (!frames)
			return -1;
		const double inRms = 0.25 / std::sqrt(2.0);
		return std::sqrt(sumSq / frames) / inRms;
	}
};

static void capture_cb(void *param, obs_source_t *, const struct audio_data *audio, bool)
{
	auto *f = static_cast<Feed *>(param);
	const float *L = reinterpret_cast<const float *>(audio->data[0]);
	if (!L)
		return;
	std::lock_guard<std::mutex> l(f->m);
	for (uint32_t i = 0; i < audio->frames; ++i) {
		if (!std::isfinite(L[i])) {
			f->nonFinite++;
			continue;
		}
		f->sumSq += double(L[i]) * L[i];
	}
	f->frames += audio->frames;
	f->packets++;
}

static void feed_thread(Feed *f)
{
	std::vector<float> L(480), R(480);
	double phase = 0;
	uint64_t ts = os_gettime_ns();
	while (f->run.load()) {
		for (int i = 0; i < 480; ++i) {
			L[i] = R[i] = float(0.25 * std::sin(phase));
			phase += 2 * 3.14159265358979323846 * 440.0 / 48000.0;
		}
		struct obs_source_audio a = {};
		a.data[0] = reinterpret_cast<uint8_t *>(L.data());
		a.data[1] = reinterpret_cast<uint8_t *>(R.data());
		a.frames = 480;
		a.speakers = SPEAKERS_STEREO;
		a.format = AUDIO_FORMAT_FLOAT_PLANAR;
		a.samples_per_sec = 48000;
		a.timestamp = ts;
		obs_source_output_audio(f->src, &a);
		ts += 10000000ULL;
		const uint64_t now = os_gettime_ns();
		if (ts > now)
			std::this_thread::sleep_for(std::chrono::nanoseconds(ts - now));
	}
}

static Feed *startFeed(obs_source_t *src)
{
	auto *f = new Feed();
	f->src = src;
	obs_source_add_audio_capture_callback(src, capture_cb, f);
	f->th = std::thread(feed_thread, f);
	return f;
}

static void stopFeed(Feed *f)
{
	f->run = false;
	f->th.join();
	obs_source_remove_audio_capture_callback(f->src, capture_cb, f);
}

// ------------------------------------------------------------------ helpers
static std::string classId(const char *configDir, const char *name)
{
	std::string path = std::string(configDir) + "/obs-vst3/vst3list.json";
	obs_data_t *root = obs_data_create_from_json_file(path.c_str());
	std::string id;
	if (root) {
		obs_data_array_t *arr = obs_data_get_array(root, "plugins");
		for (size_t i = 0; arr && i < obs_data_array_count(arr); ++i) {
			obs_data_t *o = obs_data_array_item(arr, i);
			if (std::strcmp(obs_data_get_string(o, "name"), name) == 0)
				id = obs_data_get_string(o, "id");
			obs_data_release(o);
		}
		obs_data_array_release(arr);
		obs_data_release(root);
	}
	return id;
}

static obs_source_t *addFilter(obs_source_t *src, const std::string &id, const char *name,
			       const char *stateHex = nullptr)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "vst3_plugin", id.c_str());
	if (stateHex)
		obs_data_set_string(s, "vst3_state", stateHex);
	obs_source_t *f = obs_source_create("vst3_filter", name, s, nullptr);
	obs_data_release(s);
	obs_source_filter_add(src, f);
	return f;
}

static void removeFilter(obs_source_t *src, obs_source_t *f)
{
	obs_source_filter_remove(src, f);
	obs_source_release(f);
}

static void setPlugin(obs_source_t *f, const std::string &id)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "vst3_plugin", id.c_str());
	obs_source_update(f, s);
	obs_data_release(s);
}

static long jsonFsize(const char *configDir, const char *name)
{
	std::string path = std::string(configDir) + "/obs-vst3/vst3list.json";
	obs_data_t *root = obs_data_create_from_json_file(path.c_str());
	long v = -1;
	if (root) {
		obs_data_array_t *arr = obs_data_get_array(root, "plugins");
		for (size_t i = 0; arr && i < obs_data_array_count(arr); ++i) {
			obs_data_t *o = obs_data_array_item(arr, i);
			if (std::strcmp(obs_data_get_string(o, "name"), name) == 0)
				v = (long)obs_data_get_int(o, "fsize");
			obs_data_release(o);
		}
		obs_data_array_release(arr);
		obs_data_release(root);
	}
	return v;
}

static std::string savedState(obs_source_t *f)
{
	obs_data_t *saved = obs_save_source(f);
	obs_data_t *settings = obs_data_get_obj(saved, "settings");
	std::string s = obs_data_get_string(settings, "vst3_state");
	obs_data_release(settings);
	obs_data_release(saved);
	return s;
}

static void printStats(const char *when)
{
	std::printf("RESULT %s entry=%ld exit=%ld procs=%ld terminated=%ld process=%ld violations=%ld "
		    "inactive=%ld offmain=%ld activations=%ld restartsReq=%ld lastKind=%ld\n",
		    when, stats->moduleEntry.load(), stats->moduleExit.load(), stats->processors.load(),
		    stats->processorsTerminated.load(), stats->processCalls.load(), stats->violations.load(),
		    stats->processWhileInactive.load(), stats->offMainThreadControl.load(), stats->activations.load(),
		    stats->restartsRequested.load(), stats->lastViolationKind.load());
	std::fflush(stdout);
}

static bool scanFinished()
{
	obs_properties_t *props = obs_source_properties(probeFilter);
	obs_property_t *p = obs_properties_get(props, "vst3_rescan");
	const bool done = p && obs_property_enabled(p);
	obs_properties_destroy(props);
	return done;
}

static bool waitScan(int maxMs)
{
	for (int t = 0; t < maxMs; t += 20) {
		if (scanFinished())
			return true;
		pump(20);
	}
	return scanFinished();
}

static bool hasErrorText(obs_source_t *f, std::string *text)
{
	obs_properties_t *props = obs_source_properties(f);
	obs_property_t *p = obs_properties_get(props, "vst3_error");
	if (p && text)
		*text = obs_property_description(p) ? obs_property_description(p) : "";
	obs_properties_destroy(props);
	return p != nullptr;
}

static void pressRescan(obs_source_t *f)
{
	obs_properties_t *props = obs_source_properties(f);
	obs_property_t *p = obs_properties_get(props, "vst3_rescan");
	if (p)
		obs_property_button_clicked(p, f);
	obs_properties_destroy(props);
}

int main(int argc, char **argv)
{
	if (argc < 6) {
		std::fprintf(stderr, "usage: harness <obs-vst3.so> <data dir> <testfx.so> <config dir> <scenario>\n");
		return 2;
	}
	const char *pluginSo = argv[1], *dataDir = argv[2], *testfxSo = argv[3], *configDir = argv[4];
	const std::string scenario = argv[5];

#ifdef _WIN32
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX); // a crash ends the process, no dialog
#endif
	QCoreApplication app(argc, argv);

	void *tfx = loadLib(testfxSo);
	if (!tfx) {
		std::fprintf(stderr, "could not load the test plug-in %s\n", testfxSo);
		return 2;
	}
	stats = reinterpret_cast<TestFxStats *(*)()>(getSym(tfx, "testfx_stats"))();
	reinterpret_cast<void (*)()>(getSym(tfx, "testfx_set_main_thread"))();

#ifndef _WIN32
	obs_set_nix_platform(OBS_NIX_PLATFORM_X11_EGL);
#endif
	if (!obs_startup("en-US", configDir, nullptr)) {
		std::fprintf(stderr, "obs_startup failed\n");
		return 2;
	}
	struct obs_audio_info ai = {48000, SPEAKERS_STEREO};
	if (!obs_reset_audio(&ai)) {
		std::fprintf(stderr, "obs_reset_audio failed\n");
		return 2;
	}

	struct obs_source_info tsi = {};
	tsi.id = "test_audio_src";
	tsi.type = OBS_SOURCE_TYPE_INPUT;
	tsi.output_flags = OBS_SOURCE_AUDIO;
	tsi.get_name = tsrc_name;
	tsi.create = tsrc_create;
	tsi.destroy = tsrc_destroy;
	obs_register_source(&tsi);

	obs_module_t *mod = nullptr;
	if (obs_open_module(&mod, pluginSo, dataDir) != MODULE_SUCCESS || !obs_init_module(mod)) {
		std::fprintf(stderr, "could not load obs-vst3\n");
		return 2;
	}
	probeFilter = obs_source_create_private("vst3_filter", "probe", nullptr);
	check(probeFilter != nullptr && waitScan(60000), "initial VST3 scan finished");
	std::printf("RESULT harness-alive=1 obs-version=%s\n", obs_get_version_string());
	std::fflush(stdout);

	const std::string idA = classId(configDir, "OBSTestFX A");
	const std::string idB = classId(configDir, "OBSTestFX B");
	if (scenario != "crash_initdll")
		check(!idA.empty() && !idB.empty(), "test plug-in classes found by the scanner");
	printStats("after-scan");
	const long entryAfterScan = stats->moduleEntry.load();

	obs_source_t *src = obs_source_create("test_audio_src", "mic", nullptr, nullptr);
	Feed *feed = startFeed(src);
	std::vector<obs_source_t *> filters;

	if (scenario == "basic") {
		filters.push_back(addFilter(src, idA, "f1"));
		filters.push_back(addFilter(src, idA, "f2"));
		filters.push_back(addFilter(src, idB, "f3"));
		pump(1500);
		feed->reset();
		pump(1000);
		const double r = feed->rmsRatio();
		std::printf("RESULT gain-ratio=%.4f (expected 0.125)\n", r);
		check(std::fabs(r - 0.125) < 0.01, "three filters process the audio (gain 0.5^3)");
		printStats("running");
		std::printf("RESULT module-loads-for-3-filters=%ld\n", stats->moduleEntry.load() - entryAfterScan);
		for (auto *f : filters)
			removeFilter(src, f);
		filters.clear();
		pump(800);
		printStats("removed");
		check(stats->processorsTerminated.load() == stats->processors.load(), "all plug-in instances terminated");
	} else if (scenario == "swap") {
		auto *f = addFilter(src, idA, "f1");
		filters.push_back(f);
		pump(800);
		for (int i = 0; i < 40; ++i) {
			setPlugin(f, (i % 2) ? idA : idB);
			pump(25);
		}
		pump(500);
		feed->reset();
		pump(800);
		const double r = feed->rmsRatio();
		std::printf("RESULT gain-ratio=%.4f (expected 0.5)\n", r);
		check(std::fabs(r - 0.5) < 0.02, "filter still processes after 40 swaps");
		printStats("after-swaps");
	} else if (scenario == "restart_audio" || scenario == "restart_in_active") {
		filters.push_back(addFilter(src, idA, "f1"));
		pump(3000);
		feed->reset();
		pump(1000);
		const double r = feed->rmsRatio();
		std::printf("RESULT gain-ratio=%.4f (expected ~0.5)\n", r);
		check(r > 0.3 && r < 0.7, "filter processes while latency restarts happen");
		printStats("after-restarts");
		check(stats->activations.load() >= 1 && stats->activations.load() < 1000,
		      "activation count bounded (no restart loop)");
	} else if (scenario == "init_retry") {
		filters.push_back(addFilter(src, idA, "f1"));
		pump(1000);
		const double r0 = (feed->reset(), pump(500), feed->rmsRatio());
		std::printf("RESULT gain-before-retry=%.4f (expected 1.0 = pass-through)\n", r0);
		pump(14500);
		feed->reset();
		pump(1000);
		const double r = feed->rmsRatio();
		std::printf("RESULT gain-after-retry=%.4f (expected 0.5)\n", r);
		check(std::fabs(r - 0.5) < 0.02, "plug-in loaded by the automatic retry");
		printStats("after-retry");
	} else if (scenario == "nan") {
		filters.push_back(addFilter(src, idA, "f1"));
		pump(2000);
		std::printf("RESULT non-finite-samples-out=%ld\n", feed->nonFinite);
		check(feed->nonFinite == 0, "no NaN/Inf reaches OBS");
	} else if (scenario == "state") {
		// saved state = float 0.25 (little endian) -> 0000803e
		filters.push_back(addFilter(src, idA, "f1", "0000803e"));
		pump(1200);
		feed->reset();
		pump(800);
		const double r = feed->rmsRatio();
		std::printf("RESULT gain-ratio=%.4f (expected 0.25)\n", r);
		check(std::fabs(r - 0.25) < 0.02, "saved state restored");
		obs_data_t *saved = obs_save_source(filters[0]);
		obs_data_t *settings = obs_data_get_obj(saved, "settings");
		std::printf("RESULT saved-state=%s\n", obs_data_get_string(settings, "vst3_state"));
		check(std::strcmp(obs_data_get_string(settings, "vst3_state"), "0000803e") == 0, "state saved again");
		obs_data_release(settings);
		obs_data_release(saved);
	} else if (scenario == "rescan") {
		filters.push_back(addFilter(src, idA, "f1"));
		pump(800);
		const long before = stats->moduleEntry.load();
		std::string cache = std::string(configDir) + "/obs-vst3/vst3list.json";
		std::remove(cache.c_str());
		pressRescan(filters[0]);
		waitScan(60000);
		pump(300);
		std::printf("RESULT module-loads-during-rescan=%ld\n", stats->moduleEntry.load() - before);
		printStats("after-rescan");
		feed->reset();
		pump(800);
		check(std::fabs(feed->rmsRatio() - 0.5) < 0.02, "filter keeps processing during/after rescan");
	} else if (scenario == "state_retry" || scenario == "state_reject") {
		// saved state = 0.25; the plug-in rejects the first restore (state_retry) or every restore (state_reject)
		filters.push_back(addFilter(src, idA, "f1", "0000803e"));
		pump(1200);
		std::string s1 = savedState(filters[0]);
		std::printf("RESULT saved-state-after-failed-restore=%s\n", s1.c_str());
		check(s1 == "0000803e", "saved settings kept after a rejected restore");
		if (scenario == "state_retry") {
			pump(3500);
			feed->reset();
			pump(800);
			const double r = feed->rmsRatio();
			std::printf("RESULT gain-ratio=%.4f (expected 0.25)\n", r);
			check(std::fabs(r - 0.25) < 0.02, "settings restored by the automatic re-try");
		} else {
			pump(14500);
			feed->reset();
			pump(800);
			const double r = feed->rmsRatio();
			std::printf("RESULT gain-ratio=%.4f (expected 0.5, defaults)\n", r);
			check(std::fabs(r - 0.5) < 0.02, "plug-in keeps running with its defaults");
		}
		std::string s2 = savedState(filters[0]);
		std::printf("RESULT saved-state-at-end=%s\n", s2.c_str());
		check(s2 == "0000803e", "saved settings never overwritten with defaults");
	} else if (scenario == "shell_rescan") {
		// TESTFX_DISCARDABLE=1: the test module behaves like WaveShell
		const long fs1 = jsonFsize(configDir, "OBSTestFX A");
		std::printf("RESULT fingerprint-after-first-scan=%ld\n", fs1);
		const bool fingerprinted = fs1 > 0; // bundle folders (Linux/macOS) are never fingerprinted: always re-read
		if (!fingerprinted)
			std::printf("SKIP fingerprint checks: this OS uses bundle folders, which are always re-read\n");
		filters.push_back(addFilter(src, idA, "f1"));
		pump(800);
		const long before = stats->moduleEntry.load();
		pressRescan(filters[0]);
		waitScan(60000);
		pump(300);
		std::printf("RESULT module-loads-during-rescan=%ld\n", stats->moduleEntry.load() - before);
		check(stats->moduleEntry.load() == before, "rescan read the shell from the live module (no 2nd InitDll)");
		const long fs2 = jsonFsize(configDir, "OBSTestFX A");
		std::printf("RESULT fingerprint-after-rescan=%ld\n", fs2);
		if (fingerprinted)
			check(fs2 == 0, "live shell marked for a fresh read at the next start");
		check(!classId(configDir, "OBSTestFX A").empty(), "shell classes still listed after rescan");
		feed->reset();
		pump(800);
		check(std::fabs(feed->rmsRatio() - 0.5) < 0.02, "filter keeps processing during/after rescan");
	} else if (scenario == "shell_next_start") {
		std::printf("RESULT module-loads-during-startup-scan=%ld\n", entryAfterScan);
		check(entryAfterScan == 1, "next start re-read the shell from disk");
		const long fs = jsonFsize(configDir, "OBSTestFX A");
		if (fs != 0)
			check(fs > 0, "real fingerprint stored again");
	} else if (scenario == "shell_cached") {
		std::printf("RESULT module-loads-during-startup-scan=%ld\n", entryAfterScan);
		if (jsonFsize(configDir, "OBSTestFX A") > 0)
			check(entryAfterScan == 0, "unchanged shell taken from the cache (fast start)");
		else
			std::printf("SKIP cache check: bundle folders are always re-read on this OS\n");
	} else if (scenario == "crash_process" || scenario == "throw_process") {
		// TESTFX_CRASH_CLASS=1: only plug-in A crashes (or throws) after 150 blocks; plug-in B keeps working
		filters.push_back(addFilter(src, idA, "f1"));
		filters.push_back(addFilter(src, idB, "f2"));
		pump(1200);
		feed->reset();
		pump(400);
		std::printf("RESULT gain-before-crash=%.4f (expected 0.25)\n", feed->rmsRatio());
		pump(2500);
		feed->reset();
		pump(1000);
		const double r = feed->rmsRatio();
		std::printf("RESULT harness-alive-after-crash=1 gain-after-crash=%.4f (expected 0.5)\n", r);
		check(std::fabs(r - 0.5) < 0.03, "OBS survives; crashed plug-in passes audio, the other one processes");
		std::string text;
		check(hasErrorText(filters[0], &text), "crashed filter shows an error in its properties");
		std::printf("RESULT error-text=%s\n", text.c_str());
		check(!hasErrorText(filters[1], nullptr), "the other filter shows no error");
		removeFilter(src, filters[0]);
		filters.erase(filters.begin());
		pump(600);
		feed->reset();
		pump(800);
		check(std::fabs(feed->rmsRatio() - 0.5) < 0.03, "removing the crashed filter is safe");
	} else if (scenario == "crash_init" || scenario == "crash_setstate") {
		// plug-in A crashes while loading (initialize) or while its saved state is restored (setState)
		filters.push_back(addFilter(src, idA, "f1", scenario == "crash_setstate" ? "0000803e" : nullptr));
		pump(1500);
		std::string text;
		check(hasErrorText(filters[0], &text), "crashed filter shows an error in its properties");
		std::printf("RESULT harness-alive-after-crash=1 error-text=%s\n", text.c_str());
		filters.push_back(addFilter(src, idB, "f2"));
		pump(1200);
		feed->reset();
		pump(1000);
		const double r = feed->rmsRatio();
		std::printf("RESULT gain=%.4f (expected 0.5: f1 passes audio, f2 processes)\n", r);
		check(std::fabs(r - 0.5) < 0.03, "OBS survives; the module's other plug-in still loads and works");
		const long procs = stats->processors.load();
		pump(4000);
		check(stats->processors.load() == procs, "a plug-in that crashed is not reloaded automatically");
	} else if (scenario == "crash_initdll") {
		// the module crashes in its entry point while the scanner loads it
		std::printf("RESULT harness-alive-after-scan=1 classes-found=%d\n", (int)!idA.empty());
		check(idA.empty(), "the crashing module is skipped by the scanner");
	} else if (scenario == "stress") {
		for (int round = 0; round < 12; ++round) {
			filters.push_back(addFilter(src, (round % 2) ? idA : idB, ("s" + std::to_string(round)).c_str()));
			pump(60);
			if (filters.size() > 4) {
				removeFilter(src, filters.front());
				filters.erase(filters.begin());
			}
			setPlugin(filters.back(), (round % 2) ? idB : idA);
			pump(40);
		}
		pump(1000);
		printStats("after-stress");
	}

	stopFeed(feed);
	for (auto *f : filters)
		removeFilter(src, f);
	obs_source_release(src);
	obs_source_release(probeFilter);
	pump(800);
	printStats("before-shutdown");
	const long violations = stats->violations.load();
	const long inactive = stats->processWhileInactive.load();
	const long offmain = stats->offMainThreadControl.load();
	obs_shutdown();
	printStats("after-shutdown");
	check(violations == 0, "no plug-in control call while process() was running");
	check(inactive == 0, "process() never called on an inactive plug-in");
	check(offmain == 0, "plug-in control/teardown only on the UI thread");
	if (scenario.rfind("crash_", 0) != 0 && scenario != "throw_process")
		check(stats->moduleExit.load() == stats->moduleEntry.load(), "every module entry has its exit");
	std::printf("RESULT failures=%d\n", failures);
	std::fflush(stdout);
	// Skip exit handlers: on Linux the SDK keeps a static reference to the host's run loop inside every plug-in
	// (pluginfactory.cpp, LinuxPlatformTimer::runLoop) that outlives the host; Windows hosts have no run loop.
	std::_Exit(failures ? 1 : 0);
}
