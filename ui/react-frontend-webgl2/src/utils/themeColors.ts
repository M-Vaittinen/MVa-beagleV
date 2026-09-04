/** Resolve a CSS color string (including oklch) to WebGL-ready RGB floats. */
function cssColorToGl(color: string): [number, number, number] {
  const canvas = document.createElement("canvas");
  canvas.width = canvas.height = 1;
  const ctx = canvas.getContext("2d")!;
  ctx.fillStyle = color;
  ctx.fillRect(0, 0, 1, 1);
  const [r, g, b] = ctx.getImageData(0, 0, 1, 1).data;
  return [r / 255, g / 255, b / 255];
}

/** Inject alpha into a CSS color value, e.g. oklch(L C H) → oklch(L C H / 0.2) */
function withAlpha(cssColor: string, alpha: number): string {
  return cssColor.replace(")", ` / ${alpha})`);
}

export interface ThemeColors {
  // Canvas 2D colors (CSS strings)
  border: string;
  mutedForeground: string;
  foreground: string;
  background: string;
  popover: string;
  popoverForeground: string;
  // Derived with alpha
  foreground20: string;
  foreground40: string;
  foreground60: string;
  popover90: string;
  // WebGL colors (RGB 0–1 floats)
  chart1Gl: [number, number, number];
  cardGl: [number, number, number];
  grid: string;
}

/** Read shadcn CSS custom properties from the document and resolve them. */
export function getThemeColors(): ThemeColors {
  const s = getComputedStyle(document.documentElement);
  const v = (name: string) => s.getPropertyValue(name).trim();

  const border = v("--border");
  const mutedForeground = v("--muted-foreground");
  const foreground = v("--foreground");
  const background = v("--background");
  const popover = v("--popover");
  const popoverForeground = v("--popover-foreground");
  const chart1 = v("--chart-1");
  const card = v("--card");

  return {
    border,
    mutedForeground,
    foreground,
    background,
    popover,
    popoverForeground,
    foreground20: withAlpha(foreground, 0.2),
    foreground40: withAlpha(foreground, 0.4),
    foreground60: withAlpha(foreground, 0.6),
    popover90: withAlpha(popover, 0.9),
    chart1Gl: cssColorToGl(chart1),
    cardGl: cssColorToGl(card),
    grid: withAlpha(foreground, 0.08),
  };
}
