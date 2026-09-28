"""
Sign-language reader backend (Gemini vision + gTTS)
  POST /decode       JPEG in  -> recognised letter/phrase out (plain text)
  GET  /speak?text=  text in  -> 16 kHz, 16-bit, MONO WAV out (ready for the NS4168)
  GET  /health       quick check that the server is up (also shows backend/model in use)
  GET  /last         shows the last photo the ESP32 sent (open in a browser)

Every photo is also saved in the "captures" folder next to server.py,
named with the time and what the model answered.

Install once:
    pip install flask google-genai gtts miniaudio python-dotenv

Optional settings (in a .env file next to server.py, or as environment variables):
    # GEMINI_API_KEY=...
    # GROQ_API_KEY=...           (optional fallback vision)
    # GEMINI_MODEL=gemini-3.5-flash-lite
    # GEMINI_BACKEND=auto        (auto | studio | rest | vertex)
Then run:
    python server.py            start the server
    python server.py --check    only test the key and exit
"""
import base64
import io
import json
import os
import re
import socket
import sys
import time
import types as _pytypes
import urllib.error
import urllib.request
import wave
from pathlib import Path

# Ensure immediate console output without buffering
try:
    sys.stdout.reconfigure(line_buffering=True)
    sys.stderr.reconfigure(line_buffering=True)
except Exception:
    pass

import miniaudio
from dotenv import load_dotenv
from flask import Flask, Response, request, send_file
from google import genai
from google.genai import types
from gtts import gTTS

app = Flask(__name__)

# =====================================================================
#  PASTE YOUR GEMINI API KEY BETWEEN THE QUOTES BELOW (OR IN .env)
# =====================================================================

# =====================================================================

# ---- optional .env file (next to this file) ----
load_dotenv(Path(__file__).with_name(".env"), override=True)
API_KEY = (os.getenv("GEMINI_API_KEY") or GEMINI_API_KEY).strip().strip("\"'")
GROQ_API_KEY = (os.getenv("GROQ_API_KEY") or "").strip().strip("\"'")

if not API_KEY or API_KEY == "PASTE_YOUR_KEY_HERE":
    raise SystemExit("No Gemini API key set.\n"
                     "Open server.py and paste your key into the line:\n"
                     '    GEMINI_API_KEY = "PASTE_YOUR_KEY_HERE"')

BACKEND_SETTING = os.getenv("GEMINI_BACKEND", "auto").strip().lower()
PREFERRED_MODEL = os.getenv("GEMINI_MODEL", "").strip()

# Tried in order until one works. Flash-lite models are fast and have generous free-tier limits.
MODEL_CANDIDATES = [m for m in [
    PREFERRED_MODEL,
    "gemini-3.5-flash-lite",
    "gemini-flash-lite-latest",
    "gemini-2.5-flash",
    "gemini-3.5-flash",
    "gemini-flash-latest",
    "gemini-3.8-flash",
] if m]
MODEL_CANDIDATES = list(dict.fromkeys(MODEL_CANDIDATES)
                        )  # remove duplicates, keep order

# Hand shapes that should become a whole phrase instead of a letter.
CUSTOM_SIGNS = {
    "thumb, index finger and pinky all extended, middle and ring fingers folded down "
    "(the ASL 'I love you' sign; the INDEX finger is up, unlike the letter Y)": "I love you",
    "thumb pointing up, other fingers curled into a fist": "Good",
    "open palm facing the camera with all five fingers spread apart": "Hello",
}

PROMPT = (
    "You are an American Sign Language (ASL) recognizer. The photo comes from a small, "
    "low-resolution camera and may be dark, blurry, or rotated. It shows one hand sign.\n\n"
    "STEP 1 - Special signs have PRIORITY over letters. Look carefully at which fingers "
    "are extended. If the hand matches one of these, reply with ONLY its phrase, "
    "never a letter:\n"
    + "\n".join(f"- {shape} -> {phrase}" for shape,
                phrase in CUSTOM_SIGNS.items())
    + "\n\nSTEP 2 - Only if none of the special signs match, reply with ONLY the single "
    "fingerspelled ASL letter (A-Z).\n\n"
    "Give your best guess even if the image is imperfect. "
    "Reply exactly UNCLEAR only if no hand is visible at all. "
    "No explanations, no punctuation, just the phrase or letter."
)

GEN_CONFIG = types.GenerateContentConfig(
    automatic_function_calling=types.AutomaticFunctionCallingConfig(
        disable=True),
)

CAPTURE_DIR = Path(__file__).with_name("captures")
CAPTURE_DIR.mkdir(exist_ok=True)
LAST_IMAGE = CAPTURE_DIR / "last.jpg"

SAMPLE_RATE = 16000
MAX_SECONDS = 20

last_text_cache = "UNCLEAR"

# Filled in by connect_gemini()
client = None
MODEL_NAME = None
BACKEND_NAME = None


# ---------------------------------------------------------------- Gemini setup
def error_code(e: Exception):
    """HTTP status of a google-genai error, or None for network errors etc."""
    return getattr(e, "code", None) or getattr(e, "status_code", None)


def explain_error(e: Exception) -> str:
    code = error_code(e)
    msg = str(e)
    if code in (401, 403) or "API_KEY" in msg or "PERMISSION_DENIED" in msg:
        if "leaked" in msg.lower():
            return ("Google has flagged this key as LEAKED and disabled it. "
                    "Create a new key in Google AI Studio.")
        if "billing" in msg.lower():
            return "This endpoint needs billing enabled on the Google Cloud project."
        return "Key rejected on this endpoint (wrong key type, restricted key, or project blocked)."
    if code == 404:
        return "Model name not available on this endpoint."
    if code == 429:
        return "Quota / rate limit hit (free tier limits or no quota in this project)."
    if code == 503:
        return "Model is currently experiencing high demand. Automatic failover will try alternative models."
    if code == 400 and "location" in msg.lower():
        return "Gemini API is not available from this country/region for this key."
    if code is None:
        return "Could not reach Google at all (no internet, firewall, or DNS problem)."
    return "Temporary service error."


class RestError(Exception):
    def __init__(self, code, body):
        self.code = code
        super().__init__(f"{code} {body}")


class RestClient:
    """Talks to AI Studio directly via urllib if the google-genai library gets rejected."""
    BASE = os.getenv("GEMINI_REST_BASE",
                     "https://generativelanguage.googleapis.com/v1beta")

    def __init__(self):
        self.models = self

    def generate_content(self, model, contents, config=None):
        if isinstance(contents, str):
            contents = [contents]
        parts = []
        for c in contents:
            if isinstance(c, str):
                parts.append({"text": c})
            else:  # types.Part with image bytes
                blob = getattr(c, "inline_data", None)
                if blob:
                    raw_bytes = blob.data if isinstance(
                        blob.data, (bytes, bytearray)) else blob.data.encode()
                    parts.append({"inline_data": {
                        "mime_type": blob.mime_type,
                        "data": base64.b64encode(raw_bytes).decode()}})
        body = json.dumps(
            {"contents": [{"role": "user", "parts": parts}]}).encode()
        req = urllib.request.Request(
            f"{self.BASE}/models/{model}:generateContent", data=body, method="POST",
            headers={"Content-Type": "application/json", "x-goog-api-key": API_KEY})
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                data = json.loads(r.read())
        except urllib.error.HTTPError as e:
            raise RestError(e.code, e.read().decode(
                errors="replace")[:500]) from None
        except urllib.error.URLError as e:
            raise RestError(None, e.reason) from None
        texts = [p.get("text", "")
                 for cand in data.get("candidates", [])[:1]
                 for p in cand.get("content", {}).get("parts", [])
                 if not p.get("thought")]
        return _pytypes.SimpleNamespace(text="".join(texts))


def make_client(backend: str):
    if backend == "vertex":
        return genai.Client(vertexai=True, api_key=API_KEY)
    if backend == "rest":
        return RestClient()
    return genai.Client(api_key=API_KEY)


def connect_gemini(verbose=True) -> bool:
    """Find a (backend, model) pair that actually answers. Returns True on success."""
    global client, MODEL_NAME, BACKEND_NAME

    if BACKEND_SETTING in ("studio", "rest", "vertex"):
        backends = [BACKEND_SETTING]
    else:
        backends = ["studio", "rest", "vertex"]

    key_kind = "AQ. (new format)" if API_KEY.startswith("AQ.") else (
        "AIza (classic AI Studio)" if API_KEY.startswith("AIza") else "UNKNOWN format")
    if verbose:
        print(
            f"Key: {API_KEY[:6]}...{API_KEY[-4:]}  ({len(API_KEY)} chars, {key_kind})", flush=True)

    for backend in backends:
        try:
            c = make_client(backend)
        except Exception as e:
            if verbose:
                print(f"  [{backend}] could not create client: {e}", flush=True)
            continue
        for model in MODEL_CANDIDATES:
            try:
                r = c.models.generate_content(model=model, contents="Reply with OK",
                                              config=GEN_CONFIG)
                if verbose:
                    print(
                        f"  [{backend}] {model}: WORKS (reply {(r.text or '').strip()!r})", flush=True)
                client, MODEL_NAME, BACKEND_NAME = c, model, backend
                return True
            except Exception as e:
                code = error_code(e)
                if verbose:
                    print(
                        f"  [{backend}] {model}: FAILED ({code}) - {explain_error(e)}", flush=True)
                    print(f"        detail: {str(e)[:300]}", flush=True)
                if code in (401, 403):
                    # Key rejected or permission denied on this backend -> try next backend
                    break
                # For 404, 429 (quota), 503 (high demand), 500 etc., try the next model candidate
                continue
    return False


# ---------------------------------------------------------------- helpers
def clean_reply(text: str) -> str:
    """Keep the first line, strip quotes/punctuation the model sometimes adds."""
    lines = (text or "").strip().splitlines()
    text = lines[0].strip(" .\"'`*") if lines else ""
    return text[:32] or "UNCLEAR"


def call_groq_vision(img_bytes: bytes) -> str:
    """Optional Groq vision fallback if GROQ_API_KEY is configured in .env."""
    if not GROQ_API_KEY:
        return ""
    try:
        from groq import Groq
        groq_client = Groq(api_key=GROQ_API_KEY)
        b64_img = base64.b64encode(img_bytes).decode("utf-8")
        completion = groq_client.chat.completions.create(
            model=os.getenv("GROQ_MODEL", "llama-3.2-11b-vision-preview"),
            messages=[
                {
                    "role": "user",
                    "content": [
                        {"type": "text", "text": PROMPT},
                        {
                            "type": "image_url",
                            "image_url": {"url": f"data:image/jpeg;base64,{b64_img}"}
                        }
                    ]
                }
            ],
            max_tokens=32,
        )
        return clean_reply(completion.choices[0].message.content or "")
    except Exception as e:
        print(f"Groq vision error: {e}", flush=True)
        return ""


def recognize_image(img_bytes: bytes) -> tuple[str, str]:
    """Recognize hand sign from JPEG bytes.
    Tries current MODEL_NAME, then falls back to other candidate models on 429/503/errors.
    Also falls back to Groq if configured."""
    global client, MODEL_NAME, BACKEND_NAME

    # Ensure client is connected
    if client is None and not connect_gemini(verbose=True):
        if GROQ_API_KEY:
            print("Gemini unavailable, attempting Groq fallback...", flush=True)
            groq_text = call_groq_vision(img_bytes)
            if groq_text:
                return groq_text, f"[Groq fallback] {groq_text}"
        return "ERROR", "No working vision model available"

    # Prioritize active MODEL_NAME, followed by all other candidates
    models_to_try = [MODEL_NAME] + \
        [m for m in MODEL_CANDIDATES if m != MODEL_NAME]
    last_err_msg = ""

    for model in models_to_try:
        try:
            response = client.models.generate_content(
                model=model,
                contents=[
                    PROMPT,
                    types.Part.from_bytes(
                        data=img_bytes, mime_type="image/jpeg"),
                ],
                config=GEN_CONFIG,
            )
            raw = response.text or ""
            text = clean_reply(raw)
            if model != MODEL_NAME:
                print(
                    f"  [Failover] Switched active model to {model}", flush=True)
                MODEL_NAME = model
            return text, raw
        except Exception as e:
            code = error_code(e)
            last_err_msg = f"{model} ({code}): {e}"
            print(
                f"  Gemini error on {model} ({code}): {explain_error(e)}", flush=True)
            # Try next model candidate
            continue

    # All Gemini models failed, try Groq if configured
    if GROQ_API_KEY:
        print("All Gemini candidates failed, attempting Groq fallback...", flush=True)
        groq_text = call_groq_vision(img_bytes)
        if groq_text:
            return groq_text, f"[Groq fallback] {groq_text}"

    return "ERROR", last_err_msg


# ---------------------------------------------------------------- routes
@app.get("/health")
def health():
    if client is None:
        return "OK (but Gemini NOT connected - see server console)"
    return f"OK ({BACKEND_NAME}, {MODEL_NAME})"


@app.get("/last")
def last_image():
    if not LAST_IMAGE.exists():
        return "No photo received yet", 404
    return send_file(LAST_IMAGE, mimetype="image/jpeg", max_age=0)


@app.post("/decode")
def decode():
    global last_text_cache
    img_bytes = request.get_data() or request.data
    if not img_bytes:
        return "NO IMG", 400
    LAST_IMAGE.write_bytes(img_bytes)

    text, raw = recognize_image(img_bytes)

    # Keep a copy named after the answer, e.g. captures/051039_I_love_you.jpg
    safe = re.sub(r"[^A-Za-z0-9]+", "_", text)[:20]
    saved = CAPTURE_DIR / f"{time.strftime('%H%M%S')}_{safe}.jpg"
    saved.write_bytes(img_bytes)
    print(f"/decode -> {text!r}  raw reply: {raw!r}", flush=True)
    print(f"          {len(img_bytes)} byte image saved as {saved.name}  "
          f"({BACKEND_NAME}, model {MODEL_NAME})", flush=True)
    if text != "ERROR":
        last_text_cache = text   # don't make /speak say "Error"
    return text


@app.get("/speak")
def speak():
    text = request.args.get("text", last_text_cache).strip()
    if not text:
        return "NO TEXT", 400
    try:
        # 1. Text -> MP3 (gTTS needs internet on the PC)
        mp3 = io.BytesIO()
        gTTS(text.title(), lang="en").write_to_fp(mp3)

        # 2. MP3 -> 16-bit mono PCM at SAMPLE_RATE
        pcm = miniaudio.decode(
            mp3.getvalue(),
            output_format=miniaudio.SampleFormat.SIGNED16,
            nchannels=1,
            sample_rate=SAMPLE_RATE,
        )
        samples = pcm.samples[: SAMPLE_RATE * MAX_SECONDS]

        # 3. PCM -> standard WAV (plain 44-byte header)
        out = io.BytesIO()
        with wave.open(out, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(SAMPLE_RATE)
            w.writeframes(samples.tobytes())
        data = out.getvalue()
        print(
            f"/speak '{text}' -> {len(data)} bytes, {len(samples) / SAMPLE_RATE:.1f} s", flush=True)
        # Response(bytes) sets Content-Length, which the ESP32 uses to size its buffer
        return Response(data, mimetype="audio/wav")
    except Exception as e:
        print("TTS error:", e, flush=True)
        return "TTS ERROR", 500


def check_port_available(port: int, host: str = "0.0.0.0") -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        try:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind((host, port))
            return True
        except OSError:
            return False


def free_port(port: int):
    """If port 3000 is occupied by a lingering process on Windows, terminate it."""
    try:
        import subprocess
        out = subprocess.check_output(
            f"netstat -ano | findstr :{port}", shell=True, text=True)
        current_pid = os.getpid()
        for line in out.strip().splitlines():
            if "LISTENING" in line:
                pid = line.strip().split()[-1]
                if pid.isdigit() and int(pid) != current_pid:
                    print(
                        f"Terminating lingering process PID {pid} using port {port}...", flush=True)
                    subprocess.run(
                        f"taskkill /F /PID {pid}", shell=True, capture_output=True)
                    time.sleep(1)
    except Exception as e:
        print(
            f"Note: Could not automatically free port {port}: {e}", flush=True)


if __name__ == "__main__":
    print("Testing Gemini key...", flush=True)
    ok = connect_gemini()
    if ok:
        print(
            f"==> Using {BACKEND_NAME} endpoint with model {MODEL_NAME}", flush=True)
    else:
        if GROQ_API_KEY:
            print(
                "==> Gemini connection failed, but GROQ_API_KEY is configured for fallback.", flush=True)
        else:
            print("==> No working endpoint/model found. Read the FAILED lines above.\n"
                  "    The server will still start and retry on the next /decode.", flush=True)
    if "--check" in sys.argv:
        sys.exit(0 if ok else 1)

    port = 3000
    if not check_port_available(port):
        print(
            f"Port {port} is occupied. Attempting to release it...", flush=True)
        free_port(port)

    try:
        app.run(host="0.0.0.0", port=port, threaded=True)
    except OSError as e:
        print(f"\n[ERROR] Could not bind to port {port}: {e}", flush=True)
        print("To free port 3000 on Windows PowerShell, run:")
        print("  Stop-Process -Id (Get-NetTCPConnection -LocalPort 3000).OwningProcess -Force\n", flush=True)
