# Medidor de Consumo de Energia (ESP32 + MQTT)

TCC de Engenharia Elétrica — Universidade São Francisco. O ESP32 mede tensão (ZMPT101B) e corrente (SCT-013-005), calcula V, I, P, Q, S, FP e energia, e publica via MQTT; a aplicação web (GitHub Pages) exibe potência, consumo e custo.

- `app/index.html` — aplicação web. Teste sem hardware: `?demo=1`. Medidor específico: `?id=<ID>`.
- `firmware/energy_monitor/` — firmware do ESP32 DevKit V1 (biblioteca PubSubClient; placa “DOIT ESP32 DEVKIT V1”; sensores em D34 e D35). Ajuste `WIFI_SSID`, `WIFI_PASS` e `DEVICE_ID`.
- `tools/` — `simulador_esp32.js` e `assinante.js` (Node.js, sem dependências) para testar o fluxo MQTT sem o hardware.
- `.github/workflows/pages.yml` — publica `app/` no GitHub Pages a cada push na `main`.
- `docs/Conteudo_Software_para_TCC.docx` — texto da parte de software, para inserir no TCC.

Tópicos MQTT: `usf-tcc-energia/<ID>/{data,status,history,config,cmd}` no broker `broker.hivemq.com` (TCP 1883 no ESP32, WSS 8884 no navegador).

Segurança: a corrente máxima do sensor é 5 A (≈ 635 W em 127 V). A rede elétrica é perigosa: use caixa fechada, disjuntor e sensores isolados.
