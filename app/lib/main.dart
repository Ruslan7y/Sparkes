// ============================================================
//  SPARK Controller - Flutter
//  flutter_blue_plus: ^1.31.15 | http: ^1.2.0 | mqtt_client: ^10.3.0
// ============================================================

import 'dart:async';
import 'dart:convert';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:http/http.dart' as http;
import 'package:web_socket_channel/web_socket_channel.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  SystemChrome.setPreferredOrientations([DeviceOrientation.portraitUp]);
  runApp(const SparkApp());
}

class SparkApp extends StatelessWidget {
  const SparkApp({super.key});
  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'SPARK Controller',
      debugShowCheckedModeBanner: false,
      theme: ThemeData.dark().copyWith(
        colorScheme: const ColorScheme.dark(
          primary: Color(0xFF00E5FF),
          secondary: Color(0xFFFF6D00),
          surface: Color(0xFF0D1117),
        ),
        scaffoldBackgroundColor: const Color(0xFF0D1117),
      ),
      home: const SparkHomePage(),
    );
  }
}

class SparkHomePage extends StatefulWidget {
  const SparkHomePage({super.key});
  @override
  State<SparkHomePage> createState() => _SparkHomePageState();
}

class _SparkHomePageState extends State<SparkHomePage>
    with TickerProviderStateMixin {

  // ============== BAGLANTI ==============
  bool   wifiConnected  = false;
  bool   bleConnected   = false;
  bool   mqttConnected  = false;
  bool   mqttBaglaniyor = false;
  String wifiIp         = '';
  String baglanti       = 'Baglanti yok';

  // ============== MOD ==============
  int anaMod   = 0;
  int aktifMod = 0;

  // ============== SENSOR ==============
  int  ldrDegeri = 0;
  int  pirDegeri = 0;
  bool ledAcik   = false;

  // ============== ALARM ==============
  bool alarmAktif = false;

  // ============== ZAMANLAYICI ==============
  bool   zamanlayiciAktif = false;
  String zamBas           = '--:--';
  String zamBit           = '--:--';
  final TextEditingController _zamController = TextEditingController();

  // ============== SAAT ==============
  int saat   = -1;
  int dakika = -1;

  // ============== YZ ==============
  double annCikis    = 0.0;
  bool   annKarar    = false;
  int    annDogruluk = 0;
  int    annToplam   = 0;

  // ============== BLE ==============
  BluetoothDevice?         bleDevice;
  BluetoothCharacteristic? modCharacteristic;
  BluetoothCharacteristic? durumCharacteristic;
  StreamSubscription?      durumSubscription;
  StreamSubscription?      scanSubscription;
  StreamSubscription?      connectionSubscription;
  bool                     bleAraniyor = false;

  // ============== WiFi ==============
  Timer? wifiTimer;
  final TextEditingController _ipController =
  TextEditingController(text: '192.168.1.');

  // ============== MQTT ==============
  // DÜZELTİLDİ: Tek broker tanımı — ESP32 ile aynı broker kullanılıyor
  WebSocketChannel? _wsChannel;
  StreamSubscription? _wsSubscription;
  static const String mqttBroker    = 'broker.emqx.io'; // EMQX broker
  static const int    mqttPort      = 1883;
  static const String topicDurum    = 'spark/durum';
  static const String topicMod      = 'spark/mod';
  static const String topicAnaMod   = 'spark/anamod';
  static const String topicAlarmKap = 'spark/alarm/kapat';
  static const String topicZaman    = 'spark/zamanlayici';

  // ============== DEBUG LOG ==============
  final List<String> _debugLogs = [];
  bool _debugGoster = false;

  void _log(String msg) {
    final zaman = DateTime.now().toIso8601String().substring(11, 19);
    final satir = '[$zaman] $msg';
    debugPrint('SPARK DEBUG: $satir');
    if (mounted) {
      setState(() {
        _debugLogs.insert(0, satir);
        if (_debugLogs.length > 100) _debugLogs.removeLast();
      });
    }
  }

  // ============== ANIMASYON ==============
  late AnimationController _pulseController;
  late Animation<double>   _pulseAnim;
  late AnimationController _alarmController;
  late Animation<double>   _alarmAnim;

  @override
  void initState() {
    super.initState();
    _pulseController = AnimationController(
      vsync: this, duration: const Duration(seconds: 1),
    )..repeat(reverse: true);
    _pulseAnim = Tween<double>(begin: 0.85, end: 1.0).animate(
        CurvedAnimation(parent: _pulseController, curve: Curves.easeInOut));

    _alarmController = AnimationController(
      vsync: this, duration: const Duration(milliseconds: 600),
    )..repeat(reverse: true);
    _alarmAnim = Tween<double>(begin: 0.4, end: 1.0).animate(
        CurvedAnimation(parent: _alarmController, curve: Curves.easeInOut));
  }

  @override
  void dispose() {
    _pulseController.dispose();
    _alarmController.dispose();
    wifiTimer?.cancel();
    durumSubscription?.cancel();
    scanSubscription?.cancel();
    connectionSubscription?.cancel();
    bleDevice?.disconnect();
    _wsSubscription?.cancel();
    _wsChannel?.sink.close();
    _ipController.dispose();
    _zamController.dispose();
    super.dispose();
  }

  // =================== WIFI ===================

  Future<void> wifiBaglan() async {
    final ip = _ipController.text.trim();
    if (ip.isEmpty) return;
    setState(() => wifiIp = ip);
    _log('WiFi bağlanılıyor: $ip');
    try {
      final res = await http.get(Uri.parse('http://$ip/durum'))
          .timeout(const Duration(seconds: 3));
      if (res.statusCode == 200) {
        _log('WiFi bağlandı. Yanıt: ${res.body}');
        _jsonIsle(res.body);
        setState(() { wifiConnected = true; baglanti = 'WiFi ($ip)'; });
        wifiTimer = Timer.periodic(
            const Duration(seconds: 2), (_) => _wifiDurumGetir());
      }
    } catch (e) {
      _log('WiFi hatası: $e');
      _mesaj('WiFi hatasi: $e');
    }
  }

  Future<void> _wifiDurumGetir() async {
    if (!wifiConnected || wifiIp.isEmpty) return;
    try {
      final res = await http.get(Uri.parse('http://$wifiIp/durum'))
          .timeout(const Duration(seconds: 2));
      if (res.statusCode == 200) _jsonIsle(res.body);
    } catch (_) {
      _log('WiFi bağlantısı koptu');
      setState(() { wifiConnected = false; baglanti = 'WiFi koptu'; });
      wifiTimer?.cancel();
    }
  }

  Future<void> _wifiPost(String yol) async {
    if (!wifiConnected) return;
    try {
      await http.post(Uri.parse('http://$wifiIp$yol'))
          .timeout(const Duration(seconds: 2));
    } catch (e) { _mesaj('WiFi hatasi: $e'); }
  }

  Future<void> wifiModGonder(int mod) async {
    await _wifiPost('/mod?deger=$mod');
    setState(() { aktifMod = mod; anaMod = 0; });
  }

  Future<void> wifiAnaModGonder(int yeniAnaMod) async {
    await _wifiPost('/anamod?deger=$yeniAnaMod');
    setState(() => anaMod = yeniAnaMod);
  }

  Future<void> wifiAlarmKapat() async {
    await _wifiPost('/alarm/kapat');
    setState(() => alarmAktif = false);
  }

  Future<void> wifiZamanlayiciKur(String aralik) async {
    await _wifiPost('/zamanlayici?aralik=$aralik');
  }

  Future<void> wifiZamanlayiciIptal() async {
    await _wifiPost('/zamanlayici/iptal');
    setState(() { zamanlayiciAktif = false; zamBas = '--:--'; zamBit = '--:--'; });
  }

  // =================== BLE ===================

  Future<void> bleTara() async {
    if (bleAraniyor) return;
    setState(() => bleAraniyor = true);
    _log('BLE tarama başlıyor...');
    await FlutterBluePlus.stopScan();
    try {
      await FlutterBluePlus.startScan(
        withNames: ['SPARK-S3'],
        timeout: const Duration(seconds: 8),
        androidUsesFineLocation: false,
      );
      scanSubscription?.cancel();
      scanSubscription = FlutterBluePlus.scanResults.listen((results) async {
        for (final r in results) {
          if (r.device.platformName == 'SPARK-S3') {
            _log('BLE cihaz bulundu: ${r.device.platformName}');
            await FlutterBluePlus.stopScan();
            scanSubscription?.cancel();
            await _bleBaglan(r.device);
            break;
          }
        }
      });
      await Future.delayed(const Duration(seconds: 8));
    } catch (e) {
      _log('BLE tarama hatası: $e');
      _mesaj('BLE tarama hatasi: $e');
    }
    finally { if (mounted) setState(() => bleAraniyor = false); }
  }

  Future<void> _bleBaglan(BluetoothDevice device) async {
    try {
      await device.connect(timeout: const Duration(seconds: 10));
      if (!mounted) return;
      _log('BLE bağlandı: ${device.platformName}');
      setState(() {
        bleDevice    = device;
        bleConnected = true;
        baglanti     = 'BLE (SPARK-S3)';
      });

      connectionSubscription?.cancel();
      connectionSubscription = device.connectionState.listen((state) {
        if (state == BluetoothConnectionState.disconnected && mounted) {
          _log('BLE bağlantısı koptu');
          setState(() {
            bleConnected = false;
            baglanti     = 'BLE koptu';
            bleDevice    = null;
          });
          durumSubscription?.cancel();
        }
      });

      final services = await device.discoverServices();
      for (final s in services) {
        if (s.uuid.toString().toLowerCase().contains('12345678-1234-1234-1234-123456789abc')) {
          for (final c in s.characteristics) {
            final u = c.uuid.toString().toLowerCase();
            if (u.contains('123456789abd')) modCharacteristic   = c;
            if (u.contains('123456789abe')) durumCharacteristic = c;
          }
        }
      }
      if (durumCharacteristic != null) {
        await durumCharacteristic!.setNotifyValue(true);
        durumSubscription?.cancel();
        durumSubscription = durumCharacteristic!.onValueReceived.listen(
                (val) {
              final json = String.fromCharCodes(val);
              _log('BLE veri alındı: $json');
              _jsonIsle(json);
            });
      }
    } catch (e) {
      _log('BLE bağlantı hatası: $e');
      _mesaj('BLE baglanti hatasi: $e');
      try { await device.disconnect(); } catch (_) {}
    }
  }

  Future<void> _bleKomutGonder(String komut) async {
    if (!bleConnected || modCharacteristic == null) return;
    try {
      await modCharacteristic!.write(utf8.encode(komut), withoutResponse: false);
      _log('BLE komut gönderildi: $komut');
    } catch (e) {
      _log('BLE komut gönderilemedi: $e');
      _mesaj('BLE komut gonderilemedi: $e');
    }
  }

  Future<void> bleModGonder(int mod) async {
    await _bleKomutGonder(mod.toString());
    setState(() { aktifMod = mod; anaMod = 0; });
  }

  Future<void> bleAnaModGonder(int yeniAnaMod) async {
    await _bleKomutGonder(yeniAnaMod == 1 ? 'Y' : 'M');
    setState(() => anaMod = yeniAnaMod);
  }

  Future<void> bleAlarmKapat() async {
    await _bleKomutGonder('A');
    setState(() => alarmAktif = false);
  }

  Future<void> bleZamanlayiciKur(String aralik) async {
    await _bleKomutGonder('Z$aralik');
  }

  Future<void> bleZamanlayiciIptal() async {
    await _bleKomutGonder('I');
    setState(() { zamanlayiciAktif = false; zamBas = '--:--'; zamBit = '--:--'; });
  }

  // =================== MQTT ===================

  Future<void> mqttBaglan() async {
    if (mqttBaglaniyor || mqttConnected) return;
    setState(() => mqttBaglaniyor = true);
    _log('MQTT (WebSocket) bağlanılıyor... Broker: broker.emqx.io:8083');

    for (int deneme = 1; deneme <= 3; deneme++) {
      try {
        _log('Bağlantı denemesi $deneme/3...');
        _wsChannel = WebSocketChannel.connect(
          Uri.parse('ws://broker.emqx.io:8083/mqtt'),
        );

        // MQTT CONNECT paketi gönder
        final clientId = 'spark_flutter_${DateTime.now().millisecondsSinceEpoch}';
        _wsMqttConnect(clientId);

        // Yanıt bekle
        bool connected = false;
        await for (final data in _wsChannel!.stream.timeout(
          const Duration(seconds: 10),
          onTimeout: (sink) => sink.close(),
        )) {
          if (data is List<int> && data.length >= 4 && data[0] == 0x20) {
            // CONNACK paketi — 0x20 = MQTT CONNACK
            if (data[3] == 0x00) {
              connected = true;
              _log('WebSocket MQTT bağlandı!');
              _mqttBaglandi();
              // SUBSCRIBE gönder
              _wsMqttSubscribe('spark/durum');
              // Dinlemeye devam et
              _wsSubscription = _wsChannel!.stream.listen(
                _wsVeriIsle,
                onDone: () {
                  _log('WS bağlantısı kapandı');
                  _mqttKoptu();
                },
                onError: (e) {
                  _log('WS hata: $e');
                  _mqttKoptu();
                },
              );
            } else {
              _log('CONNACK red kodu: \${data[3]}');
            }
            break;
          }
        }

        if (connected) break;
        throw Exception('CONNACK alınamadı');

      } catch (e) {
        _log('Deneme $deneme hatası: $e');
        _wsChannel?.sink.close();
        _wsChannel = null;
        if (deneme < 3) {
          await Future.delayed(const Duration(seconds: 2));
        } else {
          _mesaj('MQTT 3 denemede de bağlanamadı.');
        }
      }
    }

    if (mounted) setState(() => mqttBaglaniyor = false);
  }

  // Ham MQTT CONNECT paketi oluştur
  void _wsMqttConnect(String clientId) {
    final idBytes = clientId.codeUnits;
    final packet = <int>[
      0x10, // CONNECT
      idBytes.length + 12, // remaining length
      0x00, 0x04, 0x4D, 0x51, 0x54, 0x54, // Protocol "MQTT"
      0x04, // Protocol level 4
      0x02, // Connect flags: clean session
      0x00, 0x3C, // Keep alive: 60s
      0x00, idBytes.length, ...idBytes, // Client ID
    ];
    _wsChannel?.sink.add(packet);
  }

  // Ham MQTT SUBSCRIBE paketi
  void _wsMqttSubscribe(String topic) {
    final topicBytes = topic.codeUnits;
    final packet = <int>[
      0x82, // SUBSCRIBE
      topicBytes.length + 5,
      0x00, 0x01, // Packet ID
      0x00, topicBytes.length, ...topicBytes,
      0x00, // QoS 0
    ];
    _wsChannel?.sink.add(packet);
    _log('Subscribe gönderildi: $topic');
  }

  // Ham MQTT PUBLISH paketi gönder
  void _wsMqttPublish(String topic, String payload) {
    final topicBytes = topic.codeUnits;
    final payloadBytes = payload.codeUnits;
    final remaining = 2 + topicBytes.length + payloadBytes.length;
    final packet = <int>[
      0x30, // PUBLISH QoS 0
      remaining,
      0x00, topicBytes.length, ...topicBytes,
      ...payloadBytes,
    ];
    _wsChannel?.sink.add(packet);
  }

  // Gelen WebSocket verisini işle
  void _wsVeriIsle(dynamic data) {
    if (data is! List<int>) return;
    if (data.isEmpty) return;

    final tip = data[0] & 0xF0;
    if (tip == 0x30) {
      // PUBLISH paketi
      try {
        int idx = 1;
        // Remaining length
        int remaining = 0;
        int multiplier = 1;
        while (true) {
          final byte = data[idx++];
          remaining += (byte & 0x7F) * multiplier;
          multiplier *= 128;
          if ((byte & 0x80) == 0) break;
        }
        // Topic
        final topicLen = (data[idx] << 8) | data[idx + 1];
        idx += 2;
        final topic = String.fromCharCodes(data.sublist(idx, idx + topicLen));
        idx += topicLen;
        // Payload
        final payload = String.fromCharCodes(data.sublist(idx));
        _log('WS mesaj | Topic: $topic | Payload: $payload');
        if (topic == 'spark/durum') _jsonIsle(payload);
      } catch (e) {
        _log('WS parse hatası: $e');
      }
    } else if (tip == 0xD0) {
      // PINGRESP — bağlantı canlı
    }
  }


  void _mqttBaglandi() {
    _log('MQTT _mqttBaglandi callback tetiklendi');
    if (mounted) setState(() { mqttConnected = true; baglanti = 'MQTT (Internet)'; });
  }

  void _mqttKoptu() {
    _log('MQTT bağlantısı koptu');
    if (mounted) setState(() { mqttConnected = false; baglanti = 'MQTT koptu'; });
  }

  void _mqttYayinla(String topic, String mesaj) {
    if (!mqttConnected || _wsChannel == null) {
      _log('MQTT yayın başarısız: bağlı değil');
      return;
    }
    _wsMqttPublish(topic, mesaj);
    _log('MQTT yayınlandı | Topic: $topic | Mesaj: $mesaj');
  }

  void mqttModGonder(int mod) {
    _mqttYayinla(topicMod, mod.toString());
    setState(() { aktifMod = mod; anaMod = 0; });
  }

  void mqttAnaModGonder(int yeniAnaMod) {
    _mqttYayinla(topicAnaMod, yeniAnaMod.toString());
    setState(() => anaMod = yeniAnaMod);
  }

  void mqttAlarmKapat() {
    _mqttYayinla(topicAlarmKap, '1');
    setState(() => alarmAktif = false);
  }

  void mqttZamanlayiciKur(String aralik) {
    _mqttYayinla(topicZaman, aralik);
  }

  void mqttKes() {
    _wsSubscription?.cancel();
    _wsChannel?.sink.close();
    _wsChannel = null;
    _log('MQTT manuel olarak kesildi');
    setState(() { mqttConnected = false; baglanti = 'Baglanti yok'; });
  }

  // =================== DISPATCHER ===================

  void modDegistir(int mod) {
    if (anaMod == 1) { _mesaj('Alt mod degistirmek icin once MANUEL moda gecin'); return; }
    if (wifiConnected)      wifiModGonder(mod);
    else if (bleConnected)  bleModGonder(mod);
    else if (mqttConnected) mqttModGonder(mod);
    else _mesaj('Once bir baglanti kurun!');
  }

  void anaModDegistir(int yeniAnaMod) {
    if (wifiConnected)      wifiAnaModGonder(yeniAnaMod);
    else if (bleConnected)  bleAnaModGonder(yeniAnaMod);
    else if (mqttConnected) mqttAnaModGonder(yeniAnaMod);
    else _mesaj('Once bir baglanti kurun!');
  }

  void alarmKapatGonder() {
    if (wifiConnected)      wifiAlarmKapat();
    else if (bleConnected)  bleAlarmKapat();
    else if (mqttConnected) mqttAlarmKapat();
    else _mesaj('Once bir baglanti kurun!');
  }

  void zamanlayiciKur() {
    final aralik = _zamController.text.trim();
    final regex = RegExp(r'^([01]\d|2[0-3]):[0-5]\d-([01]\d|2[0-3]):[0-5]\d$');
    if (!regex.hasMatch(aralik)) { _mesaj('Format hatali! HH:MM-HH:MM (orn: 20:00-23:30)'); return; }
    if (wifiConnected)      wifiZamanlayiciKur(aralik);
    else if (bleConnected)  bleZamanlayiciKur(aralik);
    else if (mqttConnected) mqttZamanlayiciKur(aralik);
    else { _mesaj('Once bir baglanti kurun!'); return; }
    _mesaj('Zamanlayici kuruldu: $aralik');
  }

  void zamanlayiciIptalEt() {
    if (wifiConnected)      wifiZamanlayiciIptal();
    else if (bleConnected)  bleZamanlayiciIptal();
    else if (mqttConnected) _mqttYayinla(topicZaman, '00:00-00:00');
    else _mesaj('Once bir baglanti kurun!');
  }

  // =================== JSON ===================

  void _jsonIsle(String jsonStr) {
    _log('JSON işleniyor: $jsonStr');
    try {
      final d = jsonDecode(jsonStr);
      setState(() {
        anaMod           = d['anamod']  ?? anaMod;
        aktifMod         = d['mod']     ?? aktifMod;
        ldrDegeri        = d['ldr']     ?? ldrDegeri;
        pirDegeri        = d['pir']     ?? pirDegeri;
        ledAcik          = d['led']     ?? ledAcik;
        alarmAktif       = d['alarm']   ?? alarmAktif;
        zamanlayiciAktif = d['zam']     ?? zamanlayiciAktif;
        saat             = d['saat']    ?? saat;
        dakika           = d['dakika']  ?? dakika;
        if (d['zamBas'] != null) zamBas = d['zamBas'];
        if (d['zamBit'] != null) zamBit = d['zamBit'];
        if (d['ann'] != null) {
          final ann = d['ann'];
          annCikis    = (ann['cikis']    ?? 0).toDouble();
          annKarar    = (ann['karar']    ?? false);
          annDogruluk = (ann['dogruluk'] ?? 0);
          annToplam   = (ann['toplam']   ?? 0);
        }
      });
      _log('JSON başarıyla işlendi: ldr=$ldrDegeri pir=$pirDegeri led=$ledAcik mod=$aktifMod');
    } catch (e) {
      _log('JSON parse hatası: $e | Gelen: $jsonStr');
    }
  }

  void _mesaj(String msg) {
    if (!mounted) return;
    showDialog(
      context: context,
      builder: (ctx) => AlertDialog(
        backgroundColor: const Color(0xFF161B22),
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(16)),
        title: const Row(children: [
          Icon(Icons.error_outline, color: Colors.redAccent, size: 22),
          SizedBox(width: 8),
          Text('Hata', style: TextStyle(color: Colors.redAccent, fontSize: 16)),
        ]),
        content: Text(
          msg,
          style: const TextStyle(color: Colors.white70, fontSize: 13),
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(ctx).pop(),
            child: const Text('Tamam', style: TextStyle(color: Color(0xFF00E5FF))),
          ),
        ],
      ),
    );
  }

  String get saatStr {
    if (saat < 0) return '--:--';
    return '${saat.toString().padLeft(2, '0')}:${dakika.toString().padLeft(2, '0')}';
  }

  bool get herhangiBirBaglanti => wifiConnected || bleConnected || mqttConnected;

  // =================== UI ===================

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: SafeArea(
        child: SingleChildScrollView(
          padding: const EdgeInsets.all(16),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              _buildHeader(),
              const SizedBox(height: 20),
              if (alarmAktif) ...[
                _buildAlarmBanner(),
                const SizedBox(height: 12),
              ],
              _buildAnaModToggle(),
              const SizedBox(height: 16),
              _buildBaglantiKarti(),
              const SizedBox(height: 16),
              _buildDurumKarti(),
              const SizedBox(height: 16),
              if (anaMod == 1) ...[
                _buildYzPaneli(),
                const SizedBox(height: 16),
              ],
              _buildModSecici(),
              const SizedBox(height: 16),
              _buildZamanlayici(),
              const SizedBox(height: 16),
              // DEBUG PANEL
              _buildDebugPanel(),
              const SizedBox(height: 24),
            ],
          ),
        ),
      ),
    );
  }

  // =================== DEBUG PANEL ===================

  Widget _buildDebugPanel() {
    return Container(
      decoration: BoxDecoration(
        color: const Color(0xFF0D1117),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: Colors.green.withOpacity(0.4)),
      ),
      child: Column(
        children: [
          GestureDetector(
            onTap: () => setState(() => _debugGoster = !_debugGoster),
            child: Container(
              padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
              decoration: BoxDecoration(
                color: Colors.green.withOpacity(0.1),
                borderRadius: BorderRadius.vertical(
                  top: const Radius.circular(12),
                  bottom: _debugGoster ? Radius.zero : const Radius.circular(12),
                ),
              ),
              child: Row(children: [
                const Icon(Icons.bug_report, color: Colors.greenAccent, size: 16),
                const SizedBox(width: 8),
                const Text('DEBUG KONSOL',
                    style: TextStyle(fontSize: 11, letterSpacing: 2,
                        color: Colors.greenAccent, fontWeight: FontWeight.w700)),
                const Spacer(),
                Text('${_debugLogs.length} log',
                    style: TextStyle(fontSize: 10, color: Colors.grey[500])),
                const SizedBox(width: 8),
                Icon(_debugGoster ? Icons.expand_less : Icons.expand_more,
                    color: Colors.greenAccent, size: 18),
              ]),
            ),
          ),
          if (_debugGoster)
            Container(
              height: 220,
              padding: const EdgeInsets.all(10),
              child: _debugLogs.isEmpty
                  ? const Center(
                  child: Text('Henüz log yok...',
                      style: TextStyle(color: Colors.grey, fontSize: 12)))
                  : ListView.builder(
                itemCount: _debugLogs.length,
                itemBuilder: (ctx, i) => Padding(
                  padding: const EdgeInsets.symmetric(vertical: 1),
                  child: Text(
                    _debugLogs[i],
                    style: TextStyle(
                      fontSize: 10,
                      fontFamily: 'monospace',
                      color: _debugLogs[i].contains('hata') || _debugLogs[i].contains('hatası')
                          ? Colors.redAccent
                          : _debugLogs[i].contains('alındı') || _debugLogs[i].contains('OK')
                          ? Colors.greenAccent
                          : Colors.grey[400],
                    ),
                  ),
                ),
              ),
            ),
        ],
      ),
    );
  }

  Widget _buildHeader() {
    return Row(
      children: [
        ScaleTransition(
          scale: _pulseAnim,
          child: Container(
            width: 48, height: 48,
            decoration: BoxDecoration(
              shape: BoxShape.circle,
              color: const Color(0xFF00E5FF).withOpacity(0.15),
              border: Border.all(color: const Color(0xFF00E5FF), width: 2),
            ),
            child: const Icon(Icons.bolt, color: Color(0xFF00E5FF), size: 28),
          ),
        ),
        const SizedBox(width: 12),
        Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            const Text('SPARK',
                style: TextStyle(fontSize: 26, fontWeight: FontWeight.w900,
                    color: Color(0xFF00E5FF), letterSpacing: 4)),
            Text(baglanti,
                style: TextStyle(fontSize: 12,
                    color: herhangiBirBaglanti ? Colors.greenAccent : Colors.grey)),
          ],
        ),
        const Spacer(),
        Column(
          crossAxisAlignment: CrossAxisAlignment.end,
          children: [
            Text(saatStr, style: const TextStyle(
                fontSize: 22, fontWeight: FontWeight.w700,
                color: Colors.amberAccent, letterSpacing: 2)),
            Row(children: [
              _baglantiIkon(Icons.wifi,      wifiConnected),
              const SizedBox(width: 6),
              _baglantiIkon(Icons.bluetooth, bleConnected),
              const SizedBox(width: 6),
              _baglantiIkon(Icons.cloud,     mqttConnected),
            ]),
          ],
        ),
      ],
    );
  }

  Widget _buildAlarmBanner() {
    return FadeTransition(
      opacity: _alarmAnim,
      child: Container(
        padding: const EdgeInsets.all(14),
        decoration: BoxDecoration(
          color: Colors.red.withOpacity(0.15),
          borderRadius: BorderRadius.circular(14),
          border: Border.all(color: Colors.redAccent, width: 2),
        ),
        child: Row(children: [
          const Icon(Icons.warning_amber_rounded, color: Colors.redAccent, size: 30),
          const SizedBox(width: 10),
          const Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text('!! GUVENLIK ALARMI AKTIF !!',
                    style: TextStyle(fontSize: 14, fontWeight: FontWeight.w800,
                        color: Colors.redAccent, letterSpacing: 1.2)),
                Text('Hareket algilanmis. Sustarmak icin asagi.',
                    style: TextStyle(fontSize: 11, color: Colors.white70)),
              ],
            ),
          ),
          GestureDetector(
            onTap: alarmKapatGonder,
            child: Container(
              padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
              decoration: BoxDecoration(
                color: Colors.redAccent,
                borderRadius: BorderRadius.circular(8),
              ),
              child: const Row(children: [
                Icon(Icons.notifications_off, color: Colors.white, size: 16),
                SizedBox(width: 4),
                Text('KAPAT',
                    style: TextStyle(fontSize: 12, fontWeight: FontWeight.w800,
                        color: Colors.white, letterSpacing: 1)),
              ]),
            ),
          ),
        ]),
      ),
    );
  }

  Widget _buildAnaModToggle() {
    return Container(
      decoration: BoxDecoration(
        color: const Color(0xFF161B22),
        borderRadius: BorderRadius.circular(14),
        border: Border.all(color: Colors.white12),
      ),
      padding: const EdgeInsets.all(6),
      child: Row(children: [
        Expanded(child: _anaModSecimBtn(0, 'MANUEL KONTROL', Icons.tune, const Color(0xFF39FF14))),
        Expanded(child: _anaModSecimBtn(1, 'YAPAY SINIR AGI', Icons.psychology, const Color(0xFF00E5FF))),
      ]),
    );
  }

  Widget _anaModSecimBtn(int hedefAnaMod, String label, IconData icon, Color renk) {
    final secili = anaMod == hedefAnaMod;
    return GestureDetector(
      onTap: () => anaModDegistir(hedefAnaMod),
      child: AnimatedContainer(
        duration: const Duration(milliseconds: 200),
        padding: const EdgeInsets.symmetric(vertical: 14, horizontal: 8),
        decoration: BoxDecoration(
          color: secili ? renk.withOpacity(0.15) : Colors.transparent,
          borderRadius: BorderRadius.circular(10),
          border: Border.all(color: secili ? renk : Colors.transparent, width: 1.5),
        ),
        child: Row(mainAxisAlignment: MainAxisAlignment.center, children: [
          Icon(icon, color: secili ? renk : Colors.grey, size: 18),
          const SizedBox(width: 8),
          Text(label,
              style: TextStyle(fontSize: 12, fontWeight: FontWeight.w800,
                  color: secili ? renk : Colors.grey, letterSpacing: 1.2)),
        ]),
      ),
    );
  }

  Widget _baglantiIkon(IconData icon, bool aktif) => Container(
    padding: const EdgeInsets.all(6),
    decoration: BoxDecoration(
      shape: BoxShape.circle,
      color: aktif ? Colors.greenAccent.withOpacity(0.15) : Colors.grey.withOpacity(0.1),
    ),
    child: Icon(icon, size: 18, color: aktif ? Colors.greenAccent : Colors.grey),
  );

  Widget _buildBaglantiKarti() {
    return Container(
      decoration: BoxDecoration(
        color: const Color(0xFF161B22),
        borderRadius: BorderRadius.circular(16),
        border: Border.all(color: Colors.white12),
      ),
      padding: const EdgeInsets.all(16),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Text('BAGLANTI',
              style: TextStyle(fontSize: 11, letterSpacing: 2,
                  color: Colors.grey, fontWeight: FontWeight.w600)),
          const SizedBox(height: 12),
          // WiFi
          Row(children: [
            const Icon(Icons.wifi, color: Color(0xFF00E5FF), size: 20),
            const SizedBox(width: 8),
            Expanded(child: TextField(
              controller: _ipController,
              style: const TextStyle(fontSize: 13, color: Colors.white),
              decoration: InputDecoration(
                hintText: 'ESP32 IP (or: 192.168.1.105)',
                hintStyle: TextStyle(color: Colors.grey[600], fontSize: 12),
                isDense: true,
                contentPadding: const EdgeInsets.symmetric(horizontal: 10, vertical: 8),
                filled: true, fillColor: const Color(0xFF0D1117),
                border: OutlineInputBorder(
                    borderRadius: BorderRadius.circular(8), borderSide: BorderSide.none),
              ),
              keyboardType: TextInputType.number,
            )),
            const SizedBox(width: 8),
            _baglanBtn(
              label: wifiConnected ? 'Bagli' : 'Baglan',
              aktif: wifiConnected,
              onTap: wifiConnected ? null : wifiBaglan,
              renk: const Color(0xFF00E5FF),
            ),
          ]),
          const SizedBox(height: 10),
          // BLE
          Row(children: [
            const Icon(Icons.bluetooth, color: Color(0xFF7B61FF), size: 20),
            const SizedBox(width: 8),
            Expanded(child: Text(
              bleConnected ? 'SPARK-S3 Bagli' : bleAraniyor ? 'Araniyor...' : 'SPARK-S3 bekleniyor',
              style: TextStyle(fontSize: 13,
                  color: bleConnected ? Colors.greenAccent : Colors.grey[500]),
            )),
            _baglanBtn(
              label: bleAraniyor ? 'Ariyor...' : bleConnected ? 'Bagli' : 'Tara',
              aktif: bleConnected,
              onTap: (bleAraniyor || bleConnected) ? null : bleTara,
              renk: const Color(0xFF7B61FF),
            ),
          ]),
          const SizedBox(height: 10),
          // MQTT
          Row(children: [
            const Icon(Icons.cloud, color: Color(0xFF00C853), size: 20),
            const SizedBox(width: 8),
            Expanded(child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  mqttConnected ? 'Internet Bagli' : mqttBaglaniyor ? 'Baglaniyor...' : 'Internet (farkli ag)',
                  style: TextStyle(fontSize: 13,
                      color: mqttConnected ? Colors.greenAccent : Colors.grey[500]),
                ),
                // DÜZELTİLDİ: Doğru broker adresini göster
                Text('$mqttBroker | topic: spark/#',
                    style: TextStyle(fontSize: 10, color: Colors.grey[600])),
              ],
            )),
            mqttConnected
                ? _baglanBtn(label: 'Kes', aktif: false, onTap: mqttKes, renk: Colors.redAccent)
                : _baglanBtn(
              label: mqttBaglaniyor ? 'Bekle' : 'Baglan',
              aktif: false,
              onTap: mqttBaglaniyor ? null : mqttBaglan,
              renk: const Color(0xFF00C853),
            ),
          ]),
        ],
      ),
    );
  }

  Widget _baglanBtn({
    required String label, required bool aktif,
    required VoidCallback? onTap, required Color renk,
  }) => GestureDetector(
    onTap: onTap,
    child: Container(
      padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 8),
      decoration: BoxDecoration(
        color: aktif ? renk.withOpacity(0.15) : renk.withOpacity(0.1),
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: aktif ? renk : renk.withOpacity(0.4)),
      ),
      child: Text(label,
          style: TextStyle(fontSize: 12, fontWeight: FontWeight.w600,
              color: aktif ? renk : renk.withOpacity(0.7))),
    ),
  );

  Widget _buildDurumKarti() {
    final bool karanlik = ldrDegeri > 2500;
    final bool hareket  = pirDegeri == 1;
    return Container(
      decoration: BoxDecoration(
        color: const Color(0xFF161B22),
        borderRadius: BorderRadius.circular(16),
        border: Border.all(color: Colors.white12),
      ),
      padding: const EdgeInsets.all(16),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Text('SENSOR DURUMU',
              style: TextStyle(fontSize: 11, letterSpacing: 2,
                  color: Colors.grey, fontWeight: FontWeight.w600)),
          const SizedBox(height: 14),
          Row(children: [
            Expanded(child: _durumKutu(Icons.lightbulb, 'LED',
                ledAcik ? 'ACIK' : 'KAPALI', ledAcik ? Colors.amber : Colors.grey)),
            const SizedBox(width: 10),
            Expanded(child: _durumKutu(Icons.directions_run, 'HAREKET',
                hareket ? 'VAR' : 'YOK', hareket ? Colors.redAccent : Colors.grey)),
            const SizedBox(width: 10),
            Expanded(child: _durumKutu(
                karanlik ? Icons.nights_stay : Icons.wb_sunny, 'ORTAM',
                karanlik ? 'KARANLIK' : 'AYDINLIK',
                karanlik ? const Color(0xFF7B61FF) : Colors.orangeAccent)),
          ]),
          const SizedBox(height: 14),
          Row(mainAxisAlignment: MainAxisAlignment.spaceBetween, children: [
            Text('LDR Degeri', style: TextStyle(fontSize: 11, color: Colors.grey[400])),
            Text('$ldrDegeri / 4095', style: const TextStyle(fontSize: 11, color: Colors.white70)),
          ]),
          const SizedBox(height: 6),
          ClipRRect(
            borderRadius: BorderRadius.circular(4),
            child: LinearProgressIndicator(
              value: ldrDegeri / 4095, minHeight: 8,
              backgroundColor: Colors.white10,
              valueColor: AlwaysStoppedAnimation<Color>(
                  karanlik ? const Color(0xFF7B61FF) : Colors.orangeAccent),
            ),
          ),
        ],
      ),
    );
  }

  Widget _durumKutu(IconData icon, String baslik, String deger, Color renk) =>
      Container(
        padding: const EdgeInsets.all(10),
        decoration: BoxDecoration(
          color: renk.withOpacity(0.08),
          borderRadius: BorderRadius.circular(12),
          border: Border.all(color: renk.withOpacity(0.3)),
        ),
        child: Column(children: [
          Icon(icon, color: renk, size: 22),
          const SizedBox(height: 4),
          Text(baslik, style: TextStyle(fontSize: 9, letterSpacing: 1, color: Colors.grey[500])),
          const SizedBox(height: 2),
          Text(deger, style: TextStyle(fontSize: 10, fontWeight: FontWeight.w700, color: renk)),
        ]),
      );

  Widget _buildYzPaneli() {
    final yuzde = (annCikis * 100).toInt();
    return Container(
      decoration: BoxDecoration(
        color: const Color(0xFF161B22),
        borderRadius: BorderRadius.circular(16),
        border: Border.all(color: const Color(0xFF00E5FF).withOpacity(0.4), width: 1.5),
      ),
      padding: const EdgeInsets.all(16),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(children: [
            const Icon(Icons.psychology, color: Color(0xFF00E5FF), size: 18),
            const SizedBox(width: 6),
            const Text('YAPAY SINIR AGI',
                style: TextStyle(fontSize: 11, letterSpacing: 2,
                    color: Color(0xFF00E5FF), fontWeight: FontWeight.w700)),
            const Spacer(),
            Container(
              padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
              decoration: BoxDecoration(
                color: const Color(0xFF00E5FF).withOpacity(0.15),
                borderRadius: BorderRadius.circular(6),
              ),
              child: Text('AKTIF | $annToplam karar',
                  style: const TextStyle(fontSize: 10,
                      color: Color(0xFF00E5FF), fontWeight: FontWeight.w700)),
            ),
          ]),
          const SizedBox(height: 14),
          Row(children: [
            Expanded(child: _durumKutu(
                annKarar ? Icons.lightbulb : Icons.lightbulb_outline,
                'YZ KARARI', annKarar ? 'ACIK' : 'KAPALI',
                annKarar ? Colors.amberAccent : Colors.grey)),
            const SizedBox(width: 10),
            Expanded(child: _durumKutu(Icons.speed, 'GUVEN', '%$yuzde', const Color(0xFF00E5FF))),
            const SizedBox(width: 10),
            Expanded(child: _durumKutu(Icons.verified, 'DOGRULUK', '%$annDogruluk', const Color(0xFF39FF14))),
          ]),
          const SizedBox(height: 14),
          Row(mainAxisAlignment: MainAxisAlignment.spaceBetween, children: [
            Text('Guven seviyesi', style: TextStyle(fontSize: 11, color: Colors.grey[400])),
            Text('%$yuzde', style: const TextStyle(fontSize: 11, color: Colors.white70)),
          ]),
          const SizedBox(height: 6),
          ClipRRect(
            borderRadius: BorderRadius.circular(4),
            child: LinearProgressIndicator(
              value: annCikis, minHeight: 8,
              backgroundColor: Colors.white10,
              valueColor: const AlwaysStoppedAnimation<Color>(Color(0xFF00E5FF)),
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildModSecici() {
    final yzAktif = anaMod == 1;
    return Container(
      decoration: BoxDecoration(
        color: const Color(0xFF161B22),
        borderRadius: BorderRadius.circular(16),
        border: Border.all(color: Colors.white12),
      ),
      padding: const EdgeInsets.all(16),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(children: [
            const Text('MOD SECIMI',
                style: TextStyle(fontSize: 11, letterSpacing: 2,
                    color: Colors.grey, fontWeight: FontWeight.w600)),
            const Spacer(),
            if (yzAktif)
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 2),
                decoration: BoxDecoration(
                  color: Colors.grey.withOpacity(0.15),
                  borderRadius: BorderRadius.circular(4),
                ),
                child: const Text('YZ AKTIF — DEVRE DISI',
                    style: TextStyle(fontSize: 9, color: Colors.grey,
                        fontWeight: FontWeight.w700, letterSpacing: 0.8)),
              ),
          ]),
          const SizedBox(height: 14),
          _modBtn(0, Icons.psychology,    'AKILLI MOD',      'Hem karanlik hem hareket gerekli', const Color(0xFF00E5FF), yzAktif),
          const SizedBox(height: 10),
          _modBtn(1, Icons.directions_run,'SADECE HAREKET',  'Hareket alginlandiginda yanar',    Colors.redAccent,        yzAktif),
          const SizedBox(height: 10),
          _modBtn(2, Icons.nights_stay,   'GECE / LDR',      'Karanlikta surekli yanar',         const Color(0xFF7B61FF), yzAktif),
          const SizedBox(height: 10),
          _modBtn(3, Icons.shield,        'GUVENLIK',        'Hareket = alarm + buzzer',         Colors.redAccent.shade400, yzAktif),
        ],
      ),
    );
  }

  Widget _modBtn(int mod, IconData icon, String baslik, String aciklama, Color renk, bool devreDisi) {
    final secili = aktifMod == mod && anaMod == 0;
    return Opacity(
      opacity: devreDisi ? 0.4 : 1.0,
      child: GestureDetector(
        onTap: devreDisi ? null : () => modDegistir(mod),
        child: AnimatedContainer(
          duration: const Duration(milliseconds: 200),
          padding: const EdgeInsets.all(14),
          decoration: BoxDecoration(
            color: secili ? renk.withOpacity(0.15) : Colors.transparent,
            borderRadius: BorderRadius.circular(12),
            border: Border.all(color: secili ? renk : Colors.white12, width: secili ? 1.5 : 1),
          ),
          child: Row(children: [
            Container(
              width: 44, height: 44,
              decoration: BoxDecoration(
                shape: BoxShape.circle,
                color: renk.withOpacity(secili ? 0.2 : 0.08),
              ),
              child: Icon(icon, color: renk, size: 22),
            ),
            const SizedBox(width: 12),
            Expanded(child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(baslik, style: TextStyle(fontSize: 14, fontWeight: FontWeight.w700,
                    color: secili ? renk : Colors.white70)),
                const SizedBox(height: 2),
                Text(aciklama, style: TextStyle(fontSize: 11, color: Colors.grey[500])),
              ],
            )),
            if (secili)
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
                decoration: BoxDecoration(
                    color: renk.withOpacity(0.2), borderRadius: BorderRadius.circular(6)),
                child: Text('AKTIF', style: TextStyle(fontSize: 10, color: renk,
                    fontWeight: FontWeight.w700, letterSpacing: 1)),
              ),
          ]),
        ),
      ),
    );
  }

  Widget _buildZamanlayici() {
    return Container(
      decoration: BoxDecoration(
        color: const Color(0xFF161B22),
        borderRadius: BorderRadius.circular(16),
        border: Border.all(color: Colors.white12),
      ),
      padding: const EdgeInsets.all(16),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(children: [
            const Text('AYDINLATMA ZAMANLAYICISI',
                style: TextStyle(fontSize: 11, letterSpacing: 2,
                    color: Colors.grey, fontWeight: FontWeight.w600)),
            const Spacer(),
            if (zamanlayiciAktif)
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
                decoration: BoxDecoration(
                  color: const Color(0xFFFF6D00).withOpacity(0.15),
                  borderRadius: BorderRadius.circular(6),
                  border: Border.all(color: const Color(0xFFFF6D00).withOpacity(0.4)),
                ),
                child: const Text('CALISIYOR',
                    style: TextStyle(fontSize: 10,
                        color: Color(0xFFFF6D00), fontWeight: FontWeight.w700)),
              ),
          ]),
          const SizedBox(height: 12),
          TextField(
            controller: _zamController,
            style: const TextStyle(fontSize: 14, color: Colors.white, letterSpacing: 1.5),
            textAlign: TextAlign.center,
            decoration: InputDecoration(
              hintText: '20:00-23:30',
              hintStyle: TextStyle(color: Colors.grey[600], fontSize: 13),
              isDense: true,
              contentPadding: const EdgeInsets.symmetric(horizontal: 10, vertical: 12),
              filled: true, fillColor: const Color(0xFF0D1117),
              border: OutlineInputBorder(
                  borderRadius: BorderRadius.circular(8), borderSide: BorderSide.none),
            ),
            maxLength: 11,
          ),
          const SizedBox(height: 4),
          Row(children: [
            Expanded(child: GestureDetector(
              onTap: zamanlayiciKur,
              child: Container(
                padding: const EdgeInsets.symmetric(vertical: 12),
                decoration: BoxDecoration(
                  color: const Color(0xFF00E5FF).withOpacity(0.15),
                  borderRadius: BorderRadius.circular(8),
                  border: Border.all(color: const Color(0xFF00E5FF)),
                ),
                child: const Row(mainAxisAlignment: MainAxisAlignment.center, children: [
                  Icon(Icons.play_arrow, color: Color(0xFF00E5FF), size: 18),
                  SizedBox(width: 4),
                  Text('KUR', style: TextStyle(fontSize: 12,
                      color: Color(0xFF00E5FF), fontWeight: FontWeight.w700, letterSpacing: 1)),
                ]),
              ),
            )),
            const SizedBox(width: 10),
            Expanded(child: GestureDetector(
              onTap: zamanlayiciIptalEt,
              child: Container(
                padding: const EdgeInsets.symmetric(vertical: 12),
                decoration: BoxDecoration(
                  color: Colors.transparent,
                  borderRadius: BorderRadius.circular(8),
                  border: Border.all(color: Colors.white24),
                ),
                child: const Row(mainAxisAlignment: MainAxisAlignment.center, children: [
                  Icon(Icons.cancel, color: Colors.grey, size: 18),
                  SizedBox(width: 4),
                  Text('IPTAL', style: TextStyle(fontSize: 12,
                      color: Colors.grey, fontWeight: FontWeight.w700, letterSpacing: 1)),
                ]),
              ),
            )),
          ]),
          const SizedBox(height: 10),
          Row(mainAxisAlignment: MainAxisAlignment.spaceBetween, children: [
            Text('Mevcut Program', style: TextStyle(fontSize: 11, color: Colors.grey[400])),
            Text('$zamBas — $zamBit',
                style: const TextStyle(fontSize: 12, color: Color(0xFFFF6D00),
                    fontWeight: FontWeight.w700)),
          ]),
        ],
      ),
    );
  }
}