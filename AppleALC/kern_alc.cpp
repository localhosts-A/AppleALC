//
//  kern_alc.cpp
//  AppleALC
//
//  Dedicated ALC293 driver for Lenovo T460/T560, layout-id 29.
//  Based on AppleALC by vit9696.
//

#include <Headers/kern_api.hpp>
#include <Headers/kern_devinfo.hpp>
#include <Headers/plugin_start.hpp>
#include <Headers/kern_compression.hpp>
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <mach/vm_map.h>
#include <libkern/c++/OSUnserialize.h>

#include "kern_alc.hpp"
#include "kern_resources.hpp"

static AlcEnabler alcEnabler;
AlcEnabler* AlcEnabler::callbackAlc = nullptr;

// ============================================================================
// Lifecycle
// ============================================================================

void AlcEnabler::createShared() {
	if (callbackAlc)
		PANIC("alc", "Attempted to assign alc callback again");
	callbackAlc = &alcEnabler;
	if (!callbackAlc)
		PANIC("alc", "Failed to assign alc callback");
}

void AlcEnabler::init() {
	lilu.onPatcherLoadForce([](void *user, KernelPatcher &patcher) {
		static_cast<AlcEnabler *>(user)->updateProperties();
	}, this);

	// Disable kexts we do not need for analog-only ALC293
	if (getKernelVersion() < KernelVersion::Mojave)
		ADDPR(kextList)[KextIdAppleGFXHDA].switchOff();
	if (getKernelVersion() == KernelVersion::Tiger || getKernelVersion() >= KernelVersion::Lion)
		ADDPR(kextList)[KextIdAppleHDAPlatformDriver].switchOff();

	lilu.onKextLoadForce(ADDPR(kextList), ADDPR(kextListSize),
	[](void *user, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
		static_cast<AlcEnabler *>(user)->processKext(patcher, index, address, size);
	}, this);

	if (getKernelVersion() >= KernelVersion::Sierra && checkKernelArgument("-alcdhost")) {
		lilu.onEntitlementRequestForce([](void *user, task_t task, const char *entitlement, OSObject *&original) {
			static_cast<AlcEnabler *>(user)->handleAudioClientEntitlement(task, entitlement, original);
		}, this);
	}
}

void AlcEnabler::deinit() {
	// Nothing to clean up — no dynamic codec/controller lists
}

// ============================================================================
// Device property injection
// ============================================================================

void AlcEnabler::updateProperties() {
	auto devInfo = DeviceInfo::create();
	if (!devInfo) {
		SYSLOG("alc", "failed to obtain device info");
		return;
	}

	// Terminate built-in digital audio (HDAU) — not needed for analog-only ALC293
	if (devInfo->audioBuiltinDigital && !devInfo->reportedFramebufferIsConnectorLess) {
		WIOKit::awaitPublishing(devInfo->audioBuiltinDigital);
		auto hda = OSDynamicCast(IOService, devInfo->audioBuiltinDigital);
		auto pci = OSDynamicCast(IOService, devInfo->audioBuiltinDigital->getParentEntry(gIOServicePlane));
		if (hda && pci) {
			if (hda->requestTerminate(pci, 0) && hda->terminate())
				hda->stop(pci);
		}
	}

	// Configure the HDEF (analog audio) device
	if (devInfo->audioBuiltinAnalog) {
		uint32_t ven = 0;
		if (WIOKit::getOSDataValue(devInfo->audioBuiltinAnalog, "vendor-id", ven) && ven == WIOKit::VendorID::Intel) {
			// Always reset TCSEL for Intel HDEF — fixes warm-reboot and other quirks
			WIOKit::awaitPublishing(devInfo->audioBuiltinAnalog);
			auto hdef = static_cast<IOPCIDevice *>(devInfo->audioBuiltinAnalog->metaCast("IOPCIDevice"));
			if (hdef) {
				static constexpr size_t RegTCSEL = 0x44;
				auto value = hdef->configRead8(RegTCSEL);
				if (value != 0) {
					DBGLOG("alc", "resetting TCSEL from 0x%02X to 0x00", value);
					hdef->configWrite8(RegTCSEL, 0);
				}
			}
		}

		updateDeviceProperties(devInfo->audioBuiltinAnalog, devInfo, nullptr, true);
	}

	DeviceInfo::deleter(devInfo);
}

void AlcEnabler::updateDeviceProperties(IORegistryEntry *hdaService, DeviceInfo *info, const char *hdaGfx, bool isAnalog) {
	auto hdaPlaneName = hdaService->getName();

	// Ensure device is named HDEF (required by AppleHDAController)
	if (isAnalog && (!hdaPlaneName || strcmp(hdaPlaneName, "HDEF") != 0)) {
		DBGLOG("alc", "fixing audio plane name to HDEF");
		WIOKit::renameDevice(hdaService, "HDEF");
	}

	// Set alc-layout-id: prefer boot-arg, then existing property, then default to 29
	uint32_t layout = 0;
	if (lilu_get_boot_args("alcid", &layout, sizeof(layout))) {
		DBGLOG("alc", "found alc-layout-id override %u", layout);
		hdaService->setProperty("alc-layout-id", &layout, sizeof(layout));
	} else {
		uint32_t alcId;
		if (info->firmwareVendor == DeviceInfo::FirmwareVendor::Apple &&
			WIOKit::getOSDataValue(hdaService, "alc-layout-id", alcId)) {
			DBGLOG("alc", "found apple alc-layout-id %u", alcId);
		} else if (info->firmwareVendor != DeviceInfo::FirmwareVendor::Apple
				   || hdaService->getProperty("use-layout-id") != nullptr) {
			if (WIOKit::getOSDataValue(hdaService, "layout-id", alcId)) {
				DBGLOG("alc", "found legacy layout-id %u", alcId);
				hdaService->setProperty("alc-layout-id", &alcId, sizeof(alcId));
			} else {
				alcId = DefaultLayoutId;
				DBGLOG("alc", "no layout-id found, using default %u", alcId);
				hdaService->setProperty("alc-layout-id", &alcId, sizeof(alcId));
			}
		}
	}

	// Boot beep volume
	if (!hdaService->getProperty("MaximumBootBeepVolume")) {
		uint8_t v[] { 0x7F };
		hdaService->setProperty("MaximumBootBeepVolume", v, sizeof(v));
	}
	if (!hdaService->getProperty("MaximumBootBeepVolumeAlt")) {
		uint8_t v[] { 0x7F };
		hdaService->setProperty("MaximumBootBeepVolumeAlt", v, sizeof(v));
	}
	if (!hdaService->getProperty("PinConfigurations")) {
		uint8_t p[] { 0x00 };
		hdaService->setProperty("PinConfigurations", p, sizeof(p));
	}

	// Set layout-id for AppleHDA
	if (info->firmwareVendor != DeviceInfo::FirmwareVendor::Apple || hdaService->getProperty("use-apple-layout-id") != nullptr) {
		hdaService->setProperty("layout-id", &info->reportedLayoutId, sizeof(info->reportedLayoutId));
		layoutIdIsOverridden = true;
		layoutIdOverride = info->reportedLayoutId;
	}

	if (hdaGfx)
		hdaService->setProperty("hda-gfx", const_cast<char *>(hdaGfx), static_cast<uint32_t>(strlen(hdaGfx)+1));

	// Ensure built-in
	if (!hdaService->getProperty("built-in")) {
		uint8_t b[] { 0x00 };
		hdaService->setProperty("built-in", b, sizeof(b));
	}
}

// ============================================================================
// Entitlement hook
// ============================================================================

void AlcEnabler::handleAudioClientEntitlement(task_t task, const char *entitlement, OSObject *&original) {
	if ((!original || original != kOSBooleanTrue) && !strcmp(entitlement, "com.apple.private.audio.driver-host"))
		original = kOSBooleanTrue;
}

// ============================================================================
// Log erasure
// ============================================================================

void AlcEnabler::eraseRedundantLogs(KernelPatcher &patcher, size_t index) {
	static const uint8_t logAssertFind[] = { 0x53, 0x6F, 0x75, 0x6E, 0x64, 0x20, 0x61, 0x73 };
	static const uint8_t nullReplace[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

	KernelPatcher::LookupPatch currentPatch {
		&ADDPR(kextList)[index], nullptr, nullReplace, sizeof(nullReplace)
	};

	if (index == KextIdAppleHDAController || index == KextIdAppleHDA) {
		currentPatch.find = logAssertFind;
		currentPatch.count = (index == KextIdAppleHDAController) ? 3 : 2;
		patcher.applyLookupPatch(&currentPatch);
		patcher.clearError();
	}
}

// ============================================================================
// Kext patching — the core of the driver
// ============================================================================

void AlcEnabler::processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	size_t kextIndex = 0;
	while (kextIndex < ADDPR(kextListSize)) {
		if (ADDPR(kextList)[kextIndex].loadIndex == index)
			break;
		kextIndex++;
	}
	if (kextIndex == ADDPR(kextListSize))
		return;

	// ---- AppleGFXHDA: prevent it from claiming HDEF ----
	if (kextIndex == KextIdAppleGFXHDA) {
		KernelPatcher::RouteRequest request("__ZN21AppleGFXHDAController5probeEP9IOServicePi", gfxProbe, orgGfxProbe);
		patcher.routeMultiple(index, &request, 1, address, size);
		return;
	}

	// ---- Apply controller patches (from Controllers.plist via ResourceConverter) ----
	if (!(progressState & ProcessingState::ControllersLoaded)) {
		progressState |= ProcessingState::ControllersLoaded;

		// Apply all controller patches unconditionally — there is only one in our minimal config
		for (size_t i = 0; i < ADDPR(controllerModSize); i++) {
			auto &info = ADDPR(controllerMod)[i];
			DBGLOG("alc", "applying controller patches for %s (%X:%X)", info.name, info.vendor, info.device);
			applyPatches(patcher, index, info.patches, info.patchNum);
		}
	}

	// ---- Apply codec patches and route resource callbacks ----
	if (!(progressState & ProcessingState::CodecsLoaded)) {
		progressState |= ProcessingState::CodecsLoaded;

		// Apply all vendor/codec patches unconditionally — only ALC293 in our config
		for (size_t v = 0; v < ADDPR(vendorModSize); v++) {
			for (size_t c = 0; c < ADDPR(vendorMod)[v].codecsNum; c++) {
				auto &codec = ADDPR(vendorMod)[v].codecs[c];
				DBGLOG("alc", "applying codec patches for %s", codec.name);
				applyPatches(patcher, index, codec.patches, codec.patchNum);

				if (codec.platformNum > 0 || codec.layoutNum > 0)
					progressState |= ProcessingState::CallbacksWantRouting;
			}
		}

		if (!ADDPR(debugEnabled))
			eraseRedundantLogs(patcher, kextIndex);
	}

	// ---- Route AppleHDA callbacks for resource injection ----
	if ((progressState & ProcessingState::CallbacksWantRouting) && kextIndex == KextIdAppleHDA) {
		progressState |= ProcessingState::CallbacksHooked;

		// performPowerChange — wake-verb reinit
		KernelPatcher::RouteRequest requestPowerChange(symPerformPowerChange, performPowerChange, orgPerformPowerChange);
		patcher.routeMultiple(index, &requestPowerChange, 1, address, size);

		// initializePinConfig
		if (getKernelVersion() >= KernelVersion::SnowLeopard
			|| patcher.solveSymbol(index, "__ZN20AppleHDACodecGeneric38initializePinConfigDefaultFromOverrideEP9IOService")) {
			KernelPatcher::RouteRequest req("__ZN20AppleHDACodecGeneric38initializePinConfigDefaultFromOverrideEP9IOService", initializePinConfig, orgInitializePinConfig);
			patcher.routeMultiple(index, &req, 1, address, size);
		} else {
			patcher.clearError();
			KernelPatcher::RouteRequest req("__ZN20AppleHDACodecGeneric38initializePinConfigDefaultFromOverrideEv", initializePinConfigLegacy, orgInitializePinConfigLegacy);
			patcher.routeMultiple(index, &req, 1, address, size);
		}

		// layout/platform load callbacks (10.6.8+)
		if (patcher.solveSymbol(index, "__ZN14AppleHDADriver18layoutLoadCallbackEjiPKvjPv")) {
			KernelPatcher::RouteRequest requests[] {
				KernelPatcher::RouteRequest("__ZN14AppleHDADriver18layoutLoadCallbackEjiPKvjPv", layoutLoadCallback, orgLayoutLoadCallback),
				KernelPatcher::RouteRequest("__ZN14AppleHDADriver20platformLoadCallbackEjiPKvjPv", platformLoadCallback, orgPlatformLoadCallback)
			};
			patcher.routeMultiple(index, requests, address, size);
		} else {
			patcher.clearError();
		}

		isAppleHDAZlib = getKernelVersion() >= KernelVersion::Mavericks
			|| patcher.solveSymbol(index, "__Z24AppleHDA_zlib_uncompressPhPmPKhm") != 0;
		if (!isAppleHDAZlib)
			patcher.clearError();

		// 10.4: platforms/layouts are inside AppleHDA itself
		if (getKernelVersion() == KernelVersion::Tiger) {
			KernelPatcher::RouteRequest req("__ZN14AppleHDADriver5startEP9IOService", AppleHDADriver_start, orgAppleHDADriver_start);
			patcher.routeMultiple(index, &req, 1, address, size);
		}

		if (!ADDPR(debugEnabled))
			eraseRedundantLogs(patcher, kextIndex);
	}

	// ---- IOHDAFamily: hook executeVerb ----
	if (!(progressState & ProcessingState::PatchHDAFamily) && kextIndex == KextIdIOHDAFamily) {
		progressState |= ProcessingState::PatchHDAFamily;
		KernelPatcher::RouteRequest request(symIOHDACodecDevice_executeVerb, IOHDACodecDevice_executeVerb, orgIOHDACodecDevice_executeVerb);
		patcher.routeMultiple(index, &request, 1, address, size);
	}

	// ---- AppleHDAController: hook start for warm-reboot fix ----
	if (!(progressState & ProcessingState::PatchHDAController) && kextIndex == KextIdAppleHDAController) {
		progressState |= ProcessingState::PatchHDAController;
		KernelPatcher::RouteRequest request("__ZN18AppleHDAController5startEP9IOService", AppleHDAController_start, orgAppleHDAController_start);
		patcher.routeMultiple(index, &request, 1, address, size);
	}

	// ---- AppleHDAPlatformDriver (10.5.x–10.6.7) ----
	if (!(progressState & ProcessingState::PatchHDAPlatformDriver) && kextIndex == KextIdAppleHDAPlatformDriver) {
		progressState |= ProcessingState::PatchHDAPlatformDriver;
		KernelPatcher::RouteRequest request("__ZN22AppleHDAPlatformDriver5startEP9IOService", AppleHDAPlatformDriver_start, orgAppleHDAPlatformDriver_start);
		patcher.routeMultiple(index, &request, 1, address, size);
	}

	patcher.clearError();
}

void AlcEnabler::applyPatches(KernelPatcher &patcher, size_t index, const KextPatch *patches, size_t patchNum) {
	for (size_t p = 0; p < patchNum; p++) {
		auto &patch = patches[p];
		if (patch.patch.kext->loadIndex == index) {
			if (patcher.compatibleKernel(patch.minKernel, patch.maxKernel)) {
				DBGLOG("alc", "applying patch %lu for kext %s", p, patch.patch.kext->id);
				patcher.applyLookupPatch(&patch.patch);
				patcher.clearError();
			}
		}
	}
}

// ============================================================================
// Hooks
// ============================================================================

IOService *AlcEnabler::gfxProbe(IOService *ctrl, IOService *provider, SInt32 *score) {
	auto name = provider->getName();
	DBGLOG("alc", "AppleGFXHDA probe for %s", safeString(name));
	if (name && !strcmp(name, "HDEF")) {
		DBGLOG("alc", "blocking AppleGFXHDA for HDEF");
		return nullptr;
	}
	return FunctionCast(gfxProbe, callbackAlc->orgGfxProbe)(ctrl, provider, score);
}

// ---- Warm-reboot fix ----

void AlcEnabler::resetHDAControllerConfig(IOService *provider) {
	uint32_t warmRebootFix = 1;  // Always enabled for this dedicated driver
	lilu_get_boot_args("alcwarmpb", &warmRebootFix, sizeof(warmRebootFix));
	if (warmRebootFix == 0)
		return;

	auto hdef = static_cast<IOPCIDevice *>(provider->metaCast("IOPCIDevice"));
	if (!hdef)
		return;

	DBGLOG("alc", "applying warm-reboot fix");

	// Reset TCSEL
	static constexpr size_t RegTCSEL = 0x44;
	auto tcsel = hdef->configRead8(RegTCSEL);
	if (tcsel != 0) {
		DBGLOG("alc", "resetting TCSEL 0x%02X -> 0x00", tcsel);
		hdef->configWrite8(RegTCSEL, 0);
	}

	// Ensure Bus Master + Memory Space
	static constexpr size_t RegPCICmd = 0x04;
	auto cmd = hdef->configRead16(RegPCICmd);
	uint16_t required = 0x0004 | 0x0002;  // BusMaster | MemSpace
	if ((cmd & required) != required) {
		DBGLOG("alc", "fixing PCI cmd 0x%04X -> 0x%04X", cmd, cmd | required);
		hdef->configWrite16(RegPCICmd, cmd | required);
	}
}

bool AlcEnabler::AppleHDAController_start(IOService* service, IOService* provider) {
	uint32_t delay = 0;
	if (lilu_get_boot_args("alcdelay", &delay, sizeof(delay))) {
		DBGLOG("alc", "alc-delay override %u", delay);
		provider->setProperty("alc-delay", &delay, sizeof(delay));
	} else {
		WIOKit::getOSDataValue(provider, "alc-delay", delay);
	}

	if (delay > MaxAlcDelay) {
		SYSLOG("alc", "alc-delay %u exceeds max %u, ignoring", delay, MaxAlcDelay);
		delay = 0;
	}
	if (delay) {
		DBGLOG("alc", "delaying %u ms", delay);
		IOSleep(delay);
	}

	resetHDAControllerConfig(provider);

	return FunctionCast(AppleHDAController_start, callbackAlc->orgAppleHDAController_start)(service, provider);
}

// ---- executeVerb ----

IOReturn AlcEnabler::IOHDACodecDevice_executeVerb(void *hdaCodecDevice, uint16_t nid, uint16_t verb, uint16_t param, unsigned int *output, bool waitForSuccess) {
	if (verb & 0xff0)
		DBGLOG("alc", "executeVerb nid=0x%02X verb=0x%03X param=0x%02X", nid, verb, param);
	else
		DBGLOG("alc", "executeVerb nid=0x%02X verb=0x%X param=0x%04X", nid, verb, param);
	return FunctionCast(IOHDACodecDevice_executeVerb, callbackAlc->orgIOHDACodecDevice_executeVerb)(hdaCodecDevice, nid, verb, param, output, waitForSuccess);
}

// ---- Layout helper ----

uint32_t AlcEnabler::getAudioLayout(IOService *hdaDriver) {
	auto parent = hdaDriver->getParentEntry(gIOServicePlane);
	while (parent) {
		auto name = parent->getName();
		if (name && (!strcmp(name, "HDEF") || !strcmp(name, "HDAU"))) {
			uint32_t layout = 0;
			if (!WIOKit::getOSDataValue(parent, "layout-id", layout))
				SYSLOG("alc", "failed to get layout-id from %s", name);
			return layout;
		}
		parent = parent->getParentEntry(gIOServicePlane);
	}
	return 0;
}

// ---- Power change (wake-verb reinit) ----

IOReturn AlcEnabler::performPowerChange(IOService *hdaDriver, uint32_t from, uint32_t to, unsigned int *timer) {
	IOReturn ret = FunctionCast(performPowerChange, callbackAlc->orgPerformPowerChange)(hdaDriver, from, to, timer);

	auto hdaCodec = hdaDriver ? OSDynamicCast(IOService, hdaDriver->getParentEntry(gIOServicePlane)) : nullptr;
	if (!hdaCodec) {
		SYSLOG("alc", "power change: no codec");
		return ret;
	}

	auto pinStatus = OSDynamicCast(OSBoolean, hdaCodec->getProperty("alc-pinconfig-status"));
	auto sleepStatus = OSDynamicCast(OSBoolean, hdaCodec->getProperty("alc-sleep-status"));
	if (!pinStatus || !sleepStatus) {
		SYSLOG("alc", "power change: missing pin/sleep status");
		return ret;
	}

	if (pinStatus->getValue()) {
		if (to == ALCAudioDeviceSleep) {
			hdaCodec->setProperty("alc-sleep-status", kOSBooleanTrue);
		} else if (sleepStatus->getValue() && (to == ALCAudioDeviceIdle || to == ALCAudioDeviceActive)) {
			DBGLOG("alc", "re-sending pin config on wake");
			auto forceRet = FunctionCast(initializePinConfig, callbackAlc->orgInitializePinConfig)(hdaCodec, hdaCodec);
			SYSLOG_COND(forceRet != kIOReturnSuccess, "alc", "wake reinit returned %08X", forceRet);
			hdaCodec->setProperty("alc-sleep-status", kOSBooleanFalse);
		}
	}
	return ret;
}

// ---- PinConfig ----

void AlcEnabler::patchPinConfig(IOService *hdaCodec, IORegistryEntry *configDevice) {
	if (!hdaCodec || !configDevice || hdaCodec->getProperty("alc-pinconfig-status"))
		return;

	uint32_t appleLayout = getAudioLayout(hdaCodec);

	DBGLOG("alc", "patchPinConfig: apple layout %u", appleLayout);

	hdaCodec->setProperty("alc-pinconfig-status", kOSBooleanFalse);
	hdaCodec->setProperty("alc-sleep-status", kOSBooleanFalse);

	auto alcSelf = ADDPR(selfInstance);
	if (!alcSelf) {
		SYSLOG("alc", "invalid self reference");
		return;
	}

	if (!appleLayout)
		return;

	auto configList = OSDynamicCast(OSArray, alcSelf->getProperty("HDAConfigDefault"));
	if (!configList) {
		SYSLOG("alc", "no HDAConfigDefault found");
		return;
	}

	DBGLOG("alc", "searching HDAConfigDefault (%u entries) for layout %u", configList->getCount(), appleLayout);

	for (unsigned int i = 0; i < configList->getCount(); i++) {
		auto config = OSDynamicCast(OSDictionary, configList->getObject(i));
		if (!config)
			continue;

		auto currLayout = OSDynamicCast(OSNumber, config->getObject("LayoutID"));
		if (!currLayout || currLayout->unsigned32BitValue() != appleLayout)
			continue;

		DBGLOG("alc", "matched HDAConfigDefault entry %u", i);

		auto newConfigCollection = config->copyCollection();
		auto newConfig = OSDynamicCast(OSDictionary, newConfigCollection);
		const OSObject *newConfigObj = OSDynamicCast(OSObject, newConfigCollection);
		if (!newConfig || !newConfigObj) {
			SYSLOG("alc", "failed to copy collection");
			OSSafeReleaseNULL(newConfigCollection);
			break;
		}

		auto configData = OSDynamicCast(OSData, config->getObject("ConfigData"));
		auto wakeConfigData = OSDynamicCast(OSData, config->getObject("WakeConfigData"));
		auto reinitBool = OSDynamicCast(OSBoolean, config->getObject("WakeVerbReinit"));
		auto reinit = reinitBool ? reinitBool->getValue() : false;

		auto num = OSNumber::withNumber(appleLayout, 32);
		if (num) {
			newConfig->setObject("LayoutID", num);
			num->release();
		}

		const OSObject *objForArr = newConfigObj;
		auto arr = OSArray::withObjects(&objForArr, 1);
		if (arr) {
			configDevice->setProperty("HDAConfigDefault", arr);
			newConfig->retain();
			arr->release();
		}

		if (!reinit) {
			newConfig->release();
			break;
		}

		// Prepare wake config
		newConfigCollection = newConfig->copyCollection();
		newConfig->release();
		newConfig = OSDynamicCast(OSDictionary, newConfigCollection);
		newConfigObj = OSDynamicCast(OSObject, newConfigCollection);
		if (!newConfig || !newConfigObj) {
			SYSLOG("alc", "failed to copy collection for reinit");
			OSSafeReleaseNULL(newConfigCollection);
			break;
		}

		if (wakeConfigData) {
			if (configData)
				newConfig->setObject("BootConfigData", configData);
			newConfig->setObject("ConfigData", wakeConfigData);
			newConfig->removeObject("WakeConfigData");
		}
		objForArr = newConfigObj;
		arr = OSArray::withObjects(&objForArr, 1);
		if (arr) {
			hdaCodec->setProperty("HDAConfigDefault", arr);
			hdaCodec->setProperty("alc-pinconfig-status", kOSBooleanTrue);
			arr->release();
		} else {
			newConfig->release();
		}
		break;
	}
}

IOReturn AlcEnabler::initializePinConfigLegacy(IOService *hdaCodec) {
	auto parentDevice = hdaCodec->getParentEntry(gIOServicePlane);
	while (parentDevice) {
		auto name = parentDevice->getName();
		if (name && !strcmp(name, "AppleHDAController"))
			break;
		parentDevice = parentDevice->getParentEntry(gIOServicePlane);
	}
	if (parentDevice)
		callbackAlc->patchPinConfig(hdaCodec, parentDevice);
	else
		SYSLOG("alc", "no AppleHDAController parent found");
	return FunctionCast(initializePinConfigLegacy, callbackAlc->orgInitializePinConfigLegacy)(hdaCodec);
}

IOReturn AlcEnabler::initializePinConfig(IOService *hdaCodec, IOService *configDevice) {
	callbackAlc->patchPinConfig(hdaCodec, configDevice);
	return FunctionCast(initializePinConfig, callbackAlc->orgInitializePinConfig)(hdaCodec, configDevice);
}

// ---- Resource-load callbacks ----

void AlcEnabler::layoutLoadCallback(uint32_t requestTag, kern_return_t result, const void *resourceData, uint32_t resourceDataLength, void *context) {
	DBGLOG("alc", "layoutLoadCallback tag=%u result=%d", requestTag, result);
	callbackAlc->updateResource(Resource::Layout, result, resourceData, resourceDataLength);
	FunctionCast(layoutLoadCallback, callbackAlc->orgLayoutLoadCallback)(requestTag, result, resourceData, resourceDataLength, context);
}

void AlcEnabler::platformLoadCallback(uint32_t requestTag, kern_return_t result, const void *resourceData, uint32_t resourceDataLength, void *context) {
	DBGLOG("alc", "platformLoadCallback tag=%u result=%d", requestTag, result);
	callbackAlc->updateResource(Resource::Platform, result, resourceData, resourceDataLength);
	FunctionCast(platformLoadCallback, callbackAlc->orgPlatformLoadCallback)(requestTag, result, resourceData, resourceDataLength, context);
}

void AlcEnabler::updateResource(Resource type, kern_return_t &result, const void * &resourceData, uint32_t &resourceDataLength) {
	DBGLOG("alc", "resource request: %s", type == Resource::Platform ? "platform" : "layout");

	// Iterate through vendor/codec data from ResourceConverter — only ALC293 present
	for (size_t v = 0; v < ADDPR(vendorModSize); v++) {
		for (size_t c = 0; c < ADDPR(vendorMod)[v].codecsNum; c++) {
			auto &codec = ADDPR(vendorMod)[v].codecs[c];
			auto *files = (type == Resource::Platform) ? codec.platforms : codec.layouts;
			size_t num = (type == Resource::Platform) ? codec.platformNum : codec.layoutNum;

			for (size_t f = 0; f < num; f++) {
				auto &fi = files[f];
				if (fi.layout == DefaultLayoutId && KernelPatcher::compatibleKernel(fi.minKernel, fi.maxKernel)) {
					DBGLOG("alc", "matched %s layout %u, zlib=%u",
						type == Resource::Platform ? "platform" : "layout", fi.layout, isAppleHDAZlib);

					if (!isAppleHDAZlib) {
						uint32_t bufferLength = AppleHDADecompressBufferSize;
						auto buffer = Compression::decompress(Compression::ModeZLIB, &bufferLength, fi.data, fi.dataLength, nullptr);
						if (!buffer)
							break;
						resourceData = buffer;
						resourceDataLength = bufferLength;
					} else {
						resourceData = fi.data;
						resourceDataLength = fi.dataLength;
					}
					result = kOSReturnSuccess;
					return;
				}
			}
		}
	}
}

// ---- Legacy resource replacement (10.4/10.5) ----

OSDictionary* AlcEnabler::unserializeCodecDictionary(const uint8_t *data, uint32_t dataLength) {
	OSString *errorString = nullptr;
	uint32_t bufferLength = AppleHDADecompressBufferSize;

	auto buffer = Compression::decompress(Compression::ModeZLIB, &bufferLength, data, dataLength, nullptr);
	if (!buffer)
		return nullptr;

	OSDictionary *parsedDict = nullptr;
	if (bufferLength != 0) {
		auto parsedXML = OSUnserializeXML((char*) buffer, &errorString);
		if (parsedXML) {
			parsedDict = OSDynamicCast(OSDictionary, parsedXML);
			if (!parsedDict)
				parsedXML->release();
		}
		if (!parsedDict) {
			const char *err = (errorString && errorString->getCStringNoCopy()) ? errorString->getCStringNoCopy() : "unknown";
			SYSLOG("alc", "XML parse failed: %s", err);
		}
	}

	Buffer::deleter(buffer);
	return parsedDict;
}

bool AlcEnabler::AppleHDADriver_start(IOService *service, IOService *provider) {
	callbackAlc->replaceAppleHDADriverResources(service);
	return FunctionCast(AppleHDADriver_start, callbackAlc->orgAppleHDADriver_start)(service, provider);
}

bool AlcEnabler::AppleHDAPlatformDriver_start(IOService* service, IOService* provider) {
	callbackAlc->replaceAppleHDADriverResources(service);
	return FunctionCast(AppleHDAPlatformDriver_start, callbackAlc->orgAppleHDAPlatformDriver_start)(service, provider);
}

void AlcEnabler::replaceAppleHDADriverResources(IOService *service) {
	DBGLOG("alc", "replacing legacy resources");

	auto pathMapsArray = OSArray::withCapacity(4);
	auto layoutsArray = OSArray::withCapacity(4);
	if (!pathMapsArray || !layoutsArray) {
		SYSLOG("alc", "failed to create arrays");
		OSSafeReleaseNULL(pathMapsArray);
		OSSafeReleaseNULL(layoutsArray);
		return;
	}

	OSArray *codecInfoArray = nullptr;
	if (getKernelVersion() == KernelVersion::Tiger) {
		codecInfoArray = OSArray::withCapacity(4);
		if (!codecInfoArray) {
			OSSafeReleaseNULL(pathMapsArray);
			OSSafeReleaseNULL(layoutsArray);
			return;
		}
	}

	// Iterate through our single ALC293 codec entry
	for (size_t v = 0; v < ADDPR(vendorModSize); v++) {
		for (size_t c = 0; c < ADDPR(vendorMod)[v].codecsNum; c++) {
			auto &codec = ADDPR(vendorMod)[v].codecs[c];

			// Platform
			for (size_t f = 0; f < codec.platformNum; f++) {
				auto &fi = codec.platforms[f];
				if (fi.layout == DefaultLayoutId && KernelPatcher::compatibleKernel(fi.minKernel, fi.maxKernel)) {
					auto dict = unserializeCodecDictionary(fi.data, fi.dataLength);
					if (dict) {
						auto pm = dict->getObject("PathMaps");
						auto pmArr = pm ? OSDynamicCast(OSArray, pm) : nullptr;
						if (pmArr)
							pathMapsArray->merge(pmArr);
						dict->release();
					}
					break;
				}
			}

			// Layout
			for (size_t f = 0; f < codec.layoutNum; f++) {
				auto &fi = codec.layouts[f];
				if (fi.layout == DefaultLayoutId && KernelPatcher::compatibleKernel(fi.minKernel, fi.maxKernel)) {
					auto dict = unserializeCodecDictionary(fi.data, fi.dataLength);
					if (dict) {
						if (layoutIdIsOverridden) {
							auto num = OSNumber::withNumber(layoutIdOverride, 32);
							if (num) {
								dict->setObject("LayoutID", num);
								num->release();
							}
						}
						layoutsArray->setObject(dict);
						dict->release();
					}
					break;
				}
			}

			// 10.4 CodecInfo
			if (getKernelVersion() == KernelVersion::Tiger) {
				auto codecInfo = OSDictionary::withCapacity(3);
				if (codecInfo) {
					auto softVol = OSDictionary::withCapacity(1);
					auto vol = OSDictionary::withCapacity(1);
					auto sigProc = OSDictionary::withCapacity(1);
					auto analogOut = OSDictionary::withCapacity(1);
					auto codecId = OSNumber::withNumber(codec.codec | (codec.vendor << 16), 32);

					if (softVol && vol && sigProc && analogOut && codecId) {
						vol->setObject("SoftwareVolume", softVol);
						sigProc->setObject("Volume", vol);
						analogOut->setObject("SignalProcessing", sigProc);
						codecInfo->setObject("AnalogOut", analogOut);
						codecInfo->setObject("CodecID", codecId);
						codecInfoArray->setObject(codecInfo);
					}
					OSSafeReleaseNULL(softVol);
					OSSafeReleaseNULL(vol);
					OSSafeReleaseNULL(sigProc);
					OSSafeReleaseNULL(analogOut);
					OSSafeReleaseNULL(codecId);
					OSSafeReleaseNULL(codecInfo);
				}
			}
		}
	}

	service->setProperty("Layouts", layoutsArray);
	service->setProperty("PathMaps", pathMapsArray);
	if (getKernelVersion() == KernelVersion::Tiger) {
		service->setProperty("CodecInfo", codecInfoArray);
		codecInfoArray->release();
	}
	layoutsArray->release();
	pathMapsArray->release();
}
