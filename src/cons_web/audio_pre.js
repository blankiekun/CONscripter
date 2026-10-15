// --pre-js of conscripter.js, so every host page gets it
// SDL2 plays through a ScriptProcessorNode, which Firefox runs late whenever the main
// thread is busy (the sound hiccups); pull SDL's callback on a timer instead and queue
// the chunks ahead as buffer sources, so a stall shorter than the lead goes unheard
(() => {
    var AC = window.AudioContext || window.webkitAudioContext;
    if (!AC) return;
    AC.prototype.createScriptProcessor = function(size, inputs, channels) {
        var ctx = this, next = 0, timer;
        var node = {onaudioprocess: null, connect: () => {}, disconnect: () => clearInterval(timer)};
        timer = setInterval(() => {
            if (ctx.state !== 'running' || !node.onaudioprocess) return;
            var lead = document.hidden ? 1.5 : 0.3; // hidden tabs run timers once a second
            if (next < ctx.currentTime) next = ctx.currentTime + 0.05;
            while (next < ctx.currentTime + lead) {
                var buf = ctx.createBuffer(channels, size, ctx.sampleRate);
                node.onaudioprocess({outputBuffer: buf});
                var src = ctx.createBufferSource();
                src.buffer = buf;
                src.connect(ctx.destination);
                src.start(next);
                next += size / ctx.sampleRate;
            }
        }, 20);
        return node;
    };
})();
