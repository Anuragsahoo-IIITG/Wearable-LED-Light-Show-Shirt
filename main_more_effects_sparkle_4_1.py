from fastapi import FastAPI, WebSocket, WebSocketDisconnect, UploadFile, File, Form
from fastapi.staticfiles import StaticFiles
from fastapi.responses import HTMLResponse
from typing import Dict, List
from fastapi.middleware.cors import CORSMiddleware
import soundfile as sf
import numpy as np
import sounddevice as sd
import asyncio
import json
import uvicorn
import time
import os
import random

app = FastAPI()
is_playing = False  # Used for both full playback and preview
audio_stream_handle = {"stream": None}
sequences = {}  # Global to store precomputed sequences from upload
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)
app.mount("/static", StaticFiles(directory="static"), name="static")

class ConnectionManager:
    def __init__(self):
        self.esp_connections: Dict[str, WebSocket] = {}
        self.dashboard_connections: List[WebSocket] = []
        self.esp32_devices: List[Dict] = []
        self.esp_last_heartbeat: Dict[str, float] = {}

    async def connect_esp(self, websocket: WebSocket, device_id: str):
        if device_id in self.esp_connections:
            await self.disconnect(device_id)
        self.esp_connections[device_id] = websocket
        self.esp_last_heartbeat[device_id] = time.time()
        esp_data = {"id": device_id, "ip": websocket.client.host, "name": f"ESP32-{device_id[-4:]}"}
        self.esp32_devices.append(esp_data)
        await self.broadcast_esp_list()

    async def disconnect(self, device_id: str):
        self.esp_connections.pop(device_id, None)
        self.esp_last_heartbeat.pop(device_id, None)
        self.esp32_devices = [esp for esp in self.esp32_devices if esp["id"] != device_id]
        await self.broadcast_esp_list()

    async def connect_dashboard(self, websocket: WebSocket):
        await websocket.accept()
        self.dashboard_connections.append(websocket)
        await websocket.send_text(json.dumps({"type": "esp_list_update", "data": self.esp32_devices}))

    async def broadcast_esp_list(self):
        message = json.dumps({"type": "esp_list_update", "data": self.esp32_devices})
        for conn in self.dashboard_connections:
            try:
                await conn.send_text(message)
            except:
                pass

    async def send_to_esp(self, device_id: str, message: str):
        if device_id in self.esp_connections:
            print(f"Sending command to {device_id}: {message[:100]}...")
            await self.esp_connections[device_id].send_text(message)
        else:
            print(f"Error: ESP32 {device_id} not connected. Cannot send command: {message[:100]}...")

manager = ConnectionManager()
UPLOAD_DIR = "uploaded_audio"
os.makedirs(UPLOAD_DIR, exist_ok=True)

@app.websocket("/ws/processing")
async def processing_websocket(websocket: WebSocket):
    await websocket.accept()
    try:
        for i in range(0, 101, 10):
            await websocket.send_json({"progress": i, "message": f"Processing step {i}%"})
            await asyncio.sleep(0.5)
        await websocket.send_json({"progress": 100, "message": "OK"})
        await websocket.close()
    except Exception as e:
        print(f"WebSocket error: {e}")
        await websocket.close(code=1011)
    finally:
        print("Processing WebSocket connection closed.")

@app.post("/upload-project")
async def upload_project(
    file: UploadFile = File(...),
    timeline: str = Form(...)
):
    global sequences
    audio_path = os.path.join(UPLOAD_DIR, "uploaded.wav")
    with open(audio_path, "wb") as f:
        f.write(await file.read())
    timeline_data = json.loads(timeline)
    with open("led_timeline.json", "w") as f:
        json.dump(timeline_data, f, indent=2)
    sequences = await get_all_sequences(
        audio_path,
        timeline_data,
        chunk_size=1024,
        sample_rate=44100
    )
    # Save to disk for playback
    with open("all_sequences.json", "w") as f:
        json.dump(sequences, f, indent=2)
    return {"status": "ok", "message": "WAV and timeline uploaded."}

async def get_all_sequences(
        wav_path: str,
        timeline_data: dict,
        chunk_size: int = 1024,
        sample_rate: int = 44100
):
    data, sr = sf.read(wav_path)
    if sr != sample_rate:
        sample_rate = sr
    if len(data.shape) > 1:
        data = data[:, 0]
    total_samples = len(data)
    total_chunks = total_samples // chunk_size
    sequences_per_device: Dict[str, List[dict]] = {}
    chunk_energies = []
    for ci in range(total_chunks):
        start_samp = ci * chunk_size
        end_samp = start_samp + chunk_size
        chunk = data[start_samp:end_samp]
        rms = np.sqrt(np.mean(chunk.astype(np.float64) ** 2))
        chunk_energies.append(rms)
    max_rms = max(chunk_energies) if chunk_energies else 1.0
    for event in timeline_data.get("events", []):
        device_id = event["device_id"]
        effect = event["effect"].upper()
        target_leds = event["targetLEDS"]
        start_sec = float(event["start_sec"])
        end_sec = float(event["end_sec"])
        params = event.get("params", {})
        start_sample = int(start_sec * sample_rate)
        end_sample = int(end_sec * sample_rate)
        start_sample = max(0, min(start_sample, total_samples))
        end_sample = max(0, min(end_sample, total_samples))
        n_total_samples = end_sample - start_sample
        n_chunks = n_total_samples // chunk_size
        if n_chunks <= 0:
            continue
        if device_id not in sequences_per_device:
            sequences_per_device[device_id] = []
        if effect == "FFT":
            fft_list = generate_fft_sequence(
                data=data,
                sample_rate=sample_rate,
                chunk_size=chunk_size,
                start_sample=start_sample,
                n_chunks=n_chunks,
                target_leds=target_leds,
                energy_envelope=chunk_energies,
                max_rms=max_rms,
            )
            sequences_per_device[device_id].extend(fft_list)
        elif effect == "STATIC":
            static_list = generate_static_sequence(
                n_chunks=n_chunks,
                target_leds=target_leds,
                params=params,
                chunk_index_offset=start_sample // chunk_size,
            )
            sequences_per_device[device_id].extend(static_list)
        elif effect == "BLINK":
            blink_list = generate_blink_sequence(
                n_chunks=n_chunks,
                target_leds=target_leds,
                params=params,
                energy_envelope=chunk_energies,
                max_rms=max_rms,
                chunk_index_offset=start_sample // chunk_size,
            )
            sequences_per_device[device_id].extend(blink_list)
        elif effect == "RAINBOW":
            rainbow_list = generate_rainbow_sequence(
                n_chunks=n_chunks,
                target_leds=target_leds,
                params=params,
                chunk_index_offset=start_sample // chunk_size,
            )
            sequences_per_device[device_id].extend(rainbow_list)
        elif effect == "CHASE":
            chase_list = generate_chase_sequence(
                n_chunks=n_chunks,
                target_leds=target_leds,
                params=params,
                energy_envelope=chunk_energies,
                max_rms=max_rms,
                chunk_index_offset=start_sample // chunk_size,
            )
            sequences_per_device[device_id].extend(chase_list)
        elif effect == "SPARKLE":
            sparkle_list = generate_sparkle_sequence(
                n_chunks=n_chunks,
                target_leds=target_leds,
                params=params,
                energy_envelope=chunk_energies,
                max_rms=max_rms,
                chunk_index_offset=start_sample // chunk_size,
                data=data,  # Full audio data
                chunk_size=chunk_size,
                start_sample=start_sample
            )
            sequences_per_device[device_id].extend(sparkle_list)
        elif effect == "STROBE":
            strobe_list = generate_strobe_sequence(
                n_chunks = n_chunks,
                target_leds = target_leds,
                params = params,
                energy_envelope = chunk_energies,
                max_rms = max_rms,
                chunk_index_offset = start_sample // chunk_size,
            )
            sequences_per_device[device_id].extend(strobe_list)
        elif effect == "COMET":
            comet_list = generate_comet_sequence(
                n_chunks = n_chunks,
                target_leds = target_leds,
                params = params,
                energy_envelope = chunk_energies,
                max_rms = max_rms,
                chunk_index_offset = start_sample // chunk_size,
                data=data,  # Full audio data
                chunk_size=chunk_size,
                start_sample=start_sample
            )
            sequences_per_device[device_id].extend(comet_list)
        elif effect == "FIRE":
            fire_list = generate_fire_sequence(
                n_chunks = n_chunks,
                target_leds = target_leds,
                params = params,
                energy_envelope = chunk_energies,
                max_rms = max_rms,
                chunk_index_offset=start_sample // chunk_size,
            )
            sequences_per_device[device_id].extend(fire_list)
        else:
            continue
    for dev, seq_list in sequences_per_device.items():
        seq_list.sort(key=lambda entry: entry["chunk_index"])
    return sequences_per_device

def generate_fft_sequence(
    data: np.ndarray,
    sample_rate: int,
    chunk_size: int,
    start_sample: int,
    n_chunks: int,
    target_leds: list,
    energy_envelope: list,
    max_rms: float,
) -> List[dict]:
    fft_entries = []
    total_samples = len(data)
    half_fft_len = chunk_size // 2
    for chunk_idx_rel in range(n_chunks):
        global_chunk_idx = (start_sample // chunk_size) + chunk_idx_rel
        chunk_start = start_sample + chunk_idx_rel * chunk_size
        chunk_end = chunk_start + chunk_size
        if chunk_end > total_samples:
            break
        chunk = data[chunk_start:chunk_end]
        fft_vals = np.fft.fft(chunk)
        mag = np.abs(fft_vals[:half_fft_len])
        bass_val = np.sum(mag[0:50])
        mid_val = np.sum(mag[50:200])
        high_val = np.sum(mag[200:])
        r = int(np.interp(bass_val, [10, 5000], [0, 255]))
        g = int(np.interp(mid_val, [10, 5000], [0, 255]))
        b = int(np.interp(high_val, [10, 5000], [0, 255]))
        r = max(0, min(r, 255))
        g = max(0, min(g, 255))
        b = max(0, min(b, 255))
        fft_entries.append({
            "chunk_index": global_chunk_idx,
            "leds": target_leds.copy(),
            "rgb": [r, g, b],
            "effect_type": "FFT"
        })
    return fft_entries

def generate_static_sequence(
    n_chunks: int,
    target_leds: list,
    params: dict,
    chunk_index_offset: int
) -> List[dict]:
    color = params.get("color", [255, 255, 255])
    static_entries = []
    # Send one entry with duration
    static_entries.append({
        "chunk_index": chunk_index_offset,
        "leds": target_leds.copy(),
        "rgb": color.copy(),
        "effect_type": "STATIC",
        "duration_chunks": n_chunks
    })
    return static_entries



def generate_blink_sequence(
    n_chunks: int,
    target_leds: list,
    params: dict,
    energy_envelope: list,
    max_rms: float,
    chunk_index_offset: int
) -> List[dict]:
    """
    Generates fewer BLINK commands (one per burst), letting ESP32 handle the blinking.
    """
    on_color = params.get("on_color", [255, 255, 255])
    off_color = params.get("off_color", [0, 0, 0])
    base_rate = params.get("base_rate", 8)
    min_rate = params.get("min_rate", 2)

    entries = []
    i = 0
    while i < n_chunks:
        global_chunk = chunk_index_offset + i
        energy = energy_envelope[global_chunk] if global_chunk < len(energy_envelope) else 0
        norm = min(1.0, energy / max_rms) if max_rms > 0 else 0

        # ⏳ Determine blink rate and duration
        period = int(np.interp(norm, [0.0, 1.0], [base_rate, min_rate]))
        period = max(1, period)
        duration = period * 2  # full on+off cycle

        entries.append({
            "chunk_index": global_chunk,
            "duration_chunks": duration,
            "leds": target_leds.copy(),
            "rgb": on_color,  # ESP32 will use both on/off from params
            "effect_type": "BLINK",
            "params": {
                "on_color": on_color,
                "off_color": off_color,
                "base_rate": base_rate,
                "min_rate": min_rate
            }
        })

        i += duration  # 🚫 Skip next `duration` chunks — avoid overlap

    return entries




def generate_chase_sequence(
    n_chunks: int,
    target_leds: list,
    params: dict,
    energy_envelope: list,
    max_rms: float,
    chunk_index_offset: int
) -> List[dict]:
    start_led = target_leds[0]
    end_led = target_leds[-1]
    base_speed = params.get("base_speed", 10)
    max_speed = params.get("max_speed", 3)
    chase_color = params.get("chase_color", [255, 255, 255])
    tail_length = params.get("tail_length", 5)
    direction = params.get("direction", "FORWARD")
    return [
        {
            "chunk_index": chunk_index_offset,
            "leds": [start_led, end_led],
            "effect_type": "CHASE",
            "params": {
                "chase_color": chase_color,
                "tail_length": tail_length,
                "direction": direction,
                "base_speed": base_speed,
                "max_speed": max_speed
            },
            "duration_chunks": n_chunks
        }
    ]

def generate_rainbow_sequence(
    n_chunks: int,
    target_leds: list,
    params: dict,
    chunk_index_offset: int
) -> List[dict]:
    start_led = target_leds[0]
    end_led = target_leds[-1]
    hue_speed = params.get("hue_speed", 0.05)
    return [
        {
            "chunk_index": chunk_index_offset,
            "leds": [start_led, end_led],
            "effect_type": "RAINBOW",
            "params": {"hue_speed": hue_speed},
            "duration_chunks": n_chunks
        }
    ]

# def generate_sparkle_sequence(
#     n_chunks: int,
#     target_leds: list,
#     params: dict,
#     energy_envelope: list,
#     max_rms: float,
#     chunk_index_offset: int
# ) -> list:
#     """
#     Sparkle: Random LEDs light up briefly.
#     - density: fraction (0-1) of LEDs to sparkle per chunk at nominal energy.
#     - sparkle_color: base RGB color.
#     We modulate the density by normalized energy: higher energy → more sparkles.
#     """
#     sparkle_color = params.get("sparkle_color", [255, 255, 255])
#     density = params.get("density", 0.1)
#     seq = []
#     total_leds = len(target_leds)
#     for i in range(n_chunks):
#         global_chunk = chunk_index_offset + i
#         # normalized energy for this chunk
#         energy = energy_envelope[global_chunk] if global_chunk < len(energy_envelope) else 0
#         norm = min(1.0, energy / max_rms) if max_rms > 0 else 0
#         # number of sparkles = density * norm * total_leds
#         count = int(total_leds * density * (0.1+1.0* norm))  # baseline 50% density when norm=0 to avoid zero sparkles? Or adjust as desired.
#         # Alternatively: count = int(total_leds * density * norm)
#         if count <= 0:
#             continue
#         # randomly pick 'count' distinct LEDs from target_leds
#         chosen = random.sample(target_leds, min(count, total_leds))
#         seq.append({
#             "chunk_index": global_chunk,
#             "leds": chosen.copy(),
#             "rgb": sparkle_color.copy(),
#             "effect_type": "SPARKLE"
#         })
#     return seq

import numpy as np
import random

def calculate_zcr(chunk):
    return np.mean(np.abs(np.diff(np.sign(chunk)))) / 2  # Zero Crossing Rate

def generate_sparkle_sequence(
    n_chunks: int,
    target_leds: list,
    params: dict,
    energy_envelope: list,
    max_rms: float,
    chunk_index_offset: int,
    data: np.ndarray,
    chunk_size: int,
    start_sample: int
) -> list:
    """
    Generate a sparkle effect: random bursts of LEDs that fade over time.
    Each sparkle's count, brightness, and duration are based on the music's energy and frequency.
    """
    sparkle_color = params.get("sparkle_color", [255, 255, 255])
    density = params.get("density", 0.15)  # How aggressive sparkles are
    base_duration = params.get("base_duration", 3)  # In chunks
    seq = []

    led_start, led_end = target_leds
    total_leds = led_end - led_start + 1

    cooldown = 0  # 🛑 Prevent overlapping sparkles

    for i in range(n_chunks):
        global_chunk = chunk_index_offset + i
        chunk_start = start_sample + i * chunk_size
        chunk_end = chunk_start + chunk_size
        if chunk_end > len(data):
            break

        if cooldown > 0:
            cooldown -= 1
            continue  # ⏩ Skip this chunk — still in cooldown

        chunk = data[chunk_start:chunk_end]
        energy = energy_envelope[global_chunk] if global_chunk < len(energy_envelope) else 0
        norm_energy = min(1.0, energy / max_rms) if max_rms > 0 else 0
        zcr = calculate_zcr(chunk)
        norm_zcr = min(1.0, zcr)

        # 🌟 Sparkle count increases with energy and ZCR
        sparkle_count = int(total_leds * density * (0.3 + 1.0 * norm_energy) * (1.0 + 1.5 * norm_zcr))
        sparkle_count = max(1, min(sparkle_count, total_leds))

        # 🌟 Peak brightness
        brightness = 0.6 + 0.4 * norm_energy  # 0.6–1.0
        rgb = [int(min(255, c * brightness)) for c in sparkle_color]

        # 🌟 Sparkle duration (shorter if high ZCR → faster fade)
        duration = max(1, int(base_duration * (1.0 - 0.7 * norm_zcr)))

        # Random LED IDs (unique for this sparkle burst)
        available_leds = list(range(led_start, led_end + 1))
        selected_leds = random.sample(available_leds, sparkle_count)

        for fade_step in range(duration):
            fade_chunk = global_chunk + fade_step
            if fade_chunk >= len(energy_envelope):
                break

            fade_factor = 1.0 - (fade_step / duration)
            faded_rgb = [int(c * fade_factor) for c in rgb]



            seq.append({
                "chunk_index": fade_chunk,
                "leds": [led_start, led_end],
                "rgb": faded_rgb,
                "count": len(selected_leds),
                "effect_type": "SPARKLE",
                "duration": duration
            })
        cooldown = duration  # ⏳ Set cooldown to avoid overlap
    return seq


def generate_strobe_sequence(
    n_chunks: int,
    target_leds: list,
    params: dict,
    energy_envelope: list,
    max_rms: float,
    chunk_index_offset: int
) -> list:
    """
    Strobe: rapid on/off flashes.
    Rate (period in chunks) is interpolated by energy: higher energy -> faster strobe.
    """
    on_color = params.get("on_color", [255, 255, 255])
    off_color = params.get("off_color", [0, 0, 0])
    base_rate = params.get("base_rate", 16)  # slowest period
    min_rate = params.get("min_rate", 2)     # fastest period
    seq = []
    for i in range(n_chunks):
        global_chunk = chunk_index_offset + i
        energy = energy_envelope[global_chunk] if global_chunk < len(energy_envelope) else 0
        norm = min(1.0, energy / max_rms) if max_rms > 0 else 0
        # period: interpolate from base_rate (when norm low) to min_rate (when norm high)
        period = int(np.interp(norm, [0.0, 1.0], [base_rate, min_rate]))
        period = max(1, period)
        # simple on/off: even multiples = on, odd multiples = off
        cycle_pos = global_chunk % (2 * period)
        is_on = (cycle_pos < period)
        rgb = on_color.copy() if is_on else off_color.copy()
        seq.append({
            "chunk_index": global_chunk,
            "leds": target_leds.copy(),
            "rgb": rgb,
            "effect_type": "STROBE"
        })
    return seq


def generate_comet_sequence(
    n_chunks: int,
    target_leds: list,
    params: dict,
    energy_envelope: list,
    max_rms: float,
    chunk_index_offset: int,
    data: np.ndarray,
    chunk_size: int,
    start_sample: int
) -> list:
    """
    Generate high-level comet instructions based on Zero Crossing Rate (ZCR).
    More/faster comets when music is energetic; fewer/slower when mellow.
    """
    comet_color = params.get("comet_color", [255, 100, 0])
    min_comet_count = params.get("min_comets", 1)
    max_comet_count = params.get("max_comets", 5)
    min_comet_size = params.get("min_size", 3)
    max_comet_size = params.get("max_size", 10)
    min_duration = params.get("min_duration", 2)
    max_duration = params.get("max_duration", 6)

    seq = []
    led_start, led_end = target_leds

    for i in range(n_chunks):
        global_chunk = chunk_index_offset + i
        chunk_start = start_sample + i * chunk_size
        chunk_end = chunk_start + chunk_size
        if chunk_end > len(data):
            break

        chunk = data[chunk_start:chunk_end]
        zcr = calculate_zcr(chunk)  # 0 to 1 normalized
        norm_zcr = min(1.0, zcr)

        # More/faster comets if norm_zcr is high
        comet_count = int(np.interp(norm_zcr, [0.0, 1.0], [min_comet_count, max_comet_count]))
        comet_size  = int(np.interp(norm_zcr, [0.0, 1.0], [max_comet_size, min_comet_size]))
        duration    = int(np.interp(norm_zcr, [0.0, 1.0], [max_duration, min_duration]))

        seq.append({
            "chunk_index": global_chunk,
            "effect_type": "COMET",
            "leds": [led_start, led_end],
            "rgb": comet_color,
            "comet_count": comet_count,
            "comet_size": comet_size,
            "duration": duration
        })

    return seq




def generate_fire_sequence(
    n_chunks: int,
    target_leds: list,
    params: dict,
    energy_envelope: list,
    max_rms: float,
    chunk_index_offset: int
) -> list:
    """
    Fire: flickering orange/yellow.
    - base_color: the base RGB (e.g., [255,100,0]).
    - flicker_intensity: fraction (0-1) how much random variation around base_color.
    We modulate brightness by energy: higher energy -> brighter flicker.
    """
    base_color = params.get("base_color", [255, 100, 0])
    flicker_intensity = params.get("flicker_intensity", 0.5)
    seq = []
    for i in range(n_chunks):
        global_chunk = chunk_index_offset + i
        energy = energy_envelope[global_chunk] if global_chunk < len(energy_envelope) else 0
        norm = min(1.0, energy / max_rms) if max_rms > 0 else 0
        # brightness factor ~ norm (so quiet segments darker, loud segments brighter)
        brightness = 0.5 + 0.5 * norm  # vary between 0.5 and 1.0; adjust as desired
        # random flicker per chunk: factor in [1 - flicker_intensity, 1]
        flick = 1.0 - flicker_intensity * random.random()
        factor = brightness * flick
        # compute final rgb
        rgb = [int(max(0, min(255, c * factor))) for c in base_color]
        seq.append({
            "chunk_index": global_chunk,
            "leds": target_leds.copy(),
            "rgb": rgb,
            "effect_type": "FIRE"
        })
    return seq


@app.websocket("/ws/esp32")
async def esp32_websocket(websocket: WebSocket):
    await websocket.accept()
    device_id = await websocket.receive_text()
    await manager.connect_esp(websocket, device_id)
    while True:
        try:
            await websocket.receive_text()
            manager.esp_last_heartbeat[device_id] = time.time()
        except WebSocketDisconnect:
            await manager.disconnect(device_id)
            break

@app.on_event("startup")
async def start_heartbeat_checker():
    asyncio.create_task(heartbeat_checker())

async def heartbeat_checker():
    while True:
        now = time.time()
        dead_devices = []
        for device_id, last_time in manager.esp_last_heartbeat.items():
            if now - last_time > 10:
                dead_devices.append(device_id)
        for device_id in dead_devices:
            await manager.disconnect(device_id)
        await asyncio.sleep(5)

@app.websocket("/ws/dashboard")
async def dashboard_websocket(websocket: WebSocket):
    await websocket.accept()
    try:
        await websocket.send_text(json.dumps({"type": "esp_list_update", "data": manager.esp32_devices}))
        while True:
            await websocket.receive_text()
    except WebSocketDisconnect:
        pass

@app.get("/connected_esp32s")
async def get_connected_esp32s():
    return manager.esp32_devices

@app.post("/start-playback")
async def start_playback():
    global is_playing
    if is_playing:
        return {"status": "error", "message": "Already playing"}
    is_playing = True
    asyncio.create_task(play_audio_and_stream())
    return {"status": "ok", "message": "Playback started"}

@app.post("/stop-playback")
async def stop_playback():
    global is_playing, audio_stream_handle
    if not is_playing or audio_stream_handle is None:
        return {"status": "error", "message": "Nothing is Playing"}
    try:
        if audio_stream_handle["stream"] and audio_stream_handle["stream"].active:
            audio_stream_handle["stream"].stop()
            audio_stream_handle["stream"].close()
    except Exception as e:
        print(f"Error stopping stream: {e}")
    finally:
        is_playing = False
        audio_stream_handle["stream"] = None
        print("Playback Stopped by request.")
    return {"status": "ok", "message": "Playback Stopped"}


@app.post("/start-preview")
async def start_preview(data: dict):
    global is_playing
    if is_playing:
        return {"status": "error", "message": "Already playing"}

    start_time = float(data.get("start_time", 0))
    end_time = float(data.get("end_time", 0))
    timeline_data = data.get("timeline", [])  # Use the provided timeline

    if start_time < 0 or end_time <= start_time or not timeline_data:
        return {"status": "error", "message": "Invalid start/end times or empty timeline"}

    WAV_FILE_ON_DISK = os.path.join(UPLOAD_DIR, "uploaded.wav")
    try:
        data_wav, samplerate = sf.read(WAV_FILE_ON_DISK)
    except Exception as e:
        print(f"Could not load WAV file: {e}")
        return {"status": "error", "message": "Could not load audio."}

    if len(data_wav.shape) > 1:
        data_wav = data_wav[:, 0]

    total_samples = len(data_wav)
    chunk_size = 1024
    samples_per_second = samplerate

    start_sample = int(start_time * samples_per_second)
    end_sample = int(end_time * samples_per_second)
    start_sample = max(0, min(start_sample, total_samples))
    end_sample = max(0, min(end_sample, total_samples))

    trimmed_audio = data_wav[start_sample:end_sample]
    trimmed_total_samples = len(trimmed_audio)
    trimmed_total_chunks = trimmed_total_samples // chunk_size

    # Process only the provided timeline data for the preview window
    preview_sequences = {}
    start_chunk_index = start_sample // chunk_size

    # Precompute energies for blink/chase effects in preview
    trimmed_total_samples = len(trimmed_audio)
    trimmed_total_chunks = trimmed_total_samples // chunk_size

    # Compute chunk energies for the preview window
    chunk_energies = []
    for i in range(trimmed_total_chunks):
        start = i * chunk_size
        end = start + chunk_size
        if end > len(trimmed_audio):
            break
        chunk = trimmed_audio[start:end]
        rms = np.sqrt(np.mean(chunk.astype(np.float64) ** 2))
        chunk_energies.append(rms)

    max_rms = max(chunk_energies) if chunk_energies else 1.0

    # Generate sequences per event in the preview timeline
    for event in timeline_data:
        device_id = event["device_id"]
        effect = event["effect"].upper()
        target_leds = event["targetLEDS"]
        start_sec = float(event["start_sec"])
        end_sec = float(event["end_sec"])
        params = event.get("params", {})

        # Convert time to sample indices within the full audio file
        start_sample_event = int(start_sec * samplerate)
        end_sample_event = int(end_sec * samplerate)

        # Clip to preview window
        start_sample_event = max(start_sample, min(start_sample_event, end_sample))
        end_sample_event = max(start_sample, min(end_sample_event, end_sample))

        n_total_samples = end_sample_event - start_sample_event
        n_chunks = n_total_samples // chunk_size
        if n_chunks <= 0:
            continue

        if device_id not in preview_sequences:
            preview_sequences[device_id] = []

        # Calculate offset for chunk index in the full file
        chunk_index_offset = start_sample_event // chunk_size

        # Use local trimmed audio and computed energies
        if effect == "FFT":
            fft_list = generate_fft_sequence(
                data=trimmed_audio,
                sample_rate=samplerate,
                chunk_size=chunk_size,
                start_sample=start_sample_event - start_sample,  # relative to trimmed
                n_chunks=n_chunks,
                target_leds=target_leds,
                energy_envelope=chunk_energies,
                max_rms=max_rms,
            )
            preview_sequences[device_id].extend(fft_list)

        elif effect == "STATIC":
            static_list = generate_static_sequence(
                n_chunks=n_chunks,
                target_leds=target_leds,
                params=params,
                chunk_index_offset=chunk_index_offset
            )
            preview_sequences[device_id].extend(static_list)

        elif effect == "BLINK":
            blink_list = generate_blink_sequence(
                n_chunks=n_chunks,
                target_leds=target_leds,
                params=params,
                energy_envelope=chunk_energies,
                max_rms=max_rms,
                chunk_index_offset=chunk_index_offset
            )
            preview_sequences[device_id].extend(blink_list)

        elif effect == "RAINBOW":
            rainbow_list = generate_rainbow_sequence(
                n_chunks=n_chunks,
                target_leds=target_leds,
                params=params,
                chunk_index_offset=chunk_index_offset
            )
            preview_sequences[device_id].extend(rainbow_list)

        elif effect == "CHASE":
            chase_list = generate_chase_sequence(
                n_chunks=n_chunks,
                target_leds=target_leds,
                params=params,
                energy_envelope=chunk_energies,
                max_rms=max_rms,
                chunk_index_offset=chunk_index_offset
            )
            preview_sequences[device_id].extend(chase_list)
        elif effect == "SPARKLE":
            sparkle_list = generate_sparkle_sequence(
                n_chunks=n_chunks,
                target_leds=target_leds,
                params=params,
                energy_envelope=chunk_energies,
                max_rms=max_rms,
                chunk_index_offset=start_sample // chunk_size,
                data = trimmed_audio,  # Full audio data
                chunk_size=chunk_size,
                start_sample=start_sample
            )
            preview_sequences[device_id].extend(sparkle_list)
        elif effect == "STROBE":
            strobe_list = generate_strobe_sequence(
                n_chunks=n_chunks,
                target_leds=target_leds,
                params=params,
                energy_envelope=chunk_energies,
                max_rms=max_rms,
                chunk_index_offset=chunk_index_offset
            )
            preview_sequences[device_id].extend(strobe_list)
        elif effect == "COMET":
            comet_list = generate_comet_sequence(
                n_chunks=n_chunks,
                target_leds=target_leds,
                params=params,
                energy_envelope=chunk_energies,
                max_rms=max_rms,
                chunk_index_offset=chunk_index_offset,
                data = trimmed_audio,  # Pass the preview audio data
                chunk_size = chunk_size,  # Pass the chunk size used for processing
                start_sample = start_sample_event - start_sample  # Relative start sample in preview window
            )
            preview_sequences[device_id].extend(comet_list)
        elif effect == "FIRE":
            fire_list = generate_fire_sequence(
                n_chunks=n_chunks,
                target_leds=target_leds,
                params=params,
                energy_envelope=chunk_energies,
                max_rms=max_rms,
                chunk_index_offset=chunk_index_offset
            )
            preview_sequences[device_id].extend(fire_list)

    # Sort by chunk index
    for dev, seq_list in preview_sequences.items():
        seq_list.sort(key=lambda entry: entry["chunk_index"])

    # Start preview playback
    is_playing = True
    asyncio.create_task(play_preview_audio_and_stream(trimmed_audio, samplerate, preview_sequences, chunk_size))
    return {"status": "ok", "message": "Preview started"}


async def play_audio_and_stream():
    global is_playing, audio_stream_handle
    try:
        with open("all_sequences.json", "r") as f:
            all_sequences = json.load(f)
    except FileNotFoundError:
        print("No sequences found. Aborting playback.")
        is_playing = False
        return
    WAV_FILE_ON_DISK = os.path.join(UPLOAD_DIR, "uploaded.wav")
    try:
        data_wav, samplerate = sf.read(WAV_FILE_ON_DISK)
    except Exception as e:
        print(f"Could not load WAV file: {e}")
        is_playing = False
        return
    if len(data_wav.shape) > 1:
        data_wav = data_wav[:, 0]
    total_samples = len(data_wav)
    chunk_size = 1024
    total_chunks = total_samples // chunk_size
    per_device_index = {dev: 0 for dev in all_sequences}
    chunk_index_holder = {"idx": 0}
    loop = asyncio.get_event_loop()
    def audio_callback(outdata, frames, time_info, status):
        idx = chunk_index_holder["idx"]
        if idx >= total_chunks or not is_playing:
            raise sd.CallbackStop()
        start = idx * chunk_size
        end = start + chunk_size
        chunk = data_wav[start:end]
        outdata[:] = chunk.reshape(-1, 1)
        for device_id, seq_list in all_sequences.items():
            ptr = per_device_index.get(device_id, 0)
            while ptr < len(seq_list) and seq_list[ptr].get("chunk_index") == idx:
                entry = seq_list[ptr]
                payload_json = json.dumps(entry)
                loop.call_soon_threadsafe(asyncio.create_task, manager.send_to_esp(device_id, payload_json))
                ptr += 1
            per_device_index[device_id] = ptr
        chunk_index_holder["idx"] += 1
    try:
        with sd.OutputStream(samplerate=samplerate, channels=1, callback=audio_callback, blocksize=chunk_size) as stream:
            audio_stream_handle["stream"] = stream
            while is_playing and stream.active:
                await asyncio.sleep(0.1)
    except Exception as e:
        print(f"Playback interrupted: {e}")
    finally:
        if audio_stream_handle.get("stream") and audio_stream_handle["stream"].active:
            audio_stream_handle["stream"].stop()
            audio_stream_handle["stream"].close()
        is_playing = False
        audio_stream_handle["stream"] = None
        print("Playback + streaming completed.")

async def play_preview_audio_and_stream(audio_data, samplerate, sequences_dict, chunk_size=1024):
    global is_playing, audio_stream_handle
    total_samples = len(audio_data)
    total_chunks = total_samples // chunk_size
    per_device_index = {dev: 0 for dev in sequences_dict}
    chunk_index_holder = {"idx": 0}
    loop = asyncio.get_event_loop()
    def audio_callback(outdata, frames, time_info, status):
        idx = chunk_index_holder["idx"]
        if idx >= total_chunks or not is_playing:
            raise sd.CallbackStop()
        start = idx * chunk_size
        end = start + chunk_size
        chunk = audio_data[start:end]
        outdata[:] = chunk.reshape(-1, 1)
        for device_id, seq_list in sequences_dict.items():
            ptr = per_device_index.get(device_id, 0)
            while ptr < len(seq_list) and seq_list[ptr].get("chunk_index") == idx:
                entry = seq_list[ptr]
                payload_json = json.dumps(entry)
                loop.call_soon_threadsafe(asyncio.create_task, manager.send_to_esp(device_id, payload_json))
                ptr += 1
            per_device_index[device_id] = ptr
        chunk_index_holder["idx"] += 1
    try:
        with sd.OutputStream(samplerate=samplerate, channels=1, callback=audio_callback, blocksize=chunk_size) as stream:
            audio_stream_handle["stream"] = stream
            while is_playing and stream.active:
                await asyncio.sleep(0.1)
    except Exception as e:
        print(f"Preview playback interrupted: {e}")
    finally:
        if audio_stream_handle.get("stream") and audio_stream_handle["stream"].active:
            audio_stream_handle["stream"].stop()
            audio_stream_handle["stream"].close()
        is_playing = False
        audio_stream_handle["stream"] = None
        print("Preview playback completed.")

if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8000)
