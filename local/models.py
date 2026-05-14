"""
Local AI model wrappers.

  STT_MODEL   faster-whisper model name  (default: base)
  LLM_MODEL   Ollama model name          (default: llama3)
  TTS_VOICE   kokoro voice name          (default: af_luna)

ttsVoice in user config is a kokoro voice name (af_luna, am_michael, etc.)
ttsModel controls STT language detection only.
"""

import io
import os

import numpy as np

STT_MODEL = os.getenv("STT_MODEL", "small.en")
LLM_MODEL = os.getenv("LLM_MODEL", "gemma4:31b-cloud")
TTS_VOICE = os.getenv("TTS_VOICE", "af_bella")
TTS_SAMPLE_RATE = 24000


class STTModel:
    def __init__(self):
        from faster_whisper import WhisperModel
        self.model = WhisperModel(STT_MODEL, compute_type="float32")

    def transcribe(self, audio_bytes: bytes, language: str = "en") -> str:
        segments, _ = self.model.transcribe(io.BytesIO(audio_bytes), language=language)
        return " ".join(s.text.strip() for s in segments).strip()


class LLMModel:
    def __init__(self):
        self.model_name = LLM_MODEL

    def respond(self, user_text: str, system_prompt: str) -> str:
        import ollama
        messages = [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_text},
        ]
        response = ollama.chat(model=self.model_name, messages=messages)
        return response["message"]["content"].strip()


class TTSModel:
    def __init__(self):
        from RealtimeTTS import KokoroEngine
        self.engine = KokoroEngine(voice=TTS_VOICE)

    def speak(self, text: str, voice: str | None = None) -> bytes:
        """Returns MP3 bytes."""
        import lameenc

        if voice:
            self.engine.set_voice(voice)

        pipeline = self.engine._get_pipeline(self.engine.current_lang)
        voice_arg = self.engine.current_voice

        chunks = []
        for result in pipeline(text, voice=voice_arg, speed=self.engine.speed):
            chunks.append(result.audio.cpu().numpy())

        if not chunks:
            return b""

        audio = np.concatenate(chunks)
        pcm = (np.clip(audio, -1.0, 1.0) * 32767).astype(np.int16)
        enc = lameenc.Encoder()
        enc.set_bit_rate(32)
        enc.set_in_sample_rate(24000)
        enc.set_channels(1)
        enc.set_quality(7)
        return enc.encode(pcm.tobytes()) + enc.flush()


_stt: STTModel | None = None
_llm: LLMModel | None = None
_tts: TTSModel | None = None


def _get_stt() -> STTModel:
    global _stt
    if _stt is None:
        _stt = STTModel()
    return _stt


def _get_llm() -> LLMModel:
    global _llm
    if _llm is None:
        _llm = LLMModel()
    return _llm


def _get_tts() -> TTSModel:
    global _tts
    if _tts is None:
        _tts = TTSModel()
    return _tts


def transcribe(wav_bytes: bytes, language: str = "en") -> str:
    return _get_stt().transcribe(wav_bytes, language)


def respond(user_text: str, system_prompt: str) -> str:
    return _get_llm().respond(user_text, system_prompt)


def speak(text: str, voice: str | None = None) -> bytes:
    return _get_tts().speak(text, voice)


def stt_lang(config: dict) -> str:
    return config.get("ttsModel", "en") or "en"


KOKORO_VOICES = {
    "af_bella", "af_heart", "af_luna", "af_nicole", "af_sarah", "af_sky",
    "am_adam", "am_echo", "am_fable", "am_liam", "am_michael", "am_onyx",
    "bf_emma", "bf_isabella", "bm_george", "bm_lewis",
    "ef_dora", "em_alex",
}


def resolve_voice(config: dict) -> str | None:
    """Return kokoro voice name from user config, or None to use default."""
    v = config.get("ttsVoice") or ""
    return v if v in KOKORO_VOICES else None
