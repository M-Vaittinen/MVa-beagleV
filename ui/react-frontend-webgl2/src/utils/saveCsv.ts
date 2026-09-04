import type { PlotData } from "../types";
import { MAX_SAMPS } from "../config/constants";

function sampleToUsecs(
  idx: number,
  chunkUsecs: Float64Array,
  chunkCount: number,
): number | null {
  if (chunkCount === 0) return null;
  const ci = Math.floor(idx / MAX_SAMPS);
  const frac = (idx % MAX_SAMPS) / MAX_SAMPS;

  if (ci + 1 < chunkCount) {
    return chunkUsecs[ci] + frac * (chunkUsecs[ci + 1] - chunkUsecs[ci]);
  }
  if (ci < chunkCount) {
    if (chunkCount >= 2) {
      const rate = chunkUsecs[chunkCount - 1] - chunkUsecs[chunkCount - 2];
      return chunkUsecs[ci] + frac * rate;
    }
    return chunkUsecs[0];
  }
  if (chunkCount >= 2) {
    const rate = chunkUsecs[chunkCount - 1] - chunkUsecs[chunkCount - 2];
    return chunkUsecs[chunkCount - 1] + (ci - chunkCount + 1 + frac) * rate;
  }
  return chunkUsecs[0];
}

export function saveAsCsv(
  data: PlotData,
  onError: (msg: string) => void,
): void {
  try {
    const { ys, count, chunkUsecs, chunkCount } = data;
    if (count === 0) throw new Error("No data to export");

    const hasTime = chunkCount > 0;
    const header = hasTime ? "time_us,adc_value\n" : "adc_value\n";
    const t0 = hasTime ? (sampleToUsecs(0, chunkUsecs, chunkCount) ?? 0) : 0;

    const CHUNK = 65536;
    const parts: string[] = [header];

    for (let i = 0; i < count; i += CHUNK) {
      const end = Math.min(i + CHUNK, count);
      const rows: string[] = [];
      for (let j = i; j < end; j++) {
        if (hasTime) {
          const t = sampleToUsecs(j, chunkUsecs, chunkCount);
          rows.push(`${t !== null ? (t - t0).toFixed(3) : ""},${ys[j]}`);
        } else {
          rows.push(`${ys[j]}`);
        }
      }
      parts.push(rows.join("\n") + "\n");
    }

    const blob = new Blob(parts, { type: "text/csv" });
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = "adc_data.csv";
    link.click();
    URL.revokeObjectURL(url);
  } catch (err) {
    onError(err instanceof Error ? err.message : String(err));
  }
}
