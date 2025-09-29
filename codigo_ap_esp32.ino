/*
  ESP32 WiFi Config Portal + Persistent Storage (Preferences)
  - Arranca en STA si encuentra credenciales válidas y las conecta.
  - Si no hay credenciales o no se conecta en timeout, arranca en AP con portal cautivo.
  - Interfaz web para ingresar SSID/Password.
  - Guarda credenciales en Preferences (NVS).
  - Endpoints: / (form), /save (POST), /status (GET), /reset (POST)
  - Autor: Tu nombre
  - Fecha: 2025-09-29
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

const char* AP_SSID = "ESP32_ConfigPortal";
const char* AP_PASS = "config2025"; // opcional para AP seguro

// DNS and Web
const byte DNS_PORT = 53;
DNSServer dnsServer;
WebServer server(80);

// Preferences (NVS)
Preferences preferences;
const char* PREF_NAMESPACE = "wifi_cfg";
const char* PREF_SSID = "ssid";
const char* PREF_PASS = "pass";

// Captive portal IP
IPAddress apIP(192,168,4,1);
IPAddress netMsk(255,255,255,0);

// Timeouts
const unsigned long WIFI_CONNECT_TIMEOUT = 15000; // ms para intentar STA
const unsigned long RECONNECT_INTERVAL = 10000; // ms en loop para reintentar conexión

unsigned long lastReconnectAttempt = 0;

bool startAPMode = false;

// HTML form (simple)
const char* CONFIG_FORM_HTML = R"rawliteral(
<!DOCTYPE html>
<html>
  <head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>Configuración WiFi - ESP32</title>
    <style>
      body { font-family: Arial; padding: 20px; }
      input { width: 100%; padding: 8px; margin: 6px 0; }
      button { padding: 10px 20px; }
      .card { max-width: 420px; margin:auto; box-shadow: 0 0 8px rgba(0,0,0,0.1); padding: 16px; border-radius: 8px; }
    </style>
  </head>
  <body>
    <div class="card">
      <h2>Configura tu red Wi-Fi</h2>
      <form action="/save" method="POST">
        <label>SSID</label>
        <input name="ssid" placeholder="Nombre de la red" required />
        <label>Contraseña</label>
        <input name="pass" placeholder="Contraseña" />
        <p><small>Si la red no tiene contraseña deja vacío.</small></p>
        <button type="submit">Guardar y conectar</button>
      </form>
      <hr/>
      <form action="/reset" method="POST">
        <button type="submit">Borrar configuración guardada (reset)</button>
      </form>
      <p style="font-size:0.8em;color:gray">IP del portal: <span id="ip"></span></p>
    </div>
    <script>document.getElementById('ip').textContent = location.hostname;</script>
  </body>
</html>
)rawliteral";

void handleNotFound() {
  // redirect everything to captive portal root when in AP mode
  if (WiFi.getMode() == WIFI_AP) {
    server.sendHeader("Location", String("http://") + apIP.toString(), true);
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "Not found");
  }
}

void handleRoot() {
  server.send(200, "text/html", CONFIG_FORM_HTML);
}

// Guarda credenciales enviadas por POST (form-data x-www-form-urlencoded)
void handleSave() {
  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "Use POST");
    return;
  }

  String ssid = server.arg("ssid");
  String pass = server.arg("pass");

  if (ssid.length() == 0) {
    server.send(400, "text/plain", "SSID vacío");
    return;
  }

  // Guardar en Preferences (NVS)
  preferences.begin(PREF_NAMESPACE, false);
  preferences.putString(PREF_SSID, ssid);
  preferences.putString(PREF_PASS, pass);
  preferences.end();

  String response = "Guardado. Intentando conectar a: " + ssid + "\nReiniciando conexión...";
  server.send(200, "text/plain", response);

  // Intentar conectar inmediatamente (non-blocking restart)
  delay(500);
  ESP.restart(); // reiniciar para simplificar el flujo de conexión
}

void handleStatus() {
  // Retorna JSON simple con estado y IP si está conectado
  String json = "{";
  json += "\"mode\":\"" + String((WiFi.getMode()==WIFI_AP)?"AP":"STA") + "\",";
  json += "\"ssid\":\"" + String(WiFi.SSID()) + "\",";
  json += "\"status\":\"" + String(WiFi.status()==WL_CONNECTED?"connected":"disconnected") + "\",";
  IPAddress ip = WiFi.localIP();
  json += "\"ip\":\"" + ip.toString() + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handleReset() {
  // Borrar prefs
  preferences.begin(PREF_NAMESPACE, false);
  preferences.remove(PREF_SSID);
  preferences.remove(PREF_PASS);
  preferences.end();

  server.send(200, "text/plain", "Configuración borrada. Reiniciando...");
  delay(500);
  ESP.restart();
}

void startCaptivePortal() {
  Serial.println("Iniciando Captive Portal (AP mode) ...");
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, netMsk);
  WiFi.softAP(AP_SSID, AP_PASS);

  // DNS: responde todas las consultas con la IP del AP para crear efecto captive portal
  dnsServer.start(DNS_PORT, "*", apIP);

  // Webserver routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/reset", HTTP_POST, handleReset);
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.print("Portal listo. SSID: "); Serial.println(AP_SSID);
  Serial.print("AP IP: "); Serial.println(apIP);
}

bool tryConnectFromPrefs() {
  preferences.begin(PREF_NAMESPACE, true); // read only
  String ssid = preferences.getString(PREF_SSID, "");
  String pass = preferences.getString(PREF_PASS, "");
  preferences.end();

  if (ssid.length() == 0) {
    Serial.println("No hay credenciales guardadas.");
    return false;
  }

  Serial.print("Intentando conectar a SSID guardado: ");
  Serial.println(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());

  unsigned long start = millis();
  while (millis() - start < WIFI_CONNECT_TIMEOUT) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("");
      Serial.println("Conectado a WiFi (desde prefs)");
      Serial.print("IP: "); Serial.println(WiFi.localIP());
      return true;
    }
    delay(250);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("Timeout conectando a SSID guardado.");
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(500);

  // Intentar conectar usando credenciales guardadas
  bool connected = tryConnectFromPrefs();
  if (!connected) {
    // Si no hay credenciales o falla, iniciar AP/captive portal
    startAPMode = true;
    startCaptivePortal();
  } else {
    // Si está conectado en STA, aun así exponemos endpoint de status y reset (opcional)
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/reset", HTTP_POST, handleReset);
    server.begin();
  }
}

void loop() {
  // Si estamos en AP con captive portal debemos procesar el servidor y DNS
  if (startAPMode) {
    dnsServer.processNextRequest();
    server.handleClient();
    return;
  }

  // En modo STA: chequeamos reconnect y atendemos web server (endpoints)
  server.handleClient();

  if (WiFi.status() != WL_CONNECTED) {
    unsigned long now = millis();
    if (now - lastReconnectAttempt > RECONNECT_INTERVAL) {
      lastReconnectAttempt = now;
      Serial.println("Se perdió la conexión. Reintentando conectar con credenciales guardadas...");
      bool ok = tryConnectFromPrefs();
      if (!ok) {
        // si no conecta, caemos a portal AP
        startAPMode = true;
        // parar servicios STA y iniciar AP
        WiFi.disconnect(true);
        delay(500);
        startCaptivePortal();
      }
    }
  } else {
    // conectado correctamente: se puede poner aquí la lógica normal del dispositivo
    // ejemplo: imprimir IP cada varios segundos
    static unsigned long lastPrint = 0;
    if (millis() - lastPrint > 10000) {
      lastPrint = millis();
      Serial.print("Conectado a: "); Serial.print(WiFi.SSID());
      Serial.print(" - IP: "); Serial.println(WiFi.localIP());
    }
  }
}