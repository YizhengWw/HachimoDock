"""Guard the ESP-SR integration contracts found by physical-board testing."""
from pathlib import Path
import re

SOURCE = (Path(__file__).resolve().parents[1] / "main/pet_p4_audio_frontend.c").read_text()


def test_fd_benchmark_is_explicitly_isolated_from_shipping_firmware():
    config = (Path(__file__).resolve().parents[1] / "platformio.ini").read_text()
    assert "#ifdef PET_P4_AEC_BENCH_FD" in SOURCE
    branch = SOURCE.split("#ifdef PET_P4_AEC_BENCH_FD", 1)[1].split("#endif", 1)[0]
    candidate, shipping = branch.split("#else", 1)
    assert "AEC_MODE_FD_LOW_COST" in candidate and "AEC_NLP_LEVEL_AGGR" in candidate
    assert "AEC_MODE_VOIP_LOW_COST" in shipping
    normal = config.split("[env:esp32_p4_evboard]", 1)[1].split("[env:", 1)[0]
    assert "PET_P4_AEC_BENCH_FD" not in normal
    bench = config.split("[env:esp32_p4_aec_fd_bench]", 1)[1].split("[env:", 1)[0]
    assert "0.7.59-p4-fdbench" in bench and "-DPET_P4_AEC_BENCH_FD=1" in bench


def test_platformio_firmware_version_matches_cmake_release_version():
    root = Path(__file__).resolve().parents[1]
    cmake = re.search(r'set\(PROJECT_VER "([^"]+)"\)', (root / "CMakeLists.txt").read_text()).group(1)
    platformio = re.search(r'^custom_p4_project_version\s*=\s*(\S+)', (root / "platformio.ini").read_text(), re.M).group(1)
    assert platformio == cmake


def test_ns_uses_two_ten_ms_blocks_per_usb_frame():
    # ESP-SR 2.5.3's linked NS asserts on 20 ms despite the header's broader docs.
    assert "#define FRAME 320" in SOURCE
    assert "#define NS_FRAME 160" in SOURCE
    assert "ns = ns_create(10);" in SOURCE
    assert "offset < FRAME; offset += NS_FRAME" in SOURCE
    assert "ns_process(ns, assembled.pcm + offset, denoised + offset)" in SOURCE


def test_aec_working_memory_does_not_exhaust_usb_dma_ram():
    assert ".caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in SOURCE
    assert "aec_create_from_config(&config)" in SOURCE
    assert "aec = aec_create(" not in SOURCE
    # Small aligned DSP hot blocks remain internal for processing throughput.
    assert SOURCE.count("heap_caps_aligned_alloc(16, chunk_samples") == 3
    assert "heap_caps_calloc(REF_SIZE, sizeof(int16_t), MALLOC_CAP_SPIRAM" in SOURCE
    main = (Path(__file__).resolve().parents[1] / "main/pet_p4_main.c").read_text()
    assert main.index("BaseType_t usb_rx_created =") < main.index("pet_p4_afe_init()")


def test_frontend_failure_revokes_capabilities():
    assert "atomic_exchange(&ready, false)" in SOURCE
    assert 'fail("reference overflow")' in SOURCE
    assert 'fail("processed queue overflow")' in SOURCE
    assert 'fail("DSP too slow")' in SOURCE
    assert "frame.generation != atomic_load(&generation)" in SOURCE


def test_new_capture_timeline_resets_adaptive_filters_on_dsp_owner():
    feed = SOURCE.split("bool pet_p4_afe_feed(", 1)[1].split("bool pet_p4_afe_take(", 1)[0]
    assert "if (feed_generation != current)" in feed
    assert "aec_destroy(aec); aec = aec_create_from_config(&config);" in feed
    assert "ns_destroy(ns); ns = ns_create(10);" in feed
    assert "vad_destroy(vad); vad = vad_create_with_param(" in feed
    assert 'fail("stream DSP reset failed")' in feed


def test_audio_uses_origin_link_without_per_frame_tx_drain():
    root = Path(__file__).resolve().parents[1] / "main"
    protocol = (root / "pet_p4_protocol.c").read_text()
    conversation = protocol.split('strcmp(topic, "audio/conversation") == 0', 1)[1]
    assert conversation.index("pet_p4_audio_set_transport(send_line, ctx)") < conversation.index("pet_p4_audio_conversation_set(")
    main = (root / "pet_p4_main.c").read_text()
    assert "transport_send_line_impl(line, ctx, !audio)" in main
    logger = main.split("static int transport_log_vprintf(", 1)[1].split("static void transport_send_line_impl(", 1)[0]
    assert "if (pet_p4_audio_active())" in logger
    assert "uart_wait_tx_done" in logger
    assert "usb_serial_jtag_wait_tx_done" in logger
    assert "if (pending)" in logger


def test_protocol_json_buffers_use_psram_without_changing_capacity():
    root = Path(__file__).resolve().parents[1] / "main"
    main = (root / "pet_p4_main.c").read_text()
    assert "#define PET_P4_PROTOCOL_LINE_BUFFER_BYTES 32768" in main
    assert main.count("heap_caps_calloc(1, PET_P4_PROTOCOL_LINE_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)") == 2
    assert "sizeof(g_line_buffer)" not in main
    assert "sizeof(g_uart_line_buffer)" not in main
    diagnostics = (root / "pet_p4_diagnostics.c").read_text()
    for key in ("freeInternalBytes", "largestInternalBlockBytes", "freeDmaBytes", "largestDmaBlockBytes", "largestPsramBlockBytes"):
        assert key in diagnostics


def test_audio_deadlines_precede_animation_workers():
    audio = (Path(__file__).resolve().parents[1] / "main/pet_p4_audio.c").read_text()
    assert 'xTaskCreatePinnedToCore(audio_task, "pet_p4_audio", 8192, NULL, 18, &g_audio_task, 0)' in audio
    assert 'xTaskCreatePinnedToCore(playback_task, "pet_p4_playback", 8192, NULL, 18, &g_playback_task, 0)' in audio
    assert "conversation && !atomic_load(&g_conversation_half_duplex) && pet_p4_afe_ready()" in audio


def test_duplex_reference_clock_includes_silence_and_bounds_dma_latency():
    root = Path(__file__).resolve().parents[1]
    audio = (root / "main/pet_p4_audio.c").read_text()
    silence = audio.split("static void stream_silence_step(", 1)[1].split("static void stream_drain_step_locked(", 1)[0]
    assert "!pet_p4_audio_conversation_active() || atomic_load(&g_conversation_half_duplex)" in silence
    assert silence.index("pet_p4_afe_reference(") < silence.index("esp_codec_dev_write(")
    assert audio.count("stream_silence_step(pcm, capacity);") == 3
    bsp = (root / "components/esp32_p4_wifi6_touch_lcd_4_3/esp32_p4_wifi6_touch_lcd_4_3.c").read_text()
    assert "chan_cfg.dma_desc_num = 3;" in bsp
    assert "chan_cfg.dma_frame_num = 160;" in bsp


def test_duplex_mic_headroom_preserves_ptt_gain_and_rx_scheduling():
    audio = (Path(__file__).resolve().parents[1] / "main/pet_p4_audio.c").read_text()
    assert 'strcmp(g_microphone_codec, "ES8311") != 0' in audio
    assert "duplex ? 24.0f : 30.0f" in audio
    assert audio.count("(void) conversation_mic_gain(false);") == 2
    assert "#define PET_P4_AUDIO_PLAYBACK_VOLUME 100" in audio
    playback = audio.split("static void playback_task(", 1)[1].split("static void audio_task(", 1)[0]
    assert "Immediate reacquisition can starve play_chunk" in playback
    assert "vTaskDelay(1);" in playback
    assert "if (!atomic_load(&g_stream_playing) || stream_buffered() == 0)" not in playback
