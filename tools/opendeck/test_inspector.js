// Exercise the real inspector script without OpenDeck or a physical deck.
const vm = require('node:vm');
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const elements = {};
for (const id of ['label', 'choice', 'status', 'playSound', 'feedback', 'soundVolume', 'volumeValue',
                  'sideBlock', 'side', 'stepBlock', 'step', 'stepValue'])
    elements[id] = {replaceChildren() {}, add() {}, setAttribute() {}};
const messages = [];
class Socket {
    static OPEN = 1;
    constructor() { this.readyState = 1; Socket.instance = this; }
    send(message) { messages.push(JSON.parse(message)); }
}
const sandbox = {document: {getElementById: id => elements[id]}, WebSocket: Socket,
    Option: function(name, id) { this.name = name; this.id = id; }, setInterval() {}};
vm.createContext(sandbox);
const html = fs.readFileSync(path.join(__dirname, 'dev.getrostrum.Rostrum.sdPlugin/inspector.html'), 'utf8');
vm.runInContext(html.split('<script>')[1].split('</script>')[0], sandbox);
function connect(settings = {}, action = 'mic') {
    sandbox.connectOpenActionSocket(123, 'pi', 'registerPropertyInspector', '{}',
        JSON.stringify({context:'key', action:'dev.getrostrum.Rostrum.' + action, payload:{settings}}));
}
connect();
assert.equal(elements.feedback.hidden, false);
assert.equal(elements.choice.hidden, true);
assert.equal(elements.playSound.checked, true);
assert.equal(Number(elements.soundVolume.value), 50);
assert.equal(elements.volumeValue.textContent, '50%');
elements.soundVolume.value = '75'; elements.soundVolume.oninput();
assert.equal(elements.volumeValue.textContent, '75%');
// Background inspector refreshes must not snap back a slider being adjusted.
sandbox.document.activeElement = elements.soundVolume;
Socket.instance.onmessage({data: JSON.stringify({event:'sendToPropertyInspector',
    payload:{settings:{soundVolume:50}, available:true, scenes:[], buses:[]}})});
assert.equal(Number(elements.soundVolume.value), 75);
elements.soundVolume.onchange();
sandbox.document.activeElement = null;
assert.equal(messages.at(-1).payload.soundVolume, 75);
elements.playSound.checked = false; elements.playSound.onchange();
assert.equal(messages.at(-1).payload.playSound, false);
assert.equal(messages.at(-1).payload.soundVolume, 75);
assert.equal(elements.soundVolume.disabled, true);
connect({playSound: false, soundVolume: 75});
assert.equal(Number(elements.soundVolume.value), 75);
assert.equal(elements.soundVolume.disabled, true);
elements.playSound.checked = true; elements.playSound.onchange();
assert.equal(elements.soundVolume.disabled, false);
assert.equal(Number(elements.soundVolume.value), 75);
for (const [value, expected] of [[null,50], ['loud',50], [true,50], [150,100], [-5,0]]) {
    connect({soundVolume:value});
    assert.equal(Number(elements.soundVolume.value), expected);
}
connect({scene:'Live'}, 'scene');
assert.equal(elements.feedback.hidden, true);
assert.equal(elements.choice.hidden, false);
// Side and Step for the bus and volume keys.
connect({}, 'volumeDown');
assert.equal(elements.sideBlock.hidden, false);
assert.equal(elements.stepBlock.hidden, false);
assert.equal(elements.side.value, 'both');
assert.equal(Number(elements.step.value), 5);
assert.equal(elements.stepValue.textContent, '5%');
for (const [value, expected] of [[null, 5], ['5', 5], [true, 5], [0, 1], [400, 25], [12.4, 12]]) {
    connect({step: value}, 'volumeUp');
    assert.equal(Number(elements.step.value), expected);
}
connect({side: 'left'}, 'volumeUp');
assert.equal(elements.side.value, 'both');
connect({}, 'bus');
assert.equal(elements.stepBlock.hidden, true);
assert.equal(elements.sideBlock.hidden, false);
assert.equal(elements.label.textContent, 'Bus');
const added = [];
elements.choice.add = option => added.push(option.id);
const levels = [{id: 'mic', name: 'Mic', destination: ''}, {id: 'game', name: 'Game', destination: 'both'}];
Socket.instance.onmessage({data: JSON.stringify({event: 'sendToPropertyInspector',
    payload: {settings: {}, available: true, scenes: [], buses: levels}})});
assert.deepEqual(added, ['mic', 'game']);
added.length = 0;
elements.side.value = 'stream'; elements.side.onchange();
assert.equal(messages.at(-1).payload.side, 'stream');
assert.deepEqual(added, ['game']); // one side: playback buses only
added.length = 0;
Socket.instance.onmessage({data: JSON.stringify({event: 'sendToPropertyInspector',
    payload: {settings: {side: 'stream'}, available: true, scenes: [],
              buses: [{id: 'mic', name: 'Mic'}, {id: 'game', name: 'Game'}]}})});
assert.deepEqual(added, ['mic', 'game']); // an older Rostrum lists no destinations: show them all
connect({}, 'volumeDown');
elements.step.value = '12'; elements.step.oninput();
assert.equal(elements.stepValue.textContent, '12%');
elements.step.onchange();
assert.equal(messages.at(-1).payload.step, 12);
// A background refresh must not move a step slider being dragged.
sandbox.document.activeElement = elements.step;
Socket.instance.onmessage({data: JSON.stringify({event: 'sendToPropertyInspector',
    payload: {settings: {step: 5}, available: true, scenes: [], buses: []}})});
assert.equal(Number(elements.step.value), 12);
sandbox.document.activeElement = null;
connect({}, 'mic');
assert.equal(elements.sideBlock.hidden, true);
assert.equal(elements.stepBlock.hidden, true);
console.log('PASS: sound volume defaults, round-trip, bounds, disabled state; scene selector preserved; side and step');
