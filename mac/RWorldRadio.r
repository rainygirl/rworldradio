#include "Processes.r"
resource 'SIZE' (-1) {
	reserved, acceptSuspendResumeEvents, reserved, canBackground, doesActivateOnFGSwitch,
	backgroundAndForeground, dontGetFrontClicks, ignoreChildDiedEvents, is32BitCompatible,
	isHighLevelEventAware, onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
	reserved, reserved, reserved,
	24 * 1024 * 1024, 12 * 1024 * 1024
};
