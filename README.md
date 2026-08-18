# SPARK S3 — Neural Lighting & Security Automation System

ESP32-S3 tabanlı, hareket ve karanlık sensörlerini kullanarak evinizin aydınlatmasını ve güvenliğini otomatik yöneten akıllı ev sistemi. Sistem, kural tabanlı bir **Manuel Mod** ile kullanıcı alışkanlıklarını öğrenen bir **Yapay Sinir Ağı (ANN) Modu** arasında geçiş yapabilir. Uzaktan izleme ve kontrol; MQTT, Bluetooth (BLE) ve yerel ağ üzerinden web arayüzü ile mümkündür.

## İçindekiler

- [Genel Bakış](#genel-bakış)
- [Proje Yapısı](#proje-yapısı)
- [Çalışma Modları](#çalışma-modları)
- [Yapay Sinir Ağı (ANN) Detayı](#yapay-sinir-ağı-ann-detayı)
- [Donanım ve Pin Bağlantıları](#donanım-ve-pin-bağlantıları)
- [Uzaktan Kontrol Yöntemleri](#uzaktan-kontrol-yöntemleri)
  - [Web Arayüzü ve REST API](#web-arayüzü-ve-rest-api)
  - [MQTT](#mqtt)
  - [Bluetooth (BLE)](#bluetooth-ble)
- [Kalıcı Hafıza ve Log Sistemi](#kalıcı-hafıza-ve-log-sistemi)
- [Kurulum](#kurulum)
  - [Firmware (ESP32-S3)](#firmware-esp32-s3)
  - [Mobil Uygulama (Flutter)](#mobil-uygulama-flutter)
- [Güvenlik Notu (Wi-Fi Bilgileri)](#güvenlik-notu-wi-fi-bilgileri)
- [Yol Haritası](#yol-haritası)

## Genel Bakış

Sparkes, ESP32-S3 mikrodenetleyicisi üzerinde çalışan bir Arduino (C++) firmware'i ve buna eşlik eden bir Flutter mobil uygulamasından oluşur. Cihaz üzerinde 128x64 SSD1306 OLED ekran, canlı sensör durumu, bağlantı ikonları (WiFi/BLE/MQTT) ve mod bilgisini gösterir. Sistem tamamen offline (internetsiz) çalışabilir; RTC ve EEPROM tabanlı log sistemi bağlantı olmasa da işlevini sürdürür.

## Proje Yapısı

```
Sparkes/
├── firmware/               # ESP32-S3 için Arduino (C++) kodu
│   ├── sparkesp_ar.ino
│   └── secrets.h           # Wi-Fi bilgileri (repoya dahil değil)
├── app/                     # Flutter mobil/web uygulaması
│   ├── lib/
│   ├── android/
│   ├── ios/
│   └── ...
└── README.md
```

## Çalışma Modları

Sistemde iki ana mod bulunur ve `spark/anamod` topic'i / web arayüzündeki toggle / fiziksel alarm butonu ile aralarında geçiş yapılabilir.

### 1. Manuel Mod (`anaMod = 0`)

4 alt mod bulunur, alt moda geçiş fiziksel butonla, web arayüzünden veya MQTT (`spark/mod`) ile yapılır:

| Alt Mod | İsim | Davranış |
|---|---|---|
| 0 | **Akıllı Mod** | Ortam karanlıksa **ve** hareket algılanırsa LED açılır, hareket kesildikten ~10 saniye sonra söner (hardware timer ile geri sayım) |
| 1 | **Hareket** | Sadece PIR sensörüne göre çalışır — hareket varsa LED açık, yoksa kapalı |
| 2 | **Gece / LDR** | Sadece ortam ışığına göre çalışır — karanlıksa LED açık |
| 3 | **Güvenlik** | Hareket algılanırsa **anında** buzzer alarmı çalar, LED yanıp söner, MQTT üzerinden `spark/alarm` topic'ine anlık bildirim gönderilir |

> Zamanlayıcı (`spark/zamanlayici`) aktifse, seçili moddan bağımsız olarak LED belirlenen saat aralığında açık tutulur.

### 2. Yapay Sinir Ağı Modu (`anaMod = 1`)

Cihaz üzerinde çalışan küçük bir sinir ağı; LDR (ışık), saat dilimi ve PIR (hareket) verilerini girdi olarak alıp LED'i açıp açmama kararını kendisi verir. Model, referans kural setine (karanlık + hareket/gece saatiyse LED açık olmalı) göre **çevrimiçi (online) öğrenme** ile sürekli kendini günceller ve ağırlıklarını periyodik olarak flash hafızaya kaydeder.

## Yapay Sinir Ağı (ANN) Detayı

- **Mimari:** 3 giriş → 4 gizli nöron → 1 çıkış (tam bağlantılı, sigmoid aktivasyon)
- **Girişler:**
  - LDR değeri (0–4095 aralığından 0–1'e normalize)
  - Saat dilimi (gece 18:00–07:00 arası → 1, gündüz → 0, alacakaranlık/şafak saatlerinde 0.5)
  - PIR hareket durumu (0 veya 1)
- **Çıkış:** LED açık/kapalı kararı (0.5 eşik değeri)
- **Öğrenme yöntemi:** Online Gradient Descent / Backpropagation, öğrenme hızı `0.05`
- **Öğrenme sıklığı:** Her 500 ms'de bir referans kurala göre güncellenir
- **Kalıcılık:** Ağırlıklar her 200 karardan sonra `Preferences` (flash) içine kaydedilir, cihaz yeniden başladığında otomatik yüklenir
- **İzlenebilirlik:** Anlık çıkış değeri, karar, toplam karar sayısı ve doğruluk oranı (`annDurumString()`) hem web arayüzünde hem BLE/MQTT durum mesajlarında yayınlanır

## Donanım ve Pin Bağlantıları

| Bileşen | Pin | Açıklama |
|---|---|---|
| LDR (ışık sensörü) | GPIO 1 | Analog okuma, karanlık eşiği: `2500` |
| PIR (hareket sensörü) | GPIO 2 | Dijital giriş |
| LED / Röle çıkışı | GPIO 4 | Aydınlatma kontrolü |
| Mod değiştirme butonu | GPIO 5 | Manuel modda alt mod döngüsü (INPUT_PULLDOWN) |
| Alarm / Ana Mod butonu | GPIO 10 | Alarmı kapatır veya Manuel↔YZ arası geçiş yapar (INPUT_PULLDOWN) |
| Buzzer | GPIO 15 | Güvenlik modu alarmı |
| OLED Ekran (SSD1306) | I2C (adres `0x3C`) | 128x64 çözünürlük |

**Alarm butonunun çift işlevi:** Alarm aktifken buton **sadece alarmı susturur**; alarm aktif değilken (ve az önce susturulmamışsa) Manuel/YZ mod geçişini tetikler. Bu iki davranış fiziksel olarak birbirine karışmaması için bir "koruma" bayrağı ile ayrıştırılmıştır.

## Uzaktan Kontrol Yöntemleri

Sistem 3 farklı şekilde uzaktan izlenebilir ve yönetilebilir: **web arayüzü** (yerel ağ), **MQTT** (bulut/broker üzerinden) ve **Bluetooth BLE** (yakın mesafe).

### Web Arayüzü ve REST API

ESP32 üzerinde port `80`'de çalışan bir web sunucusu, cihazın yerel IP adresi üzerinden tam özellikli bir kontrol paneli sunar: canlı sensör verisi, ANN sinir ağı görselleştirmesi, mod seçimi, zamanlayıcı ayarı ve EEPROM log geçmişi.

| Endpoint | Method | Açıklama |
|---|---|---|
| `/` | GET | Ana kontrol paneli (HTML arayüz) |
| `/durum` | GET | Anlık sensör/mod/bağlantı durumu (JSON) |
| `/logs` | GET | EEPROM'daki geçmiş kayıtlar (JSON) |
| `/mod` | POST | `deger` (0–3) — manuel alt mod değiştirir |
| `/anamod` | POST | `deger` (0/1) — Manuel/YZ modu değiştirir |
| `/alarm/kapat` | POST | Aktif alarmı uzaktan susturur |
| `/zamanlayici` | POST | `aralik` (örn. `20:00-23:30`) — aydınlatma zamanlayıcısı kurar |
| `/zamanlayici/iptal` | POST | Aktif zamanlayıcıyı iptal eder |

### MQTT

**Broker:** `broker.hivemq.com` (public) · **Port:** `1883` · **Client ID:** `SPARK-S3-ESP32`

| Topic | Yön | Açıklama |
|---|---|---|
| `spark/durum` | Yayın (publish) | Periyodik durum mesajı (sensörler, mod, ANN durumu) |
| `spark/mod` | Abonelik (subscribe) | Manuel alt mod değiştirme (0–3) |
| `spark/anamod` | Abonelik | Manuel/YZ modu arası geçiş (`1`/`yz` → YZ, diğer → Manuel) |
| `spark/alarm` | Yayın | Güvenlik alarmı tetiklendiğinde anlık bildirim |
| `spark/alarm/kapat` | Abonelik | Alarmı uzaktan susturma (`1` veya `kapat`) |
| `spark/zamanlayici` | Abonelik | Zamanlayıcı ayarlama (format: `HH:MM-HH:MM`) |
| `spark/logs/gecmis` | Yayın | Bağlantı geri geldiğinde, offline kalan loglar toplu gönderilir |

### Bluetooth (BLE)

Cihaz adı: **`SPARK-S3`**

| UUID Türü | Değer | Özellik |
|---|---|---|
| Servis | `12345678-1234-1234-1234-123456789abc` | — |
| Mod Karakteristiği | `12345678-1234-1234-1234-123456789abd` | `WRITE` — `"0"`–`"3"` alt mod, `"M"` Manuel, `"Y"` YZ modu |
| Durum Karakteristiği | `12345678-1234-1234-1234-123456789abe` | `READ` / `NOTIFY` — anlık JSON durum verisi |

## Kalıcı Hafıza ve Log Sistemi

Sistem, internet bağlantısı olmasa bile çalışmaya devam eder:

- Aktif mod, ana mod, zamanlayıcı ayarları ve ANN ağırlıkları `Preferences` (flash) içinde saklanır, yeniden başlatmada geri yüklenir.
- Son 15 olay (`MAX_LOGS = 15`) dönen bir tampon (ring buffer) mantığıyla flash'a yazılır; kritik olaylar (güvenlik ihlali, bağlantı kesilmesi vb.) öncelikli slotlara kaydedilir ve normal loglar tarafından ezilmez.
- Wi-Fi bağlantısı geri geldiğinde, offline biriken tüm loglar otomatik olarak `spark/logs/gecmis` topic'i üzerinden MQTT'ye aktarılır.
- Zaman bilgisi için NTP (`pool.ntp.org`) senkronizasyonu kullanılır; bağlantı yoksa dahili RTC varsayılan bir tarihle (01.01.2026) çalışmaya devam eder.

## Kurulum

### Firmware (ESP32-S3)

1. Arduino IDE'yi kurun ve ESP32-S3 board desteğini ekleyin.
2. Gerekli kütüphaneleri yükleyin: `Adafruit_GFX`, `Adafruit_SSD1306`, `WiFi`, `WebServer`, `BLEDevice`, `PubSubClient`, `ESP32Time`, `Preferences`.
3. `firmware/` klasöründe `secrets.h` adlı bir dosya oluşturun (bkz. [Güvenlik Notu](#güvenlik-notu-wi-fi-bilgileri)).
4. `sparkesp_ar.ino` dosyasını ESP32-S3 kartınıza yükleyin.
5. Seri monitörden (115200 baud) IP adresini takip edin veya OLED ekrandaki bağlantı bilgilerini kontrol edin.

### Mobil Uygulama (Flutter)

```bash
cd app
flutter pub get
flutter run
```

## Güvenlik Notu (Wi-Fi Bilgileri)

Wi-Fi kimlik bilgileri kaynak koddan ayrılarak `firmware/secrets.h` dosyasında tutulur. Bu dosya `.gitignore` ile repoya dahil edilmez, bu yüzden repoyu klonladıktan sonra kendi bilgilerinizle bu dosyayı elle oluşturmanız gerekir:

```cpp
#ifndef SECRETS_H
#define SECRETS_H

const char* ssid = "WIFI_ADINIZ";
const char* password = "WIFI_SIFRENIZ";

#endif
```

