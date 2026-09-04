function renderFavicon() {
  const size = 32;
  const canvas = document.createElement("canvas");
  canvas.width = canvas.height = size;
  const ctx = canvas.getContext("2d")!;

  const s = getComputedStyle(document.documentElement);
  const bg = s.getPropertyValue("--primary").trim();
  const fg = s.getPropertyValue("--primary-foreground").trim();

  // background with rounded corners
  const r = 6;
  ctx.fillStyle = bg;
  ctx.beginPath();
  ctx.roundRect(0, 0, size, size, r);
  ctx.fill();

  // "ADC" text
  ctx.fillStyle = fg;
  ctx.font = `bold 16px monospace`;
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.fillText("ADC", size / 2, size / 2);

  const link = document.querySelector(
    "link[rel='icon']",
  ) as HTMLLinkElement | null;
  if (link) link.href = canvas.toDataURL();
}

/** Set favicon and watch for theme changes (dark/light class on <html>). */
export function initFavicon() {
  renderFavicon();

  const observer = new MutationObserver(renderFavicon);
  observer.observe(document.documentElement, {
    attributes: true,
    attributeFilter: ["class"],
  });
}
