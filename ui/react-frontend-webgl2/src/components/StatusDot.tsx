type ConnectionStatus = "connected" | "disconnected" | "error";

interface StatusDotProps {
  status: ConnectionStatus;
  streaming: boolean;
}

export function StatusDot({ status, streaming }: StatusDotProps) {
  const active = status === "connected" && streaming;
  const idle = status === "connected" && !streaming;

  const background = active ? "#39ff6e" : idle ? "#585b70" : "#e06c75";
  const boxShadow = active ? "0 0 8px #39ff6e" : "none";

  return (
    <span
      style={{
        display: "inline-block",
        width: 8,
        height: 8,
        borderRadius: "50%",
        background,
        boxShadow,
        flexShrink: 0,
      }}
      title={`${status}${streaming ? " · streaming" : ""}`}
    />
  );
}
