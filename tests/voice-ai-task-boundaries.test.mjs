// Source guards supplement (not replace) runtime policy/flow tests and board tests.
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
const source = await readFile(new URL('../components/voice_ai_service/voice_ai_service.c', import.meta.url), 'utf8');
function body(name) {
  const match = source.match(new RegExp(`(?:static\\s+)?(?:void|bool|esp_err_t)\\s+${name}\\([^)]*\\)\\s*\\{[\\s\\S]*?\\n\\}`));
  assert.ok(match, `Missing ${name}`); return match[0];
}
assert.doesNotMatch(body('voice_ai_service_on_pcm'), /esp_opus_|cloud_gateway_send|portMAX_DELAY|s_encoder_mutex/);
assert.match(body('voice_ai_service_on_pcm'), /xSemaphoreTake\(s_capture_mutex, 0\)/);
assert.doesNotMatch(body('voice_ai_service_on_wake_detected'), /portMAX_DELAY|cloud_gateway/);
assert.match(body('encode_send'), /esp_opus_enc_process/);
assert.equal((body('encode_send').match(/capture_current\(frame\)/g) || []).length, 2);
assert.match(source, /#define VOICE_AI_ENCODER_STACK_BYTES \(32U \* 1024U\)/);
assert.match(body('voice_ai_decode_worker'), /esp_opus_dec_decode/);
assert.doesNotMatch(body('voice_ai_decode_worker'), /voice_board_audio_write/);
assert.match(body('voice_ai_output_worker'), /voice_board_audio_write/);
assert.doesNotMatch(body('voice_ai_worker'), /esp_opus_.*process|esp_opus_dec_decode|voice_board_audio_write/);
assert.match(body('try_drain'), /s_decoder_mutex/);
assert.match(body('try_drain'), /s_output_mutex/);
assert.match(body('try_drain'), /voice_board_audio_drain/);
assert.match(body('voice_ai_service_stop_session'), /finish\(.*true, false\)/);
const gateway = await readFile(new URL('../components/cloud_gateway/cloud_gateway.c', import.meta.url), 'utf8');
assert.match(gateway, /queue == s_control_tx_queue[\s\S]*?control_head.audio_barrier[\s\S]*?audio_head.sequence < control_head.sequence/);
assert.match(gateway, /if \(queue == s_audio_tx_queue\)[\s\S]*?xQueuePeek\(s_control_tx_queue[\s\S]*?!control_head.audio_barrier/);
console.log('PASS: task isolation, stack sizes, cancellation/drain ownership, ordered uplink barrier (source guards)');
