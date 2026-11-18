"""
AI helpers for Pala Lite.

  * Gemini Flash (free tier)            -> transcription of voice notes
  * Text LLM (Gemini Flash or Cerebras) -> organising notes + writing briefings
  * Gemini native-audio (Live API)      -> spoken answers to spoken questions
  * Gemini TTS                          -> reading briefings aloud

Every step has a fallback so one model hitting a rate limit doesn't break the device.
"""
import asyncio
import io
import json
import os
import re
import wave
from array import array

from dotenv import load_dotenv
from google import genai
from google.genai import types

load_dotenv()

GEMINI_API_KEY = os.getenv("GEMINI_API_KEY", "")
GEMINI_TEXT_MODEL = os.getenv("GEMINI_TEXT_MODEL", "gemini-2.5-flash")
GEMINI_LIVE_MODEL = os.getenv("GEMINI_LIVE_MODEL", "gemini-2.5-flash-native-audio-preview-12-2025")
GEMINI_TTS_MODEL = os.getenv("GEMINI_TTS_MODEL", "gemini-2.5-flash-preview-tts")
VOICE = os.getenv("VOICE", "Kore")
LANGUAGE = os.getenv("LANGUAGE", "English")

TEXT_LLM = os.getenv("TEXT_LLM", "gemini").lower()          # "gemini" or "cerebras"
CEREBRAS_API_KEY = os.getenv("CEREBRAS_API_KEY", "")
CEREBRAS_BASE_URL = os.getenv("CEREBRAS_BASE_URL", "https://api.cerebras.ai/v1")
CEREBRAS_MODEL = os.getenv("CEREBRAS_MODEL", "gpt-oss-120b")

_client = None


def gclient():
    global _client
    if _client is None:
        if not GEMINI_API_KEY:
            raise RuntimeError("GEMINI_API_KEY is missing in server/.env")
        _client = genai.Client(api_key=GEMINI_API_KEY)
    return _client


# --------------------------------------------------------------------------
# Audio utilities (pure Python, no numpy needed)
# --------------------------------------------------------------------------
def wav_to_pcm(wav_bytes: bytes):
    """Return (pcm16_mono_bytes, sample_rate). Survives a broken header."""
    try:
        with wave.open(io.BytesIO(wav_bytes)) as w:
            rate, ch, sw = w.getframerate(), w.getnchannels(), w.getsampwidth()
            pcm = w.readframes(w.getnframes())
        if pcm and sw == 2:
            if ch == 2:
                a = array("h"); a.frombytes(pcm)
                pcm = a[0::2].tobytes()
            return pcm, rate
    except Exception:
        pass
    data = wav_bytes[44:] if len(wav_bytes) > 44 else b""   # device lost power mid-recording
    return data[: len(data) // 2 * 2], 16000


def pcm_to_wav(pcm: bytes, rate: int) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm)
    return buf.getvalue()


def resample(pcm: bytes, src: int, dst: int) -> bytes:
    """Linear-interpolation resampler for 16-bit mono PCM."""
    if src == dst or not pcm:
        return pcm
    a = array("h"); a.frombytes(pcm)
    n_out = int(len(a) * dst / src)
    out = array("h", [0]) * n_out
    step = src / dst
    last = len(a) - 1
    for i in range(n_out):
        pos = i * step
        j = int(pos)
        if j >= last:
            out[i] = a[last]
        else:
            f = pos - j
            out[i] = int(a[j] * (1 - f) + a[j + 1] * f)
    return out.tobytes()


# --------------------------------------------------------------------------
# Text LLM (Gemini or Cerebras)
# --------------------------------------------------------------------------
def chat(system: str, user: str, want_json: bool = False) -> str:
    if TEXT_LLM == "cerebras" and CEREBRAS_API_KEY:
        try:
            from openai import OpenAI
            c = OpenAI(base_url=CEREBRAS_BASE_URL, api_key=CEREBRAS_API_KEY)
            r = c.chat.completions.create(
                model=CEREBRAS_MODEL,
                messages=[{"role": "system", "content": system}, {"role": "user", "content": user}],
                temperature=0.2,
            )
            return r.choices[0].message.content or ""
        except Exception as e:
            print("[ai] Cerebras failed, falling back to Gemini:", e)
    cfg = types.GenerateContentConfig(
        system_instruction=system,
        temperature=0.2,
        response_mime_type="application/json" if want_json else None,
    )
    r = gclient().models.generate_content(model=GEMINI_TEXT_MODEL, contents=user, config=cfg)
    return r.text or ""


def parse_json(text: str):
    text = re.sub(r"```(json)?", "", text or "")
    s, e = text.find("{"), text.rfind("}")
    if s == -1 or e == -1:
        raise ValueError("No JSON in model reply: " + text[:200])
    return json.loads(text[s : e + 1])


# --------------------------------------------------------------------------
# 1. Transcription
# --------------------------------------------------------------------------
def transcribe(pcm: bytes, rate: int) -> str:
    r = gclient().models.generate_content(
        model=GEMINI_TEXT_MODEL,
        contents=[
            types.Part.from_bytes(data=pcm_to_wav(pcm, rate), mime_type="audio/wav"),
            "Transcribe this voice recording word for word. It may mix English and Hindi; "
            "write Hindi words in Latin letters. Output only the transcript. "
            "If there is no speech, output exactly: [silence]",
        ],
    )
    return (r.text or "").strip()


# --------------------------------------------------------------------------
# 2. Organising a note into tasks / appointments / memories
# --------------------------------------------------------------------------
ORGANIZE_SYSTEM = """You organise voice notes for a personal assistant device.
Split the transcript into one or more items. Each item is exactly one of:
- "task": something the user needs to do (optional due date/time)
- "appointment": a meeting/event at a specific date and time
- "memory": an idea, fact or anything to remember
Resolve relative dates ("tomorrow", "next Monday", "at 5") using the current local time given.
Reply with JSON only:
{"items":[{"type":"task|appointment|memory","title":"max 8 words","summary":"one sentence",
"due":"YYYY-MM-DDTHH:MM or null","start":"YYYY-MM-DDTHH:MM or null","tags":["word"]}]}
Use "due" only for tasks and "start" only for appointments. Usually there is just one item."""


def organize(transcript: str, now_local: str):
    if not transcript or transcript == "[silence]":
        return [{"type": "memory", "title": "Empty recording", "summary": "No speech detected.",
                 "due": None, "start": None, "tags": []}]
    reply = chat(ORGANIZE_SYSTEM, f"Current local time: {now_local}\nTranscript: {transcript}", want_json=True)
    items = parse_json(reply).get("items") or []
    clean = []
    for it in items:
        t = str(it.get("type", "memory")).lower()
        clean.append({
            "type": t if t in ("task", "appointment", "memory") else "memory",
            "title": str(it.get("title") or transcript[:40]),
            "summary": str(it.get("summary") or ""),
            "due": it.get("due") or None,
            "start": it.get("start") or None,
            "tags": [str(x) for x in (it.get("tags") or [])][:5],
        })
    return clean or [{"type": "memory", "title": transcript[:40], "summary": transcript,
                      "due": None, "start": None, "tags": []}]


# --------------------------------------------------------------------------
# 3. Speech output
# --------------------------------------------------------------------------
def _speech_config():
    return types.SpeechConfig(
        voice_config=types.VoiceConfig(prebuilt_voice_config=types.PrebuiltVoiceConfig(voice_name=VOICE))
    )


async def _live_session(system: str, pcm16k: bytes = None, text: str = None, timeout: float = 60):
    """One turn with the Gemini native-audio Live model. Returns (pcm24k, heard, said)."""
    cfg = types.LiveConnectConfig(
        response_modalities=[types.Modality.AUDIO],
        system_instruction=system,
        speech_config=_speech_config(),
        input_audio_transcription=types.AudioTranscriptionConfig(),
        output_audio_transcription=types.AudioTranscriptionConfig(),
    )
    audio, heard, said = bytearray(), [], []

    async def run():
        async with gclient().aio.live.connect(model=GEMINI_LIVE_MODEL, config=cfg) as s:
            if text:
                await s.send_realtime_input(text=text)
            if pcm16k:
                pcm = pcm16k + b"\x00\x00" * 16000          # 1 s of silence = "I'm done talking"
                for i in range(0, len(pcm), 3200):          # 100 ms chunks
                    await s.send_realtime_input(audio=types.Blob(data=pcm[i : i + 3200], mime_type="audio/pcm;rate=16000"))
                await s.send_realtime_input(audio_stream_end=True)
            async for msg in s.receive():
                if msg.data:
                    audio.extend(msg.data)
                sc = msg.server_content
                if sc:
                    if sc.input_transcription and sc.input_transcription.text:
                        heard.append(sc.input_transcription.text)
                    if sc.output_transcription and sc.output_transcription.text:
                        said.append(sc.output_transcription.text)
                    if sc.turn_complete:
                        break

    await asyncio.wait_for(run(), timeout)
    return bytes(audio), "".join(heard).strip(), "".join(said).strip()


def tts(text: str):
    """Text -> (pcm, rate). Tries the TTS model first, then the Live model."""
    try:
        r = gclient().models.generate_content(
            model=GEMINI_TTS_MODEL,
            contents=f"Read this aloud in a warm, clear, natural voice:\n{text}",
            config=types.GenerateContentConfig(response_modalities=["AUDIO"], speech_config=_speech_config()),
        )
        pcm = r.candidates[0].content.parts[0].inline_data.data
        if pcm:
            return pcm, 24000
    except Exception as e:
        print("[ai] TTS model failed, trying Live model:", e)
    pcm, _, _ = asyncio.run(_live_session(
        "You are a text-to-speech voice. Read the user's text aloud exactly, adding nothing.", text=text))
    return pcm, 24000


# --------------------------------------------------------------------------
# 4. Questions about previous recordings
# --------------------------------------------------------------------------
def _qa_system(context: str) -> str:
    return (f"You are Pala, a friendly voice assistant on a small pocket recorder. Answer in {LANGUAGE}. "
            "Answer the user's spoken question using ONLY the notes below. If the answer isn't in the notes, "
            "say so briefly. Keep answers short (under 30 seconds of speech), no lists or markdown.\n\n"
            f"USER'S NOTES:\n{context}")


def answer_spoken_question(pcm: bytes, rate: int, context: str):
    """Returns (pcm, rate, question_text, answer_text)."""
    pcm16 = resample(pcm, rate, 16000)
    try:
        out, heard, said = asyncio.run(_live_session(_qa_system(context), pcm16k=pcm16))
        if out:
            return out, 24000, heard, said
        print("[ai] Live model returned no audio, using fallback")
    except Exception as e:
        print("[ai] Live model failed, using fallback:", e)
    question = transcribe(pcm, rate)
    answer = answer_text_question(question, context)
    out, out_rate = tts(answer)
    return out, out_rate, question, answer


def answer_text_question(question: str, context: str) -> str:
    return chat(_qa_system(context), question).strip()


# --------------------------------------------------------------------------
# 5. Briefings
# --------------------------------------------------------------------------
def briefing_text(kind: str, now_local: str, context: str) -> str:
    period = "this week (next 7 days)" if kind == "week" else "today"
    system = (f"You write spoken briefings for a voice assistant, in {LANGUAGE}. Natural speech, no lists, "
              "no markdown, no emojis. 40-120 words. Mention appointments with times, the most important "
              "open tasks (overdue first) and, if useful, one relevant memory. Be warm and concise.")
    return chat(system, f"Current local time: {now_local}\nWrite the briefing for {period}.\n\nNOTES:\n{context}").strip()
