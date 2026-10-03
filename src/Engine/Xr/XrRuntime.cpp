/*
 * Copyright 2010-2026 OpenXcom Developers.
 *
 * This file is part of OpenXcom.
 *
 * OpenXcom is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * OpenXcom is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with OpenXcom.  If not, see <http://www.gnu.org/licenses/>.
 */
// Immersive mode for Meta Quest: the game keeps rendering its classic 2D
// surface; this thread shows it as a canvas (or table) in the room and turns
// the controllers into mouse and keyboard. The VR menu itself is a normal
// game dialog (Menu/VrMenuState), opened through takeMenuRequest().
//
// Controls (pointer hand = right by default, pressing the other trigger swaps):
//   ray             mouse position          trigger       left click (hold = drag)
//   pointer grip    right click             pointer stick map scroll (arrow keys)
//   other stick y   mouse wheel             other grip + stick: canvas width / distance
//   other grip + stick click: reset canvas  left menu button: VR menu
//   A Enter, B Escape, X Tab, Y recenter canvas
#include "XrRuntime.h"
#include "XrKeyboard.h"
#include "XrMath.h"
#include "XrMenuLogic.h"
#include "XrUi.h"
#include <SDL.h>
#include <SDL_system.h>
#include <android/log.h>
#include <jni.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

// SDL is linked statically; these internal entry points feed input exactly
// like SDL's own Android glue does from the Java thread, so SDL's mouse and
// keyboard state (SDL_GetMouseState, SDL_GetKeyboardState) stays correct.
// (SDL_MouseID is an internal Uint32 typedef in SDL 2.0.9.)
extern "C" {
int SDL_SendMouseMotion(SDL_Window *window, Uint32 mouseID, int relative, int x, int y);
int SDL_SendMouseButton(SDL_Window *window, Uint32 mouseID, Uint8 state, Uint8 button);
int SDL_SendMouseWheel(SDL_Window *window, Uint32 mouseID, float x, float y, SDL_MouseWheelDirection direction);
int SDL_SendKeyboardKey(Uint8 state, SDL_Scancode scancode);
int SDL_SendKeyboardText(const char *text);
}

#define XLOGI(...) __android_log_print(ANDROID_LOG_INFO, "OXCE-XR", __VA_ARGS__)
#define XLOGE(...) __android_log_print(ANDROID_LOG_ERROR, "OXCE-XR", __VA_ARGS__)

namespace OpenXcom
{
namespace Xr
{

namespace
{

const float KeyboardWidth = 1.0f;
const float PressThreshold = 0.6f, ReleaseThreshold = 0.4f;
const float StickPress = 0.5f, StickRelease = 0.35f;
const double ClickFreezeSeconds = 0.085; // click stabilisation
const double SaveDelaySeconds = 0.7;
const double ResumePromptSeconds = 1.5;  // shorter focus losses (screenshots) do not ask
const float SmoothingTau[3] = {0.0f, 0.025f, 0.055f};

enum SurfaceKind { SURFACE_NONE, SURFACE_PANEL, SURFACE_KEYBOARD };
enum ArrowKey { ARROW_LEFT, ARROW_RIGHT, ARROW_UP, ARROW_DOWN, ARROW_COUNT };
enum FaceButton { BUTTON_A, BUTTON_B, BUTTON_X, BUTTON_Y, BUTTON_MENU, BUTTON_COUNT };

float stickAxis(float v) { return std::fabs(v) <= 0.25f ? 0.0f : std::copysign((std::fabs(v) - 0.25f) / 0.75f, v); }

bool hysteresis(float value, bool pressed) { return pressed ? value > ReleaseThreshold : value > PressThreshold; }

XrPosef toXr(const Pose &p)
{
	XrPosef r;
	r.orientation = {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
	r.position = {p.position.x, p.position.y, p.position.z};
	return r;
}

Pose fromXr(const XrPosef &p)
{
	Pose r;
	r.orientation = {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
	r.position = {p.position.x, p.position.y, p.position.z};
	return r;
}

struct Swapchain
{
	XrSwapchain handle = XR_NULL_HANDLE;
	int32_t width = 0, height = 0;
	std::vector<XrSwapchainImageOpenGLESKHR> images;
	bool hasContent = false;
};

struct HandInput
{
	bool aimValid = false;
	Pose aim;
	float trigger = 0, squeeze = 0;
	XrVector2f stick{0, 0};
	bool stickClick = false;
};

class Runtime
{
public:
	bool start(const std::string &configFolder);
	void stop();
	bool active() const { return _active; }
	bool gameShouldRun() const { return _active && (_focused || (_selfTest && _running && _state == XR_SESSION_STATE_VISIBLE)); }
	bool selfTestActive() const { return _selfTest; }
	void debugPointer(float u, float v)
	{
		std::lock_guard<std::mutex> lock(_debugMutex);
		_debugU = u;
		_debugV = v;
		_debugPointerPending = true;
	}
	void debugClick()
	{
		std::lock_guard<std::mutex> lock(_debugMutex);
		_debugClickPending = true;
	}
	void debugMenuButton() { _menuRequest = MENU_REQUEST_TOGGLE; }
	void debugText(const std::string &utf8)
	{
		std::lock_guard<std::mutex> lock(_debugMutex);
		_debugText += utf8;
	}
	bool keyboardVisible() const { return _keyboardVisibleShared; }
	void virtualPoint(float u, float v, int &x, int &y) const { canvasToVirtual(u, v, _outX, _outY, _outW, _outH, x, y); }
	void setContext(int context) { _context = context; }
	void setMenuOpen(bool open) { _menuOpenShared = open; }
	void hapticDoublePulse() { _doublePulseRequest = true; }
	void requestSave() { _saveRequest = true; }
	int takeMenuRequest() { return _menuRequest.exchange(MENU_REQUEST_NONE); }
	void setOutputRect(int x, int y, int w, int h);
	void submitFrame(const void *pixels, int width, int height, int pitch, Uint32 format);
	void frameUnchanged() { ++_gameFrames; }
	bool takeFirstRunDefaults()
	{
		if (!_firstRunDefaults)
			return false;
		_firstRunDefaults = false;
		++_settingsVersion; // the XR thread saves xr.cfg (now with the marker) shortly after
		return true;
	}
	VrSettings settings();
	void setSettings(const VrSettings &settings);
	VrInfo info() const;
	void recenter() { _recenterRequested = true; }

private:
	// lifecycle
	std::thread _thread;
	std::atomic<bool> _quit{false}, _active{false}, _focused{false};
	std::mutex _startMutex;
	std::condition_variable _startCv;
	int _startState = 0; // 0 pending, 1 ok, -1 failed
	JavaVM *_vm = nullptr;
	jobject _activity = nullptr;
	std::string _settingsPath;

	// game <-> XR exchange
	std::mutex _frameMutex;
	std::vector<uint8_t> _frame, _upload;
	int _frameW = 0, _frameH = 0;
	uint64_t _frameSeq = 0, _uploadedSeq = 0;
	std::atomic<uint64_t> _gameFrames{0};
	std::atomic<bool> _firstRunDefaults{false}; // xr.cfg had no "xr_defaults" key
	std::atomic<int> _outX{0}, _outY{0}, _outW{VirtualWidth}, _outH{VirtualHeight};
	std::atomic<int> _context{CONTEXT_MENU}, _menuRequest{MENU_REQUEST_NONE};
	std::atomic<bool> _recenterRequested{false};
	std::atomic<int> _gameFps{0}, _xrHz{0}, _texWShared{0}, _texHShared{0};

	// self-test (see XrRuntime.h)
	bool _selfTest = false;
	std::mutex _debugMutex;
	float _debugU = 0, _debugV = 0;
	bool _debugPointerPending = false, _debugReleasePending = false;
	bool _debugClickPending = false; // guarded by _debugMutex
	std::atomic<bool> _keyboardVisibleShared{false};
	std::atomic<bool> _menuOpenShared{false};     // VR menu page on top (ray and dot stay visible)
	std::atomic<bool> _doublePulseRequest{false}; // quit button armed: two short pulses
	std::atomic<bool> _saveRequest{false};        // quit confirmed: write xr.cfg now
	double _secondPulseAt = -1;
	std::string _debugText;
	void processDebugInput();

	// settings: written by both threads under the mutex, applied by the XR thread
	mutable std::mutex _settingsMutex;
	VrSettings _settingsShared;
	std::atomic<unsigned> _settingsVersion{1};
	unsigned _appliedVersion = 0, _savedVersion = 1;
	VrSettings _cfg; // XR thread copy
	double _saveAt = 0;

	// EGL / GL
	EGLDisplay _display = EGL_NO_DISPLAY;
	EGLConfig _config = nullptr;
	EGLContext _context_ = EGL_NO_CONTEXT;
	EGLSurface _pbuffer = EGL_NO_SURFACE;
	GLuint _program = 0, _fbo = 0;
	// Game frame -> canvas: source texture + sharp-bilinear upscale pass (also flips to GL row order).
	GLuint _blitProgram = 0, _frameTex = 0;
	GLint _blitFrameSize = -1, _blitTexSize = -1;
	int _panelRectW = 0, _panelRectH = 0;
	double _waitMsSum = 0;
	int _perfSeconds = 0;
	bool _layerSettingsSupported = false;
	GLint _mvpLocation = -1;

	// OpenXR
	XrInstance _instance = XR_NULL_HANDLE;
	XrSystemId _system = XR_NULL_SYSTEM_ID;
	XrSession _session = XR_NULL_HANDLE;
	XrSpace _local = XR_NULL_HANDLE, _view = XR_NULL_HANDLE;
	std::atomic<XrSessionState> _state{XR_SESSION_STATE_UNKNOWN};
	std::atomic<bool> _running{false};
	bool _wasFocused = false, _exitRequested = false;
	int64_t _format = 0;
	Swapchain _panelSc, _keyboardSc, _eyeSc[2];
	XrActionSet _actionSet = XR_NULL_HANDLE;
	XrAction _aimAction = XR_NULL_HANDLE, _triggerAction = XR_NULL_HANDLE, _squeezeAction = XR_NULL_HANDLE;
	XrAction _stickAction = XR_NULL_HANDLE, _stickClickAction = XR_NULL_HANDLE, _hapticAction = XR_NULL_HANDLE;
	XrAction _buttonActions[BUTTON_COUNT] = {XR_NULL_HANDLE, XR_NULL_HANDLE, XR_NULL_HANDLE, XR_NULL_HANDLE, XR_NULL_HANDLE};
	XrPath _handPaths[2] = {XR_NULL_PATH, XR_NULL_PATH};
	XrSpace _aimSpaces[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
	XrTime _lastTime = 0;
	double _now = 0;

	// passthrough (XR_FB_passthrough, optional)
	bool _passthroughSupported = false, _passthroughRunning = false;
	XrPassthroughFB _passthrough = XR_NULL_HANDLE;
	XrPassthroughLayerFB _passthroughLayer = XR_NULL_HANDLE;
	PFN_xrCreatePassthroughFB _createPassthrough = nullptr;
	PFN_xrDestroyPassthroughFB _destroyPassthrough = nullptr;
	PFN_xrPassthroughStartFB _startPassthrough = nullptr;
	PFN_xrPassthroughPauseFB _pausePassthrough = nullptr;
	PFN_xrCreatePassthroughLayerFB _createPassthroughLayer = nullptr;
	PFN_xrDestroyPassthroughLayerFB _destroyPassthroughLayer = nullptr;
	PFN_xrPassthroughLayerResumeFB _resumePassthroughLayer = nullptr;
	PFN_xrPassthroughLayerPauseFB _pausePassthroughLayer = nullptr;
	PFN_xrPassthroughLayerSetStyleFB _setPassthroughStyle = nullptr;

	// canvas
	bool _placed = false;
	Vec3 _anchor{0, 0, 0};
	float _yaw = 0;
	int _texW = 0, _texH = 0;
	Pose _panelPose, _keyboardPose;

	// statistics
	double _statStart = 0;
	int _statFrames = 0;
	uint64_t _statGameSeq = 0;

	// input
	Keyboard _keyboard;
	Canvas _keyboardCanvas{Keyboard::Width, Keyboard::Height};
	bool _keyboardVisible = false;
	int _pointer = 1;
	HandInput _hands[2];
	bool _triggerPressed[2] = {false, false}, _gripPressed[2] = {false, false};
	bool _triggerDown = false, _triggerArmed = true, _gripArmed = true, _rightHeld = false;
	SurfaceKind _pressSurface = SURFACE_NONE;
	int _pressKey = -1;
	double _freezeUntil = 0, _wheelNext = 0, _unfocusedSince = -1;
	float _smoothU = 0, _smoothV = 0;
	bool _smoothValid = false;
	int _mouseX = -1, _mouseY = -1;
	bool _arrowHeld[ARROW_COUNT] = {false, false, false, false};
	bool _buttonHeld[BUTTON_COUNT] = {false, false, false, false, false};
	bool _buttonArmed[BUTTON_COUNT] = {true, true, true, true, true};
	bool _resetArmed = true;

	// laser (filled by input, drawn per eye)
	bool _rayValid = false, _rayHit = false;
	Vec3 _rayStart{0, 0, 0}, _rayEnd{0, 0, 0};
	float _rayHitDistance = 0;

	void threadMain();
	void signalStart(int state);
	bool init();
	bool initGraphics();
	bool ensureFrameTexture();
	bool initActions();
	void initPassthrough();
	void applyPassthrough();
	bool createSwapchain(Swapchain &sc, int width, int height);
	uint32_t acquire(Swapchain &sc);
	void release(Swapchain &sc);
	void cleanup();
	void pollEvents();
	void frame();
	void applySettings();
	void changeSettings(const VrSettings &changed);
	bool locate(XrSpace space, XrTime time, Pose &pose) const;
	void place(const Pose &head);
	void updatePoses();
	float panelWidth() const { return _cfg.width[_cfg.view]; }
	float panelHeight() const { return panelWidth() * (_texW > 0 ? static_cast<float>(_texH) / _texW : 0.625f); }
	void uploadPanel();
	void uploadKeyboard();
	bool renderLaser(XrTime time, XrCompositionLayerProjectionView views[2]);
	void processInput(XrTime time, float dt, const Pose &head, bool headValid);
	void readHands(XrTime time);
	bool buttonState(FaceButton button);
	void moveMouse(float u, float v);
	void mouseButton(Uint8 button, bool pressed);
	void setKey(SDL_Scancode code, bool pressed, bool &held);
	void typeKey(int key);
	void haptic(int hand);
	void releaseAll();
	void updateStatistics();
	void loadSettings();
	void saveSettings();
};

Runtime g_runtime;

void sendTap(SDL_Scancode code)
{
	SDL_SendKeyboardKey(SDL_PRESSED, code);
	SDL_SendKeyboardKey(SDL_RELEASED, code);
}

bool Runtime::start(const std::string &configFolder)
{
	JNIEnv *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
	jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
	if (!env || !activity || env->GetJavaVM(&_vm) != JNI_OK)
	{
		XLOGE("No JNI context, staying in 2D mode");
		return false;
	}
	_activity = env->NewGlobalRef(activity);
	env->DeleteLocalRef(activity);
	_settingsPath = configFolder + "xr.cfg";
	// One start only: the marker is removed right away, so a leftover file (script aborted,
	// USB pulled) can never drive a normal game session.
	const std::string selfTestMarker = configFolder + "xr-selftest.txt";
	_selfTest = std::ifstream(selfTestMarker).good();
	if (_selfTest)
		std::remove(selfTestMarker.c_str());
	if (_selfTest)
		XLOGI("SELFTEST enabled (%sxr-selftest.txt)", configFolder.c_str());
	loadSettings();

	_quit = false;
	_startState = 0;
	_thread = std::thread(&Runtime::threadMain, this);
	{
		std::unique_lock<std::mutex> lock(_startMutex);
		_startCv.wait(lock, [this] { return _startState != 0; });
	}
	if (_startState < 0)
	{
		_thread.join();
		env->DeleteGlobalRef(_activity);
		_activity = nullptr;
		XLOGI("OpenXR not available, staying in 2D mode");
		return false;
	}
	_active = true;
	XLOGI("Immersive mode active");
	return true;
}

void Runtime::stop()
{
	if (!_thread.joinable())
		return;
	_quit = true;
	_thread.join();
	_active = false;
	if (_activity)
	{
		if (JNIEnv *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv()))
			env->DeleteGlobalRef(_activity);
		_activity = nullptr;
	}
	saveSettings();
}

void Runtime::setOutputRect(int x, int y, int w, int h)
{
	_outX = x;
	_outY = y;
	_outW = std::max(1, w);
	_outH = std::max(1, h);
}

void Runtime::submitFrame(const void *pixels, int width, int height, int pitch, Uint32 format)
{
	if (!_active || width <= 0 || height <= 0)
		return;
	std::lock_guard<std::mutex> lock(_frameMutex);
	_frame.resize(static_cast<size_t>(width) * height * 4);
	if (SDL_ConvertPixels(width, height, format, pixels, pitch, SDL_PIXELFORMAT_RGBA32, _frame.data(), width * 4) != 0)
		return;
	_frameW = width;
	_frameH = height;
	++_frameSeq;
	++_gameFrames;
}

VrSettings Runtime::settings()
{
	std::lock_guard<std::mutex> lock(_settingsMutex);
	return _settingsShared;
}

void Runtime::setSettings(const VrSettings &settings)
{
	std::lock_guard<std::mutex> lock(_settingsMutex);
	_settingsShared = settings;
	_settingsShared.clamp();
	++_settingsVersion;
}

VrInfo Runtime::info() const
{
	VrInfo info;
	info.passthroughAvailable = _passthroughSupported;
	info.gameWidth = _texWShared;
	info.gameHeight = _texHShared;
	info.gameFps = _gameFps;
	info.xrHz = _xrHz;
	return info;
}

void Runtime::signalStart(int state)
{
	std::lock_guard<std::mutex> lock(_startMutex);
	_startState = state;
	_startCv.notify_all();
}

void Runtime::threadMain()
{
	if (!init())
	{
		cleanup();
		signalStart(-1);
		return;
	}
	signalStart(1);
	while (!_quit)
	{
		pollEvents();
		if (!_running)
		{
			if (_focused || _wasFocused)
				releaseAll();
			_focused = false;
			_wasFocused = false;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}
		frame();
	}
	releaseAll();
	cleanup();
	if (_exitRequested)
	{
		// The runtime closed the app (Quest menu "Quit"): let the game save and exit.
		SDL_Event quit;
		SDL_zero(quit);
		quit.type = SDL_QUIT;
		SDL_PushEvent(&quit);
	}
}

bool Runtime::init()
{
	// 1) Loader: the Khronos loader needs VM and activity on Android.
	PFN_xrInitializeLoaderKHR initLoader = nullptr;
	if (XR_FAILED(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", reinterpret_cast<PFN_xrVoidFunction *>(&initLoader))) || !initLoader)
		return false;
	XrLoaderInitInfoAndroidKHR loaderInfo{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
	loaderInfo.applicationVM = _vm;
	loaderInfo.applicationContext = _activity;
	if (XR_FAILED(initLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR *>(&loaderInfo))))
	{
		XLOGE("xrInitializeLoaderKHR failed");
		return false;
	}

	// 2) Instance and system; passthrough only if the runtime offers it.
	uint32_t extensionCount = 0;
	xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr);
	std::vector<XrExtensionProperties> available(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
	xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, available.data());
	auto offered = [&available](const char *name) {
		return std::any_of(available.begin(), available.end(), [name](const XrExtensionProperties &e) { return std::strcmp(e.extensionName, name) == 0; });
	};
	_passthroughSupported = offered(XR_FB_PASSTHROUGH_EXTENSION_NAME);
	_layerSettingsSupported = offered(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
	std::vector<const char *> extensions{XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME};
	if (_passthroughSupported)
		extensions.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
	if (_layerSettingsSupported)
		extensions.push_back(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME); // compositor sharpening for the canvas

	XrInstanceCreateInfo instanceInfo{XR_TYPE_INSTANCE_CREATE_INFO};
	std::strcpy(instanceInfo.applicationInfo.applicationName, "xr.openxcom");
	instanceInfo.applicationInfo.applicationVersion = 1;
	std::strcpy(instanceInfo.applicationInfo.engineName, "OpenXcom Extended");
	instanceInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	instanceInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
	instanceInfo.enabledExtensionNames = extensions.data();
	XrResult result = xrCreateInstance(&instanceInfo, &_instance);
	if (XR_FAILED(result))
	{
		XLOGE("xrCreateInstance failed (%d)", static_cast<int>(result));
		return false;
	}
	XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if (XR_FAILED(xrGetSystem(_instance, &systemInfo, &_system)))
	{
		XLOGE("No head-mounted display");
		return false;
	}
	if (_passthroughSupported)
	{
		XrSystemPassthroughPropertiesFB passthroughProperties{XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES_FB};
		XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
		properties.next = &passthroughProperties;
		_passthroughSupported = XR_SUCCEEDED(xrGetSystemProperties(_instance, _system, &properties)) && passthroughProperties.supportsPassthrough;
	}

	// 3) EGL context of our own (the game thread has none in immersive mode).
	if (!initGraphics())
		return false;

	PFN_xrGetOpenGLESGraphicsRequirementsKHR getRequirements = nullptr;
	xrGetInstanceProcAddr(_instance, "xrGetOpenGLESGraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction *>(&getRequirements));
	XrGraphicsRequirementsOpenGLESKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
	if (!getRequirements || XR_FAILED(getRequirements(_instance, _system, &requirements)))
		return false;

	XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
	binding.display = _display;
	binding.config = _config;
	binding.context = _context_;
	XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
	sessionInfo.next = &binding;
	sessionInfo.systemId = _system;
	if (XR_FAILED(xrCreateSession(_instance, &sessionInfo, &_session)))
	{
		XLOGE("xrCreateSession failed");
		return false;
	}

	XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	if (XR_FAILED(xrCreateReferenceSpace(_session, &spaceInfo, &_local)))
		return false;
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	if (XR_FAILED(xrCreateReferenceSpace(_session, &spaceInfo, &_view)))
		return false;

	// 4) Swapchains: sRGB if offered, so the game's colours arrive unchanged.
	uint32_t formatCount = 0;
	xrEnumerateSwapchainFormats(_session, 0, &formatCount, nullptr);
	std::vector<int64_t> formats(formatCount);
	xrEnumerateSwapchainFormats(_session, formatCount, &formatCount, formats.data());
	_format = GL_RGBA8;
	if (std::find(formats.begin(), formats.end(), static_cast<int64_t>(GL_SRGB8_ALPHA8)) != formats.end())
		_format = GL_SRGB8_ALPHA8;

	uint32_t viewCount = 0;
	xrEnumerateViewConfigurationViews(_instance, _system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
	std::vector<XrViewConfigurationView> views(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
	xrEnumerateViewConfigurationViews(_instance, _system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, views.data());
	if (viewCount < 2)
		return false;
	if (!createSwapchain(_panelSc, VirtualWidth, VirtualHeight) || !createSwapchain(_keyboardSc, Keyboard::Width, Keyboard::Height))
		return false;
	for (int eye = 0; eye < 2; ++eye)
		if (!createSwapchain(_eyeSc[eye], views[eye].recommendedImageRectWidth, views[eye].recommendedImageRectHeight))
			return false;

	if (!initActions())
		return false;
	initPassthrough();
	XLOGI("OpenXR ready: panel %dx%d, eyes %dx%d, format 0x%x, passthrough %d, sharpening %d", VirtualWidth, VirtualHeight,
	      _eyeSc[0].width, _eyeSc[0].height, static_cast<unsigned>(_format), _passthroughSupported ? 1 : 0, _layerSettingsSupported ? 1 : 0);
	return true;
}

bool Runtime::initGraphics()
{
	_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (_display == EGL_NO_DISPLAY || !eglInitialize(_display, nullptr, nullptr))
		return false;
	const EGLint configAttribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
	                                EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
	EGLint count = 0;
	if (!eglChooseConfig(_display, configAttribs, &_config, 1, &count) || count < 1)
		return false;
	const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
	_context_ = eglCreateContext(_display, _config, EGL_NO_CONTEXT, contextAttribs);
	const EGLint pbufferAttribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
	_pbuffer = eglCreatePbufferSurface(_display, _config, pbufferAttribs);
	if (_context_ == EGL_NO_CONTEXT || _pbuffer == EGL_NO_SURFACE || !eglMakeCurrent(_display, _pbuffer, _pbuffer, _context_))
		return false;

	static const char *vertexSource =
		"#version 300 es\n"
		"layout(location = 0) in vec3 aPos;\n"
		"layout(location = 1) in vec4 aColor;\n"
		"uniform mat4 uMvp;\n"
		"out vec4 vColor;\n"
		"void main() { gl_Position = uMvp * vec4(aPos, 1.0); vColor = aColor; }\n";
	static const char *fragmentSource =
		"#version 300 es\n"
		"precision mediump float;\n"
		"in vec4 vColor;\n"
		"out vec4 oColor;\n"
		"void main() { oColor = vColor; }\n";
	auto compile = [](GLenum type, const char *source) {
		GLuint shader = glCreateShader(type);
		glShaderSource(shader, 1, &source, nullptr);
		glCompileShader(shader);
		GLint ok = 0;
		glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
		if (!ok)
		{
			char log[512] = "";
			glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
			XLOGE("Shader: %s", log);
		}
		return shader;
	};
	const GLuint vs = compile(GL_VERTEX_SHADER, vertexSource), fs = compile(GL_FRAGMENT_SHADER, fragmentSource);
	_program = glCreateProgram();
	glAttachShader(_program, vs);
	glAttachShader(_program, fs);
	glLinkProgram(_program);
	glDeleteShader(vs);
	glDeleteShader(fs);
	GLint linked = 0;
	glGetProgramiv(_program, GL_LINK_STATUS, &linked);
	if (!linked)
		return false;
	_mvpLocation = glGetUniformLocation(_program, "uMvp");
	glGenFramebuffers(1, &_fbo);

	// Upscale pass game frame -> canvas swapchain. Sharp-bilinear keeps the pixel art crisp
	// without the shimmer of nearest; the texel coordinate is clamped to the frame (no seams)
	// and flipped, because row 0 of a GL swapchain image is the bottom of the canvas.
	static const char *blitVertex =
		"#version 300 es\n"
		"out vec2 vUv;\n"
		"void main() {\n"
		"  vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);\n"
		"  vUv = (p + 1.0) * 0.5;\n"
		"  gl_Position = vec4(p, 0.0, 1.0);\n"
		"}\n";
	static const char *blitFragment =
		"#version 300 es\n"
		"precision highp float;\n"
		"uniform sampler2D uTex;\n"
		"uniform vec2 uFrameSize;\n"
		"uniform vec2 uTexSize;\n"
		"in vec2 vUv;\n"
		"out vec4 oColor;\n"
		"void main() {\n"
		"  vec2 texel = vec2(vUv.x, 1.0 - vUv.y) * uFrameSize;\n"
		"  vec2 seam = floor(texel + 0.5);\n"
		"  vec2 w = max(fwidth(texel), vec2(1e-4));\n"
		"  vec2 t = seam + clamp((texel - seam) / w, -0.5, 0.5);\n"
		"  oColor = vec4(texture(uTex, clamp(t, vec2(0.5), uFrameSize - 0.5) / uTexSize).rgb, 1.0);\n"
		"}\n";
	const GLuint bvs = compile(GL_VERTEX_SHADER, blitVertex), bfs = compile(GL_FRAGMENT_SHADER, blitFragment);
	_blitProgram = glCreateProgram();
	glAttachShader(_blitProgram, bvs);
	glAttachShader(_blitProgram, bfs);
	glLinkProgram(_blitProgram);
	glDeleteShader(bvs);
	glDeleteShader(bfs);
	glGetProgramiv(_blitProgram, GL_LINK_STATUS, &linked);
	if (!linked)
		return false;
	glUseProgram(_blitProgram);
	glUniform1i(glGetUniformLocation(_blitProgram, "uTex"), 0);
	_blitFrameSize = glGetUniformLocation(_blitProgram, "uFrameSize");
	_blitTexSize = glGetUniformLocation(_blitProgram, "uTexSize");
	glUseProgram(0);

	return glGetError() == GL_NO_ERROR;
}

bool Runtime::ensureFrameTexture()
{
	if (_frameTex)
		return true;
	// As big as the virtual display (largest possible game surface). Same colour encoding as the
	// swapchain: sRGB in -> sRGB out keeps the game's values exact at texel centres and filters in
	// linear light; with an RGBA8 swapchain an sRGB source would come out too dark.
	glGenTextures(1, &_frameTex);
	glBindTexture(GL_TEXTURE_2D, _frameTex);
	glTexStorage2D(GL_TEXTURE_2D, 1, _format == GL_SRGB8_ALPHA8 ? GL_SRGB8_ALPHA8 : GL_RGBA8, VirtualWidth, VirtualHeight);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);
	if (glGetError() != GL_NO_ERROR)
	{
		XLOGE("Frame texture could not be created");
		return false;
	}
	return true;
}

bool Runtime::initActions()
{
	xrStringToPath(_instance, "/user/hand/left", &_handPaths[0]);
	xrStringToPath(_instance, "/user/hand/right", &_handPaths[1]);

	XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
	std::strcpy(setInfo.actionSetName, "oxce");
	std::strcpy(setInfo.localizedActionSetName, "OXCE");
	if (XR_FAILED(xrCreateActionSet(_instance, &setInfo, &_actionSet)))
		return false;

	auto create = [this](XrAction &action, const char *name, const char *localized, XrActionType type, bool perHand) {
		XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
		info.actionType = type;
		std::strcpy(info.actionName, name);
		std::strcpy(info.localizedActionName, localized);
		if (perHand)
		{
			info.countSubactionPaths = 2;
			info.subactionPaths = _handPaths;
		}
		return XR_SUCCEEDED(xrCreateAction(_actionSet, &info, &action));
	};
	bool ok = create(_aimAction, "aim", "Zeigen", XR_ACTION_TYPE_POSE_INPUT, true)
		&& create(_triggerAction, "trigger", "Linksklick", XR_ACTION_TYPE_FLOAT_INPUT, true)
		&& create(_squeezeAction, "squeeze", "Rechtsklick", XR_ACTION_TYPE_FLOAT_INPUT, true)
		&& create(_stickAction, "stick", "Scrollen", XR_ACTION_TYPE_VECTOR2F_INPUT, true)
		&& create(_stickClickAction, "stick_click", "Leinwand zuruecksetzen", XR_ACTION_TYPE_BOOLEAN_INPUT, true)
		&& create(_hapticAction, "haptic", "Klick-Rueckmeldung", XR_ACTION_TYPE_VIBRATION_OUTPUT, true)
		&& create(_buttonActions[BUTTON_A], "button_a", "Enter", XR_ACTION_TYPE_BOOLEAN_INPUT, false)
		&& create(_buttonActions[BUTTON_B], "button_b", "Escape", XR_ACTION_TYPE_BOOLEAN_INPUT, false)
		&& create(_buttonActions[BUTTON_X], "button_x", "Tab", XR_ACTION_TYPE_BOOLEAN_INPUT, false)
		&& create(_buttonActions[BUTTON_Y], "button_y", "Leinwand zentrieren", XR_ACTION_TYPE_BOOLEAN_INPUT, false)
		&& create(_buttonActions[BUTTON_MENU], "vr_menu", "VR-Menue", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	if (!ok)
		return false;

	std::vector<XrActionSuggestedBinding> bindings;
	auto bind = [this, &bindings](XrAction action, const char *path) {
		XrPath p = XR_NULL_PATH;
		if (XR_SUCCEEDED(xrStringToPath(_instance, path, &p)))
			bindings.push_back({action, p});
	};
	for (const char *hand : {"left", "right"})
	{
		const std::string base = std::string("/user/hand/") + hand;
		bind(_aimAction, (base + "/input/aim/pose").c_str());
		bind(_triggerAction, (base + "/input/trigger/value").c_str());
		bind(_squeezeAction, (base + "/input/squeeze/value").c_str());
		bind(_stickAction, (base + "/input/thumbstick").c_str());
		bind(_stickClickAction, (base + "/input/thumbstick/click").c_str());
		bind(_hapticAction, (base + "/output/haptic").c_str());
	}
	bind(_buttonActions[BUTTON_A], "/user/hand/right/input/a/click");
	bind(_buttonActions[BUTTON_B], "/user/hand/right/input/b/click");
	bind(_buttonActions[BUTTON_X], "/user/hand/left/input/x/click");
	bind(_buttonActions[BUTTON_Y], "/user/hand/left/input/y/click");
	bind(_buttonActions[BUTTON_MENU], "/user/hand/left/input/menu/click");
	XrPath profile = XR_NULL_PATH;
	xrStringToPath(_instance, "/interaction_profiles/oculus/touch_controller", &profile);
	XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
	suggested.interactionProfile = profile;
	suggested.suggestedBindings = bindings.data();
	suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
	if (XR_FAILED(xrSuggestInteractionProfileBindings(_instance, &suggested)))
		return false;

	// Generic fallback: pointing, clicking and the menu.
	bindings.clear();
	for (const char *hand : {"left", "right"})
	{
		const std::string base = std::string("/user/hand/") + hand;
		bind(_aimAction, (base + "/input/aim/pose").c_str());
		bind(_triggerAction, (base + "/input/select/click").c_str());
		bind(_hapticAction, (base + "/output/haptic").c_str());
	}
	bind(_buttonActions[BUTTON_MENU], "/user/hand/left/input/menu/click");
	xrStringToPath(_instance, "/interaction_profiles/khr/simple_controller", &profile);
	suggested.interactionProfile = profile;
	suggested.suggestedBindings = bindings.data();
	suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
	xrSuggestInteractionProfileBindings(_instance, &suggested);

	for (int hand = 0; hand < 2; ++hand)
	{
		XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
		spaceInfo.action = _aimAction;
		spaceInfo.subactionPath = _handPaths[hand];
		spaceInfo.poseInActionSpace.orientation.w = 1.0f;
		if (XR_FAILED(xrCreateActionSpace(_session, &spaceInfo, &_aimSpaces[hand])))
			return false;
	}
	XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	attach.countActionSets = 1;
	attach.actionSets = &_actionSet;
	return XR_SUCCEEDED(xrAttachSessionActionSets(_session, &attach));
}

void Runtime::initPassthrough()
{
	if (!_passthroughSupported)
		return;
	auto load = [this](const char *name, auto &fn) {
		PFN_xrVoidFunction pointer = nullptr;
		if (XR_FAILED(xrGetInstanceProcAddr(_instance, name, &pointer)))
			return false;
		fn = reinterpret_cast<std::decay_t<decltype(fn)>>(pointer);
		return fn != nullptr;
	};
	_passthroughSupported = load("xrCreatePassthroughFB", _createPassthrough) && load("xrDestroyPassthroughFB", _destroyPassthrough)
		&& load("xrPassthroughStartFB", _startPassthrough) && load("xrPassthroughPauseFB", _pausePassthrough)
		&& load("xrCreatePassthroughLayerFB", _createPassthroughLayer) && load("xrDestroyPassthroughLayerFB", _destroyPassthroughLayer)
		&& load("xrPassthroughLayerResumeFB", _resumePassthroughLayer) && load("xrPassthroughLayerPauseFB", _pausePassthroughLayer)
		&& load("xrPassthroughLayerSetStyleFB", _setPassthroughStyle);
	if (!_passthroughSupported)
		return;
	XrPassthroughCreateInfoFB createInfo{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
	XrPassthroughLayerCreateInfoFB layerInfo{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
	if (XR_FAILED(_createPassthrough(_session, &createInfo, &_passthrough)))
	{
		_passthroughSupported = false;
		return;
	}
	layerInfo.passthrough = _passthrough;
	layerInfo.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
	if (XR_FAILED(_createPassthroughLayer(_session, &layerInfo, &_passthroughLayer)))
	{
		_destroyPassthrough(_passthrough);
		_passthrough = XR_NULL_HANDLE;
		_passthroughSupported = false;
	}
}

void Runtime::applyPassthrough()
{
	if (!_passthroughSupported)
		return;
	if (_cfg.passthrough > 0)
	{
		if (!_passthroughRunning)
		{
			_passthroughRunning = XR_SUCCEEDED(_startPassthrough(_passthrough)) && XR_SUCCEEDED(_resumePassthroughLayer(_passthroughLayer));
		}
		XrPassthroughStyleFB style{XR_TYPE_PASSTHROUGH_STYLE_FB};
		style.textureOpacityFactor = _cfg.passthrough / 100.0f;
		style.edgeColor = {0, 0, 0, 0};
		_setPassthroughStyle(_passthroughLayer, &style);
	}
	else if (_passthroughRunning)
	{
		_pausePassthroughLayer(_passthroughLayer);
		_pausePassthrough(_passthrough);
		_passthroughRunning = false;
	}
}

bool Runtime::createSwapchain(Swapchain &sc, int width, int height)
{
	XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
	info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	info.format = _format;
	info.sampleCount = 1;
	info.width = width;
	info.height = height;
	info.faceCount = 1;
	info.arraySize = 1;
	info.mipCount = 1;
	if (XR_FAILED(xrCreateSwapchain(_session, &info, &sc.handle)))
	{
		XLOGE("xrCreateSwapchain %dx%d failed", width, height);
		return false;
	}
	sc.width = width;
	sc.height = height;
	uint32_t count = 0;
	xrEnumerateSwapchainImages(sc.handle, 0, &count, nullptr);
	sc.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
	return XR_SUCCEEDED(xrEnumerateSwapchainImages(sc.handle, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader *>(sc.images.data())));
}

uint32_t Runtime::acquire(Swapchain &sc)
{
	uint32_t index = 0;
	XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	xrAcquireSwapchainImage(sc.handle, &acquireInfo, &index);
	XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	waitInfo.timeout = XR_INFINITE_DURATION;
	xrWaitSwapchainImage(sc.handle, &waitInfo);
	return index;
}

void Runtime::release(Swapchain &sc)
{
	XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	xrReleaseSwapchainImage(sc.handle, &releaseInfo);
	sc.hasContent = true;
}

void Runtime::cleanup()
{
	if (_passthroughLayer != XR_NULL_HANDLE)
	{
		_destroyPassthroughLayer(_passthroughLayer);
		_passthroughLayer = XR_NULL_HANDLE;
	}
	if (_passthrough != XR_NULL_HANDLE)
	{
		_destroyPassthrough(_passthrough);
		_passthrough = XR_NULL_HANDLE;
	}
	_passthroughRunning = false;
	for (XrSpace &space : _aimSpaces)
		if (space != XR_NULL_HANDLE)
		{
			xrDestroySpace(space);
			space = XR_NULL_HANDLE;
		}
	for (Swapchain *sc : {&_panelSc, &_keyboardSc, &_eyeSc[0], &_eyeSc[1]})
		if (sc->handle != XR_NULL_HANDLE)
		{
			xrDestroySwapchain(sc->handle);
			sc->handle = XR_NULL_HANDLE;
		}
	if (_actionSet != XR_NULL_HANDLE)
	{
		xrDestroyActionSet(_actionSet);
		_actionSet = XR_NULL_HANDLE;
	}
	for (XrSpace *space : {&_view, &_local})
		if (*space != XR_NULL_HANDLE)
		{
			xrDestroySpace(*space);
			*space = XR_NULL_HANDLE;
		}
	if (_session != XR_NULL_HANDLE)
	{
		xrDestroySession(_session);
		_session = XR_NULL_HANDLE;
	}
	if (_instance != XR_NULL_HANDLE)
	{
		xrDestroyInstance(_instance);
		_instance = XR_NULL_HANDLE;
	}
	if (_display != EGL_NO_DISPLAY)
	{
		if (_context_ != EGL_NO_CONTEXT)
		{
			if (_fbo)
				glDeleteFramebuffers(1, &_fbo);
			if (_program)
				glDeleteProgram(_program);
			if (_blitProgram)
				glDeleteProgram(_blitProgram);
			if (_frameTex)
				glDeleteTextures(1, &_frameTex);
			_fbo = _program = _blitProgram = _frameTex = 0;
		}
		eglMakeCurrent(_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (_pbuffer != EGL_NO_SURFACE)
			eglDestroySurface(_display, _pbuffer);
		if (_context_ != EGL_NO_CONTEXT)
			eglDestroyContext(_display, _context_);
		_pbuffer = EGL_NO_SURFACE;
		_context_ = EGL_NO_CONTEXT;
		eglTerminate(_display);
		_display = EGL_NO_DISPLAY;
	}
	_running = false;
}

void Runtime::pollEvents()
{
	XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
	while (xrPollEvent(_instance, &event) == XR_SUCCESS)
	{
		if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
		{
			const auto *changed = reinterpret_cast<const XrEventDataSessionStateChanged *>(&event);
			_state = changed->state;
			XLOGI("Session state %d", static_cast<int>(_state));
			switch (_state)
			{
			case XR_SESSION_STATE_READY:
			{
				XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
				begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				_running = XR_SUCCEEDED(xrBeginSession(_session, &begin));
				_placed = false; // always start centred in front of the viewer
				_appliedVersion = 0; // re-apply settings (passthrough) in the new session
				break;
			}
			case XR_SESSION_STATE_STOPPING:
				if (_passthroughRunning)
				{
					_pausePassthroughLayer(_passthroughLayer);
					_pausePassthrough(_passthrough);
					_passthroughRunning = false;
				}
				xrEndSession(_session);
				_running = false;
				break;
			case XR_SESSION_STATE_EXITING:
			case XR_SESSION_STATE_LOSS_PENDING:
				_running = false;
				_exitRequested = true;
				_quit = true;
				break;
			default:
				break;
			}
			_focused = _state == XR_SESSION_STATE_FOCUSED;
		}
		else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING)
		{
			// Long press on the Meta button re-centres LOCAL: put the canvas back in front.
			const auto *change = reinterpret_cast<const XrEventDataReferenceSpaceChangePending *>(&event);
			if (change->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL)
				_recenterRequested = true;
		}
		else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
		{
			_exitRequested = true;
			_quit = true;
		}
		event = {XR_TYPE_EVENT_DATA_BUFFER};
	}
}

bool Runtime::locate(XrSpace space, XrTime time, Pose &pose) const
{
	XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
	if (XR_FAILED(xrLocateSpace(space, _local, time, &location)))
		return false;
	const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	if ((location.locationFlags & needed) != needed)
		return false;
	pose = fromXr(location.pose);
	return true;
}

void Runtime::applySettings()
{
	const unsigned version = _settingsVersion;
	if (version == _appliedVersion)
		return;
	{
		std::lock_guard<std::mutex> lock(_settingsMutex);
		_cfg = _settingsShared;
	}
	if (_cfg.pointerHand != _pointer)
	{
		releaseAll();
		_pointer = _cfg.pointerHand;
	}
	_appliedVersion = version;
	updatePoses();
	applyPassthrough();
	if (version != _savedVersion)
		_saveAt = _now + SaveDelaySeconds; // positions are saved after a short rest
}

void Runtime::changeSettings(const VrSettings &changed)
{
	setSettings(changed);
	applySettings();
}

void Runtime::place(const Pose &head)
{
	_anchor = head.position;
	_yaw = yawOf(head.orientation); // yaw only, never pitch
	_placed = true;
	updatePoses();
}

void Runtime::updatePoses()
{
	const Quat q = yawQuat(_yaw);
	const float distance = _cfg.distance[_cfg.view];
	if (_cfg.view == VIEW_TABLE)
	{
		// Tilted towards the viewer around the picture centre, below eye height.
		const float tilt = _cfg.tableTilt * 3.14159265f / 180.0f;
		_panelPose.orientation = mul(q, pitchQuat(-tilt));
		_panelPose.position = _anchor + rotate(q, {0.0f, -_cfg.tableHeight / 100.0f, -distance});
		_keyboardPose.orientation = mul(q, pitchQuat(-0.35f));
		_keyboardPose.position = _anchor + rotate(q, {0.0f, -0.18f, -std::max(0.45f, distance * 0.6f)});
	}
	else
	{
		_panelPose.orientation = q;
		_panelPose.position = _anchor + rotate(q, {0.0f, -0.1f, -distance});
		// Keyboard: in front of and below the canvas, tilted up towards the eyes.
		const float keyboardDistance = std::min(0.75f, std::max(0.45f, distance * 0.45f));
		_keyboardPose.orientation = mul(q, pitchQuat(-0.6f));
		_keyboardPose.position = _anchor + rotate(q, {0.0f, -0.4f, -keyboardDistance});
	}
}

void Runtime::updateStatistics()
{
	++_statFrames;
	if (_now - _statStart < 1.0)
		return;
	const uint64_t seq = _gameFrames; // new and unchanged game frames
	const double span = _now - _statStart;
	_xrHz = static_cast<int>(_statFrames / span + 0.5);
	_gameFps = static_cast<int>((seq - _statGameSeq) / span + 0.5);
	_statGameSeq = seq;
	// Performance line every 10 s for the session log (healthy: xrWaitFrame ~13 ms at 72 Hz).
	if (++_perfSeconds >= 10)
	{
		XLOGI("PERF game %d fps, XR %d Hz, xrWaitFrame %.1f ms, frame %dx%d -> %dx%d", _gameFps.load(), _xrHz.load(),
		      _waitMsSum / std::max(1, _statFrames), _texW, _texH, _panelRectW, _panelRectH);
		_perfSeconds = 0;
	}
	_waitMsSum = 0;
	_statFrames = 0;
	_statStart = _now;
}

void Runtime::frame()
{
	XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
	XrFrameState frameState{XR_TYPE_FRAME_STATE};
	const auto waitStart = std::chrono::steady_clock::now();
	const XrResult waitResult = xrWaitFrame(_session, &waitInfo, &frameState);
	_waitMsSum += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count();
	if (XR_FAILED(waitResult))
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		return;
	}
	XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
	xrBeginFrame(_session, &beginInfo);

	const XrTime time = frameState.predictedDisplayTime;
	float dt = _lastTime ? static_cast<float>((time - _lastTime) * 1e-9) : 0.0f;
	dt = std::max(0.0f, std::min(dt, 0.1f));
	_lastTime = time;
	_now += dt;
	updateStatistics();
	applySettings();

	Pose head;
	const bool headValid = locate(_view, time, head);
	if (headValid && (!_placed || _recenterRequested))
	{
		// As soon as the head pose is valid (not only once focused), and after a recenter.
		_recenterRequested = false;
		place(head);
	}

	const bool keyboardVisible = SDL_GetEventState(SDL_TEXTINPUT) == SDL_ENABLE;
	if (keyboardVisible != _keyboardVisible)
	{
		_keyboardVisible = keyboardVisible;
		if (_pressSurface == SURFACE_KEYBOARD)
		{
			_pressSurface = SURFACE_NONE;
			_triggerArmed = false;
		}
		_keyboard.reset();
		_keyboardVisibleShared = keyboardVisible;
		XLOGI("Keyboard %s", keyboardVisible ? "shown" : "hidden");
	}
	if (_selfTest)
		processDebugInput();

	if (_focused)
	{
		// Back from a longer break (headset off, Quest menu) in a running game: ask before continuing.
		if (!_wasFocused && _unfocusedSince >= 0 && _now - _unfocusedSince >= ResumePromptSeconds && _context != CONTEXT_MENU)
			_menuRequest = MENU_REQUEST_RESUME;
		_unfocusedSince = -1;
		processInput(time, dt, head, headValid);
		if (_doublePulseRequest.exchange(false))
		{
			haptic(_pointer);
			_secondPulseAt = _now + 0.12;
		}
		if (_secondPulseAt >= 0 && _now >= _secondPulseAt)
		{
			haptic(_pointer);
			_secondPulseAt = -1;
		}
	}
	else
	{
		if (_wasFocused)
			releaseAll();
		if (_unfocusedSince < 0)
			_unfocusedSince = _now;
		_rayValid = false;
	}
	_wasFocused = _focused;

	if ((_savedVersion != _appliedVersion && _saveAt > 0 && _now >= _saveAt) || _saveRequest.exchange(false))
		saveSettings();

	std::vector<const XrCompositionLayerBaseHeader *> layers;
	XrCompositionLayerPassthroughFB passthroughLayer{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
	XrCompositionLayerQuad panelLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
	XrCompositionLayerSettingsFB sharpening{XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
	sharpening.layerFlags = XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB;
	XrCompositionLayerQuad keyboardLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
	XrCompositionLayerProjection laserLayer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	XrCompositionLayerProjectionView laserViews[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
	if (frameState.shouldRender && _placed)
	{
		if (_passthroughRunning)
		{
			passthroughLayer.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			passthroughLayer.space = XR_NULL_HANDLE;
			passthroughLayer.layerHandle = _passthroughLayer;
			layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&passthroughLayer));
		}
		uploadPanel();
		if (_panelSc.hasContent && _texW > 0)
		{
			panelLayer.space = _local;
			panelLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			panelLayer.subImage.swapchain = _panelSc.handle;
			panelLayer.subImage.imageRect = {{0, 0}, {_panelRectW, _panelRectH}};
			panelLayer.pose = toXr(_panelPose);
			panelLayer.size = {panelWidth(), panelHeight()};
			if (_layerSettingsSupported)
				panelLayer.next = &sharpening; // small pixel text: compositor sharpening
			layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&panelLayer));
		}
		if (_keyboardVisible)
		{
			uploadKeyboard();
			keyboardLayer.space = _local;
			keyboardLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			keyboardLayer.subImage.swapchain = _keyboardSc.handle;
			keyboardLayer.subImage.imageRect = {{0, 0}, {Keyboard::Width, Keyboard::Height}};
			keyboardLayer.pose = toXr(_keyboardPose);
			keyboardLayer.size = {KeyboardWidth, KeyboardWidth * Keyboard::Height / Keyboard::Width};
			layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&keyboardLayer));
		}
		if (renderLaser(time, laserViews))
		{
			laserLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			laserLayer.space = _local;
			laserLayer.viewCount = 2;
			laserLayer.views = laserViews;
			layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&laserLayer));
		}
	}

	XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
	endInfo.displayTime = time;
	endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	endInfo.layerCount = static_cast<uint32_t>(layers.size());
	endInfo.layers = layers.data();
	xrEndFrame(_session, &endInfo);
}

void Runtime::uploadPanel()
{
	int width, height;
	{
		std::lock_guard<std::mutex> lock(_frameMutex);
		if (_frameSeq == _uploadedSeq || _frameW <= 0)
			return;
		_uploadedSeq = _frameSeq;
		width = std::min(_frameW, VirtualWidth); // source texture size
		height = std::min(_frameH, VirtualHeight);
		_upload.resize(static_cast<size_t>(width) * height * 4);
		for (int y = 0; y < height; ++y)
			std::memcpy(&_upload[static_cast<size_t>(y) * width * 4], &_frame[static_cast<size_t>(y) * _frameW * 4], static_cast<size_t>(width) * 4);
	}
	// 1) Frame into the source texture (top row first, as the game draws it).
	if (!ensureFrameTexture())
		return;
	glBindTexture(GL_TEXTURE_2D, _frameTex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, _upload.data());
	glBindTexture(GL_TEXTURE_2D, 0);

	// 2) Upscale into the canvas swapchain: as large as fits, aspect kept (640x400 -> 1280x800).
	int rectW, rectH;
	fitRect(width, height, _panelSc.width, _panelSc.height, rectW, rectH);
	const uint32_t index = acquire(_panelSc);
	glBindFramebuffer(GL_FRAMEBUFFER, _fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _panelSc.images[index].image, 0);
	glViewport(0, 0, rectW, rectH);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glUseProgram(_blitProgram);
	glUniform2f(_blitFrameSize, static_cast<float>(width), static_cast<float>(height));
	glUniform2f(_blitTexSize, static_cast<float>(VirtualWidth), static_cast<float>(VirtualHeight));
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, _frameTex);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindTexture(GL_TEXTURE_2D, 0);
	glUseProgram(0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	release(_panelSc);
	if (width != _texW || height != _texH)
		XLOGI("Game surface %dx%d, canvas %dx%d", width, height, rectW, rectH);
	_texW = width;
	_texH = height;
	_panelRectW = rectW;
	_panelRectH = rectH;
	_texWShared = width;
	_texHShared = height;
}

void Runtime::uploadKeyboard()
{
	if (!_keyboard.render(_keyboardCanvas) && _keyboardSc.hasContent)
		return;
	const uint32_t index = acquire(_keyboardSc);
	glBindTexture(GL_TEXTURE_2D, _keyboardSc.images[index].image);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	// Canvas rows run top-down, a GL swapchain image bottom-up: upload the rows in reverse.
	for (int y = 0; y < Keyboard::Height; ++y)
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, Keyboard::Height - 1 - y, Keyboard::Width, 1, GL_RGBA, GL_UNSIGNED_BYTE,
			_keyboardCanvas.data() + static_cast<size_t>(y) * Keyboard::Width * 4);
	glBindTexture(GL_TEXTURE_2D, 0);
	release(_keyboardSc);
}

bool Runtime::renderLaser(XrTime time, XrCompositionLayerProjectionView layerViews[2])
{
	// Pointer switches of the VR options; the VR menu and keyboard always keep ray and dot.
	const bool overlay = _menuOpenShared || _keyboardVisible;
	const bool drawRay = showRay(_cfg.showRay, overlay), drawMarker = showMarker(_cfg.showMarker, overlay);
	if (!_rayValid || (!drawRay && !(drawMarker && _rayHit)))
		return false;
	XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
	locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locateInfo.displayTime = time;
	locateInfo.space = _local;
	XrViewState viewState{XR_TYPE_VIEW_STATE};
	XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
	uint32_t count = 0;
	if (XR_FAILED(xrLocateViews(_session, &locateInfo, &viewState, 2, &count, views)) || count < 2)
		return false;
	const XrViewStateFlags needed = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
	if ((viewState.viewStateFlags & needed) != needed)
		return false;

	const Vec3 direction = normalize(_rayEnd - _rayStart);
	std::vector<float> vertices;
	auto vertex = [&vertices](const Vec3 &p, float r, float g, float b, float a) {
		vertices.insert(vertices.end(), {p.x, p.y, p.z, r * a, g * a, b * a, a}); // premultiplied
	};

	glUseProgram(_program);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	for (int eye = 0; eye < 2; ++eye)
	{
		const Pose eyePose = fromXr(views[eye].pose);
		vertices.clear();
		// Ribbon along the ray, turned towards this eye.
		const Vec3 start = _rayStart + direction * 0.03f;
		const Vec3 middle = (start + _rayEnd) * 0.5f;
		const Vec3 side = normalize(cross(direction, eyePose.position - middle)) * 0.0015f;
		const float endAlpha = _rayHit ? 0.6f : 0.0f;
		if (drawRay)
		{
			vertex(start - side, 0.85f, 0.92f, 1.0f, 0.8f);
			vertex(start + side, 0.85f, 0.92f, 1.0f, 0.8f);
			vertex(_rayEnd + side, 0.85f, 0.92f, 1.0f, endAlpha);
			vertex(start - side, 0.85f, 0.92f, 1.0f, 0.8f);
			vertex(_rayEnd + side, 0.85f, 0.92f, 1.0f, endAlpha);
			vertex(_rayEnd - side, 0.85f, 0.92f, 1.0f, endAlpha);
		}
		if (_rayHit && drawMarker)
		{
			// Target dot facing the eye.
			const Vec3 right = rotate(eyePose.orientation, {1, 0, 0}), up = rotate(eyePose.orientation, {0, 1, 0});
			const float radius = 0.004f + 0.003f * _rayHitDistance;
			const Vec3 centre = _rayEnd - direction * 0.002f;
			const int segments = 16;
			for (int i = 0; i < segments; ++i)
			{
				const float a0 = 6.2831853f * i / segments, a1 = 6.2831853f * (i + 1) / segments;
				vertex(centre, 0.35f, 0.8f, 1.0f, 1.0f);
				vertex(centre + right * (std::cos(a0) * radius) + up * (std::sin(a0) * radius), 0.35f, 0.8f, 1.0f, 1.0f);
				vertex(centre + right * (std::cos(a1) * radius) + up * (std::sin(a1) * radius), 0.35f, 0.8f, 1.0f, 1.0f);
			}
		}

		Swapchain &sc = _eyeSc[eye];
		const uint32_t index = acquire(sc);
		glBindFramebuffer(GL_FRAMEBUFFER, _fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sc.images[index].image, 0);
		glViewport(0, 0, sc.width, sc.height);
		glClearColor(0, 0, 0, 0);
		glClear(GL_COLOR_BUFFER_BIT);
		const XrFovf &fov = views[eye].fov;
		const Mat4 mvp = multiply(projectionFromFov(fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown, 0.05f, 50.0f), viewFromPose(eyePose));
		glUniformMatrix4fv(_mvpLocation, 1, GL_FALSE, mvp.m);
		glEnableVertexAttribArray(0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), vertices.data());
		glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 7 * sizeof(float), vertices.data() + 3);
		glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size() / 7));
		glDisableVertexAttribArray(0);
		glDisableVertexAttribArray(1);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		release(sc);

		layerViews[eye].pose = views[eye].pose;
		layerViews[eye].fov = views[eye].fov;
		layerViews[eye].subImage.swapchain = sc.handle;
		layerViews[eye].subImage.imageRect = {{0, 0}, {sc.width, sc.height}};
	}
	return true;
}

void Runtime::readHands(XrTime time)
{
	for (int hand = 0; hand < 2; ++hand)
	{
		HandInput &h = _hands[hand];
		XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
		info.subactionPath = _handPaths[hand];
		XrActionStateFloat value{XR_TYPE_ACTION_STATE_FLOAT};
		info.action = _triggerAction;
		h.trigger = XR_SUCCEEDED(xrGetActionStateFloat(_session, &info, &value)) && value.isActive ? value.currentState : 0.0f;
		info.action = _squeezeAction;
		value = {XR_TYPE_ACTION_STATE_FLOAT};
		h.squeeze = XR_SUCCEEDED(xrGetActionStateFloat(_session, &info, &value)) && value.isActive ? value.currentState : 0.0f;
		XrActionStateVector2f stick{XR_TYPE_ACTION_STATE_VECTOR2F};
		info.action = _stickAction;
		h.stick = XR_SUCCEEDED(xrGetActionStateVector2f(_session, &info, &stick)) && stick.isActive ? stick.currentState : XrVector2f{0, 0};
		XrActionStateBoolean click{XR_TYPE_ACTION_STATE_BOOLEAN};
		info.action = _stickClickAction;
		h.stickClick = XR_SUCCEEDED(xrGetActionStateBoolean(_session, &info, &click)) && click.isActive && click.currentState;
		h.aimValid = locate(_aimSpaces[hand], time, h.aim);
	}
}

bool Runtime::buttonState(FaceButton button)
{
	XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
	info.action = _buttonActions[button];
	XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
	return XR_SUCCEEDED(xrGetActionStateBoolean(_session, &info, &state)) && state.isActive && state.currentState;
}

void Runtime::processInput(XrTime time, float dt, const Pose &head, bool headValid)
{
	XrActiveActionSet activeSet{_actionSet, XR_NULL_PATH};
	XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
	syncInfo.countActiveActionSets = 1;
	syncInfo.activeActionSets = &activeSet;
	if (XR_FAILED(xrSyncActions(_session, &syncInfo)))
		return;
	readHands(time);

	// Trigger edges per hand, with hysteresis.
	bool triggerEdge[2];
	for (int hand = 0; hand < 2; ++hand)
	{
		const bool pressed = hysteresis(_hands[hand].trigger, _triggerPressed[hand]);
		triggerEdge[hand] = pressed && !_triggerPressed[hand];
		_triggerPressed[hand] = pressed;
	}
	// Pressing the other trigger makes that hand the pointer (release and re-arm).
	if (triggerEdge[1 - _pointer] && !_triggerDown)
	{
		VrSettings changed = _cfg;
		changed.pointerHand = 1 - _pointer;
		changeSettings(changed); // releases held input
		XLOGI("Pointer hand: %s", _pointer ? "right" : "left");
	}
	const HandInput &p = _hands[_pointer];
	const int otherHand = 1 - _pointer;
	const HandInput &o = _hands[otherHand];

	// Ray hit: keyboard in front of the canvas wins when both are hit.
	SurfaceKind surface = SURFACE_NONE;
	PanelHit hit;
	int key = -1;
	_rayValid = p.aimValid;
	if (p.aimValid)
	{
		const Vec3 origin = p.aim.position, direction = rotate(p.aim.orientation, {0, 0, -1});
		PanelHit keyboardHit, panelHit;
		const bool onKeyboard = _keyboardVisible
			&& intersectPanel(_keyboardPose, KeyboardWidth, KeyboardWidth * Keyboard::Height / Keyboard::Width, origin, direction, keyboardHit);
		const bool onPanel = _placed && _texW > 0 && intersectPanel(_panelPose, panelWidth(), panelHeight(), origin, direction, panelHit);
		if (onKeyboard && (!onPanel || keyboardHit.distance <= panelHit.distance))
		{
			surface = SURFACE_KEYBOARD;
			hit = keyboardHit;
			key = _keyboard.keyAt(hit.u, hit.v);
		}
		else if (onPanel)
		{
			surface = SURFACE_PANEL;
			hit = panelHit;
		}
		_rayStart = origin;
		_rayHit = surface != SURFACE_NONE;
		_rayEnd = _rayHit ? hit.point : origin + direction * 1.5f;
		_rayHitDistance = _rayHit ? hit.distance : 0.0f;
	}
	_keyboard.setHover(surface == SURFACE_KEYBOARD ? key : -1);

	// Time-based pointer smoothing on the canvas.
	if (surface == SURFACE_PANEL)
	{
		const float tau = SmoothingTau[_cfg.smoothing];
		if (!_smoothValid || tau <= 0.0f)
		{
			_smoothU = hit.u;
			_smoothV = hit.v;
			_smoothValid = true;
		}
		else
		{
			const float alpha = 1.0f - std::exp(-dt / tau);
			_smoothU += (hit.u - _smoothU) * alpha;
			_smoothV += (hit.v - _smoothV) * alpha;
		}
		if (_now >= _freezeUntil)
			moveMouse(_smoothU, _smoothV);
	}
	else
		_smoothValid = false;

	// Trigger = left click; a press that started elsewhere never becomes a click.
	const bool trigger = _triggerPressed[_pointer];
	if (!trigger)
		_triggerArmed = true;
	if (trigger && !_triggerDown && _triggerArmed)
	{
		_triggerDown = true;
		_triggerArmed = false;
		if (surface == SURFACE_KEYBOARD && key >= 0)
		{
			_pressSurface = SURFACE_KEYBOARD;
			_pressKey = key;
			_keyboard.setPressed(key);
			haptic(_pointer);
		}
		else if (surface == SURFACE_PANEL)
		{
			moveMouse(hit.u, hit.v);
			_smoothU = hit.u;
			_smoothV = hit.v;
			mouseButton(SDL_BUTTON_LEFT, true);
			_pressSurface = SURFACE_PANEL;
			_freezeUntil = _now + ClickFreezeSeconds;
			haptic(_pointer);
		}
		else
			_pressSurface = SURFACE_NONE;
	}
	else if (!trigger && _triggerDown)
	{
		_triggerDown = false;
		if (_pressSurface == SURFACE_KEYBOARD)
		{
			if (key == _pressKey)
				typeKey(key);
			_keyboard.setPressed(-1);
		}
		else if (_pressSurface == SURFACE_PANEL)
			mouseButton(SDL_BUTTON_LEFT, false);
		_pressSurface = SURFACE_NONE;
	}

	// Pointer grip = right mouse button: down at once, up on release.
	const bool grip = hysteresis(p.squeeze, _gripPressed[_pointer]);
	_gripPressed[_pointer] = grip;
	if (!grip)
	{
		_gripArmed = true;
		if (_rightHeld)
		{
			mouseButton(SDL_BUTTON_RIGHT, false);
			_rightHeld = false;
		}
	}
	else if (_gripArmed)
	{
		_gripArmed = false;
		if (surface == SURFACE_PANEL)
		{
			moveMouse(hit.u, hit.v);
			mouseButton(SDL_BUTTON_RIGHT, true);
			_rightHeld = true;
			haptic(_pointer);
		}
	}

	// Pointer stick = map scrolling through the game's own arrow-key scrolling.
	const struct { ArrowKey key; float value; SDL_Scancode code; } arrows[] = {
		{ARROW_LEFT, -p.stick.x, SDL_SCANCODE_LEFT}, {ARROW_RIGHT, p.stick.x, SDL_SCANCODE_RIGHT},
		{ARROW_UP, p.stick.y, SDL_SCANCODE_UP}, {ARROW_DOWN, -p.stick.y, SDL_SCANCODE_DOWN}};
	for (const auto &arrow : arrows)
	{
		const bool pressed = _arrowHeld[arrow.key] ? arrow.value > StickRelease : arrow.value > StickPress;
		setKey(arrow.code, pressed, _arrowHeld[arrow.key]);
	}

	// Other hand: grip unlocks canvas adjustment (if enabled), otherwise stick = mouse wheel.
	const bool otherGrip = hysteresis(o.squeeze, _gripPressed[otherHand]);
	_gripPressed[otherHand] = otherGrip;
	if (!o.stickClick)
		_resetArmed = true;
	if (otherGrip && _cfg.stickAdjust)
	{
		_wheelNext = 0;
		const float wx = stickAxis(o.stick.x), wy = stickAxis(o.stick.y);
		if (wx != 0.0f || wy != 0.0f)
		{
			VrSettings changed = _cfg;
			changed.distance[changed.view] += wy * 1.0f * dt;
			changed.width[changed.view] += wx * 0.8f * dt;
			changeSettings(changed);
		}
		if (o.stickClick && _resetArmed && headValid)
		{
			_resetArmed = false;
			VrSettings changed = _cfg;
			const VrSettings defaults;
			changed.width[changed.view] = defaults.width[changed.view];
			changed.distance[changed.view] = defaults.distance[changed.view];
			changeSettings(changed);
			place(head);
			haptic(otherHand);
		}
	}
	else if (!otherGrip)
	{
		const float wheel = stickAxis(o.stick.y);
		if (wheel == 0.0f)
			_wheelNext = 0;
		else if (_now >= _wheelNext)
		{
			SDL_SendMouseWheel(nullptr, 0, 0.0f, wheel > 0 ? 1.0f : -1.0f, SDL_MOUSEWHEEL_NORMAL);
			_wheelNext = _now + 0.28 - 0.18 * std::fabs(wheel);
		}
	}

	// Face buttons mirror keys; after a release-all they must be let go first.
	const SDL_Scancode buttonKeys[BUTTON_COUNT] = {SDL_SCANCODE_RETURN, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_TAB, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN};
	for (int b = 0; b < BUTTON_COUNT; ++b)
	{
		const bool down = buttonState(static_cast<FaceButton>(b));
		if (!down)
			_buttonArmed[b] = true;
		const bool pressed = down && _buttonArmed[b];
		if (buttonKeys[b] != SDL_SCANCODE_UNKNOWN)
		{
			setKey(buttonKeys[b], pressed, _buttonHeld[b]);
			continue;
		}
		if (pressed && !_buttonHeld[b])
		{
			if (b == BUTTON_Y && headValid)
			{
				place(head);
				haptic(0);
			}
			else if (b == BUTTON_MENU && !_keyboardVisible)
			{
				// The game opens or closes its VR menu dialog on the next loop.
				releaseAll();
				_buttonArmed[b] = false;
				_menuRequest = MENU_REQUEST_TOGGLE;
				haptic(0);
			}
		}
		_buttonHeld[b] = pressed;
	}
}

/**
 * Self-test input: the same moveMouse/mouseButton/text calls the controllers use.
 * A click is released one XR frame after the press, like a short trigger pull.
 */
void Runtime::processDebugInput()
{
	std::string text;
	bool pointer = false, click = false;
	float u = 0, v = 0;
	{
		// Position and click are taken together, so a press never lands at the old position.
		std::lock_guard<std::mutex> lock(_debugMutex);
		pointer = _debugPointerPending;
		_debugPointerPending = false;
		// A click waits while the previous one is still being released.
		if (!_debugReleasePending)
		{
			click = _debugClickPending;
			_debugClickPending = false;
		}
		u = _debugU;
		v = _debugV;
		text.swap(_debugText);
	}
	if (pointer)
		moveMouse(u, v);
	if (_debugReleasePending)
	{
		mouseButton(SDL_BUTTON_LEFT, false);
		_debugReleasePending = false;
	}
	else if (click)
	{
		mouseButton(SDL_BUTTON_LEFT, true);
		_debugReleasePending = true;
	}
	if (!text.empty())
		SDL_SendKeyboardText(text.c_str());
}

void Runtime::moveMouse(float u, float v)
{
	int x, y;
	virtualPoint(u, v, x, y);
	if (x == _mouseX && y == _mouseY)
		return;
	_mouseX = x;
	_mouseY = y;
	SDL_SendMouseMotion(nullptr, 0, 0, x, y);
}

void Runtime::mouseButton(Uint8 button, bool pressed)
{
	SDL_SendMouseButton(nullptr, 0, pressed ? SDL_PRESSED : SDL_RELEASED, button);
}

void Runtime::setKey(SDL_Scancode code, bool pressed, bool &held)
{
	if (pressed == held)
		return;
	held = pressed;
	SDL_SendKeyboardKey(pressed ? SDL_PRESSED : SDL_RELEASED, code);
}

void Runtime::typeKey(int key)
{
	const Keyboard::Action action = _keyboard.activate(key);
	switch (action.type)
	{
	case Keyboard::ACTION_TEXT:
		SDL_SendKeyboardText(action.text.c_str());
		break;
	case Keyboard::ACTION_BACKSPACE:
		sendTap(SDL_SCANCODE_BACKSPACE);
		break;
	case Keyboard::ACTION_ENTER:
		sendTap(SDL_SCANCODE_RETURN);
		break;
	case Keyboard::ACTION_ESCAPE:
		sendTap(SDL_SCANCODE_ESCAPE);
		break;
	default:
		break;
	}
}

void Runtime::haptic(int hand)
{
	XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
	info.action = _hapticAction;
	info.subactionPath = _handPaths[hand];
	XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
	vibration.duration = 18000000; // 18 ms
	vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
	vibration.amplitude = 0.25f;
	xrApplyHapticFeedback(_session, &info, reinterpret_cast<const XrHapticBaseHeader *>(&vibration));
}

void Runtime::releaseAll()
{
	if (_pressSurface == SURFACE_PANEL)
		mouseButton(SDL_BUTTON_LEFT, false);
	if (_rightHeld)
		mouseButton(SDL_BUTTON_RIGHT, false);
	_rightHeld = false;
	_pressSurface = SURFACE_NONE;
	_triggerDown = false;
	_triggerArmed = false;
	_gripArmed = false;
	_keyboard.setPressed(-1);
	static const SDL_Scancode arrowCodes[ARROW_COUNT] = {SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN};
	for (int i = 0; i < ARROW_COUNT; ++i)
		setKey(arrowCodes[i], false, _arrowHeld[i]);
	static const SDL_Scancode buttonCodes[BUTTON_COUNT] = {SDL_SCANCODE_RETURN, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_TAB, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN};
	for (int b = 0; b < BUTTON_COUNT; ++b)
	{
		if (buttonCodes[b] != SDL_SCANCODE_UNKNOWN)
			setKey(buttonCodes[b], false, _buttonHeld[b]);
		_buttonHeld[b] = false;
		_buttonArmed[b] = false;
	}
	_resetArmed = false;
	_wheelNext = 0;
	_smoothValid = false;
}

void Runtime::loadSettings()
{
	VrSettings s;
	std::ifstream in(_settingsPath);
	std::string line;
	_firstRunDefaults = true;
	while (std::getline(in, line))
	{
		char key[32] = "";
		float value = 0;
		if (std::sscanf(line.c_str(), "%31[^=]=%f", key, &value) != 2)
			continue;
		const std::string k = key;
		if (k == "width" || k == "canvas_width") // "width"/"distance": first version of xr.cfg
			s.width[VIEW_CANVAS] = value;
		else if (k == "distance" || k == "canvas_distance")
			s.distance[VIEW_CANVAS] = value;
		else if (k == "table_width")
			s.width[VIEW_TABLE] = value;
		else if (k == "table_distance")
			s.distance[VIEW_TABLE] = value;
		else if (k == "table_tilt")
			s.tableTilt = static_cast<int>(value);
		else if (k == "table_height")
			s.tableHeight = static_cast<int>(value);
		else if (k == "pointer_hand")
			s.pointerHand = static_cast<int>(value);
		else if (k == "smoothing")
			s.smoothing = static_cast<int>(value);
		else if (k == "view")
			s.view = static_cast<int>(value);
		else if (k == "stick_adjust")
			s.stickAdjust = value != 0.0f;
		else if (k == "passthrough")
			s.passthrough = static_cast<int>(value);
		else if (k == "show_ray")
			s.showRay = value != 0.0f;
		else if (k == "show_marker")
			s.showMarker = value != 0.0f;
		else if (k == "show_game_cursor")
			s.showGameCursor = value != 0.0f;
		else if (k == "xr_defaults" && value >= 1)
			_firstRunDefaults = false; // VR defaults were applied once; the options belong to the player now
	}
	s.clamp();
	std::lock_guard<std::mutex> lock(_settingsMutex);
	_settingsShared = s;
	_cfg = s;
	_pointer = s.pointerHand;
}

void Runtime::saveSettings()
{
	VrSettings s;
	{
		std::lock_guard<std::mutex> lock(_settingsMutex);
		s = _settingsShared;
	}
	_savedVersion = _settingsVersion;
	_saveAt = 0;
	std::ofstream out(_settingsPath);
	if (!out)
		return;
	out << "pointer_hand=" << s.pointerHand << "\nsmoothing=" << s.smoothing << "\nview=" << s.view
		<< "\ncanvas_width=" << s.width[VIEW_CANVAS] << "\ncanvas_distance=" << s.distance[VIEW_CANVAS]
		<< "\ntable_width=" << s.width[VIEW_TABLE] << "\ntable_distance=" << s.distance[VIEW_TABLE]
		<< "\ntable_tilt=" << s.tableTilt << "\ntable_height=" << s.tableHeight
		<< "\nstick_adjust=" << (s.stickAdjust ? 1 : 0) << "\npassthrough=" << s.passthrough
		<< "\nshow_ray=" << (s.showRay ? 1 : 0) << "\nshow_marker=" << (s.showMarker ? 1 : 0)
		<< "\nshow_game_cursor=" << (s.showGameCursor ? 1 : 0)
		<< "\nxr_defaults=" << (_firstRunDefaults ? 0 : 1) << "\n";
}

}

bool start(const std::string &configFolder) { return g_runtime.start(configFolder); }
bool active() { return g_runtime.active(); }
void stop() { g_runtime.stop(); }
void setOutputRect(int x, int y, int w, int h) { g_runtime.setOutputRect(x, y, w, h); }
void submitFrame(const void *pixels, int width, int height, int pitch, unsigned int sdlPixelFormat)
{
	g_runtime.submitFrame(pixels, width, height, pitch, sdlPixelFormat);
}
void frameUnchanged() { g_runtime.frameUnchanged(); }
bool takeFirstRunDefaults() { return g_runtime.active() && g_runtime.takeFirstRunDefaults(); }
bool gameShouldRun() { return g_runtime.gameShouldRun(); }
void setMenuOpen(bool open) { g_runtime.setMenuOpen(open); }
void hapticDoublePulse() { g_runtime.hapticDoublePulse(); }
void saveSettingsNow() { g_runtime.requestSave(); }
void setContext(int context) { g_runtime.setContext(context); }
int takeMenuRequest() { return g_runtime.active() ? g_runtime.takeMenuRequest() : MENU_REQUEST_NONE; }
VrSettings settings() { return g_runtime.settings(); }
void setSettings(const VrSettings &settings) { g_runtime.setSettings(settings); }
VrInfo info() { return g_runtime.info(); }
void recenter() { g_runtime.recenter(); }
void tapKey(int sdlScancode) { sendTap(static_cast<SDL_Scancode>(sdlScancode)); }
bool selfTestActive() { return g_runtime.active() && g_runtime.selfTestActive(); }
void debugPointer(float u, float v) { g_runtime.debugPointer(u, v); }
void debugClick() { g_runtime.debugClick(); }
void debugMenuButton() { g_runtime.debugMenuButton(); }
void debugText(const std::string &utf8) { g_runtime.debugText(utf8); }
bool keyboardVisible() { return g_runtime.keyboardVisible(); }
void virtualPoint(float u, float v, int &x, int &y) { g_runtime.virtualPoint(u, v, x, y); }

}
}
