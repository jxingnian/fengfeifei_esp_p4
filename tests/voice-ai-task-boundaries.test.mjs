// Source-level regression guards for the stack-overflow call path.
// This does not execute ESP-IDF, the Opus binary library, or measure stack usage.
// Run with: node tests/voice-ai-task-boundaries.test.mjs
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

const source = await readFile(new URL('../components/voice_ai_service/voice_ai_service.c', import.meta.url), 'utf8');
function body(name) {
    const match = source.match(new RegExp(`(?:static\\s+)?(?:void|bool|esp_err_t)\\s+${name}\\([^)]*\\)\\s*\\{[\\s\\S]*?\\n\\}`));
    assert.ok(match, `Missing function ${name}`);
    return match[0];
}

const capture = body('voice_ai_service_on_pcm');
assert.doesNotMatch(capture, /esp_opus_|cloud_gateway_send|s_encoder_mutex|portMAX_DELAY/,
    'The 8KB microphone task must not encode or wait for encoding/network');
assert.match(capture, /xSemaphoreTake\(s_capture_mutex, 0\)/);
assert.match(capture, /xQueueSend\(s_encode_queue, &s_capture_frame, 0\)/);
assert.match(capture, /xQueueReceive\(s_encode_queue, &discarded, 0\)/);
assert.match(capture, /s_capture_frame\.generation = atomic_load\(&s_capture_generation\)/);

const encoder = body('voice_ai_encode_worker');
assert.match(encoder, /esp_opus_enc_process/);
assert.match(encoder, /cloud_gateway_send/);
assert.equal((encoder.match(/voice_ai_pcm_frame_is_current\(&frame\)/g) ?? []).length, 2,
    'Validate session generation both before encoding and before sending');
assert.match(encoder, /uxTaskGetStackHighWaterMark\(NULL\)/);
assert.match(source, /#define VOICE_AI_ENCODER_STACK_BYTES \(32U \* 1024U\)/);
assert.match(source, /#define VOICE_AI_WORKER_STACK_BYTES \(24U \* 1024U\)/);
assert.match(body('voice_ai_service_init'), /xTaskCreate\(voice_ai_encode_worker, "voice_ai_enc", VOICE_AI_ENCODER_STACK_BYTES/);
assert.match(body('voice_ai_service_init'), /xTaskCreate\(voice_ai_worker, "voice_ai", VOICE_AI_WORKER_STACK_BYTES/);
assert.match(body('voice_ai_reset_capture'), /atomic_fetch_add\(&s_capture_generation, 1\)/);
assert.match(body('voice_ai_reset_capture'), /xQueueReset\(s_encode_queue\)/);
assert.match(body('voice_ai_finish_session'), /voice_ai_reset_capture\(\)/);
assert.match(body('voice_ai_parse_ready'), /voice_ai_reset_capture\(\)/);
assert.match(body('voice_ai_service_on_wake_detected'), /voice_ai_reset_capture\(\)/);
assert.match(body('voice_ai_worker'), /esp_opus_dec_decode/);
assert.match(body('voice_ai_worker'), /uxTaskGetStackHighWaterMark\(NULL\)/);

export const results = 'PASS: audio task boundaries, dedicated codec stacks, nonblocking PCM queue, stale-frame generation checks, stack telemetry (source guards only)';
console.log(results);
