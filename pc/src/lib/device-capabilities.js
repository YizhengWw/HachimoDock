// Keep P4's legacy defaults; new devices must explicitly advertise a feature.
export function supportsDeviceSessions(usb) {
  return usb?.runtime === "esp-p4"
    || (usb?.runtime === "esp-s3" && usb?.capabilities?.sessionSync === true);
}

export function isStopwatchDevice(usb) {
  return usb?.runtime === "esp-s3"
    && usb?.capabilities?.boardId === "m5stack-stopwatch";
}

export function hasFixedDeviceAppearance(usb) {
  return isStopwatchDevice(usb) && usb?.capabilities?.maxAppearanceSlots === 0;
}
