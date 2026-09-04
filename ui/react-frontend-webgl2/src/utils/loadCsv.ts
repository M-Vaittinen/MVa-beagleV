import type { PlotData } from "../types";
import { MAX_SAMPS } from "../config/constants";

export function loadCsvFile(
  file: File,
  onDone: (data: PlotData) => void,
  onError: (msg: string) => void,
): void {
  const reader = new FileReader();
  reader.onload = (e) => {
    try {
      const result = e.target?.result;

      // Null check + type narrowing in one shot
      if (typeof result !== "string") {
        throw new Error("Failed to read file as text");
      }

      if (!result.startsWith("time_us,adc_value")) {
        throw new Error("Invalid CSV format");
      }

      onDone(parseCsv(result));
    } catch (err) {
      onError(err instanceof Error ? err.message : String(err));
    }
  };
  reader.readAsText(file);
}

function parseCsv(text: string): PlotData {
  const lines = text.split("\n");
  if (lines.length < 2) throw new Error("CSV has no data rows");

  const header = lines[0].trim().toLowerCase();
  const hasTime = header.includes("time_us");
  // columns: [time_us,] adc_value
  const adcCol = hasTime ? 1 : 0;

  // first pass: count valid rows
  let count = 0;
  for (let i = 1; i < lines.length; i++) {
    if (lines[i].trim() !== "") count++;
  }
  if (count === 0) throw new Error("CSV has no data rows");

  const ys = new Float32Array(count);
  const chunkCount = hasTime ? Math.ceil(count / MAX_SAMPS) : 0;
  const chunkUsecs = new Float64Array(Math.max(chunkCount, 1));

  let row = 0;
  for (let i = 1; i < lines.length; i++) {
    const line = lines[i].trim();
    if (line === "") continue;

    const cols = line.split(",");
    const adc = parseFloat(cols[adcCol]);
    if (!isFinite(adc)) throw new Error(`Bad ADC value on line ${i + 1}`);
    ys[row] = adc;

    if (hasTime) {
      // Store one timestamp per chunk: use the first sample of each chunk
      const ci = Math.floor(row / MAX_SAMPS);
      if (row % MAX_SAMPS === 0) {
        const t = parseFloat(cols[0]);
        if (!isFinite(t)) throw new Error(`Bad time_us on line ${i + 1}`);
        chunkUsecs[ci] = t;
      }
    }

    row++;
  }

  return { ys, count, chunkUsecs, chunkCount };
}
