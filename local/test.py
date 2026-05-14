import ollama
from faster_whisper import WhisperModel
from RealtimeTTS import TextToAudioStream, KokoroEngine

class STTModel:
    def __init__(self, model_name="base"):
        # compute_type="float32" resolves internal float16 CPU execution warnings
        self.model = WhisperModel(model_name, compute_type="float32")

    def transcribe(self, audio_path):
        segments, info = self.model.transcribe(audio_path, beam_size=5)
        return "".join([segment.text for segment in segments])


class ChatModel:
    def __init__(self, model_name="llama3"):
        self.model_name = model_name

    def ask_stream(self, text):
        """Streams raw token chunks straight out of Ollama."""
        response_stream = ollama.chat(
            model=self.model_name,
            messages=[{'role': 'user', 'content': text}],
            stream=True
        )
        for chunk in response_stream:
            yield chunk['message']['content']


class TTSModel:
    def __init__(self):
        # Neural local engine natively compatible with token generator streaming
        # Downloads model files transparently to user directory on first runtime
        self.engine = KokoroEngine(voice="af_bella") 
        self.stream = TextToAudioStream(self.engine)

    def speak_stream(self, generator):
        """Pipes the LLM generator into the engine and blocks exit until finished."""
        self.stream.feed(generator)
        self.stream.play()


def main():
    # Model Swapping Node
    stt = STTModel("small")
    chat = ChatModel("gemma4:31b-cloud")
    tts = TTSModel()

    # Step 1: Transcribe user file
    user_text = stt.transcribe("../test/test.wav")
    print(f"USER:\n{user_text}\n")

    # Step 2: Get continuous text generator 
    reply_generator = chat.ask_stream(user_text)
    print("REPLY: (Streaming audio...)\n")

    # Step 3: Stream and voice simultaneously 
    tts.speak_stream(reply_generator)


if __name__ == "__main__":
    main()
