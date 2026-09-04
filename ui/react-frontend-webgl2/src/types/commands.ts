export type Command =
  | {
      command: "start";
      sampleRate: number;
      durationMs?: number;
      uri: string;
      device: string;
      channel: string;
    }
  | { command: "stop" }
  | { command: "get_devices"; uri: string }
  | { command: "get_channels"; uri: string; device: string }

export type ServerMessage =
  | { type: "started" }
  | { type: "stopped"; actualDurationMs: number }
  | { type: "actual_sample_rate"; value: number }
  | { type: "error"; message: string }
  | { type: "get_devices"; devices: string[] }
  | { type: "get_channels"; channels: string[] };
