#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <TFT_eSPI.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <SPI.h>
#include <FS.h>
#include <SPIFFS.h>
#include <esp_system.h>
#include <XPT2046_Touchscreen.h>
#include <deque>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <lvgl.h>
#include "ui/ui.h"

// Zmienne UI zdefiniowane w ui_Screen1.c
extern lv_obj_t * uic_clock;
extern lv_obj_t * uic_date;
extern lv_obj_t * ui_TabPage2; // POTA
extern lv_obj_t * ui_TabPage3; // WWFF
extern lv_obj_t * ui_TabPage4; // APRS
extern lv_obj_t * ui_TabPage5; // PROPA
extern lv_obj_t * ui_TabPage6; // SONDY
extern lv_obj_t * ui_TabPage7; // KONF

// === PIN DEFINITIONS ===
// Display ILI9341
#define TFT_SCLK 14
#define TFT_MOSI 13
#define TFT_MISO 12
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST   4
#define TFT_BL   21

// Touch XPT2046
#define TOUCH_CS   33
#define TOUCH_IRQ  36
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CLK  25

// Backlight PWM
#define Backlight_PWM_CHANNEL 0
#define Backlight_PWM_FREQUENCY 5000
#define Backlight_PWM_RESOLUTION 8

// === HARDWARE ===
TFT_eSPI tft;
WebServer server(80);

// Uzywamy osobnej magistrali SPI dla dotyku, poniewaz piny sa inne niz dla TFT
SPIClass touchSpi(HSPI);
//XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);
XPT2046_Touchscreen ts(TOUCH_CS);
// === KONFIG ===
String ssid = "";
String password = "";
String pota_filter = "PL,UA,RO";
String aprs_pass = "";
String aprs_callsign = "NOCALL";
bool wifi_ok = false;
bool data_updated = true;
bool aprs_updated = false;
bool show_utc = true;
float my_lat = 51.3528;
float my_lon = 19.8847;
int pota_interval = 60;
int wwff_interval = 60;
int sonde_interval = 30;
int sonde_radius = 100;

enum Page { PAGE_MAIN, PAGE_POTA, PAGE_WWFF, PAGE_APRS, PAGE_PROP, PAGE_SONDE, PAGE_CONFIG };
int currentPage = PAGE_MAIN;
bool page_changed = true;

// === DANE ===
const String CURRENT_VERSION = "0.1"; // Aktualna wersja oprogramowania
String latest_version_tag = "";
bool new_version_available = false;
lv_obj_t * uic_version_label = NULL; // Etykieta dla powiadomienia o nowej wersji

struct SondeData { String callsign; float lat, lon; int alt; time_t last_seen_time = 0; };
struct POTASpot { String country; String callsign; String freq; String mode; time_t last_seen_time = 0; };
struct WWFFSpot { String callsign; String freq; String mode; String reference; time_t last_seen_time = 0; };
struct APRSSpot { String callsign; String comment; char symbol = '>'; char table = '/'; time_t last_seen_time = 0; float lat = 0; float lon = 0; float dist_km = -1; };
struct PropData { String sfi; String sunspots; String a_index; String k_index; String xray; String bz; String sw; 
                  String day8040, night8040, day3020, night3020, day1715, night1715, day1210, night1210; };

SondeData sonde;
POTASpot pota;
WWFFSpot wwff;
APRSSpot aprs;
PropData prop;
WiFiClient aprsClient;

std::deque<SondeData> sondeHistory;
std::deque<POTASpot> potaHistory;
std::deque<APRSSpot> aprsHistory;
std::deque<WWFFSpot> wwffHistory;
std::deque<PropData> propHistory;

// Obiekty tabel LVGL
lv_obj_t *table_pota = NULL;
lv_obj_t *table_wwff = NULL;
lv_obj_t *table_aprs = NULL;
lv_obj_t *table_prop = NULL;
lv_obj_t *table_sonde = NULL;

// Mutex do ochrony danych wspoldzielonych miedzy watkami
SemaphoreHandle_t dataMutex;

const char* dni_tygodnia[] = {"Niedziela", "Poniedzialek", "Wtorek", "Sroda", "Czwartek", "Piatek", "Sobota"};

// Forward declarations
/* LVGL Display flushing */
void my_disp_flush( lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p )
{
    uint32_t w = ( area->x2 - area->x1 + 1 );
    uint32_t h = ( area->y2 - area->y1 + 1 );

    tft.startWrite();
    tft.setAddrWindow( area->x1, area->y1, w, h );
    #if LV_COLOR_DEPTH == 16
        tft.pushColors( ( uint16_t * )&color_p->full, w * h, true );
    #else
        // Konwersja 32-bit (LVGL) -> 16-bit (TFT)
        static uint16_t temp_buf[320]; // Bufor na jedną linię (max 320px)
        uint32_t len = w * h;
        uint32_t px = 0;
        while (len > 0) {
            uint32_t chunk_size = (len > 320) ? 320 : len;
            for (uint32_t i = 0; i < chunk_size; i++) {
                lv_color_t c = color_p[px++];
                temp_buf[i] = tft.color565(c.ch.red, c.ch.green, c.ch.blue);
            }
            tft.pushColors(temp_buf, chunk_size, true);
            len -= chunk_size;
        }
    #endif

    tft.endWrite();

    lv_disp_flush_ready( disp );
}

/* LVGL Touchpad reading */
void my_touchpad_read( lv_indev_drv_t * indev_driver, lv_indev_data_t * data )
{
   //Serial.println("Touch read called");
    if(ts.touched())
    {
        Serial.println("Touch detected");
        TS_Point p = ts.getPoint();
        // Mapowanie dla CYD (Landscape) - dostosowane z Twojej funkcji handleTouch
        // Wartosci surowe: X ~3800->200, Y ~200->3800
        data->point.x = map(p.x, 200, 3800, 0, 320);
        data->point.y = map(p.y, 200, 3800, 0, 240);
        data->state = LV_INDEV_STATE_PR;
    }
    else
    {
        data->state = LV_INDEV_STATE_REL;
    }
}

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[ 320 * 10 ];

void drawPage();
void drawNavBar();
void drawPageList();
void renderIP();
void renderMyCall();
void renderDate(int y,int wday, int mday, int mon, int year);
void renderHeader();
void renderData();
void renderAPRS();
void splashScreen();
void connectWiFi();
void showConfigScreen();
void pobierzPOTA();
void pobierzWWFF();
void aprsLoop();
void pobierzPropagacje();
void pobierzSondeHub();
void dataTask(void *pvParameters);
void update_main_screen_data(lv_timer_t * timer);
void init_tables();
void update_lists_if_needed(lv_timer_t * timer);
void pobierzWersje();
void update_version_label();

// Callback do zmiany strefy czasowej po kliknięciu w zegar
void on_clock_click(lv_event_t * e) {
    show_utc = !show_utc;
    update_main_screen_data(NULL); // Odśwież natychmiast
}

void saveConfig() {
  DynamicJsonDocument doc(1024);
  doc["ssid"] = ssid;
  doc["pass"] = password;
  doc["filter"] = pota_filter;
  doc["aprs_pass"] = aprs_pass;
  doc["aprs_callsign"] = aprs_callsign;
  doc["lat"] = my_lat;
  doc["lon"] = my_lon;
  doc["pota_interval"] = pota_interval;
  doc["wwff_interval"] = wwff_interval;
  doc["sonde_interval"] = sonde_interval;
  doc["sonde_radius"] = sonde_radius;
  doc["show_utc"] = show_utc;
  
  fs::File file = SPIFFS.open("/config.json", "w");
  if (file) {
    serializeJson(doc, file);
    file.close();
  }
}

void loadConfig() {
  if (SPIFFS.exists("/config.json")) {
    fs::File file = SPIFFS.open("/config.json", "r");
    if (file) {
      DynamicJsonDocument doc(1024);
      DeserializationError error = deserializeJson(doc, file);
      if (!error) {
        if (doc.containsKey("ssid")) ssid = doc["ssid"].as<String>();
        if (doc.containsKey("pass")) password = doc["pass"].as<String>();
        if (doc.containsKey("filter")) pota_filter = doc["filter"].as<String>();
        if (doc.containsKey("aprs_pass")) aprs_pass = doc["aprs_pass"].as<String>();
        if (doc.containsKey("aprs_callsign")) aprs_callsign = doc["aprs_callsign"].as<String>();
        if (doc.containsKey("lat")) my_lat = doc["lat"].as<float>();
        if (doc.containsKey("lon")) my_lon = doc["lon"].as<float>();
        if (doc.containsKey("pota_interval")) pota_interval = doc["pota_interval"].as<int>();
        if (doc.containsKey("wwff_interval")) wwff_interval = doc["wwff_interval"].as<int>();
        if (doc.containsKey("sonde_interval")) sonde_interval = doc["sonde_interval"].as<int>();
        if (doc.containsKey("sonde_radius")) sonde_radius = doc["sonde_radius"].as<int>();
        if (doc.containsKey("show_utc")) show_utc = doc["show_utc"].as<bool>();
      }
      file.close();
    }
  } else {
    Serial.println("Config file missing, creating default.");
    saveConfig();
  }
}

// === WEBSERVER ===
void handleStatus() {
  DynamicJsonDocument doc(2048);
  time_t now; time(&now);
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  doc["wifi"] = WiFi.status() == WL_CONNECTED;
  doc["ip"] = WiFi.localIP().toString();
  doc["pota"]["country"] = pota.country;
  doc["pota"]["callsign"] = pota.callsign;
  doc["pota"]["freq"] = pota.freq;
  doc["pota"]["mode"] = pota.mode;
  doc["pota"]["time_ago"] = (int)difftime(now, pota.last_seen_time);
  doc["wwff"]["callsign"] = wwff.callsign;
  doc["wwff"]["freq"] = wwff.freq;
  doc["wwff"]["mode"] = wwff.mode;
  doc["wwff"]["reference"] = wwff.reference;
  doc["wwff"]["time_ago"] = (int)difftime(now, wwff.last_seen_time);
  doc["aprs"]["callsign"] = aprs.callsign;
  doc["aprs"]["comment"] = aprs.comment;
  doc["aprs"]["symbol"] = String(aprs.symbol);
  doc["aprs"]["table"] = String(aprs.table);
  doc["aprs"]["time_ago"] = (int)difftime(now, aprs.last_seen_time);
  if (aprs.dist_km >= 0) doc["aprs"]["dist"] = aprs.dist_km;
  doc["prop"]["sfi"] = prop.sfi;
  doc["prop"]["sn"] = prop.sunspots;
  doc["prop"]["a"] = prop.a_index;
  doc["prop"]["k"] = prop.k_index;
  doc["prop"]["xray"] = prop.xray;
  doc["prop"]["bz"] = prop.bz;
  doc["prop"]["sw"] = prop.sw;
  doc["prop"]["day8040"] = prop.day8040;
  doc["prop"]["night8040"] = prop.night8040;
  doc["prop"]["day3020"] = prop.day3020;
  doc["prop"]["night3020"] = prop.night3020;
  doc["prop"]["day1715"] = prop.day1715;
  doc["prop"]["night1715"] = prop.night1715;
  doc["prop"]["day1210"] = prop.day1210;
  doc["prop"]["night1210"] = prop.night1210;
  doc["sonde"]["callsign"] = sonde.callsign;
  doc["sonde"]["alt"] = sonde.alt;
  doc["sonde"]["time_ago"] = (int)difftime(now, sonde.last_seen_time);
  xSemaphoreGive(dataMutex);
  
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handlePotaHistory() {
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.to<JsonArray>();
  time_t now; time(&now);
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  for (const auto& item : potaHistory) {
    JsonObject obj = arr.createNestedObject();
    obj["country"] = item.country;
    obj["callsign"] = item.callsign;
    obj["freq"] = item.freq;
    obj["mode"] = item.mode;
    obj["time_ago"] = (int)difftime(now, item.last_seen_time);
  }
  xSemaphoreGive(dataMutex);
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleWwffHistory() {
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.to<JsonArray>();
  time_t now; time(&now);
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  for (const auto& item : wwffHistory) {
    JsonObject obj = arr.createNestedObject();
    obj["callsign"] = item.callsign;
    obj["freq"] = item.freq;
    obj["mode"] = item.mode;
    obj["reference"] = item.reference;
    obj["time_ago"] = (int)difftime(now, item.last_seen_time);
  }
  xSemaphoreGive(dataMutex);
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleAprsHistory() {
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.to<JsonArray>();
  time_t now; time(&now);
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  for (const auto& item : aprsHistory) {
    JsonObject obj = arr.createNestedObject();
    obj["callsign"] = item.callsign;
    obj["comment"] = item.comment;
    obj["symbol"] = String(item.symbol);
    obj["table"] = String(item.table);
    obj["time_ago"] = (int)difftime(now, item.last_seen_time);
    if (item.dist_km >= 0) obj["dist"] = item.dist_km;
  }
  xSemaphoreGive(dataMutex);
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handlePropHistory() {
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.to<JsonArray>();
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  for (const auto& item : propHistory) {
    JsonObject obj = arr.createNestedObject();
    obj["sfi"] = item.sfi;
    obj["sn"] = item.sunspots;
    obj["a"] = item.a_index;
    obj["k"] = item.k_index;
    obj["day8040"] = item.day8040;
    obj["night8040"] = item.night8040;
    obj["day3020"] = item.day3020;
    obj["night3020"] = item.night3020;
    obj["day1715"] = item.day1715;
    obj["night1715"] = item.night1715;
    obj["day1210"] = item.day1210;
    obj["night1210"] = item.night1210;
  }
  xSemaphoreGive(dataMutex);
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleSondeHistory() {
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.to<JsonArray>();
  time_t now; time(&now);
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  for (const auto& item : sondeHistory) {
    JsonObject obj = arr.createNestedObject();
    obj["callsign"] = item.callsign;
    obj["lat"] = item.lat;
    obj["lon"] = item.lon;
    obj["alt"] = item.alt;
    obj["time_ago"] = (int)difftime(now, item.last_seen_time);
  }
  xSemaphoreGive(dataMutex);
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

const char config_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>SP7TEAM HamClock Config</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: sans-serif; max-width: 450px; margin: 0 auto; padding: 20px; background: #222; color: #fff; word-wrap: break-word; }
    h2 { text-align: center; color: #0af; }
    label { display: block; margin-top: 10px; }
    input { width: 100%; padding: 8px; margin-top: 5px; box-sizing: border-box; border-radius: 4px; border: 1px solid #555; background: #333; color: #fff; }
    select { width: 100%; padding: 8px; margin-top: 5px; box-sizing: border-box; border-radius: 4px; border: 1px solid #555; background: #333; color: #fff; }
    input[type=submit] { background-color: #0af; color: #000; font-weight: bold; margin-top: 0; border: none; padding: 12px; cursor: pointer; width: 100%; }
    input[type=submit]:hover { background-color: #08d; }
    fieldset { border: 1px solid #444; border-radius: 5px; margin-bottom: 15px; padding: 10px; }
    legend { color: #0af; font-weight: bold; padding: 0 5px; }
    .btn-group { margin-top: 20px; display: flex; gap: 10px; }
    .btn-cancel { background-color: #555; color: #fff; text-decoration: none; padding: 12px; border-radius: 4px; text-align: center; flex: 1; font-weight: bold; display: block; box-sizing: border-box; }
    .btn-cancel:hover { background-color: #666; }
  </style>
</head>
<body>
  <h2>Konfiguracja</h2>
  <form action="/save" method="POST">
    <fieldset>
      <legend>WiFi</legend>
      <label>WiFi SSID:</label>
      <input type="text" name="ssid" id="ssid" placeholder="Nazwa sieci">
      <label>WiFi Haslo:</label>
      <input type="password" name="pass" id="pass" placeholder="Haslo">
    </fieldset>

    <fieldset>
      <legend>APRS</legend>
      <label>Znak APRS (np. SP7XYZ):</label>
      <input type="text" name="aprs_callsign" id="aprs_callsign" placeholder="NOCALL">
      <label>Passcode APRS:</label>
      <input type="text" name="aprs_pass" id="aprs_pass" placeholder="Kod z aprs.fi">
    </fieldset>

    <fieldset>
      <legend>POTA</legend>
      <label>Filtr POTA (kraje):</label>
      <input type="text" name="filter" id="filter" placeholder="PL,UA,RO">
      <label>Interwal POTA (sekundy):</label>
      <input type="number" name="pota_interval" id="pota_interval" placeholder="60">
    </fieldset>

    <fieldset>
      <legend>WWFF</legend>
      <label>Interwal WWFF (sekundy):</label>
      <input type="number" name="wwff_interval" id="wwff_interval" placeholder="60">
    </fieldset>

    <fieldset>
      <legend>SondeHub</legend>
      <label>Interwal SondeHub (sekundy):</label>
      <input type="number" name="sonde_interval" id="sonde_interval" placeholder="30">
      <label>Promien SondeHub (km):</label>
      <input type="number" name="sonde_radius" id="sonde_radius" placeholder="100">
    </fieldset>

    <fieldset>
      <legend>Lokalizacja i Czas</legend>
      <label>Twoja szerokosc (Lat):</label>
      <input type="text" name="lat" id="lat" placeholder="51.3528">
      <label>Twoja dlugosc (Lon):</label>
      <input type="text" name="lon" id="lon" placeholder="19.8847">
      <label>Czas:</label>
      <select name="show_utc" id="show_utc">
        <option value="1">UTC</option>
        <option value="0">Lokalny (PL)</option>
      </select>
    </fieldset>

    <div class="btn-group">
      <a href="/" class="btn-cancel">Anuluj</a>
      <div style="flex: 1;"><input type="submit" value="Zapisz i Restart"></div>
    </div>
  </form>
  <script>
    fetch('/read_config').then(r=>r.json()).then(d=>{
      for(let k in d) if(document.getElementById(k)) {
        if(k==='show_utc') document.getElementById(k).value = d[k] ? 1 : 0;
        else document.getElementById(k).value=d[k];
      }
    }).catch(e=>console.log(e));
  </script>
</body>
</html>
)rawliteral";

void handleRoot() {
  if (SPIFFS.exists("/index.html")) {
    fs::File file = SPIFFS.open("/index.html", "r");
    if (file) {
      server.streamFile(file, "text/html");
      file.close();
      return;
    }
  }
  // Fallback jesli nie wgrano SPIFFS
  server.send(200, "text/html", "<h1>SP7TEAM HamClock</h1><p>Index missing (SPIFFS not uploaded?). <a href='/config'>Go to Config</a></p>");
}

void handleConfig() {
  if (SPIFFS.exists("/config.html")) {
    fs::File file = SPIFFS.open("/config.html", "r");
    if (file) {
      server.streamFile(file, "text/html");
      file.close();
      return;
    }
  }
  server.send(200, "text/html", config_html);
}

void handleSave() {
  ssid = server.arg("ssid");
  password = server.arg("pass");
  pota_filter = server.arg("filter");
  if (server.hasArg("aprs_pass")) aprs_pass = server.arg("aprs_pass");
  if (server.hasArg("aprs_callsign")) aprs_callsign = server.arg("aprs_callsign");
  if (server.hasArg("lat")) my_lat = server.arg("lat").toFloat();
  if (server.hasArg("lon")) my_lon = server.arg("lon").toFloat();
  if (server.hasArg("pota_interval")) pota_interval = server.arg("pota_interval").toInt();
  if (server.hasArg("wwff_interval")) wwff_interval = server.arg("wwff_interval").toInt();
  if (server.hasArg("sonde_interval")) sonde_interval = server.arg("sonde_interval").toInt();
  if (server.hasArg("sonde_radius")) sonde_radius = server.arg("sonde_radius").toInt();
  if (server.hasArg("show_utc")) show_utc = server.arg("show_utc").toInt();
  
  saveConfig();
  
  server.send(200, "text/html", R"(
    <h1>Saved! Restarting...</h1>
    <script>setTimeout(()=>location='/',2000)</script>)");
  
  delay(2000);
  ESP.restart();
}

// Callback obsługujący zmianę zakładki w TabView
void on_tab_changed(lv_event_t * e) {
    lv_obj_t * tv = lv_event_get_target(e);
    uint16_t tab_id = lv_tabview_get_tab_act(tv);
    
    Serial.printf("Zmieniono zakladke na: %d\n", tab_id);

    // Aktualizacja zmiennej currentPage na podstawie aktywnej zakładki
    // Kolejność zakładek musi odpowiadać kolejności w SquareLine Studio
    if (tab_id == 0) {currentPage = PAGE_MAIN;
    }
    else if (tab_id == 1) currentPage = PAGE_POTA;
    else if (tab_id == 2) currentPage = PAGE_WWFF;
    else if (tab_id == 3) currentPage = PAGE_APRS;
    else if (tab_id == 4) currentPage = PAGE_PROP;
    else if (tab_id == 5) currentPage = PAGE_SONDE;
    else if (tab_id == 6) currentPage = PAGE_CONFIG;

    // Odśwież listę na nowej zakładce
    drawPageList();
}

void setup() {
  Serial.begin(115200);
  // delay(1000);              // give monitor time to open

  // Tworzymy Mutex
  dataMutex = xSemaphoreCreateMutex();

  // Inicjalizacja SPI dla dotyku
  touchSpi.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);

  // TFT Init - uruchamiamy ekran na samym początku, aby widzieć logi
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  
  tft.setTextSize(2);
  tft.setCursor(0, 0);
  tft.println("SP7TEAM HamClock");
  tft.setTextSize(1);
  tft.println("Booting...");

  // Inicjalizacja dotyku (XPT2046) na wlasnej magistrali SPI
  if (!ts.begin(touchSpi)) {
    Serial.println("Touch controller not found!");
  }
  ts.setRotation(1);

  // === LVGL INIT ===
  lv_init();
  lv_disp_draw_buf_init( &draw_buf, buf, NULL, 320 * 10 );

  /*Initialize the display*/
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init( &disp_drv );
  disp_drv.hor_res = 320;
  disp_drv.ver_res = 240;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register( &disp_drv );

  /*Initialize the (dummy) input device driver*/
  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init( &indev_drv );
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touchpad_read;
  lv_indev_drv_register( &indev_drv );
  
  ui_init(); // Odkomentuj to, gdy wyeksportujesz pliki ze SquareLine Studio

  // // Etykieta do powiadomień o nowej wersji
  // uic_version_label = lv_label_create(lv_scr_act());
  // lv_obj_set_style_text_font(uic_version_label, &lv_font_montserrat_14, 0);
  // lv_obj_set_style_text_color(uic_version_label, lv_color_hex(0xFFD700), 0); // Złoty kolor
  // lv_label_set_text(uic_version_label, "");
  // lv_obj_align(uic_version_label, LV_ALIGN_TOP_RIGHT, -5, 5);

  // Włączamy obsługę kliknięcia dla zegara
  if (uic_clock) {
      lv_obj_add_flag(uic_clock, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(uic_clock, on_clock_click, LV_EVENT_CLICKED, NULL);
  }

  // Utworzenie timera LVGL do aktualizacji zegara i daty co sekundę
  //lv_timer_create(update_main_screen_data, 1000, NULL);
  lv_timer_create(update_main_screen_data, 500, NULL);
  
  // Ręczne tworzenie tabel, bo nie ma ich w Studio
  init_tables();

  // Timer do okresowego odświeżania list, jeśli dane się zmieniły
  lv_timer_create(update_lists_if_needed, 500, NULL);

  // Rejestracja zdarzenia zmiany zakładki dla ui_TabView1
  lv_obj_add_event_cb(ui_TabView1, on_tab_changed, LV_EVENT_VALUE_CHANGED, NULL);

  Serial.println("\n--- Setup start ---");
  Serial.print("Reset reason: "); Serial.println(esp_reset_reason());
  Serial.print("CPU freq: ");
  Serial.print(ESP.getCpuFreqMHz());
  Serial.println(" MHz");
  tft.print("CPU: "); tft.print(ESP.getCpuFreqMHz()); tft.println(" MHz");

  // ensure TCP/IP stack is initialised even if we aren't connected
  WiFi.mode(WIFI_STA);

  // SPIFFS
  tft.print("SPIFFS: ");
  if(!SPIFFS.begin(true)){
    Serial.println("SPIFFS Error");
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.println("FAIL");
    return;
  }
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.println("OK");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  
  // Preferences
  tft.print("Config: ");
  loadConfig();
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.println("OK");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  Serial.println("prefs read complete");
  Serial.print("ssid=["); Serial.print(ssid); Serial.println("]");
  Serial.print("ssid.length()="); Serial.println(ssid.length());
  Serial.print("password.length()="); Serial.println(password.length());
  
  // WebServer (routes only; server started later depending on mode)
  // server->serveStatic("/", SPIFFS, "/"); // Usunieto, aby uniknac konfliktu i crasha
  server.on("/", HTTP_GET, handleRoot);
  server.on("/config", HTTP_GET, handleConfig);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/status.json", HTTP_GET, handleStatus);
  server.on("/api/pota", HTTP_GET, handlePotaHistory);
  server.on("/api/aprs", HTTP_GET, handleAprsHistory);
  server.on("/api/wwff", HTTP_GET, handleWwffHistory);
  server.on("/api/prop", HTTP_GET, handlePropHistory);
  server.on("/api/sonde", HTTP_GET, handleSondeHistory);
  
  server.on("/aprs-symbols.png", HTTP_GET, []() {
    fs::File file = SPIFFS.open("/aprs-symbols.png", "r");
    if (file) {
      server.streamFile(file, "image/png");
      file.close();
    } else {
      server.send(404, "text/plain", "File not found");
    }
  });
  
  // Endpoint do pobierania obecnej konfiguracji przez formularz HTML
  server.on("/read_config", HTTP_GET, []() {
    fs::File file = SPIFFS.open("/config.json", "r");
    if (file) {
      server.streamFile(file, "application/json");
      file.close();
    } else {
      server.send(200, "application/json", "{}");
    }
  });

  // splashScreen(); // Wyłączamy splash screen, bo mamy logi
  
  if (ssid.length() > 0) {
    connectWiFi();
  } else {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.println("No SSID configured.");
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
  }

  if (WiFi.status() == WL_CONNECTED) {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.println("WiFi Connected!");
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.print("IP: "); tft.println(WiFi.localIP());
    Serial.println("Starting HTTP server (STA)");
    server.begin();       // HTTP server only after WiFi in STA
    delay(2000); // Czas na przeczytanie IP
    tft.fillScreen(TFT_BLACK); // Czyścimy ekran pod główny interfejs

    renderMyCall();
    renderIP();
    // Uruchamiamy watek pobierania danych na rdzeniu 0 (Core 0)
    xTaskCreatePinnedToCore(dataTask, "DataTask", 10000, NULL, 1, NULL, 0);
  } else {
    Serial.println("No SSID found or connection failed, configuring AP mode");
    // start as access point for configuration
    WiFi.mode(WIFI_AP);
    String apName = "HAMCLOCK-" + String(ESP.getEfuseMac() & 0xFFFFFF, HEX);
    WiFi.softAP(apName.c_str());
    Serial.println("Starting HTTP server (AP)");
    server.begin();

    // Display configuration instructions on TFT.
    // This screen will persist until the device is reconfigured and restarted.
    showConfigScreen();

    // Halt here in config mode. Only process web server requests.
    // This prevents the main loop() from running and overwriting the screen with LVGL.
    while(true) {
        server.handleClient();
        delay(5); // Yield to prevent watchdog issues
    }
  }
}

void loop() {
  server.handleClient();
  
  lv_tick_inc(5);
  lv_timer_handler(); /* Obsluga GUI LVGL */
  delay(5);
}

// === BACKGROUND TASK ===
void dataTask(void *pvParameters) {
  while (true) {
    if (WiFi.status() == WL_CONNECTED && wifi_ok) {
      unsigned long now = millis();
      static unsigned long last_pota = 0;
      static unsigned long last_wwff = 0;
      static unsigned long last_sonde = 0;
      static unsigned long last_version_check = 0;
      static bool first_run = true;

      aprsLoop(); // APRS dziala w petli (non-blocking stream)

      if (now - last_pota > (unsigned long)pota_interval * 1000 || last_pota == 0) {
        pobierzPOTA();
        pobierzPropagacje();
        last_pota = millis(); // Update time after fetch
        data_updated = true;
      }

      if (now - last_wwff > (unsigned long)wwff_interval * 1000 || last_wwff == 0) {
        pobierzWWFF();
        last_wwff = millis();
        data_updated = true;
      }

      if (now - last_sonde > (unsigned long)sonde_interval * 1000 || last_sonde == 0) {
        pobierzSondeHub();
        last_sonde = millis();
        data_updated = true;
      }

      // Sprawdzanie nowej wersji co 4 godziny lub przy pierwszym uruchomieniu
      if (first_run || (now - last_version_check > (4UL * 3600 * 1000))) {
        pobierzWersje();
        last_version_check = now;
        first_run = false;
      }
    }
    vTaskDelay(10 / portTICK_PERIOD_MS); // Maly delay dla stabilnosci
  }
}

// === DATA ===
void pobierzPOTA() {
  Serial.println("Fetching POTA data...");
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setUserAgent("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/87.0.4280.101 Safari/537.36");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  String url = "https://api.pota.app/spot/activator?limit=1";
  Serial.println(url);
  if (http.begin(client, url)) {
    int httpCode = http.GET();
    if (httpCode == 200) {
      DynamicJsonDocument doc(4096);
      String payload = http.getString(); // Pobieramy dane (to trwa)
      deserializeJson(doc, payload);     // Parsujemy
      
      if (doc.is<JsonArray>() && doc.size() > 0) {
        JsonObject spot = doc[0];
        
        // Sekcja krytyczna - aktualizacja zmiennych globalnych
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        pota.callsign = spot["activator"].as<String>();
        pota.freq = spot["frequency"].as<String>();
        pota.mode = spot["mode"].as<String>();
        String locationDesc = spot["locationDesc"].as<String>();
        int dashIndex = locationDesc.indexOf('-');
        if (dashIndex > 0) {
          pota.country = locationDesc.substring(0, dashIndex);
        } else {
          pota.country = locationDesc;
        }
        time(&pota.last_seen_time);
        
        if (potaHistory.empty() || potaHistory.front().callsign != pota.callsign) {
          potaHistory.push_front(pota);
          if (potaHistory.size() > 10) potaHistory.pop_back();
        }
        xSemaphoreGive(dataMutex);
        Serial.println("POTA updated");
      } else {
        Serial.println("POTA: No active spots found");
      }
    } else {
      Serial.printf("POTA fetch failed: %d\n", httpCode);
    }
    http.end();
  } else {
    Serial.println("POTA connection failed");
  }
}

void pobierzWWFF() {
  Serial.println("Fetching WWFF data...");
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setUserAgent("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/87.0.4280.101 Safari/537.36");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  String url = "https://www.cqgma.org/api/spots/wwff/";
  Serial.println(url);
  if (http.begin(client, url)) {
    int httpCode = http.GET();
    if (httpCode == 200) {
      DynamicJsonDocument doc(8192); // Increased size for WWFF
      String payload = http.getString();
      deserializeJson(doc, payload);
      
      if (doc.containsKey("RCD") && doc["RCD"].size() > 0) {
        JsonObject spot = doc["RCD"][0];
        
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        wwff.callsign = spot["ACTIVATOR"].as<String>();
        wwff.freq = spot["QRG"].as<String>();
        wwff.mode = spot["MODE"].as<String>();
        wwff.reference = spot["REF"].as<String>();
        
        time(&wwff.last_seen_time);
        
        if (wwffHistory.empty() || wwffHistory.front().callsign != wwff.callsign || wwffHistory.front().reference != wwff.reference) {
          wwffHistory.push_front(wwff);
          if (wwffHistory.size() > 10) wwffHistory.pop_back();
        }
        xSemaphoreGive(dataMutex);
        Serial.println("WWFF updated");
      } else {
        Serial.println("WWFF: No active spots found");
      }
    } else {
      Serial.printf("WWFF fetch failed: %d\n", httpCode);
    }
    http.end();
  } else {
    Serial.println("WWFF connection failed");
  }
}

float calculateDistance(float lat1, float lon1, float lat2, float lon2) {
  float R = 6371.0; // km
  float dLat = (lat2 - lat1) * DEG_TO_RAD;
  float dLon = (lon2 - lon1) * DEG_TO_RAD;
  float a = sin(dLat / 2) * sin(dLat / 2) +
            cos(lat1 * DEG_TO_RAD) * cos(lat2 * DEG_TO_RAD) *
            sin(dLon / 2) * sin(dLon / 2);
  float c = 2 * atan2(sqrt(a), sqrt(1 - a));
  return R * c;
}

void aprsLoop() {
  if (WiFi.status() != WL_CONNECTED) return;
  
  // Wymagane dane logowania (znak nie może zawierać gwiazdki, passcode musi być obecny)
  if (aprs_callsign == "NOCALL" || aprs_callsign.indexOf('*') != -1 || aprs_pass.length() == 0) {
    static bool aprs_debug_printed = false;
    if (!aprs_debug_printed) {
        Serial.println("APRS: Warunki wstepne nie sa spelnione. Pomijam.");
        Serial.printf("APRS check: callsign='%s' pass_len=%d\n", aprs_callsign.c_str(), aprs_pass.length());
        aprs_debug_printed = true;
    }
    return;
  }

  if (!aprsClient.connected()) {
    Serial.println("Connecting to APRS-IS...");
    if (aprsClient.connect("euro.aprs2.net", 14580)) {
      Serial.println("Connected to APRS-IS");
      // Logowanie: user CALLSIGN pass PASSCODE vers HamClock 1.0 filter p/SP
      // Filtr p/SP oznacza prefiks SP (Polska).
      String login = "user " + aprs_callsign + " pass " + aprs_pass + " vers SP7HamClock 1.0 filter p/SP";
      aprsClient.println(login);
    }
  } else {
    while (aprsClient.available()) {
      String line = aprsClient.readStringUntil('\n');
      if (line.startsWith("#")) continue; // Ignoruj komentarze serwera
      
      // Proste parsowanie: CALL>PATH:DATA
      int gt = line.indexOf('>');
      int colon = line.indexOf(':');
      if (gt > 0 && colon > gt) {
        String call = line.substring(0, gt);
        String payload = line.substring(colon + 1);
        
        // Zmienne tymczasowe
        String t_call = call;
        String t_comment = payload.substring(0, 30);
        time_t t_now; time(&t_now);
        
        // Parsowanie symbolu APRS (uproszczone dla formatow ! i = oraz @ i /)
        // Format: !lat/lonS lub =lat/lonS (bez czasu) -> symbol na indeksie 19
        // Format: @czas/lat/lonS (z czasem) -> symbol na indeksie 26
        char sym = '>'; // Domyslnie samochod
        char tbl = '/';
        float lat = 0, lon = 0;
        bool has_loc = false;
        
        if (payload.length() > 20) {
          char startChar = payload.charAt(0);
          int latOffset = -1;
          
          if (startChar == '!' || startChar == '=') {
             latOffset = 1;
             tbl = payload.charAt(9);
             sym = payload.charAt(19);
          } else if ((startChar == '@' || startChar == '/') && payload.length() > 27) {
             latOffset = 8;
             tbl = payload.charAt(16);
             sym = payload.charAt(26);
          }
          
          // Parsowanie pozycji (format DDMM.hhN / DDDMM.hhW)
          if (latOffset > 0 && isdigit(payload.charAt(latOffset))) {
             String latStr = payload.substring(latOffset, latOffset+8);
             String lonStr = payload.substring(latOffset+9, latOffset+18);
             
             lat = latStr.substring(0, 2).toFloat() + latStr.substring(2, 7).toFloat() / 60.0;
             if (latStr.charAt(7) == 'S') lat = -lat;
             
             lon = lonStr.substring(0, 3).toFloat() + lonStr.substring(3, 8).toFloat() / 60.0;
             if (lonStr.charAt(8) == 'W') lon = -lon;
             has_loc = true;
          }
        }
        
        // Sekcja krytyczna
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        aprs.callsign = t_call;
        aprs.comment = t_comment;
        aprs.last_seen_time = t_now;
        aprs.symbol = sym;
        aprs.table = tbl;
        aprs.lat = lat;
        aprs.lon = lon;
        aprs.dist_km = has_loc ? calculateDistance(my_lat, my_lon, lat, lon) : -1;
        
        aprsHistory.push_front(aprs);
        if (aprsHistory.size() > 10) aprsHistory.pop_back();
        aprs_updated = true;
        xSemaphoreGive(dataMutex);
      }
    }
  }
}

String extractTag(String& xml, String tag) {
  int s = xml.indexOf("<" + tag + ">");
  int e = xml.indexOf("</" + tag + ">");
  if (s > 0 && e > s) return xml.substring(s + tag.length() + 2, e);
  return "";
}

String extractBand(String& xml, String band, String time) {
  String tag = "<band name=\"" + band + "\" time=\"" + time + "\">";
  int s = xml.indexOf(tag);
  if (s < 0) return "";
  s += tag.length();
  int e = xml.indexOf("</band>", s);
  if (e < 0) return "";
  return xml.substring(s, e);
}

void pobierzPropagacje() {
  Serial.println("Fetching propagation data...");
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  String url = "https://hamqsl.com/solarxml.php";
  Serial.println(url);
  if (http.begin(client, url)) {
    int httpCode = http.GET();
    if (httpCode == 200) {
      String payload = http.getString();
      
      // Parsowanie do zmiennych lokalnych (opcjonalne, ale tu robimy wprost do structa pod mutexem bo szybkie)
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      prop.sfi = extractTag(payload, "solarflux");
      prop.sunspots = extractTag(payload, "sunspots");
      prop.a_index = extractTag(payload, "aindex");
      prop.k_index = extractTag(payload, "kindex");
      prop.xray = extractTag(payload, "xray");
      prop.bz = extractTag(payload, "magneticfield");
      prop.sw = extractTag(payload, "solarwind");
      
      prop.day8040 = extractBand(payload, "80m-40m", "day");
      prop.night8040 = extractBand(payload, "80m-40m", "night");
      prop.day3020 = extractBand(payload, "30m-20m", "day");
      prop.night3020 = extractBand(payload, "30m-20m", "night");
      prop.day1715 = extractBand(payload, "17m-15m", "day");
      prop.night1715 = extractBand(payload, "17m-15m", "night");
      prop.day1210 = extractBand(payload, "12m-10m", "day");
      prop.night1210 = extractBand(payload, "12m-10m", "night");

      bool empty = prop.sfi.length() == 0;
      if (!empty) {
         if (propHistory.empty() || propHistory.front().sfi != prop.sfi || propHistory.front().k_index != prop.k_index) {
            propHistory.push_front(prop);
            if (propHistory.size() > 10) propHistory.pop_back();
         }
      }
      xSemaphoreGive(dataMutex);
      
      if (empty) {
        Serial.println("Propagation XML parse failed");
      }
    } else {
      Serial.printf("Propagation fetch failed: %d\n", httpCode);
    }
    http.end();
  } else {
    Serial.println("Propagation connection failed");
  }
}

void pobierzSondeHub() {
  Serial.println("Fetching SondeHub data...");
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  
  String url = "https://api.v2.sondehub.org/sondes?lat=" + String(my_lat, 6) + "&lon=" + String(my_lon, 6) + "&distance=" + String(sonde_radius * 1000) + "&limit=1";
  Serial.println(url);
  if (http.begin(client, url)) {
    int httpCode = http.GET();
    if (httpCode == 200) {
      DynamicJsonDocument doc(8192);
      String payload = http.getString();
      deserializeJson(doc, payload);
      Serial.printf("SondeHub: Sondes found: %d\n", doc.size());
      
      if (doc.is<JsonObject>() && doc.size() > 0) {
        JsonObject root = doc.as<JsonObject>();
        JsonObject s = root.begin()->value();
        
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        sonde.callsign = s["serial"].as<String>();
        sonde.lat = s["lat"].as<float>();
        sonde.lon = s["lon"].as<float>();
        sonde.alt = s["alt"].as<int>();
        time(&sonde.last_seen_time);
        
        if (sonde.callsign != "BRAK" && (sondeHistory.empty() || sondeHistory.front().callsign != sonde.callsign || sondeHistory.front().lat != sonde.lat)) {
          sondeHistory.push_front(sonde);
          if (sondeHistory.size() > 10) sondeHistory.pop_back();
        }
        xSemaphoreGive(dataMutex);
        Serial.println("SondeHub updated");
      } else {
        Serial.println("SondeHub: No sondes found");
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        sonde.callsign = "BRAK";
        xSemaphoreGive(dataMutex);
      }
    } else {
      Serial.printf("SondeHub fetch failed: %d\n", httpCode);
    }
    http.end();
  } else {
    Serial.println("SondeHub connection failed");
  }
}

void update_version_label() {
    if (uic_new_version) { // Sprawdź, czy etykieta została utworzona
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        bool is_new = new_version_available;
        String tag = latest_version_tag;
        xSemaphoreGive(dataMutex);

        if (is_new) {
            char buf[32];
            snprintf(buf, sizeof(buf), "N. wer. %s", tag.c_str());
            lv_label_set_text(uic_new_version, buf);
        } else {
            lv_label_set_text(uic_new_version, "");
        }
    }
}

void pobierzWersje() {
  if (WiFi.status() != WL_CONNECTED) return;

  Serial.println("Fetching latest version info...");
  WiFiClientSecure client;
  client.setInsecure(); // GitHub API wymaga HTTPS
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("SP7HamClock-ESP32-Checker");
  
  String url = "https://api.github.com/repos/sq7br/sp7team-hamclock/releases/latest";
  
  if (http.begin(client, url)) {
    int httpCode = http.GET();
    if (httpCode == 200) {
      DynamicJsonDocument doc(2048);
      deserializeJson(doc, http.getString());
      
      if (doc.containsKey("tag_name")) {
        String tag = doc["tag_name"].as<String>();
        Serial.printf("Latest version tag from GitHub: %s\n", tag.c_str());
        
        String clean_tag = tag;
        if (clean_tag.startsWith("v")) {
            clean_tag.remove(0, 1);
        }

        if (clean_tag.length() > 0 && clean_tag != CURRENT_VERSION) {
            xSemaphoreTake(dataMutex, portMAX_DELAY);
            latest_version_tag = tag;
            new_version_available = true;
            xSemaphoreGive(dataMutex);
           
            Serial.printf("New version available: %s (current: %s)\n", tag.c_str(), CURRENT_VERSION.c_str());
        } else {
            new_version_available = false;
        }
      }
    } else {
      Serial.printf("Version check failed, HTTP code: %d\n", httpCode);
    }
    http.end();
  } else {
    Serial.println("Version check: connection failed.");
  }
}

void renderIP() {
  if (uic_IP)  lv_label_set_text(uic_IP, WiFi.localIP().toString().c_str());
} ;

void renderMyCall() {
      if (uic_znak)  lv_label_set_text(uic_znak, aprs_callsign.c_str());
};

void update_main_screen_data(lv_timer_t * timer) {
  time_t now;
  time(&now);
  struct tm timeinfo;
  
  if (show_utc) {
    gmtime_r(&now, &timeinfo);
    } else {
    // Ustawienie strefy czasowej dla Polski
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    localtime_r(&now, &timeinfo);
  }
    
  if (timeinfo.tm_year > (2016 - 1900)) { // Sprawdzamy czy czas jest ustawiony
    // --- Aktualizacja zegara ---
    char time_buf[20];
    strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &timeinfo);
    String time_str = String(time_buf);
    if (uic_clock) {
      lv_label_set_text(uic_clock, time_str.c_str());
    }
    String utc_str = show_utc ? " UTC" : "";
    if (uic_label_utc) {
      lv_label_set_text(uic_label_utc, utc_str.c_str());
    }

    // --- Aktualizacja daty ---
    char date_buf[50];
    char mday_str[3], mon_str[3];
    sprintf(mday_str, "%02d", timeinfo.tm_mday);
    sprintf(mon_str, "%02d", timeinfo.tm_mon + 1);
    sprintf(date_buf, "%s, %s-%s-%d", dni_tygodnia[timeinfo.tm_wday], mday_str, mon_str, timeinfo.tm_year + 1900);
    if (uic_date) {
      lv_label_set_text(uic_date, date_buf);
      if (timeinfo.tm_wday == 0) { // Niedziela
        lv_obj_set_style_text_color(uic_date, lv_color_hex(0xFF0000), 0);
      } else {
        lv_obj_set_style_text_color(uic_date, lv_color_hex(0xC0C0C0), 0);
      }
    }
  }
}


void renderAPRS() {
  // tft.fillRect(40, 160, 320, 40, TFT_BLACK); // Sekcja APRS 80-120
  // tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  // xSemaphoreTake(dataMutex, portMAX_DELAY);

  // if (aprs.callsign.length()) {
  //   // Rysowanie ikonki (symbolu) w ramce
  //   // Tabela '/' to glowna (zwykle niebieska/czerwona), '\' to alternatywna
  //   uint16_t bg = (aprs.table == '/') ? TFT_BLUE : TFT_RED;
  //   tft.fillRect(40, 160, 10, 10, bg);
  //   tft.drawRect(40, 160, 10, 10, TFT_WHITE);

  //   tft.setTextColor(TFT_WHITE, bg);
  //   tft.setTextSize(1);
  //   tft.setCursor(45, 160);
  //   tft.print(aprs.symbol);
    
  //   tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  //   tft.setTextSize(1); 
    
  //   String distStr = "";
  //   if (aprs.dist_km >= 0) distStr = String(aprs.dist_km, 1) + "km";

  //   tft.setCursor(75, 160);
  //   // APRS: ZNAK DYSTANS
  //   tft.printf("%s  %s", aprs.callsign.c_str(), distStr.c_str());
  // }
  // xSemaphoreGive(dataMutex);
}

void table_draw_part_event_cb(lv_event_t * e) {
    lv_obj_t * obj = lv_event_get_target(e);
    lv_obj_draw_part_dsc_t * dsc = lv_event_get_draw_part_dsc(e);

    if(dsc->part == LV_PART_ITEMS) {
        uint32_t col_cnt = lv_table_get_col_cnt(obj);
        if (col_cnt == 0) return; // Zabezpieczenie przed dzieleniem przez zero

        uint32_t row = dsc->id / col_cnt;
        uint32_t col = dsc->id % col_cnt;

        const char * val = lv_table_get_cell_value(obj, row, col);
        if (val == NULL) val = ""; // Zabezpieczenie przed NULL pointerem

        if(strcmp(val, "Good") == 0) {
            dsc->label_dsc->color = lv_color_hex(0x00FF00); // Jasny Zielony
        } else if(strcmp(val, "Fair") == 0) {
            dsc->label_dsc->color = lv_color_hex(0xFFFF00); // Jasny Żółty
        } else if(strcmp(val, "Poor") == 0) {
            dsc->label_dsc->color = lv_color_hex(0xFF0000); // Czerwony
        }
    }
}

void init_tables() {
    // Styl dla tabel (mniejsza czcionka, kompaktowe)
    static lv_style_t style_table;
    lv_style_init(&style_table);
    lv_style_set_text_font(&style_table, &lv_font_montserrat_14);
    lv_style_set_bg_color(&style_table, lv_color_hex(0x000000)); // Czarne tło
    lv_style_set_text_color(&style_table, lv_color_hex(0xFFFFFF)); // Biały tekst
    lv_style_set_border_width(&style_table, 0); // Bez ramek
    lv_style_set_pad_column(&style_table, 2); // Zmniejszenie odstępów między kolumnami

    // --- POTA Table ---
    if (ui_TabPage2) {
        table_pota = lv_table_create(ui_TabPage2);
        lv_obj_add_style(table_pota, &style_table, 0);
        lv_obj_add_style(table_pota, &style_table, LV_PART_ITEMS);
        lv_obj_set_width(table_pota, LV_PCT(100));
        lv_table_set_col_cnt(table_pota, 5);
        lv_table_set_col_width(table_pota, 0, 75); // Znak
        lv_table_set_col_width(table_pota, 1, 70); // Freq
        lv_table_set_col_width(table_pota, 2, 50); // Mode
        lv_table_set_col_width(table_pota, 3, 55); // Loc/Kraj
        lv_table_set_col_width(table_pota, 4, 40); // Czas
    }

    // --- WWFF Table ---
    if (ui_TabPage3) {
        table_wwff = lv_table_create(ui_TabPage3);
        lv_obj_add_style(table_wwff, &style_table, 0);
        lv_obj_add_style(table_wwff, &style_table, LV_PART_ITEMS);
        lv_obj_set_width(table_wwff, LV_PCT(100));
        lv_table_set_col_cnt(table_wwff, 5);
        lv_table_set_col_width(table_wwff, 0, 75); // Znak
        lv_table_set_col_width(table_wwff, 1, 80); // Ref
        lv_table_set_col_width(table_wwff, 2, 65); // Freq
        lv_table_set_col_width(table_wwff, 3, 50); // Mode
        lv_table_set_col_width(table_wwff, 4, 40); // Czas
  }

    // --- APRS Table ---
    if (ui_TabPage4) {
        table_aprs = lv_table_create(ui_TabPage4);
        lv_obj_add_style(table_aprs, &style_table, 0);
        lv_obj_add_style(table_aprs, &style_table, LV_PART_ITEMS);
        lv_obj_set_size(table_aprs, LV_PCT(100), LV_PCT(100));
        lv_obj_set_width(table_aprs, LV_PCT(100));
        lv_table_set_col_cnt(table_aprs, 5);
        lv_table_set_col_width(table_aprs, 0, 75); // Znak
        lv_table_set_col_width(table_aprs, 1, 20); // Sym
        lv_table_set_col_width(table_aprs, 3, 20); // Comment
        lv_table_set_col_width(table_aprs, 4, 50); // Czas
        lv_table_set_col_width(table_aprs, 3, 85); // Comment
        lv_table_set_col_width(table_aprs, 4, 40); // Czas
    }

    // --- PROPA Table ---
    if (ui_TabPage5) {
        table_prop = lv_table_create(ui_TabPage5);
        lv_obj_add_style(table_prop, &style_table, 0);
        lv_obj_add_style(table_prop, &style_table, LV_PART_ITEMS);
        lv_obj_set_size(table_prop, LV_PCT(100), LV_PCT(100));
        lv_obj_set_width(table_prop, LV_PCT(100));
        lv_table_set_col_cnt(table_prop, 3);
        lv_table_set_col_width(table_prop, 0, 80); // Pasmo
        lv_table_set_col_width(table_prop, 1, 80); // Dzien
        lv_obj_add_event_cb(table_prop, table_draw_part_event_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    }
    
    // --- SONDE Table ---
    if (ui_TabPage6) {
        table_sonde = lv_table_create(ui_TabPage6);
        lv_obj_add_style(table_sonde, &style_table, 0);
        lv_obj_add_style(table_sonde, &style_table, LV_PART_ITEMS);
        lv_obj_set_size(table_sonde, LV_PCT(100), LV_PCT(100));
        lv_obj_set_width(table_sonde, LV_PCT(100));
        lv_table_set_col_cnt(table_sonde, 5);
        lv_table_set_col_width(table_sonde, 0, 90); // Znak
        lv_table_set_col_width(table_sonde, 1, 70); // Alt
        lv_table_set_col_width(table_sonde, 2, 80); // Lat
        lv_table_set_col_width(table_sonde, 3, 80); // Lon
        lv_table_set_col_width(table_sonde, 4, 50); // Czas

   }
}

void renderHeader() {
  // Funkcja niepotrzebna w LVGL, etykiety statyczne są w UI
}


void renderData() {
  xSemaphoreTake(dataMutex, portMAX_DELAY);

  // POTA
  if (uic_label_pota_data) {
      if (pota.callsign.length()) {
          char buf[64];
          snprintf(buf, sizeof(buf), "%s %s", pota.callsign.c_str(), pota.freq.c_str());
          lv_label_set_text(uic_label_pota_data, buf);
      } else {
          lv_label_set_text(uic_label_pota_data, "-");
      }
  }

  // WWFF
  if (uic_label_wwff_data) {
      if (wwff.callsign.length()) {
          char buf[64];
          snprintf(buf, sizeof(buf), "%s %s", wwff.callsign.c_str(), wwff.reference.c_str());
          lv_label_set_text(uic_label_wwff_data, buf);
      } else {
          lv_label_set_text(uic_label_wwff_data, "-");
      }
  }

  // APRS
  if (uic_label_aprs_data) {
      if (aprs.callsign.length()) {
          char buf[64];
          String distStr = (aprs.dist_km >= 0) ? String(aprs.dist_km, 1) + "km" : "";
          snprintf(buf, sizeof(buf), "%c %s %s", aprs.symbol, aprs.callsign.c_str(), distStr.c_str());
          lv_label_set_text(uic_label_aprs_data, buf);
      } else {
          lv_label_set_text(uic_label_aprs_data, "-");
      }
  }

   // SONDA
  if (uic_label_sonde_data) {
      if (sonde.callsign == "BRAK") {
          lv_label_set_text(uic_label_sonde_data, "Brak");
      } else if (sonde.callsign.length()) {
          char buf[64];
          snprintf(buf, sizeof(buf), "%s %dm", sonde.callsign.c_str(), sonde.alt);
          lv_label_set_text(uic_label_sonde_data, buf);
      } else {
          lv_label_set_text(uic_label_sonde_data, "-");
      }
  }

  xSemaphoreGive(dataMutex);
}

uint16_t getCondColor(String c) {
  if (c == "Good") return TFT_GREEN;
  if (c == "Fair") return TFT_YELLOW;
  if (c == "Poor") return TFT_RED;
  return TFT_WHITE;
}

void drawPageList() {
    time_t now; 
    time(&now);

    auto formatTime = [&](time_t t) {
        int diff = (int)difftime(now, t);
        if (diff < 60) return String(diff) + "s";
        else if (diff < 3600) return String(diff / 60) + "m";
        else return String(diff / 3600) + "h";
    };

    if (currentPage == PAGE_POTA && table_pota) {
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      lv_table_set_row_cnt(table_pota, potaHistory.size());
      int row = 0;
      for (const auto& item : potaHistory) {
          lv_table_set_cell_value(table_pota, row, 0, item.callsign.c_str());
          lv_table_set_cell_value(table_pota, row, 1, item.freq.c_str());
          lv_table_set_cell_value(table_pota, row, 2, item.mode.c_str());
          lv_table_set_cell_value(table_pota, row, 3, item.country.c_str());
          lv_table_set_cell_value(table_pota, row, 4, formatTime(item.last_seen_time).c_str());
          row++;
      }
      xSemaphoreGive(dataMutex);
    } else if (currentPage == PAGE_WWFF && table_wwff) {
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      lv_table_set_row_cnt(table_wwff, wwffHistory.size());
      int row = 0;
      for (const auto& item : wwffHistory) {
          lv_table_set_cell_value(table_wwff, row, 0, item.callsign.c_str());
          lv_table_set_cell_value(table_wwff, row, 1, item.reference.c_str());
          lv_table_set_cell_value(table_wwff, row, 2, item.freq.c_str());
          lv_table_set_cell_value(table_wwff, row, 3, item.mode.c_str());
          lv_table_set_cell_value(table_wwff, row, 4, formatTime(item.last_seen_time).c_str());
          row++;
      }
      xSemaphoreGive(dataMutex);
    } else if (currentPage == PAGE_APRS && table_aprs) {
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      lv_table_set_row_cnt(table_aprs, aprsHistory.size());
      int row = 0;
      for (const auto& item : aprsHistory) {
        String distStr = "-";
        if (item.dist_km >= 0) distStr = String(item.dist_km, 1) + "km";
        char symStr[2] = {item.symbol, 0};

        lv_table_set_cell_value(table_aprs, row, 0, item.callsign.c_str());
        lv_table_set_cell_value(table_aprs, row, 1, symStr);
        lv_table_set_cell_value(table_aprs, row, 2, distStr.c_str());
        lv_table_set_cell_value(table_aprs, row, 3, item.comment.c_str());
        lv_table_set_cell_value(table_aprs, row, 4, formatTime(item.last_seen_time).c_str());
        row++;
      }
      xSemaphoreGive(dataMutex);
    } else if (currentPage == PAGE_PROP && table_prop) {
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      lv_table_set_row_cnt(table_prop, 4);
      
      auto setRow = [&](int r, const char* band, String d, String n) {
          lv_table_set_cell_value(table_prop, r, 0, band);
          lv_table_set_cell_value(table_prop, r, 1, d.c_str());
          lv_table_set_cell_value(table_prop, r, 2, n.c_str());
      };

      setRow(0, "80m-40m", prop.day8040, prop.night8040);
      setRow(1, "30m-20m", prop.day3020, prop.night3020);
      setRow(2, "17m-15m", prop.day1715, prop.night1715);
      setRow(3, "12m-10m", prop.day1210, prop.night1210);
      xSemaphoreGive(dataMutex);
    } else if (currentPage == PAGE_SONDE && table_sonde) {
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      lv_table_set_row_cnt(table_sonde, sondeHistory.size());
      int row = 0;
       for (const auto& item : sondeHistory) {
          char buf[32];
          lv_table_set_cell_value(table_sonde, row, 0, item.callsign.c_str());
          snprintf(buf, sizeof(buf), "%dm", item.alt);
          lv_table_set_cell_value(table_sonde, row, 1, buf);
          snprintf(buf, sizeof(buf), "%.2f", item.lat);
          lv_table_set_cell_value(table_sonde, row, 2, buf);
          snprintf(buf, sizeof(buf), "%.2f", item.lon);
          lv_table_set_cell_value(table_sonde, row, 3, buf);
          lv_table_set_cell_value(table_sonde, row, 4, formatTime(item.last_seen_time).c_str());
          row++;
      }
      xSemaphoreGive(dataMutex);
    }
}


void update_lists_if_needed(lv_timer_t * timer) {
    if (data_updated) {
        if (currentPage == PAGE_POTA || currentPage == PAGE_WWFF || currentPage == PAGE_SONDE || currentPage == PAGE_PROP) {
            drawPageList();
        }
        renderData(); // To aktualizuje etykiety na ekranie głównym
        data_updated = false;
    }
    if (aprs_updated) {
        if (currentPage == PAGE_APRS) {
            drawPageList();
        }
        renderData(); // To aktualizuje etykietę APRS na ekranie głównym
        aprs_updated = false;
    }

    // Sprawdzanie statusu nowej wersji i aktualizacja etykiety
    static bool last_known_version_status = false;
    if (new_version_available != last_known_version_status) {
        update_version_label();
        last_known_version_status = new_version_available;
    }
}

void splashScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(50, 100);
  tft.println("HAM CLOCK");
  tft.setTextSize(1);
  tft.setCursor(70, 140);
  tft.println("v2.0 SPIFFS");
  delay(2500);
}

void connectWiFi() {
  Serial.print("Connecting to WiFi SSID=`"); Serial.print(ssid); Serial.println("`...");
  tft.print("Connecting to WiFi SSID: "); tft.println(ssid);

  for (int attempt = 1; attempt <= 3; attempt++) {
    if (attempt > 1) {
      tft.println();
      tft.print("Retry "); tft.print(attempt); tft.println("/3 ");
      Serial.println();
      Serial.print("Retry "); Serial.print(attempt); Serial.println("/3 ");
      WiFi.disconnect();
      delay(1000);
    }
    WiFi.begin(ssid.c_str(), password.c_str());
    int i = 0;
    while (WiFi.status() != WL_CONNECTED && i < 20) {
      delay(500);
      Serial.print('.');
      tft.print(".");
      i++;
    }
    if (WiFi.status() == WL_CONNECTED) break;
  }
  tft.println();
  if (WiFi.status() == WL_CONNECTED) {
    wifi_ok = true;
    configTime(0, 3600, "pool.ntp.org");
    Serial.println("\nWiFi connected");
    Serial.print("IP: "); Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi connection failed");
  }
}

void showConfigScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 40);
  tft.println("BRAK KONF. WIFI");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(10, 80);
  tft.println("Polacz sie z siecia AP:");
  tft.setCursor(10, 100);
  tft.printf("SSID: HAMCLOCK-%06X", ESP.getEfuseMac()&0xFFFFFF);
  tft.setCursor(10, 115);
  tft.println("Haslo: (brak - siec otwarta)");
  tft.setCursor(10, 140);
  tft.println("http://192.168.4.1/config");
  tft.setCursor(10, 200);
}
