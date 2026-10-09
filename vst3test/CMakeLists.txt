cmake_minimum_required(VERSION 3.28...3.30)

# Test tools for obs-vst3 (not part of OBS): a headless libobs host and an instrumented test VST3 plug-in.
project(obs-vst3-test)

find_package(Qt6 REQUIRED Core)

add_executable(obs-vst3-harness harness.cpp)
target_link_libraries(obs-vst3-harness PRIVATE OBS::libobs Qt::Core)
target_compile_definitions(obs-vst3-harness PRIVATE NOMINMAX)

set(VST3SDK "${CMAKE_SOURCE_DIR}/plugins/obs-vst3/sdk")
set(
  TESTFX_SDK_SOURCES
  pluginterfaces/base/funknown.cpp
  pluginterfaces/base/coreiids.cpp
  pluginterfaces/base/ustring.cpp
  pluginterfaces/base/conststringtable.cpp
  base/source/baseiids.cpp
  base/source/fobject.cpp
  base/source/fstring.cpp
  base/source/fdebug.cpp
  base/source/updatehandler.cpp
  base/source/fbuffer.cpp
  base/source/fstreamer.cpp
  base/source/timer.cpp
  base/thread/source/flock.cpp
  base/thread/source/fcondition.cpp
  public.sdk/source/vst/vstaudioeffect.cpp
  public.sdk/source/vst/vstcomponent.cpp
  public.sdk/source/vst/vstcomponentbase.cpp
  public.sdk/source/vst/vsteditcontroller.cpp
  public.sdk/source/vst/vstbus.cpp
  public.sdk/source/vst/vstparameters.cpp
  public.sdk/source/vst/vstinitiids.cpp
  public.sdk/source/main/pluginfactory.cpp
  public.sdk/source/common/commonstringconvert.cpp
  public.sdk/source/common/pluginview.cpp
  public.sdk/source/vst/utility/stringconvert.cpp
)
list(TRANSFORM TESTFX_SDK_SOURCES PREPEND "${VST3SDK}/")

add_library(OBSTestFX MODULE testfx.cpp ${TESTFX_SDK_SOURCES})
target_include_directories(OBSTestFX PRIVATE "${VST3SDK}")
target_compile_definitions(OBSTestFX PRIVATE RELEASE=1 NOMINMAX)
target_compile_options(OBSTestFX PRIVATE /EHsc /W0)
set_target_properties(OBSTestFX PROPERTIES PREFIX "" SUFFIX ".vst3")
