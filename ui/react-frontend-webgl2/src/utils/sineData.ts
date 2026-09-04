import type { PlotData } from "../types";

export function generateSineData(n: number = 5_000_000): PlotData {
  const ys = new Float32Array(n);
  const mid = 32767.5;
  const amp = 32767.5;
  // Three overlaid sine frequencies so zoomed regions stay visually interesting
  for (let i = 0; i < n; i++) {
    ys[i] =
      mid +
      amp * 0.6 * Math.sin((2 * Math.PI * i) / 1000) +
      amp * 0.3 * Math.sin((2 * Math.PI * i) / 137) +
      amp * 0.1 * Math.sin((2 * Math.PI * i) / 29);
  }
  return { ys, count: n, chunkUsecs: new Float64Array(0), chunkCount: 0 };
}
