// Nexa's web remote: the page a phone opens to talk to Nexa on the PC.
//
// Requests go up a WebSocket as JSON; a spoken request goes up as one binary frame of 16 kHz mono
// 16-bit PCM. The whole conversation comes back down, whoever asked — the PC and every paired phone
// see the same thing. Text from the server is only ever set with textContent, never parsed as HTML.
'use strict';

const TOKEN_KEY = 'nexa.token';
const TARGET_RATE = 16000;
const MAX_SECONDS = 60;
const $ = (id) => document.getElementById(id);

const state = {
  token: localStorage.getItem(TOKEN_KEY),
  socket: null,
  retry: 0,
  reply: null,          // the reply being written as it streams in
  busy: false,
  voice: false,
  recorder: null,
  wantTalk: false,      // the talk button is held right now
};

function show(view) {
  $('pair').hidden = view !== 'pair';
  $('chat').hidden = view !== 'chat';
}

function setStatus(text, kind = '') {
  const status = $('status');
  status.textContent = text;
  status.className = `status ${kind}`;
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
    if (!state.token) return;           // unpaired: nothing to reconnect with
    const delay = Math.min(15000, 1000 * 2 ** state.retry++);
    setStatus(`offline — trying again in ${Math.round(delay / 1000)} s`, 'warn');
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
  item.scrollIntoView({ block: 'end' });
  return item;
}

function setBusy(busy) {
  state.busy = busy;
  $('send').textContent = busy ? '■' : '➤';
  $('send').setAttribute('aria-label', busy ? 'Stop' : 'Send');
  $('send').classList.toggle('stop', busy);
}

function handle(message) {
  switch (message.t) {
    case 'welcome':
      state.retry = 0;
      state.voice = Boolean(message.voice) && Boolean(navigator.mediaDevices && window.AudioWorkletNode);
      $('talk').hidden = !state.voice;
      setBusy(Boolean(message.busy));
      setStatus(`connected · ${message.device}`, 'ok');
      break;
    case 'accepted':
      state.reply = null;
      addMessage(message.source === 'pc' ? 'user from-pc' : 'user', message.text);
      break;
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
      break;
    case 'failed':
      state.reply = null;
      addMessage('error', message.text);
      break;
    case 'transcript':
      addMessage('heard', `“${message.text}”`);
      break;
    case 'speech':
      new Audio(message.url).play().catch(() => {});
      break;
    case 'busy':
      setBusy(Boolean(message.busy));
      break;
    case 'error':
      if (message.code === 'unauthorized') {
        state.token = null;
        localStorage.removeItem(TOKEN_KEY);
        startPairing(message.text);
        return;
      }
      addMessage('error', message.text);
      break;
    default:
      break;
  }
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

// ---------------------------------------------------------------- push to talk

async function startTalking() {
  if (state.recorder || !state.voice) return;
  let stream;
  try {
    stream = await navigator.mediaDevices.getUserMedia({
      audio: { channelCount: 1, echoCancellation: true, noiseSuppression: true, autoGainControl: true },
    });
    const context = new AudioContext();
    await context.audioWorklet.addModule('worklet.js');
    const source = context.createMediaStreamSource(stream);
    const node = new AudioWorkletNode(context, 'pcm-capture');
    const recorder = { stream, context, chunks: [], length: 0 };
    node.port.onmessage = (event) => {
      if (recorder.length >= context.sampleRate * MAX_SECONDS) {
        stopTalking();
        return;
      }
      recorder.chunks.push(event.data);
      recorder.length += event.data.length;
    };
    source.connect(node);
    // A worklet only runs while it is connected onward; a silent gain keeps the speaker quiet.
    const silence = context.createGain();
    silence.gain.value = 0;
    node.connect(silence).connect(context.destination);
    state.recorder = recorder;
    $('talk').classList.add('recording');
    setStatus('listening…', 'rec');
    // The button was let go while the microphone was still being opened.
    if (!state.wantTalk) stopTalking(true);
  } catch (error) {
    if (stream) stream.getTracks().forEach((track) => track.stop());
    addMessage('error', `The microphone is not available: ${error.message}`);
  }
}

function stopTalking(discard = false) {
  const recorder = state.recorder;
  if (!recorder) return;
  state.recorder = null;
  $('talk').classList.remove('recording');
  recorder.stream.getTracks().forEach((track) => track.stop());
  const rate = recorder.context.sampleRate;
  recorder.context.close();
  setStatus(state.socket ? 'connected' : 'offline', state.socket ? 'ok' : 'warn');
  if (discard) return;

  const pcm = toInt16(downsample(merge(recorder.chunks, recorder.length), rate, TARGET_RATE));
  if (pcm.length < TARGET_RATE / 4) return;     // a tap, not speech
  if (!state.socket || state.socket.readyState !== WebSocket.OPEN) {
    addMessage('error', 'Not connected to Nexa.');
    return;
  }
  state.socket.send(pcm.buffer);
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

const talk = $('talk');
talk.addEventListener('pointerdown', (event) => {
  event.preventDefault();
  talk.setPointerCapture(event.pointerId);
  state.wantTalk = true;
  startTalking();
});
for (const type of ['pointerup', 'pointercancel']) {
  talk.addEventListener(type, () => {
    state.wantTalk = false;
    stopTalking();
  });
}
// A long press must not open Android's context menu over the button.
talk.addEventListener('contextmenu', (event) => event.preventDefault());

// ---------------------------------------------------------------- start

// Over https with a self-signed certificate the browser refuses service workers; that only costs
// "install to home screen", so failure is fine.
if ('serviceWorker' in navigator) navigator.serviceWorker.register('sw.js').catch(() => {});
connect();
