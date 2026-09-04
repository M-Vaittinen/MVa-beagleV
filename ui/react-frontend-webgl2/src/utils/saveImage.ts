export function saveCanvasesAsImage(divId: string): void {
  const div = document.getElementById(divId);
  if (!div) return;
  const canvases = div.querySelectorAll("canvas");

  const output = document.createElement("canvas");
  output.width = div.offsetWidth;
  output.height = div.offsetHeight;
  const ctx = output.getContext("2d");
  if (!ctx) return;

  canvases.forEach((canvas) => {
    const divRect = div.getBoundingClientRect();
    const canvasRect = canvas.getBoundingClientRect();
    const x = canvasRect.left - divRect.left;
    const y = canvasRect.top - divRect.top;
    ctx.drawImage(canvas, x, y, canvas.offsetWidth, canvas.offsetHeight);
  });

  const link = document.createElement("a");
  link.download = "output.png";
  link.href = output.toDataURL("image/png");
  link.click();
}
