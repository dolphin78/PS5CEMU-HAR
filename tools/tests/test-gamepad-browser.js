// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise input/video flow without a browser dependency.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const script = fs.readFileSync(path.join(__dirname, '../../port/stream/browser.h'), 'utf8').split('<script>')[1].split('</script>')[0];

function page(live = true) {
    let now = 0, nextTimer = 0;
    const timers = new Map();
    function advance(ms) {now += ms; for (const [id, timer] of [...timers]) if (timer.at <= now) {timers.delete(id); timer.callback();}}
    const listeners = {}, windowListeners = {}, sockets = [], animation = [], revoked = [];
    const splash = {hidden: false};
    const screen = {
        hidden: true,
        naturalWidth: 854, naturalHeight: 480,
        getBoundingClientRect: () => ({left: 0, top: 0, width: 1000, height: 500}),
        setPointerCapture() {},
    };
    const document = {
        hidden: false,
        querySelector: selector => selector === '#screen' ? screen : selector === '#splash' ? splash : undefined,
        addEventListener(name, listener) {listeners[name] = listener;},
    };
    class Socket {
        constructor(url) {this.url = url; this.readyState = 0; this.bufferedAmount = 0; this.sent = []; sockets.push(this);}
        open() {this.readyState = 1; this.onopen();}
        send(message) {assert.equal(this.readyState, 1); this.sent.push(message);}
        close() {this.readyState = 3; this.onclose();}
    }
    vm.runInNewContext(script, {
        document, window: {addEventListener(name, listener) {windowListeners[name] = listener;}},
        location: {protocol: 'http:', host: '192.168.1.25:8089'}, WebSocket: Socket,
        URL: {createObjectURL: () => 'blob:test', revokeObjectURL: url => revoked.push(url)},
        requestAnimationFrame: callback => animation.push(callback),
        setInterval() {},
        setTimeout(callback, delay) {const id = ++nextTimer; timers.set(id, {callback, at: now + delay}); return id;},
        clearTimeout(id) {timers.delete(id);},
    });
    const input = sockets.find(s => s.url.endsWith('/input')), video = sockets.find(s => s.url.endsWith('/video'));
    input.open(); video.open();
    const frame = () => {video.onmessage({data: {}}); screen.onload();};
    if (live) frame();
    const down = (x = 500, y = 250) => screen.onpointerdown({pointerId: 1, clientX: x, clientY: y, preventDefault() {}});
    return {advance, frame, splash, document, screen, sockets, input, video, animation, revoked, listeners, windowListeners, down};
}

(async () => {
    const idle = page(false);
    assert.equal(idle.splash.hidden, false, 'Splash while connected without frames');
    idle.down(); assert.equal(idle.input.sent.length, 1, 'No touch before video');
    idle.video.onmessage({data: {}});
    assert.equal(idle.splash.hidden, false, 'Wait for successful image decode');
    idle.screen.onerror();
    assert.equal(idle.splash.hidden, false, 'Invalid image keeps splash');
    idle.frame(); assert.equal(idle.splash.hidden, true);
    idle.advance(3000); idle.frame(); idle.advance(3000);
    assert.equal(idle.splash.hidden, true, 'New frames extend the stream timeout');
    idle.down(); idle.advance(1000);
    assert.equal(idle.splash.hidden, false, 'Splash when an open stream stalls');
    assert.equal(idle.screen.hidden, true);
    assert(idle.input.sent.at(-1).startsWith('0,'), 'Release held touch on stall');
    idle.frame(); assert.equal(idle.splash.hidden, true, 'Recover after stall');
    idle.video.close(); assert.equal(idle.splash.hidden, false, 'Splash on disconnect');
    const p = page();
    p.down();
    assert(p.input.sent.includes('1,0.5,0.5'), 'Letterboxed touch coordinates');
    p.screen.onpointerup();
    // Fast taps preserve both edges without waiting for a server acknowledgment.
    p.down(); p.screen.onpointerup();
    assert.deepEqual(p.input.sent.slice(-2), ['1,0.5,0.5', '0,0.5,0.5']);
    p.down();
    const before = p.input.sent.length;
    for (let x = 450; x < 550; x++) p.screen.onpointermove({pointerId: 1, clientX: x, clientY: 250});
    assert.equal(p.input.sent.length, before);
    assert.equal(p.animation.length, 1);
    p.animation.shift()();
    assert.equal(p.input.sent.length, before + 1, 'Only latest drag position sent per animation frame');
    // Video sends credit only after decode: no image queue can accumulate.
    assert.deepEqual(p.video.sent, ['ready', 'ready']);
    p.video.onmessage({data: {}});
    assert.deepEqual(p.video.sent, ['ready', 'ready']);
    p.screen.onload();
    assert.deepEqual(p.video.sent, ['ready', 'ready', 'ready']);
    assert.deepEqual(p.revoked, ['blob:test', 'blob:test']);
    p.document.hidden = true; p.listeners.visibilitychange();
    assert.equal(p.input.readyState, 3); assert.equal(p.video.readyState, 3);
    assert(p.input.sent.at(-1).startsWith('0,'), 'Release before background disconnect');
    p.document.hidden = false; p.listeners.visibilitychange();
    assert.equal(p.sockets.length, 4, 'Reconnect when returning to page');
    const q = page();
    q.input.close(); q.down();
    assert.equal(q.input.sent.length, 1, 'Do not queue taps while disconnected');
    // The native 3DS picture is 4:3; touches must follow its letterboxing
    // instead of retaining the Wii U picture's aspect ratio after a game switch.
    const n = page();
    n.screen.naturalWidth = 320; n.screen.naturalHeight = 240;
    n.down(250, 125);
    const [, nx, ny] = n.input.sent.at(-1).split(',').map(Number);
    assert(Math.abs(nx - .125) < 1e-6); assert(Math.abs(ny - .25) < 1e-6);
    n.screen.onpointerup();
    n.screen.getBoundingClientRect = () => ({left: 0, top: 0, width: 500, height: 1000});
    n.down(125, 406.25);
    assert.equal(n.input.sent.at(-1), '1,0.25,0.25', '3DS touches after viewport rotation');
    console.log('PASS: independent input/video, tap edges, coalesced drags, decode backpressure, visibility reconnect, Wii U/3DS touch mapping, rotation and splash lifecycle');
})().catch(error => {console.error(error); process.exitCode = 1;});
