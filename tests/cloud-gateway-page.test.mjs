// Host check: node tests/cloud-gateway-page.test.mjs
// Exercises the actual embedded page, without firmware/network mocks claiming
// to verify ESP-IDF WebSocket or NVS behavior.
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';

const source = await readFile(new URL('../components/cloud_gateway/cloud_gateway_web.c', import.meta.url), 'utf8');
const block = source.split('static const char s_cloud_html[] =')[1].split(';\n\nstatic esp_err_t')[0];
const html = [...block.matchAll(/"(?:[^"\\]|\\.)*"/g)].map(match => JSON.parse(match[0])).join('');
const script = html.match(/<script>([\s\S]*)<\/script>/)[1];
new vm.Script(script);

const elements = new Map();
for (const id of [...html.matchAll(/id='([^']+)'/g)].map(match => match[1]))
    elements.set(id, { value: '', disabled: false, textContent: '' });
const requests = [];
let snapshot = {
    state: 3, paired: false, connected: true,
    serverUrl: 'https://example.com', tenantId: '',
    deviceUid: 'p4c5-001122334455', deviceId: '', pairingId: 'pending-a',
    pairingCode: '012345',
    message: '已连接后台，请在设备页面查看授权码并在后台或小程序输入',
};
let failNext = false;
const context = vm.createContext({
    document: { getElementById(id) {
        if (!elements.has(id)) elements.set(id, { value: '', disabled: false, textContent: '' });
        return elements.get(id);
    } },
    setTimeout() {}, // Do not run the poll recursively.
    confirm: () => true,
    fetch: async (path, options) => {
        requests.push({ path, options });
        const failed = failNext;
        failNext = false;
        return {
            ok: !failed,
            json: async () => failed ? { message: '设备忙' } : options.method ? { message: '已提交' } : snapshot,
        };
    },
});
vm.runInContext(script, context);
await vm.runInContext('refresh()', context);
assert.equal(elements.get('uid').textContent, snapshot.deviceUid);
assert.equal(elements.get('serverUrl').value, 'https://example.com');
assert.equal(elements.get('pairingCode').textContent, snapshot.pairingCode);

elements.get('serverUrl').value = ' https://new.example.com/ ';
elements.get('tenantId').value = ' tenant-a ';
vm.runInContext('configureCloud()', context);
await new Promise(resolve => setImmediate(resolve));
let last = requests.at(-1);
assert.equal(last.path, '/api/cloud/configure');
assert.deepEqual(JSON.parse(last.options.body), { serverUrl: 'https://new.example.com', tenantId: 'tenant-a' });
assert.equal(last.options.headers['Content-Type'], 'application/json');
assert.equal(last.options.headers['X-Cloud-Control'], '1');

snapshot = { ...snapshot, state: 6, paired: true, deviceId: 'device-a', pairingId: '', pairingCode: '' };
await vm.runInContext('refresh()', context);
assert.equal(elements.get('serverUrl').disabled, true);
assert.equal(elements.get('tenantId').disabled, true);
assert.equal(elements.get('did').textContent, 'device-a');

vm.runInContext('forgetCloud()', context);
await new Promise(resolve => setImmediate(resolve));
assert.equal(requests.at(-1).path, '/api/cloud/forget');
assert.deepEqual(JSON.parse(requests.at(-1).options.body), {});

failNext = true;
await vm.runInContext("command('/api/cloud/configure', {})", context);
assert.equal(elements.get('cloudMessage').textContent, '设备忙');
assert.equal(elements.get('connectButton').disabled, false);
assert.equal(elements.get('forgetButton').disabled, false);

export const results = 'PASS: embedded script syntax, status rendering, pairing-code display, server configuration, paired controls, local forget, HTTP error recovery';
console.log(results);
