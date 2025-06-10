# 🚨 Acil Durum Drone Koordinasyon Sistemi

Çoklu drone ve survivor koordinasyonu üzerine gerçek zamanlı bir istemci-sunucu uygulamasıdır. C diliyle geliştirilmiştir ve senkronizasyon, TCP/IP haberleşmesi, JSON veri formatı, thread-safe veri yapıları ve SDL2 ile görselleştirme özellikleri içerir.

---

## 📌 Proje Özeti

Bu projede:

- 🌐 Sunucu birimi çoklu istemci bağlantısını destekler.
- 🚁 Drone'lar, hayatta kalanlara yardım ulaştırmak üzere sunucu tarafından (öncelik, yaş ve mesafeye dayalı bir skorlama ile) otomatik olarak görevlendirilir.
- 📨 Tüm mesajlaşmalar JSON formatında gerçekleştirilir.
- 🖼️ SDL2 ile gerçek zamanlı görsel izleme yapılabilir.
- 🔐 Tüm paylaşılan veri yapıları (listeler, bireysel drone verileri) **thread-safe** olarak tasarlanmıştır.

---

## 🧩 Modüller ve Özellikler

- ✅ **Sunucu**: Çoklu istemci bağlantısı yönetimi, görev atama (AI controller), hayatta kalan üretimi, durumun view istemcisine yayını.
- ✅ **Drone İstemcisi**: Sunucuya bağlanma, periyodik durum güncellemesi, görev alma ve görev tamamlama bildirimi.
- ✅ **Görselleştirme İstemcisi**: Sunucudan gelen verilerle hayatta kalan ve drone pozisyonlarının, durumlarının anlık gösterimi (SDL2).
- ✅ **Thread-safe Veri Yapısı**: Drone ve survivor listeleri `mutex` ve `condition variable` ile, bireysel drone verileri ise kendi `mutex`'leri ile korunur.
- ✅ **İletişim Protokolü**: JSON tabanlı mesajlaşma sistemi (ana mesaj türleri aşağıda örneklendirilmiştir).

---

## 🗃️ Kod Yapısı

| Dosya Adı                   | Açıklama                                           |
|----------------------------|----------------------------------------------------|
| `server.c`                 | Sunucu mantığı, bağlantı yönetimi, görev atama, survivor üretimi, view yayını |
| `client.c`                 | Drone istemcisi, sunucuya bağlantı, navigasyon ve görev bildirimi |
| `view.c`                   | SDL2 ile görselleştirme istemcisi                    |
| `drone.[ch]`               | Drone veri yapısı ve yardımcı fonksiyonlar         |
| `survivor.[ch]`            | Survivor veri yapısı ve yardımcı fonksiyonlar      |
| `list.[ch]`                | Thread-safe bağlantılı liste yapısı                |


---

## 🛠️ Derleme Talimatları

> Derleme için `gcc`, `pthread`, `libjson-c`, `libsdl2`, ve `libsdl2-ttf` gereklidir.

---

### 🔧 Sunucu Derleme

```bash
gcc server.c drone.c survivor.c list.c bounded_buffer.c -o server -ljson-c -lpthread -Wall -Wextra -g
```

🚁 Drone İstemcisi Derleme
```bash
gcc client.c drone.c bounded_buffer.c -o client -ljson-c -lpthread -Wall -Wextra -g
```

👁️ Görselleştirme Arayüzü Derleme
```bash
gcc view.c drone.c survivor.c list.c -o view $(sdl2-config --cflags --libs) -lSDL2_ttf -ljson-c -Wall
```

▶️ Çalıştırma

🌐 Sunucuyu Başlatma
```bash
./server
```
Sunucu drone bağlantıları için 8080, view istemcileri için 8081 portunu dinler.

---

👁️ Görselleştirme Arayüzünü Başlatma (Önerilir)
```bash
./view
```
---

🚁 Drone İstemcilerini Başlatma

Her drone için ayrı bir terminalde çalıştırın:
```bash
./client D1      # Drone D1'i başlatır
./client D2      # Drone D2'yi başlatır 
./client D_ABC   # Drone D_ABC'yi başlatır
```

---
## 🧠 Öğrenme Çıktıları

- 🔐 Mutex ve koşul değişkenleri ile senkronizasyon  
- 🔄 Çoklu thread ve TCP/IP socket ile haberleşme  
- 🧱 Thread-safe veri yapısı tasarımı (list.c ve bireysel Drone kilitleri)  
- 🧭 Ağırlıklı skorlama ile (öncelik, yaş, mesafe) görev yönlendirme algoritması  
- 📊 SDL2 ile gerçek zamanlı görselleştirme  
- 📦 JSON mesaj formatı ile iletişim protokolü ve json-c kütüphanesi kullanımı  

---

## 🙌 Katkıda Bulunanlar

- 👩‍💻 23120205064 Sevde Gül Şahin  
- 👩‍💻 23120205031 Süeda Nur Sarıcan  
- 👩‍💻 23120205039 Pelin Almalı  

---

📝 Proje Tanımı

Bu proje, dağıtık sistemler, senkronizasyon, socket programlama ve gerçek zamanlı uygulamalar gibi konuları kapsayan kapsamlı bir mühendislik çalışmasıdır. Sistem, gerçek dünya arama-kurtarma senaryolarını simüle eden akademik ve uygulamalı bir örnek niteliği taşır.
