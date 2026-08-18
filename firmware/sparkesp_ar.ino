#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <WebServer.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <PubSubClient.h>
#include <time.h>
#include <ESP32Time.h>
#include <Preferences.h>
#include <math.h>  // expf için
#include "secrets.h" //wifi siresi icin

// ===================== EKRAN AYARLARI =====================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define SCREEN_ADDRESS 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ===================== PIN TANIMLARI =====================
const int ldrPin        = 1;
const int pirPin        = 2;
const int ledPin        = 4;
const int butonPin      = 5;
const int alarmButonPin = 10;
const int buzzerPin     = 15;

const int karanlikEsigi = 2500;

// ===================== ANA MOD SİSTEMİ =====================
// anamod=0 -> MANUEL (aktifMod 0-3 geçerli)
// anamod=1 -> YZ MODU (ANN karar verir)
int anaMod = 0; // 0=MANUEL, 1=YZ

// ===================== DONANIMSAL TIMER =====================
hw_timer_t * donanimTimer = NULL;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool saniyeTetiklendi = false;

void IRAM_ATTR onTimerKesmesi() {
  portENTER_CRITICAL_ISR(&timerMux);
  saniyeTetiklendi = true;
  portEXIT_CRITICAL_ISR(&timerMux);
}

// ===================== KALICI HAFIZA =====================
Preferences preferences;
ESP32Time rtc;

#define MAX_LOGS 15
int normalLogIndeks = 0;
bool eskiWifiDurumu = false;

// ===================== FONKSİYON PROTOTİPLERİ =====================
void modDegistirEkrani();
void bleDurumGonder(int ldr, int pir, int mod, bool ledAcik);
void wifiKurulum();
void bleKurulum();
void mqttBaglan();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void guvenlikModuIsle(int ldrDegeri, int hareketVarMi);
void zamanlayiciKontrol();
void saatEkrani();
bool ntpSaatiAl(int &saat, int &dakika, int &saniye);
void kaliciHafizadanYukle();
bool zamanlayiciAyarla(String mesaj);
void eepromLogYaz(const char* olayMetni, bool isKritik);
void offlineLoglariMqttGonder();

// ANN fonksiyonları
void annKurulum();
bool annKarar(float ldrNorm, float saatNorm, float pirVal);
void annOgren(float ldrNorm, float saatNorm, float pirVal, float hedef);
void annAgirliklarKaydet();
void annAgirliklarYukle();
String annDurumString();

// ===================== MOD DEĞİŞKENLERİ =====================
int   aktifMod     = 0;
bool butonBasildi = false;

// ===================== GÜVENLİK MODU =====================
bool alarmAktif         = false;
bool alarmButonBasildi  = false;
unsigned long alarmBaslangic = 0;

// ===================== ZAMANLAYICI =====================
int   zamBasSaat    = -1;
int   zamBasDakika  = -1;
int   zamBitSaat    = -1;
int   zamBitDakika  = -1;
bool zamanlayiciAktifMi = false;

// ===================== AKILLI MOD ZAMANLAYICI =====================
bool akilliModGeriSayimAktif = false;
int akilliModKalanSure = 0;

// ===================== EKRAN UYKU =====================
unsigned long sonHareketZamani = 0;
const unsigned long UYKU_SURESI = 10000;

// ===================== NTP =====================
const char* ntpSunucu  = "pool.ntp.org";
const long  gmtOffset  = 10800;
const int   dstOffset  = 0;
bool        ntpHazir   = false;

// ===================== WiFi =====================

WebServer server(80);

// ===================== MQTT =====================
const char* mqttBroker = "broker.hivemq.com";
const int   mqttPort     = 1883;
const char* mqttClientId = "SPARK-S3-ESP32";

const char* topicDurum       = "spark/durum";
const char* topicMod         = "spark/mod";
const char* topicAlarm       = "spark/alarm";
const char* topicAlarmKapat  = "spark/alarm/kapat";
const char* topicZaman       = "spark/zamanlayici";
const char* topicGecmisLogs  = "spark/logs/gecmis";
const char* topicAnaMod      = "spark/anamod"; // YZ/MANUEL geçiş

WiFiClient   espClient;
PubSubClient mqttClient(espClient);

// ===================== BLE UUID =====================
#define SERVICE_UUID    "12345678-1234-1234-1234-123456789abc"
#define CHAR_MOD_UUID   "12345678-1234-1234-1234-123456789abd"
#define CHAR_DURUM_UUID "12345678-1234-1234-1234-123456789abe"

BLEServer* pServer      = nullptr;
BLECharacteristic* pModChar     = nullptr;
BLECharacteristic* pDurumChar   = nullptr;
bool                bleConnected = false;

// =====================================================================
// YAPAY SİNİR AĞI (ANN) - 3 Giriş -> 4 Gizli -> 1 Çıkış
// Girişler: LDR (normalize 0-1), Saat (normalize 0-1), PIR (0 veya 1)
// Çıkış: LED Aç/Kapa kararı (0.5 eşiği)
// Öğrenme: Online Gradient Descent (her karar sonrası küçük güncelleme)
// =====================================================================

#define ANN_GIRIS   3
#define ANN_GIZLI   4
#define ANN_CIKIS   1
#define ANN_OGR_HIZ 0.05f

// Ağırlık matrisleri
float w1[ANN_GIRIS][ANN_GIZLI]; // Giriş -> Gizli
float b1[ANN_GIZLI];             // Gizli bias
float w2[ANN_GIZLI][ANN_CIKIS]; // Gizli -> Çıkış
float b2[ANN_CIKIS];             // Çıkış bias

// Forward pass ara değerleri (backprop için)
float gizliAktiv[ANN_GIZLI];
float cikisAktiv[ANN_CIKIS];

// ANN istatistikleri
int annToplamKarar    = 0;
int annDogruKarar     = 0;
float annSonCikis     = 0.0f;
bool annSonKarar      = false;
unsigned long sonAnnOgrenme = 0;

// ===================== SIGMOID FONKSİYONU =====================
float sigmoid(float x) {
  return 1.0f / (1.0f + expf(-x));
}

// ===================== ANN KURULUM (Başlangıç Ağırlıkları) =====================
void annKurulum() {
  // Xavier başlatma (küçük rastgele değerler)
  // Deterministik başlatma - gerçek projede random kullanılabilir
  float init[ANN_GIRIS][ANN_GIZLI] = {
    {0.42f, -0.31f,  0.55f, -0.18f},  // LDR girişi ağırlıkları
    {0.67f,  0.24f, -0.43f,  0.71f},  // Saat girişi ağırlıkları
    {0.38f,  0.82f,  0.29f, -0.56f}   // PIR girişi ağırlıkları
  };
  float initB1[ANN_GIZLI] = {0.1f, -0.1f, 0.05f, -0.05f};
  float initW2[ANN_GIZLI][ANN_CIKIS] = {{0.6f}, {0.5f}, {-0.4f}, {0.7f}};
  float initB2[ANN_CIKIS] = {-0.2f};

  for (int i = 0; i < ANN_GIRIS; i++)
    for (int j = 0; j < ANN_GIZLI; j++)
      w1[i][j] = init[i][j];
  for (int j = 0; j < ANN_GIZLI; j++) b1[j] = initB1[j];
  for (int j = 0; j < ANN_GIZLI; j++) w2[j][0] = initW2[j][0];
  b2[0] = initB2[0];
}

// ===================== ANN AĞIRLIK KAYDET/YÜKLE =====================
void annAgirliklarKaydet() {
  preferences.begin("ann-weights", false);
  for (int i = 0; i < ANN_GIRIS; i++)
    for (int j = 0; j < ANN_GIZLI; j++) {
      String key = "w1_" + String(i) + "_" + String(j);
      preferences.putFloat(key.c_str(), w1[i][j]);
    }
  for (int j = 0; j < ANN_GIZLI; j++) {
    preferences.putFloat(("b1_" + String(j)).c_str(), b1[j]);
    preferences.putFloat(("w2_" + String(j)).c_str(), w2[j][0]);
  }
  preferences.putFloat("b2_0", b2[0]);
  preferences.end();
}

void annAgirliklarYukle() {
  preferences.begin("ann-weights", true);
  bool kayitliMi = preferences.isKey("w1_0_0");
  if (kayitliMi) {
    for (int i = 0; i < ANN_GIRIS; i++)
      for (int j = 0; j < ANN_GIZLI; j++) {
        String key = "w1_" + String(i) + "_" + String(j);
        w1[i][j] = preferences.getFloat(key.c_str(), w1[i][j]);
      }
    for (int j = 0; j < ANN_GIZLI; j++) {
      b1[j] = preferences.getFloat(("b1_" + String(j)).c_str(), b1[j]);
      w2[j][0] = preferences.getFloat(("w2_" + String(j)).c_str(), w2[j][0]);
    }
    b2[0] = preferences.getFloat("b2_0", b2[0]);
    Serial.println("ANN ağırlıkları flash'tan yüklendi.");
  } else {
    Serial.println("ANN: İlk çalıştırma, varsayılan ağırlıklar kullanılıyor.");
  }
  preferences.end();
}

// ===================== ANN İLERİ HESAPLAMA (FORWARD PASS) =====================
bool annKarar(float ldrNorm, float saatNorm, float pirVal) {
  float giris[ANN_GIRIS] = {ldrNorm, saatNorm, pirVal};

  // Gizli katman hesapla
  for (int j = 0; j < ANN_GIZLI; j++) {
    float toplam = b1[j];
    for (int i = 0; i < ANN_GIRIS; i++)
      toplam += giris[i] * w1[i][j];
    gizliAktiv[j] = sigmoid(toplam);
  }

  // Çıkış katmanı hesapla
  float toplamCikis = b2[0];
  for (int j = 0; j < ANN_GIZLI; j++)
    toplamCikis += gizliAktiv[j] * w2[j][0];
  cikisAktiv[0] = sigmoid(toplamCikis);

  annSonCikis = cikisAktiv[0];
  annSonKarar = (cikisAktiv[0] >= 0.5f);
  annToplamKarar++;
  return annSonKarar;
}

// ===================== ANN GERİ YAYILIM (BACKPROPAGATION) =====================
// hedef: 0.0 (LED kapalı olmalı) veya 1.0 (LED açık olmalı)
void annOgren(float ldrNorm, float saatNorm, float pirVal, float hedef) {
  // Önce forward pass yap (ara değerleri doldur)
  annKarar(ldrNorm, saatNorm, pirVal);

  float giris[ANN_GIRIS] = {ldrNorm, saatNorm, pirVal};

  // Çıkış hatası (MSE türevi * sigmoid türevi)
  float cikisHata = (cikisAktiv[0] - hedef) * cikisAktiv[0] * (1.0f - cikisAktiv[0]);

  // Gizli katman hataları
  float gizliHata[ANN_GIZLI];
  for (int j = 0; j < ANN_GIZLI; j++) {
    gizliHata[j] = cikisHata * w2[j][0] * gizliAktiv[j] * (1.0f - gizliAktiv[j]);
  }

  // Ağırlıkları güncelle - Çıkış katmanı
  for (int j = 0; j < ANN_GIZLI; j++)
    w2[j][0] -= ANN_OGR_HIZ * cikisHata * gizliAktiv[j];
  b2[0] -= ANN_OGR_HIZ * cikisHata;

  // Ağırlıkları güncelle - Gizli katman
  for (int j = 0; j < ANN_GIZLI; j++) {
    for (int i = 0; i < ANN_GIRIS; i++)
      w1[i][j] -= ANN_OGR_HIZ * gizliHata[j] * giris[i];
    b1[j] -= ANN_OGR_HIZ * gizliHata[j];
  }

  // Doğruluk güncelle
  bool tahmin = (cikisAktiv[0] >= 0.5f);
  bool gercek  = (hedef >= 0.5f);
  if (tahmin == gercek) annDogruKarar++;

  sonAnnOgrenme = millis();
}

// ===================== ANN DURUM STRING =====================
String annDurumString() {
  int dogruluk = (annToplamKarar > 0) ? (annDogruKarar * 100 / annToplamKarar) : 0;
  String s = "{\"cikis\":" + String(annSonCikis, 3) +
             ",\"karar\":" + String(annSonKarar ? "true" : "false") +
             ",\"toplam\":" + String(annToplamKarar) +
             ",\"dogru\":" + String(annDogruKarar) +
             ",\"dogruluk\":" + String(dogruluk) + "}";
  return s;
}

// ===================== BLE SUNUCU CALLBACK =====================
class SunucuCallback : public BLEServerCallbacks {
  void onConnect(BLEServer* pSvr) override    { bleConnected = true; }
  void onDisconnect(BLEServer* pSvr) override {
    bleConnected = false;
    BLEDevice::startAdvertising();
  }
};

// ===================== BLE MOD YAZMA CALLBACK =====================
class ModYazmaCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) override {
    std::string val = pChar->getValue();
    if (val.length() > 0) {
      // "A0"-"A3" -> manuel mod seçimi, "Y" -> YZ modu
      if (val[0] == 'Y') {
        anaMod = 1;
        eepromLogYaz("BLE: YZ Moduna Geçildi", false);
      } else if (val[0] == 'M') {
        anaMod = 0;
        eepromLogYaz("BLE: Manuel Moda Geçildi", false);
      } else {
        int yeniMod = val[0] - '0';
        if (yeniMod >= 0 && yeniMod <= 3) {
          aktifMod = yeniMod;
          anaMod = 0; // Manuel moda geç
          preferences.begin("spark-data", false);
          preferences.putInt("aktifMod", aktifMod);
          preferences.end();
          sonHareketZamani = millis();
          modDegistirEkrani();
          eepromLogYaz("BLE Üzerinden Mod Değişti", false);
        }
      }
      preferences.begin("spark-data", false);
      preferences.putInt("anaMod", anaMod);
      preferences.end();
    }
  }
};

// ===================== KALICI HAFIZA YÜKLEME =====================
void kaliciHafizadanYukle() {
  preferences.begin("spark-data", false);
  aktifMod   = preferences.getInt("aktifMod", 0);
  anaMod     = preferences.getInt("anaMod", 0);
  zamBasSaat = preferences.getInt("bSaat", -1);
  zamBasDakika = preferences.getInt("bDakika", -1);
  zamBitSaat   = preferences.getInt("bitSaat", -1);
  zamBitDakika = preferences.getInt("bitDakika", -1);
  preferences.end();

  preferences.begin("spark-logs", false);
  normalLogIndeks = preferences.getInt("normIdx", 0);
  preferences.end();
  Serial.println("Kalıcı veriler yüklendi.");
}

// ===================== EEPROM LOG YAZMA =====================
void eepromLogYaz(const char* olayMetni, bool isKritik) {
  int hedefSira = -1;
  char zamanBuf[6];
  sprintf(zamanBuf, "%02d:%02d", rtc.getHour(true), rtc.getMinute());

  preferences.begin("spark-logs", false);

  if (isKritik) {
    for (int i = 0; i < MAX_LOGS; i++) {
      String keyKritik = "k_" + String(i);
      if (!preferences.getBool(keyKritik.c_str(), false)) {
        hedefSira = i;
        break;
      }
    }
    if (hedefSira == -1) hedefSira = normalLogIndeks;
  } else {
    int denemeSayisi = 0;
    hedefSira = normalLogIndeks;
    while (denemeSayisi < MAX_LOGS) {
      String keyKritik = "k_" + String(hedefSira);
      if (!preferences.getBool(keyKritik.c_str(), false)) break;
      hedefSira = (hedefSira + 1) % MAX_LOGS;
      denemeSayisi++;
    }
    normalLogIndeks = (hedefSira + 1) % MAX_LOGS;
    preferences.putInt("normIdx", normalLogIndeks);
  }

  String kZam   = "z_" + String(hedefSira);
  String kOlay  = "o_" + String(hedefSira);
  String kKritik= "k_" + String(hedefSira);

  preferences.putString(kZam.c_str(), zamanBuf);
  preferences.putString(kOlay.c_str(), olayMetni);
  preferences.putBool(kKritik.c_str(), isKritik);
  preferences.end();

  Serial.printf("EEPROM Log [Slot %d]: %s (%s)\n", hedefSira, olayMetni, zamanBuf);
}

// ===================== OFFLINE LOGLARI MQTT'YE GÖNDER =====================
void offlineLoglariMqttGonder() {
  if (!mqttClient.connected()) return;
  Serial.println("Çevrimdışı kayıtlar MQTT'ye aktarılıyor...");
  preferences.begin("spark-logs", true);
  for (int i = 0; i < MAX_LOGS; i++) {
    String kZam = "z_" + String(i);
    if (preferences.isKey(kZam.c_str())) {
      String z = preferences.getString(kZam.c_str(), "00:00");
      String o = preferences.getString(("o_" + String(i)).c_str(), "Bilinmiyor");
      bool k   = preferences.getBool(("k_" + String(i)).c_str(), false);
      String payload = "{\"slot\":" + String(i) + ",\"zaman\":\"" + z + "\",\"olay\":\"" + o + "\",\"kritik\":" + (k ? "true" : "false") + "}";
      mqttClient.publish(topicGecmisLogs, payload.c_str());
      delay(50);
    }
  }
  preferences.end();
}

// ===================== GÜVENLİK MODU =====================
void guvenlikModuIsle(int ldrDegeri, int hareketVarMi) {
  display.setCursor(0, 15);
  display.print("Mod[3]: GUVENLIK");
  // NOT: Buton okuma burada yok, tamamı loop() icinde merkezi yonetiliyor

  if (alarmAktif) {
    digitalWrite(buzzerPin, HIGH);
    digitalWrite(ledPin, (millis() / 300) % 2);
    display.setTextSize(2); display.setCursor(0, 28); display.print("!! ALARM !!");

    static unsigned long sonAlarmMesaj = 0;
    if (millis() - sonAlarmMesaj > 3000 && mqttClient.connected()) {
      String alarmJson = "{\"durum\":\"ALARM\",\"pir\":" + String(hareketVarMi) + ",\"ldr\":" + String(ldrDegeri) + "}";
      mqttClient.publish(topicAlarm, alarmJson.c_str());
      sonAlarmMesaj = millis();
    }
  } else {
    if (hareketVarMi == HIGH) {
      alarmAktif = true;
      alarmBaslangic = millis();
      digitalWrite(buzzerPin, HIGH);
      Serial.println("HAREKET ALGILANDI! Alarm basliyor!");
      eepromLogYaz("TEHLİKE: Güvenlik İhlali!", true);
      // Alarm logunu MQTT ile acil gönder
      if (mqttClient.connected()) {
        String alarmJson = "{\"durum\":\"ALARM_BASLADI\",\"saat\":\"" +
          String(rtc.getHour(true)) + ":" + String(rtc.getMinute()) + "\"}";
        mqttClient.publish(topicAlarm, alarmJson.c_str());
      }
    } else {
      digitalWrite(buzzerPin, LOW);
      display.setTextSize(1); display.setCursor(0, 28); display.print("Durum: Korunan Alan");
      if (!zamanlayiciAktifMi && !akilliModGeriSayimAktif) digitalWrite(ledPin, LOW);
    }
  }
}

// ===================== DAHİLİ RTC'DEN SAAT =====================
bool ntpSaatiAl(int &saat, int &dakika, int &saniye) {
  saat   = rtc.getHour(true);
  dakika = rtc.getMinute();
  saniye = rtc.getSecond();
  return true;
}

// ===================== ZAMANLAYICI AYARLA =====================
bool zamanlayiciAyarla(String mesaj) {
  if (mesaj.length() != 11) return false;
  if (mesaj[2] != ':' || mesaj[5] != '-' || mesaj[8] != ':') return false;

  int bs  = mesaj.substring(0, 2).toInt();
  int bd  = mesaj.substring(3, 5).toInt();
  int bts = mesaj.substring(6, 8).toInt();
  int btd = mesaj.substring(9, 11).toInt();

  if (bs < 0 || bs > 23 || bd < 0 || bd > 59) return false;
  if (bts < 0 || bts > 23 || btd < 0 || btd > 59) return false;

  zamBasSaat = bs; zamBasDakika = bd;
  zamBitSaat = bts; zamBitDakika = btd;

  preferences.begin("spark-data", false);
  preferences.putInt("bSaat", zamBasSaat); preferences.putInt("bDakika", zamBasDakika);
  preferences.putInt("bitSaat", zamBitSaat); preferences.putInt("bitDakika", zamBitDakika);
  preferences.end();
  eepromLogYaz("Zamanlayıcı Programı Güncellendi", false);
  return true;
}

// ===================== ZAMANLAYICI KONTROL =====================
void zamanlayiciKontrol() {
  if (zamBasSaat < 0) return;
  int saat, dakika, saniye;
  ntpSaatiAl(saat, dakika, saniye);
  int simdi = saat * 60 + dakika;
  int basla = zamBasSaat * 60 + zamBasDakika;
  int bitis = zamBitSaat * 60 + zamBitDakika;
  bool aralikIcinde = (basla <= bitis) ? (simdi >= basla && simdi < bitis) : (simdi >= basla || simdi < bitis);

  if (aralikIcinde && !zamanlayiciAktifMi) {
    zamanlayiciAktifMi = true;
    eepromLogYaz("Zamanlayıcı Başladı: LED AÇIK", false);
  } else if (!aralikIcinde && zamanlayiciAktifMi) {
    zamanlayiciAktifMi = false;
    digitalWrite(ledPin, LOW);
    eepromLogYaz("Zamanlayıcı Bitti: LED KAPALI", false);
  }
  if (zamanlayiciAktifMi) digitalWrite(ledPin, HIGH);
}

// ===================== SAAT EKRANI (UYKU MODU) =====================
void saatEkrani() {
  int saat, dakika, saniye;
  display.clearDisplay();
  ntpSaatiAl(saat, dakika, saniye);
  display.setTextSize(3); display.setCursor(8, 10);
  char saatBuf[6]; sprintf(saatBuf, "%02d:%02d", saat, dakika); display.print(saatBuf);
  display.setTextSize(2); display.setCursor(96, 16);
  char saniyeBuf[3]; sprintf(saniyeBuf, "%02d", saniye); display.print(saniyeBuf);
  display.setTextSize(1); display.drawFastHLine(0, 50, 128, SSD1306_WHITE);
  display.setCursor(0, 54);
  if (anaMod == 1) {
    display.print("YZ:");
    display.print((int)(annSonCikis * 100));
    display.print("% ");
    display.print(annSonKarar ? "ACIK" : "KAPALI");
  } else {
    display.print("Mod:");
    if (aktifMod == 0) display.print("AKILLI");
    else if (aktifMod == 1) display.print("HAREKET");
    else if (aktifMod == 2) display.print("LDR");
    else display.print("GUVENLIK");
  }
  display.display();
}

// ===================== MQTT CALLBACK =====================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String mesaj = "";
  for (unsigned int i = 0; i < length; i++) mesaj += (char)payload[i];

  if (String(topic) == topicMod) {
    int yeniMod = mesaj.toInt();
    if (yeniMod >= 0 && yeniMod <= 3) {
      aktifMod = yeniMod; anaMod = 0;
      preferences.begin("spark-data", false);
      preferences.putInt("aktifMod", aktifMod);
      preferences.putInt("anaMod", 0);
      preferences.end();
      if (aktifMod != 3) { alarmAktif = false; digitalWrite(buzzerPin, LOW); }
      sonHareketZamani = millis();
      modDegistirEkrani();
      eepromLogYaz("MQTT: Manuel Mod Değişti", false);
    }
  }

  if (String(topic) == topicAnaMod) {
    if (mesaj == "1" || mesaj == "yz") {
      anaMod = 1;
      preferences.begin("spark-data", false); preferences.putInt("anaMod", 1); preferences.end();
      eepromLogYaz("MQTT: YZ Moduna Geçildi", false);
    } else {
      anaMod = 0;
      preferences.begin("spark-data", false); preferences.putInt("anaMod", 0); preferences.end();
      eepromLogYaz("MQTT: Manuel Moda Geçildi", false);
    }
  }

  if (String(topic) == topicAlarmKapat && (mesaj == "1" || mesaj == "kapat")) {
    alarmAktif = false;
    digitalWrite(buzzerPin, LOW); digitalWrite(ledPin, LOW);
    eepromLogYaz("Uzaktan Telefon: Alarm Kapatıldı", true);
  }

  if (String(topic) == topicZaman) {
    if (zamanlayiciAyarla(mesaj)) {
      sonHareketZamani = millis();
    }
  }
}

// ===================== MQTT BAĞLAN =====================
void mqttBaglan() {
  if (!mqttClient.connected() && WiFi.status() == WL_CONNECTED) {
    if (mqttClient.connect(mqttClientId)) {
      mqttClient.subscribe(topicMod);
      mqttClient.subscribe(topicAlarmKapat);
      mqttClient.subscribe(topicZaman);
      mqttClient.subscribe(topicAnaMod);
      offlineLoglariMqttGonder();
    }
  }
}

// ===================== MOD DEĞİŞTİRME EKRANI =====================
void modDegistirEkrani() {
  display.clearDisplay();
  display.setTextSize(1); display.setCursor(5, 5);
  if (anaMod == 1) {
    display.print(">> YZ MODU AKTİF <<");
    display.setTextSize(2); display.setCursor(5, 28); display.print("YAPAY ZKA");
  } else {
    display.setCursor(20, 15); display.print("Mod Degistirildi:");
    display.setTextSize(2); display.setCursor(10, 35);
    if (aktifMod == 0) display.print("AKILLI");
    if (aktifMod == 1) display.print("HAREKET");
    if (aktifMod == 2) display.print("GECE/LDR");
    if (aktifMod == 3) display.print("GUVENLIK");
  }
  display.display();
  delay(1000);
}

// ===================== BLE DURUM GÖNDER =====================
void bleDurumGonder(int ldr, int pir, int mod, bool ledAcik) {
  if (!bleConnected || pDurumChar == nullptr) return;
  String json = "{\"anamod\":" + String(anaMod) +
                ",\"mod\":"   + String(mod) +
                ",\"ldr\":"   + String(ldr) +
                ",\"pir\":"   + String(pir) +
                ",\"led\":"   + String(ledAcik ? "true" : "false") +
                ",\"alarm\":" + String(alarmAktif ? "true" : "false") +
                ",\"ann\":"   + annDurumString() + "}";
  pDurumChar->setValue(json.c_str());
  pDurumChar->notify();
}

// ===================== WiFi ENDPOINT KURULUM =====================
void wifiKurulum() {
  server.on("/", HTTP_GET, []() {
    String html = R"=====(
<!DOCTYPE html>
<html lang='tr'>
<head>
    <meta charset='UTF-8'>
    <meta name='viewport' content='width=device-width, initial-scale=1.0'>
    <title>SPARK S3 — Neural Control Center</title>
    <link rel='preconnect' href='https://fonts.googleapis.com'>
    <link href='https://fonts.googleapis.com/css2?family=Share+Tech+Mono&family=Rajdhani:wght@500;700&display=swap' rel='stylesheet'>
    <style>
        :root {
            --bg: #050a0f;
            --panel: #0a1520;
            --border: #0f3a5c;
            --accent: #00d4ff;
            --accent2: #ff6b35;
            --accent3: #39ff14;
            --warn: #ffcc00;
            --danger: #ff1a1a;
            --text: #c8e6f0;
            --muted: #4a7a8f;
            --font-mono: 'Share Tech Mono', monospace;
            --font-head: 'Rajdhani', sans-serif;
        }
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body {
            background: var(--bg);
            color: var(--text);
            font-family: var(--font-mono);
            min-height: 100vh;
            padding: 20px;
            background-image:
                radial-gradient(ellipse at 20% 50%, rgba(0, 80, 120, 0.08) 0%, transparent 60%),
                radial-gradient(ellipse at 80% 10%, rgba(0, 212, 255, 0.05) 0%, transparent 40%);
        }
        .header {
            text-align: center;
            padding: 30px 0 20px;
            border-bottom: 1px solid var(--border);
            margin-bottom: 25px;
            position: relative;
        }
        .header::before {
            content: '';
            position: absolute;
            bottom: -1px; left: 50%;
            transform: translateX(-50%);
            width: 200px; height: 2px;
            background: linear-gradient(90deg, transparent, var(--accent), transparent);
        }
        h1 {
            font-family: var(--font-head);
            font-size: 38px;
            font-weight: 700;
            letter-spacing: 8px;
            color: var(--accent);
            text-shadow: 0 0 30px rgba(0,212,255,0.4);
        }
        .subtitle { color: var(--muted); font-size: 12px; letter-spacing: 3px; margin-top: 6px; }

        .grid-2 { display: grid; grid-template-columns: 1fr 1fr; gap: 20px; margin-bottom: 20px; }
        .grid-3 { display: grid; grid-template-columns: repeat(3, 1fr); gap: 15px; }
        @media(max-width: 700px) { .grid-2, .grid-3 { grid-template-columns: 1fr; } }

        .panel {
            background: var(--panel);
            border: 1px solid var(--border);
            border-radius: 8px;
            padding: 20px;
            position: relative;
            overflow: hidden;
        }
        .panel::before {
            content: '';
            position: absolute;
            top: 0; left: 0; right: 0;
            height: 2px;
            background: linear-gradient(90deg, transparent, var(--accent), transparent);
            opacity: 0.4;
        }
        .panel-title {
            font-family: var(--font-head);
            font-size: 14px;
            letter-spacing: 3px;
            color: var(--accent);
            margin-bottom: 16px;
            text-transform: uppercase;
        }

        /* ANA MOD TOGGLE */
        .mode-toggle {
            display: flex;
            background: #030810;
            border: 1px solid var(--border);
            border-radius: 8px;
            overflow: hidden;
            margin-bottom: 20px;
        }
        .toggle-btn {
            flex: 1;
            padding: 14px;
            border: none;
            background: transparent;
            cursor: pointer;
            font-family: var(--font-head);
            font-size: 15px;
            font-weight: 700;
            letter-spacing: 2px;
            color: var(--muted);
            transition: all 0.3s;
        }
        .toggle-btn.active-manual {
            background: linear-gradient(135deg, #0d2a1a, #0a2010);
            color: var(--accent3);
            text-shadow: 0 0 12px rgba(57,255,20,0.6);
            border-right: 1px solid rgba(57,255,20,0.3);
        }
        .toggle-btn.active-ann {
            background: linear-gradient(135deg, #001a2e, #00142a);
            color: var(--accent);
            text-shadow: 0 0 12px rgba(0,212,255,0.6);
        }

        /* SENSÖR KUTULARI */
        .sensor-val { font-size: 28px; font-weight: bold; color: var(--accent); line-height: 1; }
        .sensor-label { font-size: 10px; color: var(--muted); letter-spacing: 2px; margin-top: 4px; }
        .status-on  { color: var(--accent3) !important; text-shadow: 0 0 8px rgba(57,255,20,0.5); }
        .status-off { color: var(--danger) !important; }
        .status-warn{ color: var(--warn) !important; text-shadow: 0 0 8px rgba(255,204,0,0.5); }

        /* YZ VİZÜALİZASYON */
        .ann-viz {
            display: flex;
            align-items: center;
            justify-content: space-around;
            padding: 15px 0;
            gap: 10px;
        }
        .ann-layer { display: flex; flex-direction: column; gap: 8px; align-items: center; }
        .ann-node {
            width: 32px; height: 32px;
            border-radius: 50%;
            border: 2px solid var(--border);
            background: #030810;
            display: flex; align-items: center; justify-content: center;
            font-size: 8px; color: var(--muted);
            transition: all 0.5s;
            position: relative;
        }
        .ann-node.active {
            border-color: var(--accent);
            background: rgba(0,212,255,0.1);
            box-shadow: 0 0 12px rgba(0,212,255,0.4);
            color: var(--accent);
        }
        .ann-node.output-on {
            border-color: var(--accent3);
            background: rgba(57,255,20,0.1);
            box-shadow: 0 0 16px rgba(57,255,20,0.5);
        }
        .ann-node.output-off {
            border-color: var(--danger);
            background: rgba(255,26,26,0.05);
        }
        .ann-connector { flex: 1; height: 1px; background: var(--border); position: relative; }
        .ann-layer-label { font-size: 9px; color: var(--muted); letter-spacing: 1px; margin-top: 4px; }

        /* DOĞRULUK ÇUBUĞU */
        .acc-bar-wrap { background: #030810; border-radius: 4px; height: 8px; overflow: hidden; margin-top: 8px; }
        .acc-bar { height: 100%; background: linear-gradient(90deg, var(--accent), var(--accent3)); border-radius: 4px; transition: width 0.8s ease; }

        /* MANUEL MOD BUTONLARI */
        .mod-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
        .mod-btn {
            background: #030810;
            border: 1px solid var(--border);
            color: var(--muted);
            padding: 12px;
            border-radius: 6px;
            cursor: pointer;
            font-family: var(--font-head);
            font-weight: 700;
            font-size: 13px;
            letter-spacing: 1px;
            transition: all 0.2s;
        }
        .mod-btn:hover { border-color: var(--accent); color: var(--accent); }
        .mod-btn.active {
            background: rgba(0,212,255,0.08);
            border-color: var(--accent);
            color: var(--accent);
            box-shadow: 0 0 12px rgba(0,212,255,0.2);
        }
        .mod-btn.disabled-mode { opacity: 0.3; cursor: not-allowed; pointer-events: none; }

        /* ALARM BUTONU */
        .btn-alarm {
            display: none; width: 100%; margin-top: 12px;
            background: rgba(255,26,26,0.1);
            border: 1px solid var(--danger);
            color: var(--danger);
            padding: 14px;
            border-radius: 6px;
            cursor: pointer;
            font-family: var(--font-head);
            font-size: 16px; font-weight: 700;
            letter-spacing: 2px;
            animation: blink 1s infinite;
        }
        @keyframes blink { 0%,100%{opacity:1} 50%{opacity:0.5} }

        /* ZAMANLAYICI */
        input[type='text'] {
            width: 100%; padding: 10px 14px;
            background: #030810; border: 1px solid var(--border);
            border-radius: 6px; color: var(--text);
            font-family: var(--font-mono); font-size: 16px;
            text-align: center; margin-bottom: 12px;
        }
        input[type='text']:focus { outline: none; border-color: var(--accent); }
        .btn-row { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
        .btn-primary {
            background: rgba(0,212,255,0.1); border: 1px solid var(--accent);
            color: var(--accent); padding: 10px; border-radius: 6px;
            cursor: pointer; font-family: var(--font-head); font-weight: 700; font-size: 13px;
        }
        .btn-secondary {
            background: #030810; border: 1px solid var(--border);
            color: var(--muted); padding: 10px; border-radius: 6px;
            cursor: pointer; font-family: var(--font-head); font-weight: 700; font-size: 13px;
        }

        /* LOG TABLOSU */
        .tab-row { display: flex; gap: 8px; margin-bottom: 12px; }
        .tab-pill {
            padding: 5px 14px; border-radius: 20px;
            background: #030810; border: 1px solid var(--border);
            color: var(--muted); cursor: pointer; font-size: 11px; letter-spacing: 1px;
        }
        .tab-pill.act { background: rgba(0,212,255,0.1); border-color: var(--accent); color: var(--accent); }
        .tab-pill.act-warn { background: rgba(255,26,26,0.1); border-color: var(--danger); color: var(--danger); }
        table { width: 100%; border-collapse: collapse; font-size: 12px; }
        th { text-align: left; padding: 8px 10px; background: #030810; color: var(--muted); letter-spacing: 1px; border-bottom: 1px solid var(--border); }
        td { padding: 8px 10px; border-bottom: 1px solid rgba(15,58,92,0.4); color: var(--text); }
        tr:hover td { background: rgba(0,212,255,0.03); }
        .badge { padding: 2px 8px; border-radius: 4px; font-size: 10px; font-weight: bold; letter-spacing: 1px; }
        .badge-alarm  { background: rgba(255,26,26,0.15); color: var(--danger); border: 1px solid rgba(255,26,26,0.3); }
        .badge-normal { background: rgba(0,212,255,0.1); color: var(--accent); border: 1px solid rgba(0,212,255,0.2); }
        .badge-ann    { background: rgba(57,255,20,0.1); color: var(--accent3); border: 1px solid rgba(57,255,20,0.2); }

        /* DURUM GÖSTERGELERİ */
        .conn-dots { display: flex; gap: 10px; flex-wrap: wrap; }
        .dot { display: flex; align-items: center; gap: 5px; font-size: 11px; }
        .dot-led { width: 8px; height: 8px; border-radius: 50%; background: var(--muted); }
        .dot-led.on { background: var(--accent3); box-shadow: 0 0 6px var(--accent3); }
        .dot-led.warn { background: var(--danger); box-shadow: 0 0 6px var(--danger); }

        /* ALEV ETKİSİ - ALARM DURUMU */
        .alarm-banner {
            display: none;
            background: rgba(255,26,26,0.1);
            border: 1px solid var(--danger);
            border-radius: 6px;
            padding: 12px;
            text-align: center;
            font-family: var(--font-head);
            font-size: 18px;
            color: var(--danger);
            letter-spacing: 3px;
            margin-bottom: 15px;
            animation: blink 0.5s infinite;
        }

        .zam-indicator {
            display: inline-block; padding: 3px 10px;
            border-radius: 4px; font-size: 11px;
            background: rgba(255,107,53,0.1);
            border: 1px solid var(--accent2);
            color: var(--accent2); margin-left: 8px;
        }
    </style>
</head>
<body>
<div style='max-width:960px; margin:0 auto;'>

    <div class='header'>
        <h1>⚡ SPARK S3</h1>
        <div class='subtitle'>Neural Lighting &amp; Security Automation System</div>
    </div>

    <div id='alarmBanner' class='alarm-banner'>⚠ GÜVENLİK İHLALİ TESPİT EDİLDİ — ALARM AKTİF ⚠</div>

    <!-- ANA MOD SEÇİCİ -->
    <div class='mode-toggle'>
        <button class='toggle-btn' id='toggleManuel' onclick='anaMod(0)'>⚙ MANUEL KONTROL</button>
        <button class='toggle-btn' id='toggleAnn' onclick='anaMod(1)'>🧠 YAPAY SİNİR AĞI</button>
    </div>

    <div class='grid-2'>
        <!-- SENSÖRLER -->
        <div class='panel'>
            <div class='panel-title'>▸ CANLI SENSÖR VERİSİ</div>
            <div class='grid-3' style='margin-bottom:12px;'>
                <div>
                    <div class='sensor-val' id='ldrVal'>—</div>
                    <div class='sensor-label'>LDR IŞIK</div>
                </div>
                <div>
                    <div class='sensor-val' id='pirVal'>—</div>
                    <div class='sensor-label'>PIR HAREKET</div>
                </div>
                <div>
                    <div class='sensor-val' id='ledVal'>—</div>
                    <div class='sensor-label'>LED ÇIKIŞ</div>
                </div>
            </div>
            <div style='display:flex; justify-content:space-between; align-items:center;'>
                <div>
                    <div style='font-size:22px; color:var(--warn);' id='saatVal'>—</div>
                    <div class='sensor-label'>SİSTEM SAATİ (RTC)</div>
                </div>
                <div class='conn-dots'>
                    <div class='dot'><div class='dot-led' id='dotWifi'></div><span>WiFi</span></div>
                    <div class='dot'><div class='dot-led' id='dotBle'></div><span>BLE</span></div>
                    <div class='dot'><div class='dot-led' id='dotMqtt'></div><span>MQTT</span></div>
                    <div class='dot'><div class='dot-led' id='dotZam'></div><span>ZAM</span></div>
                </div>
            </div>
        </div>

        <!-- YZ PANELİ -->
        <div class='panel'>
            <div class='panel-title'>▸ YZ SİNİR AĞI DURUMU <span id='annModLabel' style='color:var(--muted);font-size:11px;'></span></div>
            <div class='ann-viz'>
                <div class='ann-layer'>
                    <div class='ann-node' id='n_ldr'>LDR</div>
                    <div class='ann-node' id='n_saat'>SAT</div>
                    <div class='ann-node' id='n_pir'>PIR</div>
                    <div class='ann-layer-label'>GİRİŞ</div>
                </div>
                <div class='ann-connector'></div>
                <div class='ann-layer'>
                    <div class='ann-node' id='h1'>H1</div>
                    <div class='ann-node' id='h2'>H2</div>
                    <div class='ann-node' id='h3'>H3</div>
                    <div class='ann-node' id='h4'>H4</div>
                    <div class='ann-layer-label'>GİZLİ</div>
                </div>
                <div class='ann-connector'></div>
                <div class='ann-layer'>
                    <div class='ann-node' id='n_out'>OUT</div>
                    <div class='ann-layer-label'>ÇIKIŞ</div>
                </div>
            </div>
            <div style='display:flex; justify-content:space-between; font-size:11px; color:var(--muted); margin-top:4px;'>
                <span>Güven: <b id='annConf' style='color:var(--accent);'>—%</b></span>
                <span>Karar: <b id='annKarar'>—</b></span>
                <span>Doğruluk: <b id='annAcc' style='color:var(--accent3);'>—%</b></span>
                <span>Toplam: <b id='annTotal'>—</b></span>
            </div>
            <div class='acc-bar-wrap'><div class='acc-bar' id='accBar' style='width:0%'></div></div>
        </div>
    </div>

    <div class='grid-2'>
        <!-- MANUEL MOD KONTROL -->
        <div class='panel'>
            <div class='panel-title'>▸ MANUEL MOD SEÇİMİ</div>
            <div class='mod-grid'>
                <button class='mod-btn' id='m0' onclick='modSec(0)'>🤖 AKILLI MOD</button>
                <button class='mod-btn' id='m1' onclick='modSec(1)'>🏃 HAREKET</button>
                <button class='mod-btn' id='m2' onclick='modSec(2)'>🌙 GECE / LDR</button>
                <button class='mod-btn' id='m3' onclick='modSec(3)'>🛡 GÜVENLİK</button>
            </div>
            <button class='btn-alarm' id='alarmBtn' onclick='alarmKapat()'>
                🚨 ALARMI KAPAT 🚨
            </button>
        </div>

        <!-- ZAMANLAYICI -->
        <div class='panel'>
            <div class='panel-title'>▸ AYDINLATMA ZAMANLAYICISI</div>
            <input type='text' id='zamanAralik' placeholder='Örn: 20:00-23:30' maxlength='11'>
            <div class='btn-row'>
                <button class='btn-primary' onclick='zamanAyarla()'>▶ ZAMANLAYICI KUR</button>
                <button class='btn-secondary' onclick='zamanIptal()'>✕ İPTAL</button>
            </div>
            <div style='margin-top:12px; font-size:11px; color:var(--muted);'>
                Mevcut Program: <b id='zamProg' style='color:var(--accent2);'>—</b>
            </div>
        </div>
    </div>

    <!-- EEPROM LOG TABLOSU -->
    <div class='panel'>
        <div class='panel-title'>▸ EEPROM GEÇMİŞ KAYITLARI — RTC Destekli, İnternet Bağımsız</div>
        <div class='tab-row'>
            <button class='tab-pill act' id='tabAll' onclick='filtre("all")'>📋 TÜMÜ</button>
            <button class='tab-pill' id='tabAlarm' onclick='filtre("alarm")'>🚨 GÜVENLİK ALARMLARI</button>
            <button class='tab-pill' id='tabSys' onclick='filtre("sys")'>💡 SİSTEM</button>
        </div>
        <table>
            <thead>
                <tr>
                    <th>ZAMAN (RTC)</th>
                    <th>OLAY AÇIKLAMASI</th>
                    <th>TÜR</th>
                    <th>DEPOLAMA</th>
                </tr>
            </thead>
            <tbody id='logBody'>
                <tr><td colspan='4' style='text-align:center;color:var(--muted);padding:20px;'>Kayıtlar yükleniyor...</td></tr>
            </tbody>
        </table>
    </div>

</div>

<script>
let aktifFiltre = 'all';
let aktifAnaModVal = 0;
let zamBasSaatVal = -1, zamBitSaatVal = -1, zamBasDakVal = -1, zamBitDakVal = -1;

function veriGuncelle() {
    fetch('/durum').then(r => r.json()).then(d => {
        // Sensörler
        document.getElementById('ldrVal').innerText = d.ldr;
        let pirEl = document.getElementById('pirVal');
        if(d.pir == 1) { pirEl.innerText = '⚠ VAR'; pirEl.className = 'sensor-val status-warn'; }
        else { pirEl.innerText = 'YOK'; pirEl.className = 'sensor-val'; }
        let ledEl = document.getElementById('ledVal');
        if(d.led) { ledEl.innerText = '💡 AÇIK'; ledEl.className = 'sensor-val status-on'; }
        else { ledEl.innerText = 'KAPALI'; ledEl.className = 'sensor-val status-off'; }
        let s = d.saat >= 0 ? String(d.saat).padStart(2,'0')+':'+String(d.dakika).padStart(2,'0') : '--:--';
        document.getElementById('saatVal').innerText = s;

        // Bağlantı noktaları
        setDot('dotWifi', d.wifi);
        setDot('dotBle', d.ble);
        setDot('dotMqtt', d.mqtt);
        setDot('dotZam', d.zam, true);

        // Alarm banner
        document.getElementById('alarmBanner').style.display = d.alarm ? 'block' : 'none';
        document.getElementById('alarmBtn').style.display = d.alarm ? 'block' : 'none';

        // Mod butonları
        for(let i=0;i<=3;i++) {
            let b = document.getElementById('m'+i);
            b.classList.remove('active');
            b.classList.toggle('disabled-mode', d.anamod == 1);
        }
        if(d.anamod == 0) document.getElementById('m'+d.mod).classList.add('active');

        // Ana mod toggle
        aktifAnaModVal = d.anamod;
        document.getElementById('toggleManuel').className = 'toggle-btn' + (d.anamod==0 ? ' active-manual' : '');
        document.getElementById('toggleAnn').className    = 'toggle-btn' + (d.anamod==1 ? ' active-ann' : '');

        // YZ paneli
        if(d.ann) {
            let conf = Math.round(d.ann.cikis * 100);
            document.getElementById('annConf').innerText = conf + '%';
            let karEl = document.getElementById('annKarar');
            if(d.ann.karar) { karEl.innerText = '💡 AÇIK'; karEl.style.color = 'var(--accent3)'; }
            else { karEl.innerText = 'KAPALI'; karEl.style.color = 'var(--danger)'; }
            document.getElementById('annAcc').innerText = d.ann.dogruluk + '%';
            document.getElementById('annTotal').innerText = d.ann.toplam;
            document.getElementById('accBar').style.width = d.ann.dogruluk + '%';
            document.getElementById('annModLabel').innerText = d.anamod==1 ? '[AKTİF]' : '[PASIF]';

            // Node animasyonu
            let ldrActive = d.ldr > 2500;
            let pirActive = d.pir == 1;
            let saatActive = d.saat >= 18 || d.saat < 7;
            setNode('n_ldr', ldrActive);
            setNode('n_saat', saatActive);
            setNode('n_pir', pirActive);
            for(let h of ['h1','h2','h3','h4']) setNode(h, Math.random() > 0.4);
            let outEl = document.getElementById('n_out');
            outEl.className = 'ann-node ' + (d.ann.karar ? 'output-on' : 'output-off');
        }

        // Zamanlayıcı göster
        if(zamBasSaatVal >= 0) {
            document.getElementById('zamProg').innerText =
                String(zamBasSaatVal).padStart(2,'0')+':'+String(zamBasDakVal).padStart(2,'0')
                + ' — ' + String(zamBitSaatVal).padStart(2,'0')+':'+String(zamBitDakVal).padStart(2,'0');
        }
    }).catch(()=>{});
}

function setDot(id, isOn, isWarn) {
    let el = document.getElementById(id);
    el.className = 'dot-led' + (isOn ? (isWarn ? ' warn' : ' on') : '');
}
function setNode(id, isActive) {
    document.getElementById(id).className = 'ann-node' + (isActive ? ' active' : '');
}

function anaMod(val) {
    let fd = new FormData(); fd.append('deger', val);
    fetch('/anamod', {method:'POST', body:fd}).then(()=>{ veriGuncelle(); logGuncelle(); });
}
function modSec(val) {
    let fd = new FormData(); fd.append('deger', val);
    fetch('/mod', {method:'POST', body:fd}).then(()=>{ veriGuncelle(); setTimeout(logGuncelle, 400); });
}
function alarmKapat() {
    fetch('/alarm/kapat', {method:'POST'}).then(()=>veriGuncelle());
}
function zamanAyarla() {
    let aralik = document.getElementById('zamanAralik').value.trim();
    if(aralik.length == 11 && aralik[5] == '-') {
        let parts = aralik.split('-');
        let bs = parts[0].split(':'); let bt = parts[1].split(':');
        zamBasSaatVal=parseInt(bs[0]); zamBasDakVal=parseInt(bs[1]);
        zamBitSaatVal=parseInt(bt[0]); zamBitDakVal=parseInt(bt[1]);
    }
    let fd = new FormData(); fd.append('aralik', aralik);
    fetch('/zamanlayici', {method:'POST', body:fd}).then(r=>r.json()).then(d=>{
        if(d.sonuc=='ok') setTimeout(logGuncelle,400);
        else alert('Format hatası! HH:MM-HH:MM');
    });
}
function zamanIptal() {
    fetch('/zamanlayici/iptal', {method:'POST'}).then(()=>{
        zamBasSaatVal=-1; document.getElementById('zamAralik').value='';
        document.getElementById('zamProg').innerText='—';
        setTimeout(logGuncelle,400);
    });
}

function filtre(tip) {
    aktifFiltre = tip;
    ['tabAll','tabAlarm','tabSys'].forEach(id => {
        let b = document.getElementById(id);
        b.className = 'tab-pill';
        if(id=='tabAll' && tip=='all') b.className = 'tab-pill act';
        if(id=='tabAlarm' && tip=='alarm') b.className = 'tab-pill act-warn';
        if(id=='tabSys' && tip=='sys') b.className = 'tab-pill act';
    });
    logGuncelle();
}

function logGuncelle() {
    fetch('/logs').then(r=>r.json()).then(logs=>{
        let tb = document.getElementById('logBody'); tb.innerHTML='';
        let cnt = 0;
        logs.forEach(log => {
            if(aktifFiltre == 'alarm' && !log.kritik) return;
            if(aktifFiltre == 'sys' && log.kritik) return;
            cnt++;
            let badge, bClass;
            if(log.kritik) { badge='🚨 ALARM'; bClass='badge badge-alarm'; }
            else if(log.olay && log.olay.indexOf('YZ')>=0) { badge='🧠 YZ'; bClass='badge badge-ann'; }
            else { badge='💡 SİSTEM'; bClass='badge badge-normal'; }
            let tr = document.createElement('tr');
            tr.innerHTML = `<td><b>${log.zaman}</b></td><td>${log.olay}</td>
                <td><span class='${bClass}'>${badge}</span></td>
                <td style='color:var(--accent);font-size:11px;'>EEPROM Flash</td>`;
            tb.appendChild(tr);
        });
        if(cnt==0) tb.innerHTML='<tr><td colspan="4" style="text-align:center;color:var(--muted);padding:20px;">Kayıt bulunamadı.</td></tr>';
    });
}

setInterval(veriGuncelle, 1000);
setInterval(logGuncelle, 6000);
veriGuncelle(); logGuncelle();
</script>
</body>
</html>
    )=====";
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "text/html", html);
  });

  // ANA MOD GEÇİŞ ENDPOİNT
  server.on("/anamod", HTTP_POST, []() {
    if (server.hasArg("deger")) {
      int val = server.arg("deger").toInt();
      anaMod = (val == 1) ? 1 : 0;
      preferences.begin("spark-data", false);
      preferences.putInt("anaMod", anaMod);
      preferences.end();
      sonHareketZamani = millis();
      if (anaMod == 1) {
        eepromLogYaz("Web: YZ Moduna Geçildi", false);
        modDegistirEkrani();
      } else {
        eepromLogYaz("Web: Manuel Moda Geçildi", false);
        modDegistirEkrani();
      }
      server.sendHeader("Access-Control-Allow-Origin", "*");
      server.send(200, "application/json", "{\"sonuc\":\"ok\",\"anamod\":" + String(anaMod) + "}");
    }
  });

  server.on("/logs", HTTP_GET, []() {
    String json = "[";
    preferences.begin("spark-logs", true);
    bool ilk = true;
    for (int i = 0; i < MAX_LOGS; i++) {
      String kZam = "z_" + String(i);
      if (preferences.isKey(kZam.c_str())) {
        String z = preferences.getString(kZam.c_str(), "00:00");
        String o = preferences.getString(("o_" + String(i)).c_str(), "?");
        bool k   = preferences.getBool(("k_" + String(i)).c_str(), false);
        if (!ilk) json += ",";
        json += "{\"zaman\":\"" + z + "\",\"olay\":\"" + o + "\",\"kritik\":" + (k ? "true" : "false") + "}";
        ilk = false;
      }
    }
    preferences.end();
    json += "]";
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
  });

  server.on("/durum", HTTP_GET, []() {
    int ldr = analogRead(ldrPin);
    int pir = digitalRead(pirPin);
    bool led = digitalRead(ledPin);
    int saat = -1, dakika = -1, saniye = -1;
    ntpSaatiAl(saat, dakika, saniye);
    bool wifiConn = (WiFi.status() == WL_CONNECTED);
    String json = "{\"anamod\":"   + String(anaMod) +
                  ",\"mod\":"      + String(aktifMod) +
                  ",\"ldr\":"      + String(ldr) +
                  ",\"pir\":"      + String(pir) +
                  ",\"led\":"      + String(led ? "true" : "false") +
                  ",\"alarm\":"    + String(alarmAktif ? "true" : "false") +
                  ",\"zam\":"      + String(zamanlayiciAktifMi ? "true" : "false") +
                  ",\"saat\":"     + String(saat) +
                  ",\"dakika\":"   + String(dakika) +
                  ",\"wifi\":"     + String(wifiConn ? "true" : "false") +
                  ",\"ble\":"      + String(bleConnected ? "true" : "false") +
                  ",\"mqtt\":"     + String(mqttClient.connected() ? "true" : "false") +
                  ",\"ann\":"      + annDurumString() + "}";
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
  });

  server.on("/mod", HTTP_POST, []() {
    if (server.hasArg("deger")) {
      int yeniMod = server.arg("deger").toInt();
      if (yeniMod >= 0 && yeniMod <= 3) {
        aktifMod = yeniMod; anaMod = 0;
        preferences.begin("spark-data", false);
        preferences.putInt("aktifMod", aktifMod);
        preferences.putInt("anaMod", 0);
        preferences.end();
        if (aktifMod != 3) { alarmAktif = false; digitalWrite(buzzerPin, LOW); }
        sonHareketZamani = millis();
        modDegistirEkrani();
        eepromLogYaz("Web: Manuel Mod Değişti", false);
        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.send(200, "application/json", "{\"sonuc\":\"ok\",\"mod\":" + String(aktifMod) + "}");
      } else { server.send(400, "application/json", "{\"sonuc\":\"hata\"}"); }
    }
  });

  server.on("/alarm/kapat", HTTP_POST, []() {
    alarmAktif = false;
    digitalWrite(buzzerPin, LOW); digitalWrite(ledPin, LOW);
    eepromLogYaz("Web: Alarm Susturuldu", true);
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", "{\"sonuc\":\"alarm kapatildi\"}");
  });

  server.on("/zamanlayici", HTTP_POST, []() {
    if (server.hasArg("aralik")) {
      String aralik = server.arg("aralik");
      if (zamanlayiciAyarla(aralik)) {
        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.send(200, "application/json", "{\"sonuc\":\"ok\"}");
      } else { server.send(400, "application/json", "{\"sonuc\":\"format hatasi\"}"); }
    }
  });

  server.on("/zamanlayici/iptal", HTTP_POST, []() {
    zamBasSaat = -1; zamBasDakika = -1; zamBitSaat = -1; zamBitDakika = -1;
    zamanlayiciAktifMi = false;
    preferences.begin("spark-data", false);
    preferences.putInt("bSaat", -1); preferences.putInt("bDakika", -1);
    preferences.putInt("bitSaat", -1); preferences.putInt("bitDakika", -1);
    preferences.end();
    digitalWrite(ledPin, LOW);
    eepromLogYaz("Zamanlayıcı İptal", false);
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", "{\"sonuc\":\"ok\"}");
  });

  server.begin();
}

// ===================== BLE KURULUM =====================
void bleKurulum() {
  BLEDevice::init("SPARK-S3");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new SunucuCallback());
  BLEService* pService = pServer->createService(SERVICE_UUID);
  pModChar = pService->createCharacteristic(CHAR_MOD_UUID, BLECharacteristic::PROPERTY_WRITE);
  pModChar->setCallbacks(new ModYazmaCallback());
  pDurumChar = pService->createCharacteristic(CHAR_DURUM_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  pDurumChar->addDescriptor(new BLE2902());
  pService->start();
  BLEAdvertising* pAdv = BLEDevice::getAdvertising();
  pAdv->addServiceUUID(SERVICE_UUID);
  pAdv->setScanResponse(true);
  BLEDevice::startAdvertising();
}

// ===================== SETUP =====================
void setup() {
  Serial.begin(115200);
  kaliciHafizadanYukle();
  annKurulum();
  annAgirliklarYukle();  // Flash'tan varsa yükle

  donanimTimer = timerBegin(0, 80, true);
  timerAttachInterrupt(donanimTimer, &onTimerKesmesi, true);
  timerAlarmWrite(donanimTimer, 1000000, true);
  timerAlarmEnable(donanimTimer);

  pinMode(pirPin,        INPUT);
  pinMode(ledPin,        OUTPUT);
  pinMode(butonPin,      INPUT_PULLDOWN);
  pinMode(alarmButonPin, INPUT_PULLDOWN);
  pinMode(buzzerPin,     OUTPUT);
  digitalWrite(buzzerPin, LOW);

  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) for(;;);
  display.setTextColor(SSD1306_WHITE);

  // ── EKRAN 1: Açılış logosu ──
  display.clearDisplay();
  display.setTextSize(2); display.setCursor(20, 0);  display.print("SPARK S3");
  display.setTextSize(1); display.setCursor(15, 22); display.print("Neural IoT System");
  display.drawFastHLine(0, 32, 128, SSD1306_WHITE);
  display.setCursor(5, 38);  display.print("ANN: 3->4->1 Katman");
  display.setCursor(5, 50);  display.print("Baslatiliyor...");
  display.display();
  delay(1200);

  // ── EKRAN 2: WiFi bağlanıyor animasyonu ──
  display.clearDisplay();
  display.setTextSize(1); display.setCursor(0, 0); display.print("[ 1/3 ] WiFi");
  display.drawFastHLine(0, 10, 128, SSD1306_WHITE);
  display.setCursor(0, 16); display.print("Ag: "); display.print(ssid);
  display.setCursor(0, 30); display.print("Baglaniliyor");
  display.display();

  WiFi.begin(ssid, password);
  int deneme = 0;
  int nokta = 0;
  while (WiFi.status() != WL_CONNECTED && deneme < 20) {
    delay(400); deneme++;
    // Animasyon noktaları
    display.fillRect(0, 40, 128, 10, SSD1306_BLACK);
    display.setCursor(0, 40);
    for (int k = 0; k < (deneme % 4) + 1; k++) display.print(".");
    display.display();
  }

  if (WiFi.status() == WL_CONNECTED) {
    // ── EKRAN 3: WiFi bağlandı bilgileri ──
    display.clearDisplay();
    display.setTextSize(1); display.setCursor(0, 0); display.print("[ 1/3 ] WiFi");
    display.drawFastHLine(0, 10, 128, SSD1306_WHITE);
    display.setCursor(0, 14); display.print("BAGLANDI! [OK]");
    display.setCursor(0, 26); display.print("Ag : "); display.print(ssid);
    display.setCursor(0, 37); display.print("IP : "); display.print(WiFi.localIP().toString());
    display.setCursor(0, 48); display.print("Web: http://");
    display.setCursor(0, 57); display.print(WiFi.localIP().toString());
    display.display();
    delay(2500);

    configTime(gmtOffset, dstOffset, ntpSunucu);
    struct tm timeInfo;
    if (getLocalTime(&timeInfo)) {
      ntpHazir = true;
      rtc.setTimeStruct(timeInfo);
      eepromLogYaz("Cihaz Açıldı: NTP+ANN Aktif", false);
    }
    wifiKurulum();
    mqttClient.setServer(mqttBroker, mqttPort);
    mqttClient.setCallback(mqttCallback);
    mqttBaglan();
    eskiWifiDurumu = true;

    // ── EKRAN 4: MQTT bağlantı bilgileri ──
    display.clearDisplay();
    display.setTextSize(1); display.setCursor(0, 0); display.print("[ 2/3 ] MQTT");
    display.drawFastHLine(0, 10, 128, SSD1306_WHITE);
    if (mqttClient.connected()) {
      display.setCursor(0, 14); display.print("BAGLANDI! [OK]");
    } else {
      display.setCursor(0, 14); display.print("Baglaniyor...");
    }
    display.setCursor(0, 26); display.print("Broker: "); display.print(mqttBroker);
    display.setCursor(0, 37); display.print("Port  : "); display.print(mqttPort);
    display.setCursor(0, 48); display.print("ID    : "); display.print(mqttClientId);
    display.setCursor(0, 57); display.print("Topic : spark/#");
    display.display();
    delay(2500);

  } else {
    // ── WiFi YOK ekranı ──
    display.clearDisplay();
    display.setTextSize(1); display.setCursor(0, 0); display.print("[ 1/3 ] WiFi");
    display.drawFastHLine(0, 10, 128, SSD1306_WHITE);
    display.setCursor(0, 14); display.print("BAGLANTI YOK! [!!]");
    display.setCursor(0, 26); display.print("Offline modda calis.");
    display.setCursor(0, 37); display.print("RTC + ANN aktif.");
    display.setCursor(0, 48); display.print("MQTT: Devre disi");
    display.display();
    rtc.setTime(0, 0, 0, 1, 1, 2026);
    eepromLogYaz("Offline: RTC+ANN Aktif", false);
    eskiWifiDurumu = false;
    delay(2000);
  }

  // ── EKRAN 5: BLE bilgileri ──
  bleKurulum();

  display.clearDisplay();
  display.setTextSize(1); display.setCursor(0, 0); display.print("[ 3/3 ] Bluetooth BLE");
  display.drawFastHLine(0, 10, 128, SSD1306_WHITE);
  display.setCursor(0, 14); display.print("AKTIF! [OK]");
  display.setCursor(0, 26); display.print("Cihaz Adi: SPARK-S3");
  display.setCursor(0, 37); display.print("Mod UUID:");
  display.setCursor(0, 47); display.print("..789abd");
  display.setCursor(0, 57); display.print("Durum: Kesfedilebilir");
  display.display();
  delay(2000);

  // ── EKRAN 6: Özet / Hazır ──
  display.clearDisplay();
  display.setTextSize(1); display.setCursor(20, 0); display.print("SISTEM HAZIR");
  display.drawFastHLine(0, 10, 128, SSD1306_WHITE);
  // WiFi satırı
  display.setCursor(0, 14);
  if (WiFi.status() == WL_CONNECTED) {
    display.print("WiFi : "); display.print(WiFi.localIP().toString());
  } else {
    display.print("WiFi : OFFLINE");
  }
  // MQTT satırı
  display.setCursor(0, 24);
  display.print("MQTT : ");
  display.print(mqttClient.connected() ? "BAGLI" : "YOK");
  // BLE satırı
  display.setCursor(0, 34); display.print("BLE  : SPARK-S3");
  // Mod satırı
  display.setCursor(0, 44);
  display.print("Mod  : ");
  display.print(anaMod == 1 ? "YZ MODU" : "MANUEL");
  display.drawFastHLine(0, 54, 128, SSD1306_WHITE);
  display.setCursor(10, 57); display.print("Baslamak icin hazir!");
  display.display();
  delay(2500);

  sonHareketZamani = millis();
}

// ===================== ANA DÖNGÜ =====================
void loop() {
  bool suAnkiWifi = (WiFi.status() == WL_CONNECTED);

  if (suAnkiWifi) {
    server.handleClient();
    if (!mqttClient.connected()) {
      static unsigned long sonMqttDeneme = 0;
      if (millis() - sonMqttDeneme > 5000) { sonMqttDeneme = millis(); mqttBaglan(); }
    } else {
      mqttClient.loop();
    }
    if (!eskiWifiDurumu) {
      eepromLogYaz("Bağlantı Geri Geldi!", false);
      offlineLoglariMqttGonder();
      eskiWifiDurumu = true;
    }
  } else {
    if (eskiWifiDurumu) {
      eepromLogYaz("Kritik: İnternet Kesildi!", true);
      eskiWifiDurumu = false;
    }
  }

  // Hardware Timer
  if (saniyeTetiklendi) {
    portENTER_CRITICAL(&timerMux);
    saniyeTetiklendi = false;
    portEXIT_CRITICAL(&timerMux);

    if (aktifMod == 0 && akilliModGeriSayimAktif && anaMod == 0) {
      if (akilliModKalanSure > 0) {
        akilliModKalanSure--;
        if (akilliModKalanSure == 0) {
          akilliModGeriSayimAktif = false;
          if (!zamanlayiciAktifMi) digitalWrite(ledPin, LOW);
        }
      }
    }
  }

  int   ldrDegeri    = analogRead(ldrPin);
  int   hareketVarMi = digitalRead(pirPin);
  int   butonDurumu  = digitalRead(butonPin);
  int   alarmButon   = digitalRead(alarmButonPin);
  bool  ledDurumu    = digitalRead(ledPin);

  // ===================== ALARM BUTONU - CIFT GOREVLI =====================
  // Kural: alarm aktifse SADECE alarmi kapat, baska hicbir sey tetiklenmesin.
  // Alarm kapandiktan sonra buton FIZIKSEL OLARAK BIRAKILMADAN mod gecisi olmaz.
  static bool alarmKapatildiKoruma = false;

  if (alarmButon == LOW) {
    alarmButonBasildi = false;
    alarmKapatildiKoruma = false; // Buton birakildi, koruma kalkar
  }

  if (alarmButon == HIGH && !alarmButonBasildi) {
    delay(50);
    if (digitalRead(alarmButonPin) == HIGH) {
      alarmButonBasildi = true;

      if (alarmAktif) {
        // Alarm aktifse: SADECE alarmi kapat
        alarmAktif = false;
        digitalWrite(buzzerPin, LOW);
        digitalWrite(ledPin, LOW);
        alarmKapatildiKoruma = true; // Buton birakilana kadar mod gecisini kilitle
        Serial.println("Alarm buton ile kapatildi.");
        eepromLogYaz("Fiziksel: Alarm Kapatildi", true);

      } else if (!alarmKapatildiKoruma) {
        // Alarm yok VE bu basilma alarm kapamadan gelmiyorsa: mod gecis
        anaMod = (anaMod == 0) ? 1 : 0;
        preferences.begin("spark-data", false);
        preferences.putInt("anaMod", anaMod);
        preferences.end();
        sonHareketZamani = millis();
        modDegistirEkrani();
        if (anaMod == 1) eepromLogYaz("Buton: YZ Moduna Gecildi", false);
        else eepromLogYaz("Buton: Manuel Moda Gecildi", false);
      }
    }
  }

  // ===================== MANUEL BUTON - alt mod geçişi =====================
  if (butonDurumu == HIGH && !butonBasildi) {
    delay(50);
    if (digitalRead(butonPin) == HIGH) {
      butonBasildi = true;
      if (anaMod == 0) {
        // Sadece manuel modda alt mod değiştirir
        aktifMod++;
        if (aktifMod > 3) aktifMod = 0;
        preferences.begin("spark-data", false);
        preferences.putInt("aktifMod", aktifMod);
        preferences.end();
        if (aktifMod != 3) { alarmAktif = false; digitalWrite(buzzerPin, LOW); }
        sonHareketZamani = millis();
        modDegistirEkrani();
        eepromLogYaz("Buton: Alt Mod Değişti", false);
      }
      // YZ modunda buton tepki vermez (sadece alarm butonu ana mod değiştirir)
    }
  }
  if (butonDurumu == LOW) butonBasildi = false;

  // Hareket varsa ekran uyanık kalsın
  if (hareketVarMi == HIGH) {
    if (anaMod == 1 || aktifMod != 0) sonHareketZamani = millis();
    else if (aktifMod == 0 && ldrDegeri > karanlikEsigi) sonHareketZamani = millis();
  }

  zamanlayiciKontrol();
  if (zamanlayiciAktifMi) ledDurumu = true;

  // ===================== ÇALIŞMA MANTIĞI =====================

  if (anaMod == 1) {
    // ========== YZ MODU ==========
    // Güvenlik modu YZ'yi ezmez
    float ldrNorm  = (float)ldrDegeri / 4095.0f;
    int saat, dakika, saniye;
    ntpSaatiAl(saat, dakika, saniye);
    // Saati sinüzoidal normalize et (gece=1, gündüz=0)
    float saatNorm  = (float)((saat >= 18 || saat < 7) ? 1 : 0);
    // Alacakaranlık/şafak bölgesi için kısmi değer
    if (saat == 17) saatNorm = 0.5f;
    if (saat == 7)  saatNorm = 0.5f;
    float pirNorm   = (float)(hareketVarMi == HIGH ? 1 : 0);

    bool annLedKarar = annKarar(ldrNorm, saatNorm, pirNorm);

    // Gerçek duruma göre öğren (çevrimiçi öğrenme - 500ms'de bir)
    static unsigned long sonOgrenme = 0;
    if (millis() - sonOgrenme > 500) {
      sonOgrenme = millis();
      // Manuel referans kurallar ile öğren:
      // Karanlıksa VE (hareket var VEYA gece saati) -> led açık olmalı
      bool referansHedef = (ldrDegeri > karanlikEsigi) && (hareketVarMi == HIGH || saat >= 18 || saat < 7);
      annOgren(ldrNorm, saatNorm, pirNorm, referansHedef ? 1.0f : 0.0f);

      // Her 200 karardan sonra ağırlıkları flash'a kaydet
      if (annToplamKarar % 200 == 0 && annToplamKarar > 0) {
        annAgirliklarKaydet();
        eepromLogYaz("YZ: Ağırlıklar Flash'a Kaydedildi", false);
      }
    }

    if (!zamanlayiciAktifMi) {
      digitalWrite(ledPin, annLedKarar ? HIGH : LOW);
    }

    // YZ modunda hareket varsa ekran uyanık olsun
    sonHareketZamani = millis();

  } else {
    // ========== MANUEL MODLAR ==========
    switch (aktifMod) {
      case 0: // AKILLI MOD
        if (ldrDegeri > karanlikEsigi) {
          if (hareketVarMi == HIGH) {
            if (!zamanlayiciAktifMi) digitalWrite(ledPin, HIGH);
            if (!akilliModGeriSayimAktif) eepromLogYaz("Akıllı: LED Açıldı", false);
            akilliModGeriSayimAktif = true;
            akilliModKalanSure = 10;
          }
        } else {
          if (akilliModGeriSayimAktif) { akilliModGeriSayimAktif = false; akilliModKalanSure = 0; }
          if (!zamanlayiciAktifMi) digitalWrite(ledPin, LOW);
        }
        break;
      case 1:
        if (!zamanlayiciAktifMi) digitalWrite(ledPin, hareketVarMi == HIGH ? HIGH : LOW);
        break;
      case 2:
        if (!zamanlayiciAktifMi) digitalWrite(ledPin, ldrDegeri > karanlikEsigi ? HIGH : LOW);
        break;
      case 3:
        sonHareketZamani = millis(); // Güvenlik modunda ekran kapanmaz
        break;
    }
  }

  ledDurumu = digitalRead(ledPin);

  // ===================== EKRAN: UYKU vs NORMAL =====================
  bool uykuModuAktif = (millis() - sonHareketZamani > UYKU_SURESI) &&
                       !(anaMod == 0 && aktifMod == 3) &&
                       !akilliModGeriSayimAktif;

  if (uykuModuAktif) {
    saatEkrani();
    static unsigned long sonBleUyku = 0;
    if (millis() - sonBleUyku > 1000) {
      bleDurumGonder(ldrDegeri, hareketVarMi, aktifMod, ledDurumu);
      if (suAnkiWifi && mqttClient.connected()) {
        String json = "{\"anamod\":" + String(anaMod) + ",\"mod\":" + String(aktifMod) +
                      ",\"ldr\":" + String(ldrDegeri) + ",\"pir\":" + String(hareketVarMi) +
                      ",\"led\":" + String(ledDurumu ? "true" : "false") +
                      ",\"alarm\":" + String(alarmAktif ? "true" : "false") +
                      ",\"ann\":" + annDurumString() + "}";
        mqttClient.publish(topicDurum, json.c_str());
      }
      sonBleUyku = millis();
    }
    delay(30);
    return;
  }

  // ===================== NORMAL EKRAN =====================
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);

  // Başlık
  if (anaMod == 1) {
    display.setCursor(10, 0); display.print(">> YZ MODU AKTİF <<");
  } else {
    display.setCursor(10, 0); display.print("SPARK MULTI-MODE");
  }
  display.drawFastHLine(0, 10, 128, SSD1306_WHITE);

  // Bağlantı ikonları
  display.setCursor(74, 0);
  display.print(suAnkiWifi ? "W+" : "W-");
  display.print(bleConnected ? " B+" : " B-");
  display.print(mqttClient.connected() ? " M+" : " M-");

  if (zamanlayiciAktifMi) { display.setCursor(0, 0); display.print("[ZAM]"); }

  // Mod içeriği
  if (anaMod == 1) {
    // YZ MODU EKRANI
    display.setCursor(0, 15); display.print("LDR:"); display.print(ldrDegeri);
    display.print(" PIR:"); display.print(hareketVarMi);
    display.setCursor(0, 25); display.print("YZ Kararı: ");
    display.setTextSize(2);
    display.setCursor(60, 20);
    display.print(annSonKarar ? "ACIK" : "KAPALI");
    display.setTextSize(1);
    display.setCursor(0, 40); display.print("Guven: ");
    display.print((int)(annSonCikis * 100));
    display.print("% Dogr: ");
    int acc = (annToplamKarar > 0) ? (annDogruKarar * 100 / annToplamKarar) : 0;
    display.print(acc); display.print("%");
  } else if (aktifMod == 0) {
    display.setCursor(0, 15); display.print("Mod[0]: AKILLI MOD");
    if (akilliModGeriSayimAktif) {
      display.setTextSize(2); display.setCursor(0, 26); display.print("HAREKET!");
      display.setTextSize(1); display.setCursor(0, 45);
      display.print("Kalan: "); display.setTextSize(2); display.print(akilliModKalanSure); display.print("s");
    } else {
      display.setCursor(0, 32); display.print(ldrDegeri > karanlikEsigi ? "Gece/Taranıyor" : "Gündüz/Kapalı");
    }
  } else if (aktifMod == 1) {
    display.setCursor(0, 15); display.print("Mod[1]: HAREKET");
    display.setTextSize(2); display.setCursor(0, 28); display.print(hareketVarMi == HIGH ? "NESNE VAR!" : "SAKIN");
  } else if (aktifMod == 2) {
    display.setCursor(0, 15); display.print("Mod[2]: GECE/LDR");
    display.setTextSize(2); display.setCursor(0, 28); display.print(ldrDegeri > karanlikEsigi ? "GECE MODU" : "GÜNDÜZ");
  } else if (aktifMod == 3) {
    guvenlikModuIsle(ldrDegeri, hareketVarMi);
  }

  // Alt çubuk
  display.setTextSize(1); display.drawFastHLine(0, 54, 128, SSD1306_WHITE);
  display.setCursor(0, 56); display.print("LDR:"); display.print(ldrDegeri);
  display.setCursor(55, 56); display.print("PIR:"); display.print(hareketVarMi);
  int s, d, sn; ntpSaatiAl(s, d, sn);
  display.setCursor(90, 56);
  char buf[6]; sprintf(buf, "%02d:%02d", s, d); display.print(buf);

  display.display();

  // BLE + MQTT rutin gönderim
  static unsigned long sonBle = 0;
  if (millis() - sonBle > 500) {
    bleDurumGonder(ldrDegeri, hareketVarMi, aktifMod, ledDurumu);
    if (suAnkiWifi && mqttClient.connected()) {
      String json = "{\"anamod\":" + String(anaMod) + ",\"mod\":" + String(aktifMod) +
                    ",\"ldr\":" + String(ldrDegeri) + ",\"pir\":" + String(hareketVarMi) +
                    ",\"led\":" + String(ledDurumu ? "true" : "false") +
                    ",\"alarm\":" + String(alarmAktif ? "true" : "false") +
                    ",\"ann\":" + annDurumString() + "}";
      mqttClient.publish(topicDurum, json.c_str());
    }
    sonBle = millis();
  }

  delay(20);
}
