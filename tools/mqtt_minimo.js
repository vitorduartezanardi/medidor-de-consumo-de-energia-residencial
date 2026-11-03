// Cliente MQTT 3.1.1 mínimo (somente QoS 0, sem dependências) usado pelas ferramentas de teste.
// Mostra na prática como é o protocolo: pacotes CONNECT, PUBLISH e SUBSCRIBE sobre TCP.
const net = require('net');

const str = s => { const b = Buffer.from(s); return Buffer.concat([Buffer.from([b.length >> 8, b.length & 255]), b]); };
function pacote(tipoFlags, corpo) {
  const rl = []; let n = corpo.length;
  do { let d = n % 128; n = Math.floor(n / 128); if (n > 0) d |= 128; rl.push(d); } while (n > 0);
  return Buffer.concat([Buffer.from([tipoFlags, ...rl]), corpo]);
}

function conectar({ host = 'broker.hivemq.com', port = 1883, clientId, will }, aoMensagem, aoConectar) {
  const sock = net.connect(port, host);
  let buf = Buffer.alloc(0), pid = 1;
  const api = {
    publicar(topic, payload, retain = false) {
      sock.write(pacote(0x30 | (retain ? 1 : 0), Buffer.concat([str(topic), Buffer.from(payload)])));
    },
    assinar(filtro) {
      sock.write(pacote(0x82, Buffer.concat([Buffer.from([0, pid++]), str(filtro), Buffer.from([0])])));
    },
    fechar() { sock.write(Buffer.from([0xe0, 0])); sock.end(); },
  };
  sock.on('connect', () => {
    const flags = will ? (0x02 | 0x04 | 0x20) : 0x02;   // clean session [+ last will retida, QoS 0]
    const corpo = [str('MQTT'), Buffer.from([4])];
    corpo.push(Buffer.from([flags, 0, 60]), str(clientId));
    if (will) corpo.push(str(will.topic), str(will.payload));
    sock.write(pacote(0x10, Buffer.concat(corpo)));
  });
  sock.on('data', d => {
    buf = Buffer.concat([buf, d]);
    while (buf.length >= 2) {
      let i = 1, mult = 1, len = 0, b;
      do { if (i >= buf.length) return; b = buf[i++]; len += (b & 127) * mult; mult *= 128; } while (b & 128);
      if (buf.length < i + len) return;
      const cab = buf[0], tipo = cab >> 4, corpo = buf.subarray(i, i + len);
      buf = buf.subarray(i + len);
      if (tipo === 2) aoConectar && aoConectar(api);               // CONNACK
      if (tipo === 3) {                                              // PUBLISH
        const tl = corpo.readUInt16BE(0), topic = corpo.toString('utf8', 2, 2 + tl);
        aoMensagem && aoMensagem(topic, corpo.subarray(2 + tl).toString(), { retain: !!(cab & 1) });
      }
    }
  });
  sock.on('error', e => { console.error('[MQTT] erro:', e.message); process.exit(1); });
  setInterval(() => sock.write(Buffer.from([0xc0, 0])), 30000).unref();   // PINGREQ
  return api;
}
module.exports = { conectar };
