"""
ADC WebSocket + Static File Server
------------------------------------
Reads real ADC data directly from the mvaring shared-memory ring buffer and
forwards it to the React frontend via WebSocket as raw binary frames.
Falls back to simulated data when the ring buffer is unavailable.

  GET  /*         -> serves React app (react-frontend/dist/)
  WS   /ws        -> ADC binary stream (send "start" / "stop")

Binary wire format (WebSocket binary frames):
  One or more concatenated mvaring chunks, each:
    [usecs:    uint32 LE ]          -- microsecond timestamp
    [samples:  MAX_SAMPS × uint32 LE] -- raw SPI words; decode with ADC_RAW_VAL()
    [gpio_lev0:MAX_SAMPS × uint32 LE] -- GPIO level snapshot per sample

  ADC value extraction: byte_swap16(sample & 0xFFFF), masked client-side
    based on the selected ADC bit depth (e.g. & 0x7FF for 11-bit, & 0xFFF for 12-bit)

Environment variables:
  ADC_SERVER_PORT     Port this server listens on   (default: 8765)
  ADC_USE_SIM         Set to "1" to force simulation mode

Install:  pip install aiohttp
Run:      python adc_server.py
"""

import asyncio
import json
import math
import os
import random
import struct
import sys
import time
import logging
from pathlib import Path
from aiohttp import web
import libiio_wrapper as iio

logging.basicConfig(
    level=logging.DEBUG,
    format="%(asctime)s %(levelname)-8s %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("adc_server")
logging.getLogger("aiohttp").setLevel(logging.ERROR)

MAX_SAMPS = 36864

# ── Configuration ─────────────────────────────────────────────────────────────
SERVER_PORT = int(os.environ.get("ADC_SERVER_PORT", "80"))
# Force simulation if the env variable is set OR if mvaring is not available.
USE_SIM = os.environ.get("ADC_USE_SIM", "").lower() in ("1", "true", "yes")

# Simulation parameters
# Waveform amplitudes are scaled for the 12-bit ADC range (0..4095).
VIRTUAL_RATE = 4000   # simulated samples/sec
ADC_MID      = 2048   # midpoint of 12-bit range
NOISE_AMP    = 12     # gaussian noise amplitude (12-bit scale)

# DIST_DIR = Path(__file__).resolve().parent / "react-frontend" / "dist"
DIST_DIR = Path(__file__).resolve().parent / "react-frontend-webgl2" / "dist"

BASE_DIR = os.path.dirname(os.path.abspath(__file__))


# ── CSV-to-binary conversion (legacy / TCP backup path) ──────────────────────
def csv_to_binary(csv_line: str) -> bytes | None:
    """Parse a CSV line of integer ADC values into little-endian Uint16 bytes."""
    try:
        values = [max(0, min(65535, int(v)))
                  for v in csv_line.split(",") if v.strip()]
        if not values:
            return None
        return struct.pack(f"<{len(values)}H", *values)
    except (ValueError, struct.error):
        return None


# ── Simulation data generator ─────────────────────────────────────────────────
def encode_adc_val(val: int) -> int:
    """Encode a 12-bit ADC value into the raw SPI word format used by MCP3202.

    This is the inverse of the C macro:
        ADC_RAW_VAL(d) = (((uint16_t)(d)<<8 | (uint16_t)(d)>>8) & 0xfff)

    So encode_adc_val(v) produces a raw word d such that ADC_RAW_VAL(d) == v.
    """
    val &= 0xFFF  # clamp to 12 bits
    return ((val << 8) | (val >> 8)) & 0xFFFF

def generate_sim_batch(t_offset: float, rate_hz: int = VIRTUAL_RATE) -> bytes:
    """Generate one simulated mvaring chunk in the same binary format as real data.

    Output binary layout (identical to struct adc_data serialised LE):
        [usecs:     uint32 LE           ]   4 bytes
        [samples:   MAX_SAMPS × uint32 LE]  MAX_SAMPS * 4 bytes
        [gpio_lev0: MAX_SAMPS × uint32 LE]  MAX_SAMPS * 4 bytes  (zeroed)

    Sample values are raw SPI words encoded with encode_adc_val(), so the
    JavaScript consumer can apply the same ADC_RAW_VAL extraction as for real
    data without any special-casing for the simulation path.
    """
    usecs = int(t_offset * 1_000_000) & 0xFFFF_FFFF
    samples = []
    for i in range(MAX_SAMPS):
        t = t_offset + i / rate_hz
        drift    = 150 * math.sin(2 * math.pi * 0.05 * t)
        periodic = 250 * math.sin(2 * math.pi * 1.2  * t)
        harmonic =  60 * math.sin(2 * math.pi * 3.6  * t)
        noise    = random.gauss(0, NOISE_AMP)
        value    = int(ADC_MID + drift + periodic + harmonic + noise)
        value    = max(0, min(4095, value))  # clamp to 12-bit range
        samples.append(encode_adc_val(value))
    return (struct.pack("<I", usecs)
            + struct.pack(f"<{MAX_SAMPS}I", *samples)
            + bytes(MAX_SAMPS * 4))  # gpio_lev0 zeroed for simulation


# ── WebSocket handler ─────────────────────────────────────────────────────────
async def ws_handler(request):
    ws = web.WebSocketResponse()
    await ws.prepare(request)

    streaming      = False
    task           = None
    streamer_proc  = None
    stderr_task    = None
    watcher_task   = None
    duration_task  = None
    t_stream_start = None

    async def kill_streamer(proc, stderr_log_task, watch_task):
        """Send SIGINT for graceful DMA shutdown, wait, then SIGKILL if needed."""
        if watch_task:
            watch_task.cancel()
        if proc is None or proc.returncode is not None:
            if stderr_log_task:
                stderr_log_task.cancel()
            return
        try:
            import signal
            proc.send_signal(signal.SIGINT)   # graceful: lets rpi_adc_stream release DMA
            await asyncio.wait_for(proc.wait(), timeout=5.0)
        except asyncio.TimeoutError:
            proc.kill()
            await proc.wait()
        if stderr_log_task:
            await asyncio.gather(stderr_log_task, return_exceptions=True)
        if watch_task:
            await asyncio.gather(watch_task, return_exceptions=True)
        log.info("rpi_adc_stream process stopped (pid %d, rc %d)", proc.pid, proc.returncode)

    async def stream_from_libiio(uri: str, dev: str, chan: str):
        """Stream blocks from LibIIO using the C/Python wrapper"""

        try:
            if iio.connect(uri):
                iio.disconnect()
                return

            devices = iio.get_devices()
            if not devices:
                iio.disconnect()
                return
            device_index = devices.index(dev)
            iio.set_device(device_index)

            channels = iio.get_channels(device_index)
            if not channels:
                iio.disconnect()
                return
            iio.set_channel(channels.index(chan))

            while True:
                samples = iio.get()[:MAX_SAMPS]
                payload = (struct.pack("<I", 0) +
                            struct.pack(f"<{MAX_SAMPS}f", *samples) +
                            bytes(MAX_SAMPS * 4))
                await ws.send_bytes(payload)

        except asyncio.CancelledError:
            iio.disconnect()
            pass

    async def stream_sim(rate_hz: int):
        """Generate simulated ADC data in the same binary format as real mvaring chunks."""
        # Batch multiple chunks per send (same as RING_READ_CHUNKS for real ADC)
        # so the send rate stays manageable at high sample rates.
        chunks_per_send = max(1, rate_hz // (MAX_SAMPS * 50))  # target ~50 sends/sec
        samples_per_send = MAX_SAMPS * chunks_per_send
        send_interval = samples_per_send / rate_hz  # seconds between sends
        sample_counter = 0
        try:
            loop      = asyncio.get_event_loop()
            next_send = loop.time()
            while True:
                payload = b"".join(
                    generate_sim_batch(
                        (sample_counter + i * MAX_SAMPS) / rate_hz,
                        rate_hz,
                    )
                    for i in range(chunks_per_send)
                )
                await ws.send_bytes(payload)
                sample_counter += samples_per_send
                next_send      += send_interval
                # Always sleep (even 0) so the event loop can process stop/duration.
                await asyncio.sleep(max(0.0, next_send - loop.time()))
        except asyncio.CancelledError:
            pass

    async def duration_watcher(duration_ms: float, t_start: float):
        """Stop streaming automatically after the requested duration."""
        nonlocal streaming, task, streamer_proc, stderr_task, watcher_task
        try:
            await asyncio.sleep(duration_ms / 1000)
            elapsed_ms = (time.perf_counter() - t_start) * 1000
            streaming = False
            if task:
                task.cancel()
                await task
                task = None
            await kill_streamer(streamer_proc, stderr_task, watcher_task)
            streamer_proc = None
            stderr_task = None
            watcher_task = None
            await ws.send_str(json.dumps({
                "type": "stopped",
                "actualDurationMs": round(elapsed_ms, 2),
            }))
            log.info("Duration-triggered stop after %.1f ms", elapsed_ms)
        except asyncio.CancelledError:
            pass  # manual stop arrived first — it will send its own confirmation

    try:
        async for msg in ws:
            from aiohttp import WSMsgType
            if msg.type != WSMsgType.TEXT:
                continue

            try:
                data = json.loads(msg.data)
                cmd_name = data["command"]
            except (json.JSONDecodeError, KeyError):
                await ws.send_str(json.dumps({
                    "type": "error",
                    "message": "Invalid command"
                }))
                continue

            match cmd_name:
                case "start":
                    if not streaming:
                        streaming = True
                        sample_rate_hz  = data.get("sampleRate", 0) or VIRTUAL_RATE
                        duration_ms     = data.get("durationMs")    # None = stream indefinitely
                        t_stream_start  = time.perf_counter()

                        if USE_SIM:
                            task = asyncio.create_task(stream_sim(sample_rate_hz))
                            log.info("Streaming started (simulation, %d Hz)", sample_rate_hz)
                        else:
                            if not "uri" in data or not "device" in data or not "channel" in data:
                                await ws.send_str(json.dumps({
                                    "type": "error",
                                    "message": "Missing URI, device or channel"
                                }))
                                continue

                            task = asyncio.create_task(stream_from_libiio(data["uri"], data["device"], data["channel"]))
                            log.info("Streaming started (ADC via LibIIO)")

                        await ws.send_str(json.dumps({"type": "started"}))
                        if duration_ms:
                            duration_task = asyncio.create_task(
                                duration_watcher(duration_ms, t_stream_start)
                            )

                case "stop":
                    if streaming:
                        t_elapsed_ms = (time.perf_counter() - t_stream_start) * 1000 if t_stream_start else 0
                        if duration_task and not duration_task.done():
                            duration_task.cancel()
                            await asyncio.gather(duration_task, return_exceptions=True)

                        duration_task = None
                        streaming = False
                        if task:
                            task.cancel()
                            await task
                            task = None

                        await kill_streamer(streamer_proc, stderr_task, watcher_task)
                        streamer_proc = None
                        stderr_task = None
                        watcher_task = None

                        await ws.send_str(json.dumps({
                            "type": "stopped",
                            "actualDurationMs": round(t_elapsed_ms, 2),
                        }))
                        log.info("Streaming stopped (manual, %.1f ms)", t_elapsed_ms)

                case "get_devices":
                    if "uri" in data:
                        await ws.send_str(json.dumps({
                            "type": "get_devices",
                            "devices": iio.get_devices_once(data["uri"]),
                        }))
                    else:
                        await ws.send_str(json.dumps({
                            "type": "error",
                            "message": "Missing URI"
                        }))

                case "get_channels":
                    if "uri" in data and "device" in data:
                        await ws.send_str(json.dumps({
                            "type": "get_channels",
                            "channels": iio.get_channels_once(data["uri"], data["device"])
                        }))
                    else:
                        await ws.send_str(json.dumps({
                            "type": "error",
                            "message": "Missing URI or device"
                        }))

    except Exception as exc:
        log.error("WS error: %s", exc)
    finally:
        if duration_task and not duration_task.done():
            duration_task.cancel()
            await asyncio.gather(duration_task, return_exceptions=True)
        if task:
            task.cancel()
            try:
                await task
            except asyncio.CancelledError:
                pass
        await kill_streamer(streamer_proc, stderr_task, watcher_task)
        log.info("Client disconnected")

    return ws


# ── Static file / SPA handler ─────────────────────────────────────────────────
async def static_handler(request):
    rel_path = request.match_info.get("path", "")
    target   = DIST_DIR / rel_path

    if target.is_dir():
        target = target / "index.html"

    if target.is_file():
        return web.FileResponse(target)

    index = DIST_DIR / "index.html"
    if index.is_file():
        return web.FileResponse(index)

    raise web.HTTPNotFound(text="dist/ not found. Did you run `npm run build`?")


# ── App factory ───────────────────────────────────────────────────────────────
def make_app() -> web.Application:
    app = web.Application()
    app.router.add_get("/ws", ws_handler)
    app.router.add_get("/",          static_handler)
    app.router.add_get("/{path:.+}", static_handler)
    return app


async def main():
    host, port = "0.0.0.0", SERVER_PORT
    app = make_app()
    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, host, port)
    await site.start()

    mode = "SIMULATION" if USE_SIM else "LIBIIO"
    log.info("ADC server running on http://%s:%d", host, port)
    log.info("  Mode       ->  %s", mode)
    log.info("  React app  ->  http://localhost:%d/", port)
    log.info("  WebSocket  ->  ws://localhost:%d/ws", port)
    if not DIST_DIR.is_dir():
        log.warning("'%s' not found - run `npm run build` first", DIST_DIR)

    try:
        await asyncio.Event().wait()
    except (asyncio.CancelledError, KeyboardInterrupt):
        pass
    finally:
        log.info("Shutting down...")
        await runner.cleanup()
        log.info("Server stopped cleanly")


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
