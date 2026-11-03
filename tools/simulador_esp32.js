// Simulador do firmware: publica as mesmas topics e o mesmo JSON do ESP32 (teste do fluxo sem hardware).
// Uso: node simulador_esp32.js [ID_DO_MEDIDOR] [segundos_de_execucao]
const { conectar } = require('./mqtt_minimo');
const ID = process.argv[2] || 'medidor-01', DURACAO = Number(process.argv[3] || 0);
const ROOT = `usf-tcc-energia/${ID}`;
const cargas = [0, 40, 60, 100, 160, 300];   // W: nenhuma, ventilador, lâmpada, 2 lâmpadas, ...
let carga = 60, wh = 0, seg = 0, acc = 0, hist = [];
for (let k = 0; k < 600; k++) hist.push(Math.round(cargas[Math.floor(k / 40) % cargas.length] * (1 + (Math.random() - .5) * .08)));

conectar({ clientId: `sim-${ID}`, will: { topic: `${ROOT}/status`, payload: 'offline' } },
  (t, m) => { if (t.endsWith('/cmd') && m === 'reset') { wh = 0; seg = 0; console.log('[CMD] reset recebido'); } },
  mqtt => {
    console.log(`[MQTT] conectado ao broker, medidor "${ID}"`);
    mqtt.publicar(`${ROOT}/status`, 'online', true);
    mqtt.publicar(`${ROOT}/history`, JSON.stringify({ w: hist }), true);
    mqtt.assinar(`${ROOT}/cmd`);
    setInterval(() => {
      if (seg % 20 === 0) carga = cargas[Math.floor(Math.random() * cargas.length)];
      seg++;
      const v = 126.5 + (Math.random() - .5), p = carga ? carga * (1 + (Math.random() - .5) * .03) : 0;
      const pf = carga > 100 ? 0.99 : 0.86, i = p ? p / (v * pf) : 0, s = v * i;
      const q = Math.sqrt(Math.max(0, s * s - p * p));
      wh += p / 3600; acc += p;
      if (seg % 60 === 0) { hist.push(Math.round(acc / 60)); acc = 0; }
      const msg = { v: +v.toFixed(1), i: +i.toFixed(3), p: +p.toFixed(1), s: +s.toFixed(1), q: +q.toFixed(1), pf: p ? +pf.toFixed(2) : 0, kwh: +(wh / 1000).toFixed(5), seconds: seg };
      mqtt.publicar(`${ROOT}/data`, JSON.stringify(msg));
      console.log(`V=${msg.v} V  I=${msg.i} A  P=${msg.p} W  S=${msg.s} VA  Q=${msg.q} var  FP=${msg.pf}  E=${msg.kwh} kWh`);
      if (DURACAO && seg >= DURACAO) { mqtt.fechar(); setTimeout(() => process.exit(0), 300); }
    }, 1000);
  });
