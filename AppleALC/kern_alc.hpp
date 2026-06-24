//
//  kern_alc.hpp
//  AppleALC
//
//  Dedicated ALC293 driver for Lenovo T460/T560, layout-id 29.
//  Based on AppleALC by vit9696.
//

#ifndef kern_alc_hpp
#define kern_alc_hpp

#include <Headers/kern_patcher.hpp>
#include <Headers/kern_devinfo.hpp>

#include "kern_resources.hpp"

class AlcEnabler {
public:
	void init();
	void deinit();

	static void createShared();

	static AlcEnabler* getShared() {
		return callbackAlc;
	}

	/**
	 *  executeVerb method symbol
	 */
#if defined(__i386__)
	static constexpr const char *symIOHDACodecDevice_executeVerb = "__ZN16IOHDACodecDevice11executeVerbEtttPmb";
#elif defined(__x86_64__)
	static constexpr const char *symIOHDACodecDevice_executeVerb = "__ZN16IOHDACodecDevice11executeVerbEtttPjb";
#else
#error Unsupported arch
#endif

	static IOReturn IOHDACodecDevice_executeVerb(void *hdaCodecDevice, uint16_t nid, uint16_t verb, uint16_t param, unsigned int *output, bool waitForSuccess);

	mach_vm_address_t orgIOHDACodecDevice_executeVerb {0};

	/**
	 *  ALC293 codec constants: Realtek vendor (0x10EC), codec ID (0x0293)
	 */
	static constexpr uint16_t AlcCodecVendor {0x10EC};
	static constexpr uint16_t AlcCodecId     {0x0293};

	/**
	 *  Default layout-id for ALC293 (Lenovo T460/T560)
	 */
	static constexpr uint32_t DefaultLayoutId {29};

private:
	static AlcEnabler* callbackAlc;

	// ---- Device property injection ----

	void updateProperties();
	void updateDeviceProperties(IORegistryEntry *hdaService, DeviceInfo *info, const char *hdaGfx, bool isAnalog);

	// ---- Kext patching ----

	void processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size);
	void eraseRedundantLogs(KernelPatcher &patcher, size_t index);
	void applyPatches(KernelPatcher &patcher, size_t index, const KextPatch *patches, size_t patchesNum);

	// ---- AppleGFXHDA probe hook (prevent HDEF claim) ----

	static IOService *gfxProbe(IOService *ctrl, IOService *provider, SInt32 *score);
	mach_vm_address_t orgGfxProbe {0};

	// ---- AppleHDAController::start hook (warm-reboot fix + delay) ----

	static bool AppleHDAController_start(IOService* service, IOService* provider);
	static void resetHDAControllerConfig(IOService *provider);
	mach_vm_address_t orgAppleHDAController_start {0};

	// ---- performPowerChange hook (wake-verb reinit) ----

#if defined(__i386__)
	static constexpr const char *symPerformPowerChange = "__ZN14AppleHDADriver23performPowerStateChangeE24_IOAudioDevicePowerStateS0_Pm";
#elif defined(__x86_64__)
	static constexpr const char *symPerformPowerChange = "__ZN14AppleHDADriver23performPowerStateChangeE24_IOAudioDevicePowerStateS0_Pj";
#else
#error Unsupported arch
#endif

	enum ALCAudioDevicePowerState {
		ALCAudioDeviceSleep  = 0,
		ALCAudioDeviceIdle   = 1,
		ALCAudioDeviceActive = 2
	};

	static IOReturn performPowerChange(IOService *hdaDriver, uint32_t from, uint32_t to, unsigned int *timer);
	mach_vm_address_t orgPerformPowerChange {0};

	// ---- PinConfig hooks ----

	void patchPinConfig(IOService *hdaCodec, IORegistryEntry *configDevice);
	static IOReturn initializePinConfigLegacy(IOService *hdaCodec);
	static IOReturn initializePinConfig(IOService *hdaCodec, IOService *configDevice);
	mach_vm_address_t orgInitializePinConfigLegacy {0};
	mach_vm_address_t orgInitializePinConfig {0};

	// ---- Resource-load callbacks ----

	static void layoutLoadCallback(uint32_t requestTag, kern_return_t result, const void *resourceData, uint32_t resourceDataLength, void *context);
	static void platformLoadCallback(uint32_t requestTag, kern_return_t result, const void *resourceData, uint32_t resourceDataLength, void *context);
	mach_vm_address_t orgLayoutLoadCallback {0};
	mach_vm_address_t orgPlatformLoadCallback {0};

	enum class Resource { Layout, Platform };
	void updateResource(Resource type, kern_return_t &result, const void * &resourceData, uint32_t &resourceDataLength);

	// ---- Legacy (10.4/10.5) resource replacement ----

	static bool AppleHDADriver_start(IOService* service, IOService* provider);
	static bool AppleHDAPlatformDriver_start(IOService* service, IOService* provider);
	mach_vm_address_t orgAppleHDADriver_start {0};
	mach_vm_address_t orgAppleHDAPlatformDriver_start {0};

	void replaceAppleHDADriverResources(IOService *service);
	OSDictionary *unserializeCodecDictionary(const uint8_t *data, uint32_t dataLength);

	// ---- Entitlement hook ----

	static void handleAudioClientEntitlement(task_t task, const char *entitlement, OSObject *&original);

	// ---- Layout-id helper ----

	static uint32_t getAudioLayout(IOService *hdaDriver);

	bool layoutIdIsOverridden {false};
	uint32_t layoutIdOverride {0};
	bool isAppleHDAZlib {false};

	// State tracking for processKext
	struct ProcessingState {
		enum {
			NotReady               = 0,
			ControllersLoaded      = 1,
			CodecsLoaded           = 2,
			CallbacksWantRouting   = 4,
			PatchHDAFamily         = 8,
			PatchHDAController     = 16,
			PatchHDAPlatformDriver = 32
		};
	};
	volatile int progressState {ProcessingState::NotReady};

	static constexpr uint32_t MaxAlcDelay {3000};
	static constexpr uint32_t AppleHDADecompressBufferSize {0x7A000};
};

#endif /* kern_alc_hpp */
