declare const __APP_VERSION__: string;
export const APP_VERSION = __APP_VERSION__;

const wsHost = import.meta.env.VITE_WS_HOST ?? window.location.hostname;
// const wsHost = window.location.hostname;
export const WS_URL = `ws://${wsHost}:80/ws`;
export const NOT_IMPLEMENTED = true;

// mvaring chunk layout (must match struct adc_data in C code)
export const MAX_SAMPS = 36864;
export const CHUNK_BYTES = 4 + MAX_SAMPS * 4 + MAX_SAMPS * 4;

// CPU / GPU buffer pre-allocation
export const INIT_CAP = 1_000_000;
export const INIT_GPU_CAP = 1_000_000;

// Plot interaction
export const ZOOM_FACTOR = 1.15;
export const MIN_VISIBLE = 10; // minimum samples visible when zoomed in

// Streaming defaults
export const DEFAULT_SAMPLE_RATE = 200_000;
export const LIVE_WINDOW_SIZE = 50_000;
export const LIVE_WINDOW_MIN = 100;
export const LIVE_WINDOW_MAX = 1_000_000;
export const LIVE_WINDOW_STEP_SIZE = 100;
