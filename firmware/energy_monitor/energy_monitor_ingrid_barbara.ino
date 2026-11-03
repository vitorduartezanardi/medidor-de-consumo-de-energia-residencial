/*
 * Sistema de monitoramento de grandezas elétricas e consumo de energia residencial
 * Firmware do ESP32 (Arduino core 2.x/3.x) — publica as medições via MQTT.
 *
 * Sensores:
 *   - ZMPT101B  (tensão)   -> GPIO34 (ADC1_CH6)
 *   - SCT-013-005 (corrente, 5 A = 1 V) -> GPIO35 (ADC1_CH7), com bias de 1,65 V
 *     (divisor 2 x 15 kΩ entre 3,3 V e GND + capacitor de 10 µF, conforme o TCC)
 *
 * Placa: ESP32 DevKit V1 (módulo ESP-WROOM-32, USB-serial CP2102, 30 pinos).
 *   Arduino IDE: Ferramentas > Placa > "DOIT ESP32 DEVKIT V1"  (FQBN esp32:esp32:esp32doit-devkit-v1)
 *
 * Ligações (nomes serigrafados na placa):
 *   D34 ........ saída do ZMPT101B (OUT)               [entrada somente, ADC1]
 *   D35 ........ saída do SCT-013-005 após o bias       [entrada somente, ADC1]
 *   3V3 / GND .. alimentação do divisor de bias (2 x 15 kΩ) e referência comum
 *   D2 ......... LED azul da placa: indicador de estado (ver atualizarLed)
 *
 * Biblioteca necessária: "PubSubClient" (Nick O'Leary).
 */
#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>

// ---------------- Configuração ----------------
const char* WIFI_SSID   = "SUA_REDE";
const char* WIFI_PASS   = "SUA_SENHA";
const char* MQTT_HOST   = "broker.hivemq.com";      //  broker público (TCP 1883). Público: "broker.hivemq.com"
const uint16_t MQTT_PORT = 1883;
const char* TOPIC_ROOT  = "usf-tcc-energia";
const char* DEVICE_ID   = "medidor-01";   // TROQUE por um ID único e use o mesmo nos Ajustes do app (o broker é público)

const int PIN_V = 34;
const int PIN_I = 35;
const int PIN_LED = 2;                   // LED azul da placa DevKit V1
float CAL_V = 0.25f;      // V por contagem do ADC (calibrar com multímetro)
float CAL_I = 0.00403f;   // A por contagem (SCT-013-005: 5 A = 1 V -> 5*3,3/4095)
const float I_MIN = 0.05f;               // abaixo disso a corrente é considerada ruído
const int   N_MAX = 8000;                // capacidade do buffer (a janela de tempo é que limita a coleta)
const unsigned long WINDOW_US = 200000;  // janela de 200 ms = 12 ciclos de 60 Hz

// ---------------- Estado ----------------
WiFiClient net;
PubSubClient mqtt(net);
Preferences prefs;
uint16_t bufV[N_MAX], bufI[N_MAX];
char tData[96], tStatus[96], tHist[96], tCmd[96];

float vrms, irms, pAtiva, pAparente, pReativa, fp;
double energiaWh = 0;
unsigned long segundosMedidos = 0;

const int HIST_N = 1440;                 // 24 h, 1 ponto por minuto (W médios)
uint16_t hist[HIST_N];
int histCount = 0, histHead = 0;
double minAcc = 0; int minSecs = 0;
unsigned long lastTick = 0, lastHist = 0, lastSave = 0, lastTry = 0;

// ---------------- Indicador de estado (LED da placa) ----------------
// pisca rápido = sem Wi-Fi | pisca lento = sem broker MQTT | aceso fixo = medidor online
void atualizarLed() {
  static unsigned long t = 0; static bool aceso = false;
  unsigned long agora = millis();
  if (WiFi.status() != WL_CONNECTED) {
    if (agora - t > 150) { t = agora; aceso = !aceso; digitalWrite(PIN_LED, aceso); }
  } else if (!mqtt.connected()) {
    if (agora - t > 800) { t = agora; aceso = !aceso; digitalWrite(PIN_LED, aceso); }
  } else {
    digitalWrite(PIN_LED, HIGH);
  }
}

// ---------------- Aquisição e cálculo ----------------
void medir() {
  int n = 0;
  unsigned long t0 = micros();
  while (n < N_MAX && micros() - t0 < WINDOW_US) {   // amostra v e i quase simultaneamente
    bufV[n] = analogRead(PIN_V);
    bufI[n] = analogRead(PIN_I);
    n++;
  }
  if (n < 100) return;

  double mv = 0, mi = 0;                              // componente contínua (offset do bias)
  for (int k = 0; k < n; k++) { mv += bufV[k]; mi += bufI[k]; }
  mv /= n; mi /= n;

  double sv = 0, si = 0, sp = 0;
  for (int k = 0; k < n; k++) {
    double v = (bufV[k] - mv) * CAL_V;                // tensão instantânea (V)
    double i = (bufI[k] - mi) * CAL_I;                // corrente instantânea (A)
    sv += v * v; si += i * i; sp += v * i;
  }
  vrms = sqrt(sv / n);                                // valor eficaz da tensão
  irms = sqrt(si / n);                                // valor eficaz da corrente
  if (irms < I_MIN) { irms = pAtiva = pAparente = pReativa = fp = 0; return; }

  pAparente = vrms * irms;                            // S = Vef · Ief          (Eq. 4)
  pAtiva    = fabs(sp / n);                           // P = média de v(t)·i(t)
  if (pAtiva > pAparente) pAtiva = pAparente;
  fp        = pAtiva / pAparente;                     // FP = P / S             (Eq. 6)
  pReativa  = sqrt(pAparente * pAparente - pAtiva * pAtiva);   // Q = √(S²−P²) (Eq. 5)
}

// ---------------- MQTT ----------------
void publicarDados() {
  char out[200];
  snprintf(out, sizeof(out),
    "{\"v\":%.1f,\"i\":%.3f,\"p\":%.1f,\"s\":%.1f,\"q\":%.1f,\"pf\":%.2f,\"kwh\":%.5f,\"seconds\":%lu}",
    vrms, irms, pAtiva, pAparente, pReativa, fp, energiaWh / 1000.0, segundosMedidos);
  mqtt.publish(tData, out);                           // QoS 0, não retida (1 Hz)
}

void publicarHistorico() {                            // mensagem retida: quem abre o app depois recebe o histórico
  if (histCount == 0) return;
  String s; s.reserve(HIST_N * 5 + 16);
  s = "{\"w\":[";
  for (int k = 0; k < histCount; k++) {
    int idx = (histHead - histCount + k + HIST_N) % HIST_N;   // do mais antigo ao mais novo
    s += hist[idx];
    if (k < histCount - 1) s += ',';
  }
  s += "]}";
  mqtt.publish(tHist, (const uint8_t*)s.c_str(), s.length(), true);
}

void aoReceber(char* topic, byte* payload, unsigned int len) {
  String msg; for (unsigned int k = 0; k < len; k++) msg += (char)payload[k];
  if (msg == "reset") {                               // comando enviado pelo aplicativo
    energiaWh = 0; segundosMedidos = 0;
    prefs.putFloat("energiaWh", 0); prefs.putULong("secs", 0);
  }
}

void conectarMQTT() {
  if (mqtt.connected() || millis() - lastTry < 5000) return;
  lastTry = millis();
  String cid = String("esp32-") + DEVICE_ID;
  // Last Will: se o ESP32 cair, o broker publica "offline" na topic de status
  if (mqtt.connect(cid.c_str(), nullptr, nullptr, tStatus, 1, true, "offline")) {
    mqtt.publish(tStatus, "online", true);
    mqtt.subscribe(tCmd, 1);
    Serial.println("[MQTT] conectado ao broker");
  } else {
    Serial.printf("[MQTT] falha rc=%d, nova tentativa em 5 s\n", mqtt.state());
  }
}

// ---------------- Wi-Fi ----------------
// Lista as redes visíveis (só 2,4 GHz aparece) e tenta conectar; a cada 20 s sem sucesso repete e mostra o motivo.
void conectarWiFi() {
  WiFi.mode(WIFI_STA);
  while (true) {
    Serial.println("\n[WiFi] redes 2,4 GHz visíveis:");
    int n = WiFi.scanNetworks();
    bool achou = false;
    for (int k = 0; k < n; k++) {
      Serial.printf("  %-32s %d dBm\n", WiFi.SSID(k).c_str(), WiFi.RSSI(k));
      if (WiFi.SSID(k) == WIFI_SSID) achou = true;
    }
    if (!achou) Serial.printf("[WiFi] a rede \"%s\" NAO foi encontrada (nome errado ou rede só 5 GHz)\n", WIFI_SSID);

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("[WiFi] conectando");
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
      delay(250); digitalWrite(PIN_LED, !digitalRead(PIN_LED)); Serial.print('.');
    }
    if (WiFi.status() == WL_CONNECTED) break;
    Serial.printf("\n[WiFi] falhou (status %d: %s)\n", WiFi.status(),
      WiFi.status() == WL_NO_SSID_AVAIL ? "rede não encontrada" :
      WiFi.status() == WL_CONNECT_FAILED ? "senha incorreta" : "sem resposta");
    WiFi.disconnect(true); delay(500);
  }
  Serial.printf("\n[WiFi] IP %s\n", WiFi.localIP().toString().c_str());
}

// ---------------- Arduino ----------------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  prefs.begin("energia", false);
  energiaWh = prefs.getFloat("energiaWh", 0);
  segundosMedidos = prefs.getULong("secs", 0);

  snprintf(tData,   sizeof(tData),   "%s/%s/data",    TOPIC_ROOT, DEVICE_ID);
  snprintf(tStatus, sizeof(tStatus), "%s/%s/status",  TOPIC_ROOT, DEVICE_ID);
  snprintf(tHist,   sizeof(tHist),   "%s/%s/history", TOPIC_ROOT, DEVICE_ID);
  snprintf(tCmd,    sizeof(tCmd),    "%s/%s/cmd",     TOPIC_ROOT, DEVICE_ID);

  conectarWiFi();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(10240);                          // comporta o histórico de 24 h
  mqtt.setCallback(aoReceber);
  lastTick = millis();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
  conectarMQTT();
  mqtt.loop();
  atualizarLed();

  unsigned long now = millis();
  if (now - lastTick < 1000) return;
  double dt = (now - lastTick) / 1000.0;
  lastTick = now;

  medir();
  energiaWh += pAtiva * dt / 3600.0;                  // E = ∫ P dt
  segundosMedidos += (unsigned long)(dt + 0.5);
  minAcc += pAtiva; minSecs++;
  if (minSecs >= 60) {                                // fecha 1 minuto: guarda a potência média
    hist[histHead] = (uint16_t)constrain(minAcc / minSecs, 0, 65535);
    histHead = (histHead + 1) % HIST_N;
    if (histCount < HIST_N) histCount++;
    minAcc = 0; minSecs = 0;
  }

  if (mqtt.connected()) {
    publicarDados();
    if (now - lastHist > 300000UL || lastHist == 0) { lastHist = now; publicarHistorico(); }
  }
  Serial.printf("V=%.1f V  I=%.3f A  P=%.1f W  S=%.1f VA  Q=%.1f var  FP=%.2f  E=%.4f kWh\n",
                vrms, irms, pAtiva, pAparente, pReativa, fp, energiaWh / 1000.0);

  if (now - lastSave > 600000UL) {                    // grava a cada 10 min (poupa a flash)
    lastSave = now;
    prefs.putFloat("energiaWh", (float)energiaWh);
    prefs.putULong("secs", segundosMedidos);
  }
}
