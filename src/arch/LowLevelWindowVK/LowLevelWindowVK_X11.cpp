#include "LowLevelWindowVK_X11.h"
#include "Core/Services/Locator.hpp"
#include "RageUtil/Graphics/RageDisplay.h"
#include "archutils/Unix/X11Helper.h"
#include "Etterna/Models/Misc/DisplaySpec.h"
#include "Etterna/Globals/GameLoop.h"
#include <X11/extensions/Xrandr.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <cstring>
#include <set>
#include <cmath>
#include <exception>

using namespace X11Helper;

const std::string ID_XSCREEN = "XSCREEN_RANDR";

static std::string FAILED_CONNECTION_XSERVER(
  "LowLevelWindowVK_X11: "
  "Failed to establish a connection with the X server");

bool
LowLevelWindowVK_X11::NetWMSupported(Display* Dpy, Atom feature) const
{
	Atom net_supported = XInternAtom(Dpy, "_NET_SUPPORTED", False);
	Atom actual_type_return = BadAtom;
	int actual_format_return = 0;
	unsigned long nitems_return = 0;
	unsigned long bytes_after_return = 0;
	Atom* prop_return;
	Status status =
	  XGetWindowProperty(Dpy,
						 RootWindow(Dpy, DefaultScreen(Dpy)),
						 net_supported,
						 0,
						 8192,
						 False,
						 XA_ATOM,
						 &actual_type_return,
						 &actual_format_return,
						 &nitems_return,
						 &bytes_after_return,
						 reinterpret_cast<unsigned char**>(&prop_return));
	if (status != Success) {
		return false;
	}

	auto supported =
	  std::find(prop_return, prop_return + nitems_return, feature) !=
	  prop_return + nitems_return;
	XFree(prop_return);
	return supported;
}

inline float
calcRandRRefresh(unsigned long iPixelClock, int iHTotal, int iVTotal)
{
	return (iPixelClock) / (iHTotal * iVTotal);
}

LowLevelWindowVK_X11::LowLevelWindowVK_X11()
{
	if (!OpenXConnection())
		throw std::runtime_error(FAILED_CONNECTION_XSERVER);

	if (XRRQueryVersion(Dpy, &m_iRandRVerMajor, &m_iRandRVerMinor) &&
		m_iRandRVerMajor >= 1 && m_iRandRVerMinor >= 2)
		m_bUseXRandR12 = true;

	const int iScreen = DefaultScreen(Dpy);
	Locator::getLogger()->info(
	  "Display: {} (screen {})", DisplayString(Dpy), iScreen);
	int iXServerVersion = XVendorRelease(Dpy);
	int iMajor = iXServerVersion / 10000000;
	iXServerVersion %= 10000000;
	int iMinor = iXServerVersion / 100000;
	iXServerVersion %= 100000;
	int iRevision = iXServerVersion / 1000;
	iXServerVersion %= 1000;
	int iPatch = iXServerVersion;
	Locator::getLogger()->info("X server vendor: {} [{}.{}.{}.{}]",
							   XServerVendor(Dpy),
							   iMajor,
							   iMinor,
							   iRevision,
							   iPatch);

	m_bWasWindowed = true;
	m_pScreenConfig =
	  XRRGetScreenInfo(Dpy, RootWindow(Dpy, DefaultScreen(Dpy)));
}

LowLevelWindowVK_X11::~LowLevelWindowVK_X11()
{
	if (!m_bWasWindowed) {
		if (m_bChangedScreenSize) {
			XRRSetScreenConfig(Dpy,
							   m_pScreenConfig,
							   RootWindow(Dpy, DefaultScreen(Dpy)),
							   m_iOldSize,
							   m_OldRotation,
							   CurrentTime);
		}
		if (m_usedCrtc != None) {
			XRRScreenResources* res = XRRGetScreenResources(Dpy, Win);
			XRRCrtcInfo* conf = XRRGetCrtcInfo(Dpy, res, m_usedCrtc);
			XRRSetCrtcConfig(Dpy,
							 res,
							 m_usedCrtc,
							 conf->timestamp,
							 conf->x,
							 conf->y,
							 m_originalRandRMode,
							 conf->rotation,
							 conf->outputs,
							 conf->noutput);
			XRRFreeScreenResources(res);
			XRRFreeCrtcInfo(conf);
		}
		XUngrabKeyboard(Dpy, CurrentTime);
	}

	if (Win != None) {
		XDestroyWindow(Dpy, Win);
		Win = None;
	}
	CloseXConnection();
}

std::string
LowLevelWindowVK_X11::TryVideoMode(const VideoModeParams& p,
								   bool& bNewDeviceOut)
{
	bNewDeviceOut = true;

	if (Win != None) {
		XDestroyWindow(Dpy, Win);
		Win = None;
	}

	int screen = DefaultScreen(Dpy);
	int depth = DefaultDepth(Dpy, screen);
	Visual* visual = DefaultVisual(Dpy, screen);

	if (!MakeWindow(Win, screen, depth, visual, p.width, p.height, !p.windowed))
		return "Failed to create window.";
	XSelectInput(Dpy,
				 Win,
				 StructureNotifyMask | KeyPressMask | KeyReleaseMask |
				   ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
				   ExposureMask | FocusChangeMask);
	XMapWindow(Dpy, Win);
	XFlush(Dpy);

	int max_attempts = 1000;
	int attempt = 0;
	bool mapped = false;
	while (attempt < max_attempts) {
		XEvent ev;
		if (XCheckWindowEvent(Dpy, Win, StructureNotifyMask, &ev) &&
			ev.type == MapNotify) {
			mapped = true;
			break;
		}
		usleep(1000);
		attempt++;
	}
	if (!mapped) {
		return "Timeout waiting for window to map";
	}

	if (!p.windowed) {
		if (m_bChangedScreenSize) {
			XRRSetScreenConfig(Dpy,
							   m_pScreenConfig,
							   RootWindow(Dpy, DefaultScreen(Dpy)),
							   m_iOldSize,
							   m_OldRotation,
							   CurrentTime);
		}
		if (m_usedCrtc != None) {
			XRRScreenResources* res = XRRGetScreenResources(Dpy, Win);
			XRRCrtcInfo* conf = XRRGetCrtcInfo(Dpy, res, m_usedCrtc);
			XRRSetCrtcConfig(Dpy,
							 res,
							 m_usedCrtc,
							 conf->timestamp,
							 conf->x,
							 conf->y,
							 m_originalRandRMode,
							 conf->rotation,
							 conf->outputs,
							 conf->noutput);
			XRRFreeScreenResources(res);
			XRRFreeCrtcInfo(conf);
			m_usedCrtc = None;
		}

		if (p.sDisplayId.empty() || p.sDisplayId == ID_XSCREEN) {
			int nsizes;
			XRRScreenSize* sizes = XRRSizes(Dpy, DefaultScreen(Dpy), &nsizes);
			int sizeMatch = -1;
			for (int i = 0; i < nsizes; ++i) {
				if (sizes[i].width == p.width && sizes[i].height == p.height) {
					sizeMatch = i;
					break;
				}
			}
			if (sizeMatch != -1) {
				Rotation curRot;
				m_iOldSize =
				  XRRConfigCurrentConfiguration(m_pScreenConfig, &curRot);
				m_OldRotation = curRot;
				XRRSetScreenConfig(Dpy,
								   m_pScreenConfig,
								   RootWindow(Dpy, DefaultScreen(Dpy)),
								   sizeMatch,
								   1,
								   CurrentTime);
				m_bChangedScreenSize = true;
			}
			XMoveWindow(Dpy, Win, 0, 0);
		} else if (m_bUseXRandR12) {
			XRRScreenResources* scrRes = XRRGetScreenResources(Dpy, Win);
			if (!scrRes)
				return "XRandR 1.2 not available.";

			RROutput targetOut = None;
			for (unsigned int i = 0; i < scrRes->noutput; ++i) {
				XRROutputInfo* outInfo =
				  XRRGetOutputInfo(Dpy, scrRes, scrRes->outputs[i]);
				std::string outName(outInfo->name, outInfo->nameLen);
				if (outName == p.sDisplayId) {
					targetOut = scrRes->outputs[i];
				}
				XRRFreeOutputInfo(outInfo);
			}
			if (targetOut == None) {
				if (m_iRandRVerMajor >= 1 && m_iRandRVerMinor >= 3)
					targetOut = XRRGetOutputPrimary(Dpy, Win);
				else {
					for (unsigned int i = 0;
						 i < scrRes->noutput && targetOut == None;
						 ++i) {
						XRROutputInfo* outInfo =
						  XRRGetOutputInfo(Dpy, scrRes, scrRes->outputs[i]);
						if (outInfo->connection == RR_Connected)
							targetOut = scrRes->outputs[i];
						XRRFreeOutputInfo(outInfo);
					}
				}
			}

			if (targetOut == None) {
				XRRFreeScreenResources(scrRes);
				return "No suitable output found.";
			}

			XRROutputInfo* outInfo = XRRGetOutputInfo(Dpy, scrRes, targetOut);
			if (!outInfo) {
				XRRFreeScreenResources(scrRes);
				return "Failed to get output info.";
			}

			RRCrtc crtc = outInfo->crtc;
			if (crtc == None && outInfo->ncrtc > 0) {
				for (unsigned int i = 0; i < outInfo->ncrtc; ++i) {
					XRRCrtcInfo* crtcInfo =
					  XRRGetCrtcInfo(Dpy, scrRes, outInfo->crtcs[i]);
					if (crtcInfo->mode == None) {
						crtc = outInfo->crtcs[i];
						XRRFreeCrtcInfo(crtcInfo);
						break;
					}
					XRRFreeCrtcInfo(crtcInfo);
				}
			}
			if (crtc == None) {
				XRRFreeOutputInfo(outInfo);
				XRRFreeScreenResources(scrRes);
				return "No usable CRTC for output.";
			}

			XRRCrtcInfo* oldConf = XRRGetCrtcInfo(Dpy, scrRes, crtc);
			if (!oldConf) {
				XRRFreeOutputInfo(outInfo);
				XRRFreeScreenResources(scrRes);
				return "Failed to get CRTC info.";
			}

			// Find a mode matching desired resolution and closest refresh rate
			float bestRefresh = 0.0f;
			RRMode bestMode = None;
			const bool bPortrait =
			  (oldConf->rotation & (RR_Rotate_90 | RR_Rotate_270)) != 0;
			for (unsigned int i = 0; i < scrRes->nmode; ++i) {
				const XRRModeInfo& mi = scrRes->modes[i];
				unsigned int modeWidth = bPortrait ? mi.height : mi.width;
				unsigned int modeHeight = bPortrait ? mi.width : mi.height;
				if (modeWidth == p.width && modeHeight == p.height) {
					float refresh =
					  calcRandRRefresh(mi.dotClock, mi.hTotal, mi.vTotal);
					if ((p.rate != REFRESH_DEFAULT &&
						 std::abs(p.rate - refresh) <
						   std::abs(p.rate - bestRefresh)) ||
						(p.rate == REFRESH_DEFAULT && refresh > bestRefresh)) {
						for (unsigned int j = 0; j < outInfo->nmode; ++j) {
							if (outInfo->modes[j] == mi.id) {
								bestMode = mi.id;
								bestRefresh = refresh;
								break;
							}
						}
					}
				}
			}

			if (bestMode == None) {
				XRRFreeCrtcInfo(oldConf);
				XRRFreeOutputInfo(outInfo);
				XRRFreeScreenResources(scrRes);
				return "No matching mode found.";
			}

			m_usedCrtc = crtc;
			m_originalRandRMode = oldConf->mode;

			Status s = XRRSetCrtcConfig(Dpy,
										scrRes,
										crtc,
										oldConf->timestamp,
										oldConf->x,
										oldConf->y,
										bestMode,
										oldConf->rotation,
										oldConf->outputs,
										oldConf->noutput);
			XRRFreeCrtcInfo(oldConf);
			XRRFreeOutputInfo(outInfo);
			XRRFreeScreenResources(scrRes);

			if (s != Success)
				return "Failed to set CRTC config.";

			XMoveWindow(Dpy, Win, 0, 0);
		}

		while (XGrabKeyboard(
		  Dpy, Win, True, GrabModeAsync, GrabModeAsync, CurrentTime))
			;

		m_bWasWindowed = false;
	} else {
		if (m_bWasWindowed == false) {
			if (m_bChangedScreenSize) {
				XRRSetScreenConfig(Dpy,
								   m_pScreenConfig,
								   RootWindow(Dpy, DefaultScreen(Dpy)),
								   m_iOldSize,
								   m_OldRotation,
								   CurrentTime);
				m_bChangedScreenSize = false;
			}
			if (m_usedCrtc != None) {
				XRRScreenResources* res = XRRGetScreenResources(Dpy, Win);
				XRRCrtcInfo* conf = XRRGetCrtcInfo(Dpy, res, m_usedCrtc);
				XRRSetCrtcConfig(Dpy,
								 res,
								 m_usedCrtc,
								 conf->timestamp,
								 conf->x,
								 conf->y,
								 m_originalRandRMode,
								 conf->rotation,
								 conf->outputs,
								 conf->noutput);
				XRRFreeScreenResources(res);
				XRRFreeCrtcInfo(conf);
				m_usedCrtc = None;
			}
			XUngrabKeyboard(Dpy, CurrentTime);
			m_bWasWindowed = true;
		}

		XSizeHints hints;
		hints.flags = PMinSize | PMaxSize | PWinGravity;
		hints.min_width = hints.max_width = p.width;
		hints.min_height = hints.max_height = p.height;
		hints.win_gravity = CenterGravity;
		XSetWMNormalHints(Dpy, Win, &hints);

		Atom fullscreen = XInternAtom(Dpy, "_NET_WM_STATE_FULLSCREEN", False);
		Atom net_wm_state = XInternAtom(Dpy, "_NET_WM_STATE", False);
		XChangeProperty(Dpy,
						Win,
						net_wm_state,
						XA_ATOM,
						32,
						PropModeReplace,
						(unsigned char*)&fullscreen,
						0);
	}

	CurrentParams = std::make_unique<ActualVideoModeParams>(p);
	return "";
}

void
LowLevelWindowVK_X11::Update()
{
	XEvent event;
	if (XCheckTypedEvent(Dpy, ClientMessage, &event) &&
		event.xclient.data.l[0] == wmDeleteMessage) {
		GameLoop::setUserQuit();
	}
}

void
LowLevelWindowVK_X11::GetDisplaySpecs(DisplaySpecs& out) const
{
	int screenNum = DefaultScreen(Dpy);
	Screen* screen = ScreenOfDisplay(Dpy, screenNum);

	Rotation curRotation;
	XRRScreenConfiguration* screenConf = XRRGetScreenInfo(Dpy, Win);
	short curRate = XRRConfigCurrentRate(screenConf);
	SizeID curSizeId = XRRConfigCurrentConfiguration(screenConf, &curRotation);

	std::set<DisplayMode> screenModes;
	int nsizes = 0;
	XRRScreenSize* screenSizes = XRRSizes(Dpy, screenNum, &nsizes);
	DisplayMode screenCurMode = { 0 };
	for (int szIdx = 0, mode_idx = 0; szIdx < nsizes; ++szIdx) {
		XRRScreenSize& size = screenSizes[szIdx];
		int nrates = 0;
		short* rates = XRRRates(Dpy, screenNum, szIdx, &nrates);
		for (int rIdx = 0; rIdx < nrates; ++rIdx, ++mode_idx) {
			DisplayMode m = { static_cast<unsigned int>(size.width),
							  static_cast<unsigned int>(size.height),
							  static_cast<double>(rates[rIdx]) };
			screenModes.insert(m);
			if (rates[rIdx] == curRate && szIdx == curSizeId) {
				screenCurMode = m;
			}
		}
	}
	const RectI screenBounds(
	  0, 0, screenSizes[curSizeId].width, screenSizes[curSizeId].height);
	out.insert(DisplaySpec(
	  ID_XSCREEN, "X Screen", screenModes, screenCurMode, screenBounds, true));
	XRRFreeScreenConfigInfo(screenConf);

	if (m_bUseXRandR12) {
		XRRScreenResources* scrRes = XRRGetScreenResources(Dpy, Win);
		if (scrRes) {
			std::map<RRMode, DisplayMode> outputModes;
			for (unsigned int i = 0; i < scrRes->nmode; ++i) {
				const XRRModeInfo& mode = scrRes->modes[i];
				DisplayMode m = { mode.width,
								  mode.height,
								  calcRandRRefresh(
									mode.dotClock, mode.hTotal, mode.vTotal) };
				outputModes[mode.id] = m;
			}

			for (unsigned int outIdx = 0; outIdx < scrRes->noutput; ++outIdx) {
				XRROutputInfo* outInfo =
				  XRRGetOutputInfo(Dpy, scrRes, scrRes->outputs[outIdx]);
				if (outInfo->nmode > 0) {
					RRMode curRRMode = None;
					bool bPortrait = false;
					int crtcX = 0, crtcY = 0;
					if (outInfo->crtc != None) {
						XRRCrtcInfo* conf =
						  XRRGetCrtcInfo(Dpy, scrRes, outInfo->crtc);
						curRRMode = conf->mode;
						bPortrait = (conf->rotation &
									 (RR_Rotate_90 | RR_Rotate_270)) != 0;
						crtcX = conf->x;
						crtcY = conf->y;
						XRRFreeCrtcInfo(conf);
					}
					std::set<DisplayMode> supported;
					DisplayMode curMode = { 0 };
					RectI bounds;
					for (unsigned int modeIdx = 0; modeIdx < outInfo->nmode;
						 ++modeIdx) {
						DisplayMode mode = outputModes[outInfo->modes[modeIdx]];
						unsigned int modeWidth =
						  bPortrait ? mode.height : mode.width;
						unsigned int modeHeight =
						  bPortrait ? mode.width : mode.height;
						DisplayMode m = { modeWidth,
										  modeHeight,
										  mode.refreshRate };
						supported.insert(m);
						if (curRRMode != None &&
							outInfo->modes[modeIdx] == curRRMode) {
							curMode = m;
							bounds = RectI(crtcX,
										   crtcY,
										   crtcX + modeWidth,
										   crtcY + modeHeight);
						}
					}
					std::string outId(outInfo->name, outInfo->nameLen);
					std::string outName(outId);
					if (curRRMode != None) {
						out.insert(DisplaySpec(
						  outId, outName, supported, curMode, bounds));
					} else {
						out.insert(DisplaySpec(outId, outName, supported));
					}
				}
				XRRFreeOutputInfo(outInfo);
			}
			XRRFreeScreenResources(scrRes);
		}
	}
}

const ActualVideoModeParams*
LowLevelWindowVK_X11::GetActualVideoModeParams() const
{
	return CurrentParams.get();
}

bool
LowLevelWindowVK_X11::SupportsFullscreenBorderlessWindow() const
{
	Atom fullscreen = XInternAtom(Dpy, "_NET_WM_STATE_FULLSCREEN", False);
	return NetWMSupported(Dpy, fullscreen);
}
