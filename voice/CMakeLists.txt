add_library(nexa_voice STATIC
    VoiceBackends.h VoiceBackends.cc
    VoiceTools.h VoiceTools.cc
    Wav.h Wav.cc
    capture/AudioCapture.h capture/AudioCapture.cc
    capture/AudioConverter.h capture/AudioConverter.cc
    capture/EnergyVad.h capture/EnergyVad.cc
    capture/IAudioInput.h
    pipeline/VoicePipeline.h pipeline/VoicePipeline.cc
    pipeline/WakeWordDetector.h pipeline/WakeWordDetector.cc
    stt/CloudRecognizer.h stt/CloudRecognizer.cc
    stt/ISpeechRecognizer.h
    stt/RecognizerChain.h stt/RecognizerChain.cc
    stt/Transcript.h stt/Transcript.cc
    tts/AudioFileDecoder.h tts/AudioFileDecoder.cc
    tts/CloudSynthesizer.h tts/CloudSynthesizer.cc
    tts/ISpeechSynthesizer.h
    tts/ITextToSpeech.h
    tts/PcmPlayer.h tts/PcmPlayer.cc
    tts/PiperSynthesizer.h tts/PiperSynthesizer.cc
    tts/SentenceChunker.h tts/SentenceChunker.cc
    tts/SpeechdTtsBackend.h tts/SpeechdTtsBackend.cc
    tts/SynthesizedSpeech.h tts/SynthesizedSpeech.cc
    tts/TtsFileRenderer.h tts/TtsFileRenderer.cc
    wake/WordPieces.h wake/WordPieces.cc
)

target_link_libraries(nexa_voice
    PUBLIC
        nexa_core
        Qt6::Multimedia
        Qt6::TextToSpeech
    PRIVATE
        nexa_options
)

if(NEXA_BUILD_WHISPER)
    target_sources(nexa_voice PRIVATE stt/WhisperRecognizer.h stt/WhisperRecognizer.cc)
    target_compile_definitions(nexa_voice PRIVATE NEXA_HAVE_WHISPER)
    target_link_libraries(nexa_voice PRIVATE whisper)
endif()
