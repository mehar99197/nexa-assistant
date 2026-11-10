// Nexa's web remote: the page a phone opens to talk to Nexa on the PC.
//
// Requests go up a WebSocket as JSON; a spoken request goes up as one binary frame of 16 kHz mono
// 16-bit PCM. The whole conversation comes back down, whoever asked — the PC and every paired phone
// see the same thing — and so does the state of the Nexa core, which this page draws as SVG with
// the same shapes as the PC's widget (ui/NexaCorePainter.cc); style.css moves it. Text from the
// server is only ever set with textContent, never parsed as HTML.
'use strict';

const TOKEN_KEY = 'nexa.token';
const TARGET_RATE = 16000;
const MAX_SECONDS = 60;
const HOLD_MS = 300;               // a press longer than this is push-to-talk; shorter is a tap
const SILENCE_MS = 1200;           // a tap's recording ends this long after the last speech
const NO_SPEECH_MS = 6000;         // … or after this long without any
const SPEECH_RMS = 0.015;
const $ = (id) => document.getElementById(id);

const MODES = ['idle', 'wake', 'listen', 'stt', 'think', 'act', 'speak', 'fast', 'offline'];
const NODES = ['voice', 'pc', 'tools', 'phone'];

const state = {
  token: localStorage.getItem(TOKEN_KEY),
  socket: null,
  retry: 0,
  reply: null,          // the reply being written as it streams in
  replyMeta: null,      // its "NEXA" line
  busy: false,
  voice: false,
  recorder: null,
  wantTalk: false,      // the talk button is held right now
  tapMode: false,       // the recording was started with a tap: it ends at the pause
  pcMode: 'idle',       // the core's state on the PC
  pcNodes: [],
  localMode: null,      // what this phone itself is doing (recording, waiting, playing)
  speaking: null,       // the <audio> reading a reply
};

function show(view) {
  $('pair').hidden = view !== 'pair';
  $('chat').hidden = view !== 'chat';
}

function setStatus(text, kind = '') {
  const status = $('status');
  $('status-text').textContent = text;
  status.className = `pill ${kind}`;
}

function deviceName() {
  const ua = navigator.userAgent;
  const browser = /SamsungBrowser/.test(ua) ? 'Samsung Internet'
    : /Firefox/.test(ua) ? 'Firefox'
    : /Edg\//.test(ua) ? 'Edge'
    : /Chrome/.test(ua) ? 'Chrome' : 'Browser';
  const system = /Android/.test(ua) ? 'Android'
    : /iPhone|iPad/.test(ua) ? 'iOS'
    : /Windows/.test(ua) ? 'Windows'
    : /Mac OS/.test(ua) ? 'macOS'
    : /Linux/.test(ua) ? 'Linux' : '';
  return system ? `${browser} on ${system}` : browser;
}

// ---------------------------------------------------------------- the core

const SVG = 'http://www.w3.org/2000/svg';

function svgElement(name, attributes = {}, parent = null) {
  const element = document.createElementNS(SVG, name);
  for (const [key, value] of Object.entries(attributes)) element.setAttribute(key, value);
  if (parent) parent.append(element);
  return element;
}

// A node's icon, in a box 12 units wide: the same strokes as NexaCorePainter::paintNodeIcon.
function nodeIcon(group, node) {
  const icon = svgElement('g', { class: 'icon' }, group);
  switch (node) {
    case 'voice':
      svgElement('rect', { x: -2.04, y: -5.52, width: 4.08, height: 6.48, rx: 2.04 }, icon);
      svgElement('path', { d: 'M-4.08 -1.52 A4.08 4.08 0 0 0 4.08 -1.52 M0 2.64 V5.04 M-1.92 5.04 H1.92' }, icon);
      break;
    case 'pc':
      svgElement('rect', { x: -5.28, y: -4.56, width: 10.56, height: 6.72, rx: 0.96 }, icon);
      svgElement('path', { d: 'M0 2.16 V4.56 M-2.4 4.56 H2.4' }, icon);
      break;
    case 'tools':
      svgElement('path', { d: 'M-4.8 -3.6 L-1.2 0 L-4.8 3.6 M0.6 3.84 H5.04' }, icon);
      break;
    case 'phone':
      svgElement('rect', { x: -2.88, y: -5.28, width: 5.76, height: 10.56, rx: 1.2 }, icon);
      svgElement('path', { d: 'M-0.84 3.6 H0.84' }, icon);
      break;
    default:
      break;
  }
}

// Builds the core into `container`: the halo, the dotted ring, every state's motion (hidden until
// its state), the four nodes, the disc with the N, and the packets. Units: the reference radius is
// 100, the dotted ring sits at 80 and the core at 31, as in NexaCorePainter.
function buildCore(container, { nodes = true } = {}) {
  container.replaceChildren();
  const svg = svgElement('svg', { viewBox: nodes ? '-100 -100 200 200' : '-90 -90 180 180' }, container);
  svgElement('circle', { class: 'halo-outer', r: 60 }, svg);
  svgElement('circle', { class: 'halo-inner', r: 49 }, svg);
  svgElement('circle', { class: 'ring', r: 80 }, svg);

  const sonar = svgElement('circle', { class: 'sonar', r: 31 }, svg);
  sonar.setAttribute('vector-effect', 'non-scaling-stroke');

  const ticks = svgElement('g', { class: 'ticks' }, svg);
  for (const [ring, radius] of [['inner', 35.5], ['outer', 46]]) {
    const group = svgElement('g', { class: ring }, ticks);
    for (let i = 0; i < 48; i++) {
      svgElement('line', { x1: 0, y1: -radius, x2: 0, y2: -(radius + 16), transform: `rotate(${(360 * i) / 48})` }, group);
    }
  }

  const scan = svgElement('g', { class: 'scan' }, svg);
  svgElement('path', { class: 'tail', d: 'M-44.3 -7.8 A45 45 0 0 1 0 -45' }, scan);
  svgElement('path', { d: 'M0 -45 A45 45 0 0 1 44.3 -7.8' }, scan);

  const satellites = svgElement('g', { class: 'satellites' }, svg);
  [40, 47, 55].forEach((radius, i) => {
    const orbit = svgElement('g', { class: `orbit o${i + 1}` }, satellites);
    svgElement('circle', { class: 'path', r: radius }, orbit);
    svgElement('circle', { class: 'dot', cx: 0, cy: -radius, r: 2.6 }, orbit);
  });

  const comet = svgElement('g', { class: 'comet' }, svg);
  svgElement('path', { class: 'tail', d: 'M61.3 -51.4 A80 80 0 0 1 80 0' }, comet);
  svgElement('circle', { class: 'head-glow', cx: 80, cy: 0, r: 5 }, comet);
  svgElement('circle', { class: 'head', cx: 80, cy: 0, r: 2.2 }, comet);

  if (nodes) {
    const places = { voice: [0, -80], pc: [80, 0], tools: [0, 80], phone: [-80, 0] };
    for (const node of NODES) {
      const [x, y] = places[node];
      const group = svgElement('g', { class: `node node-${node}`, transform: `translate(${x} ${y})` }, svg);
      svgElement('circle', { class: 'glow', r: 17.8 }, group);
      svgElement('circle', { class: 'body', r: 11.5 }, group);
      nodeIcon(group, node);
    }
  }

  const disc = svgElement('g', { class: 'disc-group' }, svg);
  svgElement('circle', { class: 'disc', r: 31 }, disc);
  svgElement('circle', { class: 'glint', cx: -11.8, cy: -11.8, r: 6.2 }, disc);
  svgElement('path', {
    class: 'glyph',
    d: 'M-11.8 -13.6 h5 v27.2 h-5 z M6.8 -13.6 h5 v27.2 h-5 z M-11.8 -13.6 h6.5 L11.8 13.6 h-6.5 z',
  }, disc);

  const packets = svgElement('g', { class: 'packets' }, svg);
  for (const node of NODES) {
    const group = svgElement('g', { class: `packet packet-${node}` }, packets);
    svgElement('circle', { r: 2.6 }, group);
  }
}

// Shows one state on a core, with the nodes an action lights.
function paintCore(container, mode, nodes) {
  for (const m of MODES) container.classList.toggle(`mode-${m}`, m === mode);
  const lit = new Set(nodes);
  if (mode === 'listen' || mode === 'speak' || mode === 'stt' || mode === 'wake') lit.add('voice');
  if ((mode === 'act' || mode === 'fast') && lit.size === 0) { lit.add('pc'); lit.add('tools'); }
  for (const node of NODES) container.classList.toggle(`lit-${node}`, lit.has(node));
}

const WORDS = {
  idle: ['Ready', 'Say a request, or type one.'],
  wake: ['Wake word on', 'The PC is listening for its name.'],
  listen: ['Listening', 'Let go to send.'],
  stt: ['Transcribing', 'Recognising what you said…'],
  think: ['Thinking', 'Nexa is writing a reply…'],
  act: ['Acting', 'Running tools on the PC…'],
  speak: ['Speaking', 'Reading the reply aloud.'],
  fast: ['Fast path', 'Done directly, without the model.'],
  offline: ['Offline', 'The PC cannot reach its AI provider.'],
};

// What this phone is doing wins over what the PC reports; without a PC, the core is offline.
function currentMode() {
  if (!state.socket || state.socket.readyState !== WebSocket.OPEN) return 'offline';
  if (state.localMode) return state.localMode;
  return state.pcMode;
}

function showCore() {
  const mode = currentMode();
  const nodes = mode === state.pcMode ? state.pcNodes : [];
  for (const id of ['core', 'talk-core', 'brand-core']) paintCore($(id), mode, nodes);
  const [title, hint] = WORDS[mode] || WORDS.idle;
  $('state').textContent = title;
  $('hint').textContent = mode === 'listen' && state.tapMode ? 'Tap again, or pause, to send.'
    : mode === 'offline' && !(state.socket && state.socket.readyState === WebSocket.OPEN) ? 'Not connected to the PC.'
    : hint;
  $('state').style.setProperty('--accent', `var(--nx-${mode === 'offline' ? 'error' : mode})`);
}

function setPcMode(mode, nodes = []) {
  state.pcMode = MODES.includes(mode) ? mode : 'idle';
  state.pcNodes = nodes.filter((n) => NODES.includes(n));
  showCore();
}

function setLocalMode(mode) {
  state.localMode = mode;
  showCore();
}

// ---------------------------------------------------------------- pairing

async function pair(code) {
  $('pair-error').textContent = '';
  try {
    const response = await fetch('/api/pair', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ code, name: $('pair-name').value.trim() || deviceName() }),
    });
    const body = await response.json().catch(() => ({}));
    if (!response.ok || !body.token) throw new Error(body.error || `Pairing failed (${response.status}).`);
    state.token = body.token;
    localStorage.setItem(TOKEN_KEY, body.token);
    $('pair-code').value = '';
    connect();
  } catch (error) {
    $('pair-error').textContent = error.message;
  }
}

function startPairing(reason = '') {
  show('pair');
  setStatus('not paired', 'warn');
  if (!$('pair-name').value) $('pair-name').value = deviceName();
  $('pair-error').textContent = reason;
  // The PC's QR code carries the code after #pair= — the fragment never reaches any server log.
  const match = location.hash.match(/pair=(\d{6})/);
  if (match) {
    history.replaceState(null, '', location.pathname);
    $('pair-code').value = match[1];
    pair(match[1]);
  }
}

// ---------------------------------------------------------------- connection

function connect() {
  if (!state.token) {
    startPairing();
    return;
  }
  show('chat');
  setStatus('connecting…');
  showCore();
  const scheme = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const socket = new WebSocket(`${scheme}//${location.host}/ws`);
  socket.binaryType = 'arraybuffer';
  state.socket = socket;
  socket.onopen = () => socket.send(JSON.stringify({ t: 'hello', token: state.token }));
  socket.onmessage = (event) => {
    if (typeof event.data !== 'string') return;
    try {
      handle(JSON.parse(event.data));
    } catch (error) {
      console.warn('Nexa: unreadable message', error);
    }
  };
  socket.onclose = () => {
    if (state.socket !== socket) return;
    state.socket = null;
    showCore();
    if (!state.token) return;           // unpaired: nothing to reconnect with
    const delay = Math.min(15000, 1000 * 2 ** state.retry++);
    setStatus(`offline — trying again in ${Math.round(delay / 1000)} s`, 'bad');
    setTimeout(connect, delay);
  };
}

function send(message) {
  if (!state.socket || state.socket.readyState !== WebSocket.OPEN) return false;
  state.socket.send(JSON.stringify(message));
  return true;
}

// Keeps the connection alive through Wi-Fi power saving and idle-closing routers.
setInterval(() => send({ t: 'ping' }), 25000);

// ---------------------------------------------------------------- the conversation

function addMessage(kind, text) {
  const item = document.createElement('li');
  item.className = kind;
  item.textContent = text;
  $('messages').append(item);
  $('chat').classList.add('has-messages');
  item.scrollIntoView({ block: 'end' });
  return item;
}

// "YOU  voice" on the right, "NEXA" on the left: who said what, and how it got here.
function addMeta(who, detail, right) {
  const item = document.createElement('li');
  item.className = right ? 'meta right' : 'meta';
  item.textContent = who;
  if (detail) {
    const span = document.createElement('span');
    span.className = 'detail';
    span.textContent = `  ${detail}`;
    item.append(span);
  }
  $('messages').append(item);
  return item;
}

// ---------------------------------------------------------------- hardcore mode and the questions tasks wait on

// While a hardcore task runs, the PC's keyboard and mouse are locked: this phone is one of the ways to end it.
function showLock(locked, text) {
  $('lock-bar').hidden = !locked;
  $('lock-text').textContent = text || "Your PC's keyboard and mouse are locked.";
  $('unlock').disabled = false;
}

$('unlock').addEventListener('click', () => {
  if (send({ t: 'unlock' })) $('unlock').disabled = true;
});

// One card per question, answered once; the PC says when it was settled (here, on the PC, or on another phone).
function addApproval(message) {
  const id = Number(message.id);
  if (!Number.isSafeInteger(id) || id < 1 || document.querySelector(`#approvals li[data-id="${id}"]`)) return;
  const card = document.createElement('li');
  card.className = 'approval';
  card.dataset.id = String(id);
  const title = document.createElement('p');
  title.className = 'approval-title';
  title.textContent = `${message.agent || 'A task'} needs you · ${message.tool || ''}`;
  const question = document.createElement('p');
  question.textContent = message.text || '';
  const row = document.createElement('div');
  row.className = 'approval-buttons';
  for (const [label, allow] of [['Approve', true], ['Deny', false]]) {
    const button = document.createElement('button');
    button.type = 'button';
    button.textContent = label;
    button.className = allow ? 'primary' : '';
    button.addEventListener('click', () => {
      if (!send({ t: 'answer', id, allow })) return;
      for (const other of row.querySelectorAll('button')) other.disabled = true;
    });
    row.append(button);
  }
  card.append(title, question, row);
  $('approvals').append(card);
}

function settleApproval(id) {
  const card = document.querySelector(`#approvals li[data-id="${Number(id)}"]`);
  if (card) card.remove();
}

function setBusy(busy) {
  state.busy = busy;
  $('send').setAttribute('aria-label', busy ? 'Stop' : 'Send');
  $('send').classList.toggle('stop', busy);
}

function handle(message) {
  switch (message.t) {
    case 'welcome':
      state.retry = 0;
      state.voice = Boolean(message.voice) && Boolean(navigator.mediaDevices && window.AudioWorkletNode);
      $('talk-row').hidden = !state.voice;
      setBusy(Boolean(message.busy));
      setStatus('Your PC · connected', 'ok');
      $('status').title = message.device;
      setPcMode(message.mode || (message.busy ? 'think' : 'idle'));
      // The PC sends the lock and what waits right after this, so whatever was shown before is out of date.
      showLock(false);
      $('approvals').replaceChildren();
      break;
    case 'lock':
      showLock(Boolean(message.locked), message.text);
      break;
    case 'approval':
      addApproval(message);
      break;
    case 'settled':
      settleApproval(message.id);
      break;
    case 'state':
      setPcMode(message.mode, Array.isArray(message.nodes) ? message.nodes : []);
      break;
    case 'accepted': {
      state.reply = null;
      const fromPc = message.source === 'pc';
      addMeta('You', fromPc ? 'PC' : message.source === 'phone-voice' ? 'voice' : '', true);
      addMessage(fromPc ? 'user from-pc' : 'user', message.text);
      state.replyMeta = addMeta('Nexa', '', false);
      break;
    }
    case 'delta':
      if (!state.reply) state.reply = addMessage('nexa', '');
      state.reply.textContent += message.text;
      state.reply.scrollIntoView({ block: 'end' });
      break;
    case 'step':
      addMessage(message.ok ? 'step' : 'step failed', message.text);
      break;
    case 'finished':
      state.reply = null;
      state.replyMeta = null;
      if (state.localMode === 'stt') setLocalMode(null);
      break;
    case 'failed':
      state.reply = null;
      state.replyMeta = null;
      if (state.localMode === 'stt') setLocalMode(null);
      addMessage('error', message.text);
      break;
    case 'transcript':
      addMessage('heard', `“${message.text}”`);
      if (state.localMode === 'stt') setLocalMode(null);
      break;
    case 'speech':
      playSpeech(message.url);
      break;
    case 'busy':
      setBusy(Boolean(message.busy));
      if (!message.mode && !message.busy && state.pcMode === 'think') setPcMode('idle');
      break;
    case 'error':
      if (message.code === 'unauthorized') {
        state.token = null;
        localStorage.removeItem(TOKEN_KEY);
        startPairing(message.text);
        return;
      }
      if (state.localMode === 'stt') setLocalMode(null);
      addMessage('error', message.text);
      break;
    default:
      break;
  }
}

function playSpeech(url) {
  const audio = new Audio(url);
  state.speaking = audio;
  const done = () => {
    if (state.speaking !== audio) return;
    state.speaking = null;
    if (state.localMode === 'speak') setLocalMode(null);
  };
  audio.onended = done;
  audio.onerror = done;
  audio.play().then(() => setLocalMode('speak')).catch(done);
}

// ---------------------------------------------------------------- asking

$('ask').addEventListener('submit', (event) => {
  event.preventDefault();
  if (state.busy) {
    send({ t: 'cancel' });
    return;
  }
  const text = $('text').value.trim();
  if (!text) return;
  if (send({ t: 'text', text })) {
    $('text').value = '';
  } else {
    addMessage('error', 'Not connected to Nexa.');
  }
});

$('text').addEventListener('keydown', (event) => {
  if (event.key === 'Enter' && !event.shiftKey) {
    event.preventDefault();
    $('ask').requestSubmit();
  }
});

$('pair-form').addEventListener('submit', (event) => {
  event.preventDefault();
  pair($('pair-code').value.trim());
});

// ---------------------------------------------------------------- talking

async function startTalking() {
  if (state.recorder || !state.voice) return;
  if (state.speaking) {
    // Talking over a reply cuts it, as on the PC.
    state.speaking.pause();
    state.speaking = null;
    if (state.localMode === 'speak') setLocalMode(null);
  }
  let stream;
  try {
    stream = await navigator.mediaDevices.getUserMedia({
      audio: { channelCount: 1, echoCancellation: true, noiseSuppression: true, autoGainControl: true },
    });
    const context = new AudioContext();
    await context.audioWorklet.addModule('worklet.js');
    const source = context.createMediaStreamSource(stream);
    const node = new AudioWorkletNode(context, 'pcm-capture');
    const recorder = { stream, context, chunks: [], length: 0, started: performance.now(), lastSpeech: 0, level: 0 };
    node.port.onmessage = (event) => {
      if (state.recorder !== recorder) return;
      if (recorder.length >= context.sampleRate * MAX_SECONDS) {
        stopTalking();
        return;
      }
      recorder.chunks.push(event.data);
      recorder.length += event.data.length;
      // The level drives the ticks; in tap mode, the pause after speech ends the recording.
      let sum = 0;
      for (const sample of event.data) sum += sample * sample;
      const rms = Math.sqrt(sum / Math.max(1, event.data.length));
      recorder.level = Math.max(rms * 6, recorder.level * 0.92);
      const now = performance.now();
      if (rms > SPEECH_RMS) recorder.lastSpeech = now;
      if (state.tapMode) {
        if (recorder.lastSpeech && now - recorder.lastSpeech > SILENCE_MS) stopTalking();
        else if (!recorder.lastSpeech && now - recorder.started > NO_SPEECH_MS) stopTalking(true);
      }
    };
    source.connect(node);
    // A worklet only runs while it is connected onward; a silent gain keeps the speaker quiet.
    const silence = context.createGain();
    silence.gain.value = 0;
    node.connect(silence).connect(context.destination);
    state.recorder = recorder;
    setLocalMode('listen');
    setStatus('listening…', 'rec');
    showLevel();
    // The button was let go while the microphone was still being opened.
    if (!state.wantTalk && !state.tapMode) stopTalking(true);
  } catch (error) {
    if (stream) stream.getTracks().forEach((track) => track.stop());
    state.tapMode = false;
    addMessage('error', `The microphone is not available: ${error.message}`);
  }
}

function showLevel() {
  const recorder = state.recorder;
  const level = recorder ? Math.min(1, recorder.level) : 0;
  for (const id of ['core', 'talk-core']) $(id).style.setProperty('--level', level.toFixed(3));
  if (recorder) requestAnimationFrame(showLevel);
}

function stopTalking(discard = false) {
  const recorder = state.recorder;
  if (!recorder) return;
  state.recorder = null;
  state.tapMode = false;
  recorder.stream.getTracks().forEach((track) => track.stop());
  const rate = recorder.context.sampleRate;
  recorder.context.close();
  setStatus(state.socket ? 'Your PC · connected' : 'offline', state.socket ? 'ok' : 'bad');
  for (const id of ['core', 'talk-core']) $(id).style.setProperty('--level', '0');
  if (discard) {
    setLocalMode(null);
    return;
  }

  const pcm = toInt16(downsample(merge(recorder.chunks, recorder.length), rate, TARGET_RATE));
  if (pcm.length < TARGET_RATE / 4) {        // a tap, not speech
    setLocalMode(null);
    return;
  }
  if (!state.socket || state.socket.readyState !== WebSocket.OPEN) {
    setLocalMode(null);
    addMessage('error', 'Not connected to Nexa.');
    return;
  }
  state.socket.send(pcm.buffer);
  setLocalMode('stt');
}

function merge(chunks, length) {
  const samples = new Float32Array(length);
  let offset = 0;
  for (const chunk of chunks) {
    samples.set(chunk.subarray(0, Math.min(chunk.length, length - offset)), offset);
    offset += chunk.length;
    if (offset >= length) break;
  }
  return samples;
}

// Averages each window of input samples into one output sample: a box filter, which keeps what lies
// above the new Nyquist frequency from folding back into the speech band.
function downsample(samples, from, to) {
  if (from === to) return samples;
  const ratio = from / to;
  const output = new Float32Array(Math.floor(samples.length / ratio));
  for (let i = 0; i < output.length; i++) {
    const start = Math.floor(i * ratio);
    const end = Math.min(samples.length, Math.floor((i + 1) * ratio));
    let sum = 0;
    for (let j = start; j < end; j++) sum += samples[j];
    output[i] = end > start ? sum / (end - start) : 0;
  }
  return output;
}

function toInt16(samples) {
  const pcm = new Int16Array(samples.length);
  for (let i = 0; i < samples.length; i++) {
    const clamped = Math.max(-1, Math.min(1, samples[i]));
    pcm[i] = clamped < 0 ? clamped * 0x8000 : clamped * 0x7fff;
  }
  return pcm;
}

// Tap: listen until the pause after the request (tap again to send early). Hold: push-to-talk.
const talk = $('talk');
let pressedAt = 0;
talk.addEventListener('pointerdown', (event) => {
  event.preventDefault();
  talk.setPointerCapture(event.pointerId);
  pressedAt = performance.now();
  if (state.recorder && state.tapMode) {
    stopTalking();          // the second tap sends
    pressedAt = 0;
    return;
  }
  state.wantTalk = true;
  state.tapMode = false;
  startTalking();
});
for (const type of ['pointerup', 'pointercancel']) {
  talk.addEventListener(type, () => {
    state.wantTalk = false;
    if (!pressedAt) return;
    const held = performance.now() - pressedAt >= HOLD_MS;
    pressedAt = 0;
    if (held) {
      stopTalking();
    } else {
      state.tapMode = true;   // keep listening until the pause
      showCore();
    }
  });
}
// A long press must not open Android's context menu over the button.
talk.addEventListener('contextmenu', (event) => event.preventDefault());

// ---------------------------------------------------------------- start

for (const [id, nodes] of [['core', true], ['pair-core', true], ['talk-core', false], ['brand-core', false]]) {
  buildCore($(id), { nodes });
}
paintCore($('pair-core'), 'idle', []);
showCore();
// Over https with a self-signed certificate the browser refuses service workers; that only costs
// "install to home screen", so failure is fine.
if ('serviceWorker' in navigator) navigator.serviceWorker.register('sw.js').catch(() => {});
connect();
