"""
AI model wrappers for SoloRolls D&D DM.

Uses:
  - Google Gemini (gemini-3.1-flash-lite) for both STT and LLM responses
  - Fish Audio for TTS (voice cloning + emotion support)

Environment variables:
  GEMINI_MODEL      Gemini model name           (default: gemini-3.1-flash-lite)
  GOOGLE_API_KEY    Google AI API key            (required)
  FISH_API_KEY      Fish Audio API key           (required)
  FISH_VOICE_ID     Fish Audio voice/model ID    (optional — uses default if not set)
"""

import io
import os

GEMINI_MODEL = os.getenv("GEMINI_MODEL", "gemini-3.1-flash-lite")
FISH_VOICE_ID = os.getenv("FISH_VOICE_ID", "")


class GeminiModel:
    """Handles both STT (audio transcription) and LLM responses via Gemini."""

    def __init__(self):
        from google import genai
        self.client = genai.Client()
        self.model = GEMINI_MODEL

    def transcribe(self, audio_bytes: bytes, language: str = "en") -> str:
        """Transcribe WAV audio to text using Gemini's audio understanding."""
        from google.genai import types

        prompt = (
            "Transcribe the following audio exactly as spoken. "
            "Return ONLY the transcription text, nothing else. "
            "Do not add timestamps, labels, or formatting."
        )

        response = self.client.models.generate_content(
            model=self.model,
            contents=[
                prompt,
                types.Part.from_bytes(data=audio_bytes, mime_type="audio/wav"),
            ],
        )
        return (response.text or "").strip()

    def detect_new_game_intent(self, text: str) -> bool:
        """Check if the player's text indicates they want to start a new game/campaign."""
        prompt = (
            "You are classifying player intent in a tabletop RPG session. "
            "Does the following player statement indicate they want to START A NEW GAME, "
            "reset the campaign, begin a fresh adventure, or create a new character from scratch? "
            "Respond with ONLY 'yes' or 'no'. Nothing else.\n\n"
            f"Player said: \"{text}\""
        )
        response = self.client.models.generate_content(
            model=self.model,
            contents=[prompt],
        )
        answer = (response.text or "").strip().lower()
        return answer.startswith("yes")

    def respond(self, user_text: str, system_prompt: str) -> str:
        """Single-turn LLM response (no history)."""
        from google.genai import types

        response = self.client.models.generate_content(
            model=self.model,
            contents=[user_text],
            config=types.GenerateContentConfig(
                system_instruction=system_prompt,
            ),
        )
        return (response.text or "").strip()

    def respond_with_history(
        self, user_text: str, system_prompt: str, history: list[dict]
    ) -> str:
        """Multi-turn LLM response with full conversation history."""
        from google.genai import types

        # Build contents list from history
        contents = []
        for msg in history:
            role = "user" if msg["role"] == "user" else "model"
            contents.append(
                types.Content(
                    role=role,
                    parts=[types.Part(text=msg["content"])],
                )
            )
        # Add current user message
        contents.append(
            types.Content(
                role="user",
                parts=[types.Part(text=user_text)],
            )
        )

        response = self.client.models.generate_content(
            model=self.model,
            contents=contents,
            config=types.GenerateContentConfig(
                system_instruction=system_prompt,
            ),
        )
        return (response.text or "").strip()


class FishTTS:
    """Text-to-speech using Fish Audio."""

    def __init__(self):
        from fishaudio import FishAudio
        self.client = FishAudio()  # reads FISH_API_KEY from env

    def speak(self, text: str, voice: str | None = None) -> bytes:
        """Synthesize text to MP3 bytes using Fish Audio."""
        reference_id = voice if voice else FISH_VOICE_ID

        kwargs = {"text": text, "format": "mp3"}
        if reference_id:
            kwargs["reference_id"] = reference_id

        return self.client.tts.convert(**kwargs)


_gemini: GeminiModel | None = None
_tts: FishTTS | None = None


def _get_gemini() -> GeminiModel:
    global _gemini
    if _gemini is None:
        _gemini = GeminiModel()
    return _gemini


def _get_tts() -> FishTTS:
    global _tts
    if _tts is None:
        _tts = FishTTS()
    return _tts


def transcribe(wav_bytes: bytes, language: str = "en") -> str:
    return _get_gemini().transcribe(wav_bytes, language)


def detect_new_game_intent(text: str) -> bool:
    """Check if the player wants to start a new game/campaign."""
    return _get_gemini().detect_new_game_intent(text)


def respond(user_text: str, system_prompt: str) -> str:
    return _get_gemini().respond(user_text, system_prompt)


def respond_with_history(
    user_text: str, system_prompt: str, history: list[dict]
) -> str:
    """Respond using full conversation history for multi-turn campaigns."""
    return _get_gemini().respond_with_history(user_text, system_prompt, history)


def speak(text: str, voice: str | None = None) -> bytes:
    return _get_tts().speak(text, voice)


def stt_lang(config: dict) -> str:
    return config.get("ttsModel", "en") or "en"


def resolve_voice(config: dict) -> str | None:
    """Return Fish Audio voice/reference ID from user config, or None to use default."""
    v = config.get("ttsVoice") or ""
    return v if v else None
