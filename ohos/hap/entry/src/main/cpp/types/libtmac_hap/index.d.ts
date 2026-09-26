/**
 * LUT-SA T-MAC native module (libtmac_hap.so).
 * Runs the LUT kernel on the device/emulator and returns a status string.
 */
export const selfTest: () => string;
export const bench: (steps: number) => string;
