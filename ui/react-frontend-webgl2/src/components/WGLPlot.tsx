import { useEffect, useRef } from "react";
import type { PlotData, View, HoverPhys } from "../types";
import { VERT, FRAG, compileShader } from "../utils/glsl";
import { PAD, drawAxes, drawCrosshair, drawOverlays } from "../utils/plotDraw";
import { getThemeColors } from "../utils/themeColors";
import { INIT_GPU_CAP, ZOOM_FACTOR, MIN_VISIBLE } from "../config/constants";

/* ── component ───────────────────────────────────────────────────────────── */

interface WGLPlotProps {
  id: string;
  dataRef: React.RefObject<PlotData>;
  height?: number | string;
  style?: React.CSSProperties;
  sampleRate?: number;
  actualSampleRate?: number | null;
  adcMax?: number;
  live?: boolean;
  windowSize?: number;
  /** When true, always show the full data range and ignore zoom/pan. */
  fitAll?: boolean;
  onWheel?: (e: React.WheelEvent<HTMLDivElement>) => void;
}

export function WGLPlot({
  id,
  dataRef,
  height = 400,
  style,
  sampleRate = 1,
  actualSampleRate = null,
  adcMax = 65535,
  live = false,
  windowSize = 50_000,
  fitAll = false,
  onWheel,
}: WGLPlotProps) {
  const wrapRef = useRef<HTMLDivElement>(null);
  const viewRef = useRef<View>({ xMin: 0, xMax: 1 });
  const autoFitRef = useRef(true);
  const wasLiveRef = useRef(false);
  const fitAllRef = useRef(fitAll);
  useEffect(() => {
    fitAllRef.current = fitAll;
  }, [fitAll]);
  const actualSampleRateRef = useRef(actualSampleRate);
  useEffect(() => {
    actualSampleRateRef.current = actualSampleRate;
  }, [actualSampleRate]);

  useEffect(() => {
    const _wrap = wrapRef.current;
    if (!_wrap) return;
    const wrap: HTMLDivElement = _wrap;

    /* ── three stacked canvases ──────────────────────────────────────── */
    const glCanvas = document.createElement("canvas");
    const axisCanvas = document.createElement("canvas");
    const hoverCanvas = document.createElement("canvas");

    for (const c of [glCanvas, axisCanvas, hoverCanvas]) {
      Object.assign(c.style, {
        position: "absolute",
        top: "0",
        left: "0",
        width: "100%",
        height: "100%",
      });
    }
    axisCanvas.style.pointerEvents = "none";
    hoverCanvas.style.pointerEvents = "none";
    glCanvas.style.cursor = "crosshair";

    wrap.appendChild(axisCanvas);
    wrap.appendChild(glCanvas);
    wrap.appendChild(hoverCanvas);

    /* ── pixel dimensions ────────────────────────────────────────────── */
    const dpr = window.devicePixelRatio || 1;
    let W = 0,
      H = 0;
    let dirty = true;

    function onResize() {
      const r = wrap.getBoundingClientRect();
      W = Math.round(r.width * dpr);
      H = Math.round(r.height * dpr);
      glCanvas.width = axisCanvas.width = hoverCanvas.width = W;
      glCanvas.height = axisCanvas.height = hoverCanvas.height = H;
      dirty = true;
    }
    onResize();
    const ro = new ResizeObserver(onResize);
    ro.observe(wrap);

    /* ── WebGL2 setup ────────────────────────────────────────────────── */
    const _gl = glCanvas.getContext("webgl2", {
      antialias: true,
      preserveDrawingBuffer: true,
    });
    if (!_gl) {
      wrap.insertAdjacentHTML(
        "beforeend",
        '<p style="color:salmon;padding:1rem;font-family:monospace">WebGL2 is required but not supported.</p>',
      );
      return;
    }
    const gl: WebGL2RenderingContext = _gl;
    const colors = getThemeColors();

    const prog = gl.createProgram();
    if (!prog) throw new Error("Failed to create WebGL program");
    gl.attachShader(prog, compileShader(gl, gl.VERTEX_SHADER, VERT));
    gl.attachShader(prog, compileShader(gl, gl.FRAGMENT_SHADER, FRAG));
    gl.linkProgram(prog);
    if (!gl.getProgramParameter(prog, gl.LINK_STATUS))
      throw new Error(gl.getProgramInfoLog(prog) ?? "Program link failed");

    const locs = {
      a_y: gl.getAttribLocation(prog, "a_y"),
      iOffset: gl.getUniformLocation(prog, "u_iOffset"),
      xMin: gl.getUniformLocation(prog, "u_xMin"),
      xMax: gl.getUniformLocation(prog, "u_xMax"),
      yMin: gl.getUniformLocation(prog, "u_yMin"),
      yMax: gl.getUniformLocation(prog, "u_yMax"),
      color: gl.getUniformLocation(prog, "u_color"),
    };

    const _vao = gl.createVertexArray();
    const _vbo = gl.createBuffer();
    if (!_vao || !_vbo) throw new Error("Failed to create VAO/VBO");
    const vao: WebGLVertexArrayObject = _vao;
    const vbo: WebGLBuffer = _vbo;

    gl.bindVertexArray(vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
    let gpuCap = INIT_GPU_CAP;
    gl.bufferData(gl.ARRAY_BUFFER, gpuCap * 4, gl.DYNAMIC_DRAW);
    gl.enableVertexAttribArray(locs.a_y);
    gl.vertexAttribPointer(locs.a_y, 1, gl.FLOAT, false, 0, 0);
    gl.bindVertexArray(null);

    let gpuUploaded = 0;

    const _ctx2 = axisCanvas.getContext("2d");
    const _ctxHover = hoverCanvas.getContext("2d");
    if (!_ctx2 || !_ctxHover) throw new Error("Failed to get 2D contexts");
    const ctx2: CanvasRenderingContext2D = _ctx2;
    const ctxHover: CanvasRenderingContext2D = _ctxHover;

    let hoverPhys: HoverPhys = null;

    /* ── view state (sample-index space) ────────────────────────────── */
    // Restore view across effect re-runs; suppress autoFit when leaving live mode
    let view: View = viewRef.current;
    let autoFit = wasLiveRef.current && !live ? false : autoFitRef.current;
    wasLiveRef.current = live;

    function tFromPhysX(physX: number): number {
      const pl = PAD.l * dpr;
      const pw = W - (PAD.l + PAD.r) * dpr;
      return Math.max(0, Math.min(1, (physX - pl) / pw));
    }

    function clampView(v: View, count: number): View {
      let { xMin, xMax } = v;
      const range = xMax - xMin;
      if (xMin < 0) {
        xMin = 0;
        xMax = range;
      }
      if (xMax > count - 1) {
        xMax = count - 1;
        xMin = xMax - range;
      }
      xMin = Math.max(0, xMin);
      xMax = Math.min(count - 1, xMax);
      return { xMin, xMax };
    }

    /* ── pointer / wheel interaction ─────────────────────────────────── */
    let drag: { startClientX: number; startView: View } | null = null;

    function onWheel(e: WheelEvent) {
      e.preventDefault();
      if (live && fitAllRef.current) return;
      const count = dataRef.current.count;
      if (count < 2) return;

      const r = glCanvas.getBoundingClientRect();
      const t = tFromPhysX((e.clientX - r.left) * dpr);
      const pivot = view.xMin + t * (view.xMax - view.xMin);

      const factor = e.deltaY > 0 ? ZOOM_FACTOR : 1 / ZOOM_FACTOR;
      const newMin = pivot + (view.xMin - pivot) * factor;
      const newMax = pivot + (view.xMax - pivot) * factor;

      if (newMax - newMin < MIN_VISIBLE) return;

      autoFit = false;
      view = clampView({ xMin: newMin, xMax: newMax }, count);
      autoFitRef.current = false;
      viewRef.current = view;
      dirty = true;
    }

    function onMouseDown(e: MouseEvent) {
      if (e.button !== 0) return;
      drag = { startClientX: e.clientX, startView: { ...view } };
      hoverPhys = null;
      glCanvas.style.cursor = "grabbing";
    }

    function onMouseMove(e: MouseEvent) {
      if (!drag) {
        const r = glCanvas.getBoundingClientRect();
        hoverPhys = {
          x: (e.clientX - r.left) * dpr,
          y: (e.clientY - r.top) * dpr,
        };
      }
      if (!drag) return;
      const count = dataRef.current.count;
      if (count < 2) return;

      if (live && fitAllRef.current) return;
      const pw = (W - (PAD.l + PAD.r) * dpr) / dpr;
      const dxSamples =
        ((drag.startClientX - e.clientX) / pw) *
        (drag.startView.xMax - drag.startView.xMin);

      autoFit = false;
      view = clampView(
        {
          xMin: drag.startView.xMin + dxSamples,
          xMax: drag.startView.xMax + dxSamples,
        },
        count,
      );
      autoFitRef.current = false;
      viewRef.current = view;
      dirty = true;
    }

    function onMouseUp() {
      drag = null;
      glCanvas.style.cursor = "crosshair";
    }
    function onMouseLeave() {
      drag = null;
      hoverPhys = null;
      glCanvas.style.cursor = "crosshair";
    }

    function onDblClick() {
      if (live && fitAllRef.current) return;
      const count = dataRef.current.count;
      autoFit = true;
      if (count >= 2) view = { xMin: 0, xMax: count - 1 };
      autoFitRef.current = true;
      viewRef.current = view;
      dirty = true;
    }

    glCanvas.addEventListener("wheel", onWheel, { passive: false });
    glCanvas.addEventListener("mousedown", onMouseDown);
    glCanvas.addEventListener("mousemove", onMouseMove);
    glCanvas.addEventListener("mouseup", onMouseUp);
    glCanvas.addEventListener("mouseleave", onMouseLeave);
    glCanvas.addEventListener("dblclick", onDblClick);

    /* ── RAF render loop ─────────────────────────────────────────────── */
    let rafId: number;

    function frame() {
      const { ys, count } = dataRef.current;

      /* incremental GPU upload — only new samples are transferred */
      if (count !== gpuUploaded) {
        if (count > gpuUploaded) {
          gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
          if (count > gpuCap) {
            gpuCap = count * 2;
            gl.bufferData(gl.ARRAY_BUFFER, gpuCap * 4, gl.DYNAMIC_DRAW);
            gpuUploaded = 0;
          }
          gl.bufferSubData(
            gl.ARRAY_BUFFER,
            gpuUploaded * 4,
            ys.subarray(gpuUploaded, count),
          );
        }
        gpuUploaded = count;
        dirty = true;
      }

      /* keep view in sync with streaming / auto-fit */
      if (count >= 2) {
        if (live && fitAllRef.current) {
          if (view.xMin !== 0 || view.xMax !== count - 1) {
            view = { xMin: 0, xMax: count - 1 };
            viewRef.current = view;
            dirty = true;
          }
        } else if (live) {
          const newMin = Math.max(0, count - windowSize);
          const newMax = count - 1;
          if (view.xMin !== newMin || view.xMax !== newMax) {
            view = { xMin: newMin, xMax: newMax };
            viewRef.current = view;
            dirty = true;
          }
        } else if (autoFit) {
          if (view.xMin !== 0 || view.xMax !== count - 1) {
            view = { xMin: 0, xMax: count - 1 };
            viewRef.current = view;
            dirty = true;
          }
        }
      }

      if (dirty && W > 0 && H > 0) {
        dirty = false;

        gl.clearColor(0, 0, 0, 0);
        gl.clear(gl.COLOR_BUFFER_BIT);
        ctx2.clearRect(0, 0, W, H);
        ctx2.fillStyle = colors.background;
        ctx2.fillRect(0, 0, W, H);

        if (count >= 2) {
          const { xMin, xMax } = view;
          const pl = PAD.l * dpr,
            pb = PAD.b * dpr,
            pt = PAD.t * dpr,
            pr = PAD.r * dpr;
          const pw = W - pl - pr;

          const drawFirst = Math.max(0, Math.floor(xMin));
          const drawLast = Math.min(count - 1, Math.ceil(xMax));
          const drawCount = drawLast - drawFirst + 1;

          if (drawCount > 0) {
            gl.viewport(pl, pb, pw, H - pt - PAD.b * dpr);
            gl.useProgram(prog);
            gl.uniform1i(locs.iOffset, drawFirst);
            gl.uniform1f(locs.xMin, xMin - drawFirst);
            gl.uniform1f(locs.xMax, xMax - drawFirst);
            gl.uniform1f(locs.yMin, 0);
            gl.uniform1f(locs.yMax, adcMax);
            gl.uniform3fv(locs.color, [57 / 255, 255 / 255, 110 / 255]);
            gl.bindVertexArray(vao);
            gl.drawArrays(gl.LINE_STRIP, drawFirst, drawCount);
            gl.bindVertexArray(null);
          }
        }

        drawAxes(
          ctx2,
          W,
          H,
          dpr,
          count,
          count >= 2 ? view.xMin : 0,
          count >= 2 ? view.xMax : 1,
          adcMax,
          colors,
          dataRef.current.chunkUsecs,
          dataRef.current.chunkCount,
        );
      }

      // crosshair is always redrawn — independent of the dirty flag
      if (W > 0 && H > 0) {
        drawCrosshair(
          ctxHover,
          W,
          H,
          dpr,
          dataRef.current.ys,
          dataRef.current.count,
          view,
          hoverPhys,
          sampleRate,
          adcMax,
          colors,
          dataRef.current.chunkUsecs,
          dataRef.current.chunkCount,
        );
        drawOverlays(
          ctxHover,
          W,
          H,
          dpr,
          dataRef.current.count,
          actualSampleRateRef.current,
          sampleRate,
          colors,
        );
      }

      rafId = requestAnimationFrame(frame);
    }

    rafId = requestAnimationFrame(frame);

    /* ── cleanup ─────────────────────────────────────────────────────── */
    return () => {
      cancelAnimationFrame(rafId);
      ro.disconnect();
      glCanvas.removeEventListener("wheel", onWheel);
      glCanvas.removeEventListener("mousedown", onMouseDown);
      glCanvas.removeEventListener("mousemove", onMouseMove);
      glCanvas.removeEventListener("mouseup", onMouseUp);
      glCanvas.removeEventListener("mouseleave", onMouseLeave);
      glCanvas.removeEventListener("dblclick", onDblClick);
      gl.deleteVertexArray(vao);
      gl.deleteBuffer(vbo);
      gl.deleteProgram(prog);
      glCanvas.remove();
      axisCanvas.remove();
      hoverCanvas.remove();
    };
  }, [dataRef, live, windowSize, sampleRate, adcMax]);

  return (
    <div
      id={id}
      ref={wrapRef}
      className="relative w-full overflow-hidden rounded-sm bg-card"
      style={{ height, ...style }}
      onWheel={onWheel}
    />
  );
}
