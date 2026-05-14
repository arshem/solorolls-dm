This is hte start of a local STT -> chat -> TTS pipeline, for local use. It needs a lot of tuning.

## configuration

make a src/local/users.json file liek this:

```json
{
	"admin": "ADMIN_KEY",
	"API_KEY": {
		"name": "Test Guy",
		"personality": "You are a helpful pocket assistant. Keep it very brief. Your responses will be spoken aloud, so get right to the point. No emojis, markdown, or fluff, just get to the point.",
		"ttsModel": "en",
		"ttsVoice": "am_michael"
	}
}
```

replace `ADMIN_KEY` and `API_KEY` with yours.

Run with `python server.py`

You also have the same UI for editing, so you can tweak the personality.

Check out [this](https://github.com/sancliffe/ollama-STT-TTS) for examples of lots of stuff. Each model needs a lot of configuration and code to get it working, so it's not very modular. it would be better if it could be all centralized,