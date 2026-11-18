#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <DHT.h>

// ---------- Pinos ----------
#define DHT_INT_PIN  4
#define DHT_EXT_PIN  5
#define RELE_PIN     26

// ---------- Parâmetros ----------
#define LIMITE        40.0      // % — abaixo disso liga o exaustor
#define HISTERESE      3.0      // desliga em LIMITE + HISTERESE
#define TEMPO_MAX_MS  300000UL  // máx. 5 min ligado por ciclo
#define PAUSA_MS      120000UL  // pausa de 2 min após estourar o tempo

// ---------- WiFi ----------
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";

// ---------- Telegram ----------
const char* BOT_TOKEN = "9169398730:AAF751yos4LXJCAOQn_EEDFKNyI4KDSrAik";
const char* CHAT_ID   = "5283610588";
const char* TG_HOST   = "api.telegram.org";

// ---------- Objetos ----------
DHT dhtInt(DHT_INT_PIN, DHT22);
DHT dhtExt(DHT_EXT_PIN, DHT22);

// ---------- Variáveis de estado ----------
bool exaustorLigado  = false;
bool estadoAnterior  = false;
unsigned long inicioCiclo = 0;
unsigned long fimPausa    = 0;

// ---------- Flag de interrupção ----------
volatile bool flagLeitura = false;

// ---------- Timer ----------
hw_timer_t* timer = NULL;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;

// ISR do timer — apenas seta a flag, sem lógica pesada
void IRAM_ATTR onTimer() {
  portENTER_CRITICAL_ISR(&timerMux);
  flagLeitura = true;
  portEXIT_CRITICAL_ISR(&timerMux);
}

// ---------- Funções WiFi ----------
void conectaWiFi() {
  Serial.print("Conectando ao WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println("\nWiFi conectado! IP: " + WiFi.localIP().toString());
}

// ---------- Envio de mensagem ao Telegram ----------
void enviarTelegram(const String& mensagem) {
  WiFiClientSecure client;
  client.setInsecure(); // Wokwi não suporta validação de certificado

  if (!client.connect(TG_HOST, 443)) {
    Serial.println("[Telegram] Falha na conexão HTTPS");
    return;
  }

  // Monta a URL com o texto codificado (substitui espaços por %20)
  String texto = mensagem;
  texto.replace(" ", "%20");
  texto.replace("\n", "%0A");

  String url = "/bot" + String(BOT_TOKEN) +
               "/sendMessage?chat_id=" + String(CHAT_ID) +
               "&text=" + texto;

  client.println("GET " + url + " HTTP/1.1");
  client.println("Host: " + String(TG_HOST));
  client.println("Connection: close");
  client.println();

  // Aguarda resposta (até 3 s)
  unsigned long t = millis();
  while (client.connected() && millis() - t < 3000) {
    if (client.available()) {
      String linha = client.readStringUntil('\n');
      if (linha.startsWith("HTTP/1.1")) {
        Serial.println("[Telegram] Resposta: " + linha);
      }
    }
  }
  client.stop();
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);

  pinMode(RELE_PIN, OUTPUT);
  digitalWrite(RELE_PIN, LOW);

  dhtInt.begin();
  dhtExt.begin();

  conectaWiFi();

  // Configura timer de hardware (ESP32 Core 3.x):
  // Frequência de 1.000.000 Hz → 1 tick = 1 µs
  // Alarme a cada 2.000.000 µs = 2 s, com recarga automática (true) e ilimitado (0)
  timer = timerBegin(1000000);
  timerAttachInterrupt(timer, &onTimer);
  timerAlarm(timer, 150000, true, 0);


  Serial.println("Sistema iniciado. Timer configurado para leitura a cada 2 s.");
}

// ---------- Loop ----------
void loop() {
  // Mantém conexão WiFi
  if (WiFi.status() != WL_CONNECTED) conectaWiFi();

  // Só executa quando o timer disparar a interrupção
  bool deveExecutar = false;
  portENTER_CRITICAL(&timerMux);
  if (flagLeitura) {
    flagLeitura  = false;
    deveExecutar = true;
  }
  portEXIT_CRITICAL(&timerMux);

  if (!deveExecutar) return;

  // ---------- Leitura dos sensores ----------
  float umidInt = dhtInt.readHumidity();
  float tempInt = dhtInt.readTemperature();
  float umidExt = dhtExt.readHumidity();
  float tempExt = dhtExt.readTemperature();

  if (isnan(umidInt) || isnan(umidExt) || isnan(tempInt) || isnan(tempExt)) {
    Serial.println("Falha na leitura dos sensores!");
    return;
  }

  unsigned long agora = millis();
  bool emPausa = agora < fimPausa;

  // ---------- Lógica do exaustor ----------
  if (!exaustorLigado) {
    // Liga se: interno seco, externo mais úmido e fora da pausa
    if (!emPausa && umidInt < LIMITE && umidExt > umidInt) {
      exaustorLigado = true;
      inicioCiclo    = agora;
    }
  } else {
    bool atingiuMeta   = umidInt >= LIMITE + HISTERESE;
    bool foraNaoAjuda  = umidExt <= umidInt;
    bool estourouTempo = (agora - inicioCiclo) >= TEMPO_MAX_MS;

    if (atingiuMeta || foraNaoAjuda || estourouTempo) {
      exaustorLigado = false;
      if (estourouTempo) fimPausa = agora + PAUSA_MS;
    }
  }

  // Atualiza o relé (e o LED conectado a ele)
  digitalWrite(RELE_PIN, exaustorLigado ? HIGH : LOW);

  // ---------- Serial ----------
  Serial.printf(
    "Int: %.1f%% / %.1fC | Ext: %.1f%% / %.1fC | Exaustor: %s%s\n",
    umidInt, tempInt,
    umidExt, tempExt,
    exaustorLigado ? "LIGADO" : "DESLIGADO",
    emPausa        ? " (em pausa)" : ""
  );

  // ---------- Notificação Telegram ----------
  // Envia mensagem apenas quando o exaustor ACABOU de ser ligado
  if (exaustorLigado && !estadoAnterior) {
    String msg = "🚨 Exaustor ACIONADO!\n"
                 "Umidade interna: " + String(umidInt, 1) + "%\n" +
                 "Temperatura interna: " + String(tempInt, 1) + "°C\n" +
                 "Umidade externa: " + String(umidExt, 1) + "%\n" +
                 "Temperatura externa: " + String(tempExt, 1) + "°C\n" +
                 "Motivo: umidade interna abaixo de " + String(LIMITE, 0) + "%";
    enviarTelegram(msg);
  }

  // Envia mensagem quando o exaustor for desligado
  if (!exaustorLigado && estadoAnterior) {
    String msg = "✅ Exaustor DESLIGADO.\n"
                 "Umidade interna atual: " + String(umidInt, 1) + "%\n" +
                 "Temperatura interna: " + String(tempInt, 1) + "°C";
    enviarTelegram(msg);
  }

  estadoAnterior = exaustorLigado;
}