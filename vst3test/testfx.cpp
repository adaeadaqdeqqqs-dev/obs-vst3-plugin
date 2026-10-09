// Instrumented test VST3 plug-in for the obs-vst3 host robustness tests.
// Two effect classes ("OBSTestFX A" / "OBSTestFX B"), gain 0.5 by default. Counts host threading violations:
//  - any control call (setActive, setProcessing, setState, setupProcessing, setBusArrangements, terminate) made
//    while process() is running on another thread
//  - process() called while the component is not active / not processing
//  - control calls / teardown made on a thread other than the host UI thread
// Misbehaviour switches (environment variables, read on every call):
//  TESTFX_SLOW_PROCESS_US   : sleep inside process()
//  TESTFX_SLOW_ACTIVE_MS    : sleep inside setActive()
//  TESTFX_RESTART_EVERY     : call restartComponent(kLatencyChanged) from process() every N blocks (audio thread)
//  TESTFX_RESTART_IN_ACTIVE : call restartComponent(kLatencyChanged) synchronously inside setActive(true)
//  TESTFX_FAIL_INIT         : the first N initialize() calls fail
//  TESTFX_NAN_EVERY         : write NaN into every Nth output block
//  TESTFX_FAIL_SETSTATE     : the first N component setState() calls are rejected
//  TESTFX_DISCARDABLE       : the factory reports kClassesDiscardable (like Waves' WaveShell)
// Crash switches (Windows crash-guard tests; TESTFX_CRASH_CLASS = 1: only class A, 2: only class B, 0: both):
//  TESTFX_CRASH_PROCESS_AFTER : access violation in process() at block N
//  TESTFX_THROW_PROCESS_AFTER : C++ exception thrown out of process() at block N
//  TESTFX_CRASH_INIT          : access violation in initialize()
//  TESTFX_CRASH_SETSTATE      : access violation in setState()
//  TESTFX_CRASH_INITDLL       : access violation in the module entry (InitDll / ModuleEntry)

#include "pluginterfaces/base/fplatform.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/vsteditcontroller.h"

#include "pluginterfaces/gui/iplugview.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <thread>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace Steinberg {
DEF_CLASS_IID(IPlugView)
#ifdef __linux__
namespace Linux {
DEF_CLASS_IID(IRunLoop)
DEF_CLASS_IID(ITimerHandler)
DEF_CLASS_IID(IEventHandler)
} // namespace Linux
#endif
} // namespace Steinberg

static void crashNow()
{
	volatile int *volatile p = nullptr;
	*p = 1; // genuine access violation
}

struct TestFxStats {
	std::atomic<long> moduleEntry{0}, moduleExit{0};
	std::atomic<long> processors{0}, processorsTerminated{0};
	std::atomic<long> processCalls{0}, violations{0}, processWhileInactive{0};
	std::atomic<long> offMainThreadControl{0}, activations{0}, restartsRequested{0};
	std::atomic<long> initFailuresGiven{0};
	std::atomic<long> lastViolationKind{0};
};
static TestFxStats gStats;
static std::atomic<std::thread::id> gMainThread{};
static std::atomic<IComponentHandler *> gHandler{nullptr};

static long envLong(const char *name)
{
	const char *v = std::getenv(name);
	return v ? std::atol(v) : 0;
}

extern "C" {
SMTG_EXPORT_SYMBOL TestFxStats *testfx_stats()
{
	return &gStats;
}
SMTG_EXPORT_SYMBOL void testfx_set_main_thread()
{
	gMainThread.store(std::this_thread::get_id());
}
// The SDK host (module_linux.cpp / module_win32.cpp) calls these; counted without reference counting on purpose.
#ifdef _WIN32
SMTG_EXPORT_SYMBOL bool PLUGIN_API InitDll()
{
	if (envLong("TESTFX_CRASH_INITDLL"))
		crashNow();
	gStats.moduleEntry++;
	return true;
}
SMTG_EXPORT_SYMBOL bool PLUGIN_API ExitDll()
{
	gStats.moduleExit++;
	return true;
}
#else
SMTG_EXPORT_SYMBOL bool ModuleEntry(void *)
{
	if (envLong("TESTFX_CRASH_INITDLL"))
		crashNow();
	gStats.moduleEntry++;
	return true;
}
SMTG_EXPORT_SYMBOL bool ModuleExit(void)
{
	gStats.moduleExit++;
	return true;
}
#endif
}
void *moduleHandle = nullptr;

static void checkMain(long kind)
{
	const auto main = gMainThread.load();
	if (main != std::thread::id() && std::this_thread::get_id() != main) {
		gStats.offMainThreadControl++;
		gStats.lastViolationKind = kind;
	}
}

static const FUID kProcA(0x1A2B3C4D, 0x11111111, 0x22222222, 0x00000001);
static const FUID kProcB(0x1A2B3C4D, 0x11111111, 0x22222222, 0x00000002);
static const FUID kCtrlA(0x1A2B3C4D, 0x33333333, 0x44444444, 0x00000001);
static const FUID kCtrlB(0x1A2B3C4D, 0x33333333, 0x44444444, 0x00000002);

class TestProcessor : public AudioEffect {
public:
	TestProcessor(const FUID &ctrl, long cls) : classIndex(cls) { setControllerClass(ctrl); }
	bool crashClass() const
	{
		const long only = envLong("TESTFX_CRASH_CLASS");
		return only == 0 || only == classIndex;
	}

	tresult PLUGIN_API initialize(FUnknown *context) override
	{
		checkMain(1);
		if (envLong("TESTFX_CRASH_INIT") && crashClass())
			crashNow();
		const long fail = envLong("TESTFX_FAIL_INIT");
		if (fail > 0 && gStats.initFailuresGiven.load() < fail) {
			gStats.initFailuresGiven++;
			return kResultFalse;
		}
		tresult r = AudioEffect::initialize(context);
		if (r != kResultOk)
			return r;
		addAudioInput(STR16("In"), SpeakerArr::kStereo);
		addAudioOutput(STR16("Out"), SpeakerArr::kStereo);
		gStats.processors++;
		return kResultOk;
	}
	tresult PLUGIN_API terminate() override
	{
		guard(2);
		checkMain(2);
		gStats.processorsTerminated++;
		return AudioEffect::terminate();
	}
	tresult PLUGIN_API setActive(TBool state) override
	{
		guard(3);
		checkMain(3);
		const long ms = envLong("TESTFX_SLOW_ACTIVE_MS");
		if (ms > 0)
			std::this_thread::sleep_for(std::chrono::milliseconds(ms));
		active = state;
		if (state) {
			gStats.activations++;
			if (envLong("TESTFX_RESTART_IN_ACTIVE") && gHandler.load()) {
				gStats.restartsRequested++;
				gHandler.load()->restartComponent(kLatencyChanged);
			}
		}
		return AudioEffect::setActive(state);
	}
	tresult PLUGIN_API setProcessing(TBool state) override
	{
		guard(4);
		processing = state;
		return kResultOk;
	}
	tresult PLUGIN_API setupProcessing(ProcessSetup &setup) override
	{
		guard(5);
		checkMain(5);
		return AudioEffect::setupProcessing(setup);
	}
	tresult PLUGIN_API setBusArrangements(SpeakerArrangement *inputs, int32 numIns, SpeakerArrangement *outputs,
					      int32 numOuts) override
	{
		guard(6);
		return AudioEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
	}
	tresult PLUGIN_API setState(IBStream *state) override
	{
		guard(7);
		if (envLong("TESTFX_CRASH_SETSTATE") && crashClass())
			crashNow();
		static std::atomic<long> rejected{0};
		if (rejected.load() < envLong("TESTFX_FAIL_SETSTATE")) {
			rejected++;
			return kResultFalse;
		}
		float g = 0.5f;
		int32 read = 0;
		if (state && state->read(&g, sizeof(g), &read) == kResultOk && read == sizeof(g))
			gain = g;
		return kResultOk;
	}
	tresult PLUGIN_API getState(IBStream *state) override
	{
		float g = gain;
		int32 written = 0;
		return state ? state->write(&g, sizeof(g), &written) : kResultFalse;
	}
	uint32 PLUGIN_API getLatencySamples() override { return 32; }

	tresult PLUGIN_API process(ProcessData &data) override
	{
		inProcess++;
		gStats.processCalls++;
		if (!active || !processing)
			gStats.processWhileInactive++;

		if (data.inputParameterChanges) {
			const int32 n = data.inputParameterChanges->getParameterCount();
			for (int32 i = 0; i < n; ++i) {
				IParamValueQueue *q = data.inputParameterChanges->getParameterData(i);
				if (q && q->getParameterId() == 0 && q->getPointCount() > 0) {
					int32 offset = 0;
					ParamValue v = 0;
					if (q->getPoint(q->getPointCount() - 1, offset, v) == kResultOk)
						gain = static_cast<float>(v);
				}
			}
		}

		const long us = envLong("TESTFX_SLOW_PROCESS_US");
		if (us > 0)
			std::this_thread::sleep_for(std::chrono::microseconds(us));

		const long blocks = ++blockCount;
		const long crashAt = envLong("TESTFX_CRASH_PROCESS_AFTER");
		if (crashAt > 0 && blocks == crashAt && crashClass())
			crashNow();
		const long throwAt = envLong("TESTFX_THROW_PROCESS_AFTER");
		if (throwAt > 0 && blocks == throwAt && crashClass())
			throw std::runtime_error("test plug-in exception from process()");
		const long restartEvery = envLong("TESTFX_RESTART_EVERY");
		if (restartEvery > 0 && blocks % restartEvery == 0 && gHandler.load()) {
			gStats.restartsRequested++;
			gHandler.load()->restartComponent(kLatencyChanged); // from the audio thread, on purpose
		}

		const long nanEvery = envLong("TESTFX_NAN_EVERY");
		if (data.numInputs > 0 && data.numOutputs > 0) {
			for (int32 ch = 0; ch < data.outputs[0].numChannels; ++ch) {
				float *in = data.inputs[0].channelBuffers32[ch];
				float *out = data.outputs[0].channelBuffers32[ch];
				for (int32 i = 0; i < data.numSamples; ++i)
					out[i] = in[i] * gain;
				if (nanEvery > 0 && blocks % nanEvery == 0)
					out[0] = std::nanf("");
			}
		}
		inProcess--;
		return kResultOk;
	}

	static FUnknown *createA(void *) { return (IAudioProcessor *)new TestProcessor(kCtrlA, 1); }
	static FUnknown *createB(void *) { return (IAudioProcessor *)new TestProcessor(kCtrlB, 2); }

private:
	long classIndex = 0;
	void guard(long kind)
	{
		if (inProcess.load() > 0) {
			gStats.violations++;
			gStats.lastViolationKind = kind;
		}
	}
	std::atomic<int> inProcess{0};
	std::atomic<bool> active{false}, processing{false};
	std::atomic<long> blockCount{0};
	float gain = 0.5f;
};

class TestController : public EditController {
public:
	tresult PLUGIN_API initialize(FUnknown *context) override
	{
		tresult r = EditController::initialize(context);
		if (r == kResultOk)
			parameters.addParameter(STR16("Gain"), nullptr, 0, 0.5, ParameterInfo::kCanAutomate, 0);
		return r;
	}
	tresult PLUGIN_API setComponentHandler(IComponentHandler *handler) override
	{
		gHandler.store(handler);
		return EditController::setComponentHandler(handler);
	}
	tresult PLUGIN_API terminate() override
	{
		checkMain(8);
		if (gHandler.load() == componentHandler)
			gHandler.store(nullptr);
		return EditController::terminate();
	}
	tresult PLUGIN_API setComponentState(IBStream *state) override
	{
		float g = 0.5f;
		int32 read = 0;
		if (state && state->read(&g, sizeof(g), &read) == kResultOk && read == sizeof(g))
			setParamNormalized(0, g);
		return kResultOk;
	}
	static FUnknown *create(void *) { return (IEditController *)new TestController; }
};

BEGIN_FACTORY("OBS test", "https://example.invalid", "mailto:none@example.invalid",
	      PFactoryInfo::kUnicode | (envLong("TESTFX_DISCARDABLE") ? PFactoryInfo::kClassesDiscardable : 0))
DEF_CLASS2(INLINE_UID_FROM_FUID(kProcA), PClassInfo::kManyInstances, kVstAudioEffectClass, "OBSTestFX A",
	   Vst::kDistributable, "Fx", "1.0.0", kVstVersionString, TestProcessor::createA)
DEF_CLASS2(INLINE_UID_FROM_FUID(kCtrlA), PClassInfo::kManyInstances, kVstComponentControllerClass,
	   "OBSTestFX A Controller", 0, "", "1.0.0", kVstVersionString, TestController::create)
DEF_CLASS2(INLINE_UID_FROM_FUID(kProcB), PClassInfo::kManyInstances, kVstAudioEffectClass, "OBSTestFX B",
	   Vst::kDistributable, "Fx", "1.0.0", kVstVersionString, TestProcessor::createB)
DEF_CLASS2(INLINE_UID_FROM_FUID(kCtrlB), PClassInfo::kManyInstances, kVstComponentControllerClass,
	   "OBSTestFX B Controller", 0, "", "1.0.0", kVstVersionString, TestController::create)
END_FACTORY
