"""
Pala Lite server – run this on a laptop on the same Wi-Fi as the ESP32.

    pip install -r requirements.txt
    python server.py

Then open http://localhost:5000 for the web viewer.
"""
import datetime as dt
import os
import socket
import unicodedata
import uuid
from zoneinfo import ZoneInfo

from dotenv import load_dotenv
from flask import Flask, Response, abort, jsonify, render_template, request, send_from_directory

load_dotenv()
import ai  # noqa: E402
import db  # noqa: E402

TZ = ZoneInfo(os.getenv("TIMEZONE", "Asia/Kolkata"))
DEVICE_TOKEN = os.getenv("DEVICE_TOKEN", "change-me")
AUDIO_DIR = os.path.join(os.path.dirname(__file__), "data", "audio")
os.makedirs(AUDIO_DIR, exist_ok=True)

app = Flask(__name__)
db.init()


# --------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------
def now():
    return dt.datetime.now(TZ)


def now_str():
    return now().strftime("%A %Y-%m-%d %H:%M")


def iso(d):
    return d.strftime("%Y-%m-%dT%H:%M")


def check_token():
    if request.headers.get("X-Token") != DEVICE_TOKEN:
        abort(401)


def ascii_only(s, n=200):
    s = unicodedata.normalize("NFKD", s or "").encode("ascii", "ignore").decode()
    return " ".join(s.split())[:n]


def save_audio(pcm, rate, prefix):
    name = f"{prefix}_{now().strftime('%Y%m%d_%H%M%S')}_{uuid.uuid4().hex[:6]}.wav"
    with open(os.path.join(AUDIO_DIR, name), "wb") as f:
        f.write(ai.pcm_to_wav(pcm, rate))
    return name


def recorded_at_from_filename(fname):
    """Device names files note_<unix>.wav when its clock is synced."""
    try:
        ts = int(fname.split("_")[1].split(".")[0])
        if ts > 1700000000:
            return iso(dt.datetime.fromtimestamp(ts, TZ))
    except Exception:
        pass
    return iso(now())


def human_when(s):
    try:
        d = dt.datetime.fromisoformat(s)
    except Exception:
        return s or ""
    days = (d.date() - now().date()).days
    day = "Today" if days == 0 else "Tomorrow" if days == 1 else d.strftime("%a %d %b")
    return f"{day} {d.strftime('%H:%M')}"


def build_context():
    """Everything the AI needs to answer questions / make briefings (kept small for free tiers)."""
    n = now()
    lines = [f"Now: {now_str()}", "", "OPEN TASKS:"]
    for t in db.open_tasks()[:40]:
        lines.append(f"- {t['title']}" + (f" (due {t['due']})" if t["due"] else "") +
                     (f": {t['summary']}" if t["summary"] else ""))
    lines += ["", "APPOINTMENTS (past 7 days and next 30 days):"]
    for a in db.appointments_between(iso(n - dt.timedelta(days=7)), iso(n + dt.timedelta(days=30))):
        lines.append(f"- {a['start']}: {a['title']}" + (f" - {a['summary']}" if a["summary"] else ""))
    lines += ["", "MEMORIES (newest first):"]
    for m in db.recent_memories(60):
        lines.append(f"- [{m['created_at'][:10]}] {m['title']}: {m['summary'] or m['transcript'] or ''}")
    return "\n".join(lines)[:12000]


def process_recording(pcm, rate, audio_name, markers, created_at):
    """Transcribe + organise. If AI fails, keep the recording as a 'pending' memory."""
    try:
        transcript = ai.transcribe(pcm, rate)
        items = ai.organize(transcript, now_str())
    except Exception as e:
        print("[server] AI failed, saving as pending:", e)
        it = {"type": "memory", "title": "Unprocessed recording", "summary": "AI was unavailable - click Reprocess.",
              "due": None, "start": None, "tags": []}
        it["id"] = db.add_item(it, "", audio_name, markers, created_at, pending=True)
        return [it], ""
    for it in items:
        it["id"] = db.add_item(it, transcript, audio_name, markers, created_at)
    return items, transcript


def wav_response(pcm, rate, text=""):
    pcm16 = ai.resample(pcm, rate, 16000)        # 16 kHz keeps files small on the ESP32
    resp = Response(ai.pcm_to_wav(pcm16, 16000), mimetype="audio/wav")
    resp.headers["X-Answer-Text"] = ascii_only(text)
    return resp


# --------------------------------------------------------------------------
# device API
# --------------------------------------------------------------------------
@app.post("/api/note")
def api_note():
    check_token()
    pcm, rate = ai.wav_to_pcm(request.get_data())
    fname = request.headers.get("X-Filename", "")
    audio = save_audio(pcm, rate, "note")
    items, transcript = process_recording(pcm, rate, audio, request.headers.get("X-Markers", ""),
                                          recorded_at_from_filename(fname))
    print(f"[note] {transcript!r} -> {[(i['type'], i['title']) for i in items]}")
    return jsonify(items=[{"type": i["type"], "title": ascii_only(i["title"], 60)} for i in items],
                   transcript=transcript)


@app.post("/api/ask")
def api_ask():
    check_token()
    pcm, rate = ai.wav_to_pcm(request.get_data())
    out, out_rate, question, answer = ai.answer_spoken_question(pcm, rate, build_context())
    db.add_qa(question, answer, save_audio(out, out_rate, "answer"), iso(now()))
    print(f"[ask] Q: {question!r}\n      A: {answer!r}")
    return wav_response(out, out_rate, answer)


@app.get("/api/briefing")
def api_briefing():
    if request.headers.get("X-Token") and request.headers.get("X-Token") != DEVICE_TOKEN:
        abort(401)
    kind = "week" if request.args.get("kind") == "week" else "day"
    text = ai.briefing_text(kind, now_str(), build_context())
    pcm, rate = ai.tts(text)
    print(f"[briefing:{kind}] {text}")
    return wav_response(pcm, rate, text)


@app.get("/api/home")
def api_home():
    tasks = db.open_tasks()
    n = now()
    appts = db.appointments_between(iso(n), iso(n + dt.timedelta(days=60)))
    a = appts[0] if appts else None
    return jsonify(
        open_tasks=len(tasks),
        top_task=ascii_only(tasks[0]["title"], 40) if tasks else "",
        next_appt=ascii_only(a["title"], 40) if a else "",
        next_when=human_when(a["start"]) if a else "",
    )


# --------------------------------------------------------------------------
# web viewer API
# --------------------------------------------------------------------------
@app.get("/")
def index():
    return render_template("index.html")


@app.get("/api/items")
def api_items():
    return jsonify(db.list_items(request.args.get("q", ""), request.args.get("type", "")))


@app.post("/api/items/<int:item_id>")
def api_update(item_id):
    data = request.get_json(force=True)
    allowed = {k: v for k, v in data.items() if k in ("done", "title", "summary", "due", "start", "type")}
    if "done" in allowed:
        allowed["done"] = int(bool(allowed["done"]))
    db.update_item(item_id, **allowed)
    return jsonify(db.get_item(item_id))


@app.delete("/api/items/<int:item_id>")
def api_delete(item_id):
    db.delete_item(item_id)
    return jsonify(ok=True)


@app.post("/api/items/<int:item_id>/reprocess")
def api_reprocess(item_id):
    it = db.get_item(item_id)
    if not it or not it["audio_file"]:
        abort(404)
    with open(os.path.join(AUDIO_DIR, it["audio_file"]), "rb") as f:
        pcm, rate = ai.wav_to_pcm(f.read())
    items, _ = process_recording(pcm, rate, it["audio_file"], it["markers"], it["created_at"])
    if not (len(items) == 1 and items[0]["title"] == "Unprocessed recording"):
        db.delete_item(item_id)
    return jsonify(items=items)


@app.post("/api/text_note")
def api_text_note():
    text = (request.get_json(force=True).get("text") or "").strip()
    items = ai.organize(text, now_str())
    for it in items:
        it["id"] = db.add_item(it, text, None, "", iso(now()))
    return jsonify(items=items)


@app.post("/api/ask_text")
def api_ask_text():
    q = (request.get_json(force=True).get("question") or "").strip()
    a = ai.answer_text_question(q, build_context())
    db.add_qa(q, a, None, iso(now()))
    return jsonify(answer=a)


@app.get("/api/qa")
def api_qa():
    return jsonify(db.list_qa())


@app.get("/audio/<path:name>")
def audio(name):
    return send_from_directory(AUDIO_DIR, name, as_attachment=request.args.get("download") == "1")


def lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except Exception:
        return "127.0.0.1"
    finally:
        s.close()


if __name__ == "__main__":
    ip = lan_ip()
    print("=" * 60)
    print(f" Pala Lite server running")
    print(f" Web viewer : http://localhost:5000")
    print(f" Put this in config.h ->  #define SERVER_HOST \"{ip}\"")
    print("=" * 60)
    app.run(host="0.0.0.0", port=5000, threaded=True)
