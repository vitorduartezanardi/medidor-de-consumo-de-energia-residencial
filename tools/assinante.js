// Assina todas as topics de um medidor e imprime as mensagens (equivalente ao mosquitto_sub -v).
// Uso: node assinante.js [ID_DO_MEDIDOR] [segundos]
const { conectar } = require('./mqtt_minimo');
const ID = process.argv[2] || 'medidor-01', DURACAO = Number(process.argv[3] || 0);
conectar({ clientId: `assinante-${Math.random().toString(16).slice(2, 8)}` }, (t, m, { retain }) => {
  console.log(`${t}${retain ? ' (retida)' : ''}  ${m.length > 90 ? m.slice(0, 64) + '…  [' + m.length + ' bytes]' : m}`);
}, mqtt => {
  console.log(`[MQTT] assinando usf-tcc-energia/${ID}/#`);
  mqtt.assinar(`usf-tcc-energia/${ID}/#`);
  if (DURACAO) setTimeout(() => { mqtt.fechar(); setTimeout(() => process.exit(0), 300); }, DURACAO * 1000);
});
