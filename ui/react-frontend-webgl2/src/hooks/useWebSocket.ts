import { useState, useEffect, useRef, useCallback } from "react";
import {
  WS_URL,
  MAX_SAMPS,
  CHUNK_BYTES,
} from "../config/constants";
import type { Command, ServerMessage } from "../types/commands";

type ConnectionStatus = "connected" | "disconnected" | "error";

/**
 * Decoded result from one WebSocket binary frame.
 */
export interface ParsedFrame {
  samples: Float32Array;
  chunkUsecs: number[];
}

/**
 * Decode one WebSocket binary frame containing one or more mvaring chunks.
 *
 * Wire format per chunk (little-endian):
 *   [usecs: uint32][samples: MAX_SAMPS × uint32][gpio_lev0: MAX_SAMPS × uint32]
 *
 * ADC extraction mirrors the C macro ADC_RAW_VAL:
 *   byte_swap16(raw & 0xFFFF) & adcMask  →  ADC value (0..adcMask)
 *
 * adcMask must be 2^n - 1 (e.g. 2047, 4095, 65535).
 */
function parseAdcFrame(buf: ArrayBuffer): ParsedFrame {
  const numChunks = Math.floor(buf.byteLength / CHUNK_BYTES);
  if (numChunks === 0) return { samples: new Float32Array(0), chunkUsecs: [] };

  const samples = new Float32Array(numChunks * MAX_SAMPS);
  const chunkUsecs: number[] = [];
  const dv = new DataView(buf);
  let outIdx = 0;

  for (let c = 0; c < numChunks; c++) {
    const chunkBase = c * CHUNK_BYTES;
    chunkUsecs.push(dv.getUint32(chunkBase, true));

    const samplesOffset = chunkBase + 4; // skip usecs
    for (let i = 0; i < MAX_SAMPS; i++)
      samples[outIdx++] = dv.getFloat32(samplesOffset + i * 4, true);
  }

  return { samples: samples.subarray(0, outIdx), chunkUsecs };
}

interface UseWebSocketParams {
  url?: string;
  onData: (frame: ParsedFrame) => void;
  /** Bitmask derived from the selected ADC bit depth, e.g. 2047 (11-bit) or 4095 (12-bit). */
  adcMax?: number;
  /** Called when the server sends a JSON info message. */
  onInfo?: (msg: ServerMessage) => void;
}

interface UseWebSocketReturn {
  status: ConnectionStatus;
  streaming: boolean;
  sendCommand: (cmd: Command) => void;
}

const RECONNECT_INTERVAL_MS = 2000;

export function useWebSocket({
  url = WS_URL,
  onData,
  onInfo,
}: UseWebSocketParams): UseWebSocketReturn {
  const [status, setStatus] = useState<ConnectionStatus>("disconnected");
  const [streaming, setStreaming] = useState(false);

  const wsRef = useRef<WebSocket | null>(null);
  const reconnectTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  // Refs so changes to onData/adcMax/onInfo never trigger WS reconnection
  const onDataRef = useRef(onData);
  useEffect(() => {
    onDataRef.current = onData;
  }, [onData]);
  const onInfoRef = useRef(onInfo);
  useEffect(() => {
    onInfoRef.current = onInfo;
  }, [onInfo]);

  useEffect(() => {
    let cancelled = false;

    function connect() {
      if (cancelled) return;

      const ws = new WebSocket(url);
      ws.binaryType = "arraybuffer";
      wsRef.current = ws;

      ws.onopen = () => {
        if (cancelled) return;
        setStatus("connected");
      };

      ws.onclose = () => {
        if (cancelled) return;
        setStatus("disconnected");
        setStreaming(false);
        reconnectTimerRef.current = setTimeout(connect, RECONNECT_INTERVAL_MS);
      };

      ws.onerror = () => {
        if (cancelled) return;
        setStatus("error");
      };

      ws.onmessage = (e: MessageEvent) => {
        if (cancelled) return;
        if (typeof e.data === "string") {
          try {
            const msg = JSON.parse(e.data) as ServerMessage;
            if (msg.type === "stopped") {
              setStreaming(false);
            }
            onInfoRef.current?.(msg);
          } catch {
            // ignore malformed text frames
          }
          return;
        }
        if (!(e.data instanceof ArrayBuffer)) return;
        const frame = parseAdcFrame(e.data);

        if (frame.samples.length > 0) {
          // // DEBUG: mirror the Python [adc] log for comparison
          // const N_PREVIEW = 8;
          // const usecs = frame.chunkUsecs[0] ?? 0;
          // const adcParts = Array.from(
          //   { length: N_PREVIEW },
          //   (_, i) => `[${i}]=${frame.samples[i]}`,
          // );
          // console.log(
          //   `[adc] chunks=${frame.chunkUsecs.length} usecs=${usecs} ${adcParts.join(" ")} bytes=${e.data.byteLength}`,
          // );
          onDataRef.current?.(frame);
        }
      };
    }

    reconnectTimerRef.current = setTimeout(connect, 50);

    return () => {
      cancelled = true;
      if (reconnectTimerRef.current) clearTimeout(reconnectTimerRef.current);
      wsRef.current?.close();
    };
  }, [url]);

  const sendCommand = useCallback((cmd: Command) => {
    if (wsRef.current?.readyState !== WebSocket.OPEN) return;
    wsRef.current.send(JSON.stringify(cmd));
    if (cmd.command === "start") {
      setStreaming(true);
    } else {
      // Immediate UI fallback; authoritative false comes from { type: "stopped" }
      setStreaming(false);
    }
  }, []);

  return { status, streaming, sendCommand };
}
