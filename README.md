
# SP7TEAM-hamclock

Program powstał z pomysłu SP3KON (https://github.com/SP3KON/ESP32-HAM-CLOCK/) - za sprawą kolegi Artura SQ7ACP (https://www.youtube.com/watch?v=9HV83y6wuOQ)


Wsparcie projektu -SP7TEAM:
- Marcin SQ5PGC
- Artur SQ7ACP
- Piotr SQ7FJB
- Pior SQ7KHZ


# Sprzęt

Projekt jest rozwijany i testowany na popularnym module **ESP32-2432S028R**, znanym również jako "Cheap Yellow Display" (CYD).

Jest to płytka rozwojowa z ESP32, która posiada zintegrowany wyświetlacz ILI9341 o przekątnej 2.8 cala (320x240) z rezystancyjnym panelem dotykowym XPT2046.

## Pinout dla ESP32-2432S028R (CYD)

Poniższa konfiguracja pinów jest używana w projekcie:

**Wyświetlacz ILI9341:**
- `TFT_SCLK`: 14
- `TFT_MOSI`: 13
- `TFT_MISO`: 12
- `TFT_CS`:   15
- `TFT_DC`:    2
- `TFT_RST`:   4
- `TFT_BL`:   21

**Panel dotykowy XPT2046:**
- `TOUCH_CS`:   33
- `TOUCH_IRQ`:  36
- `TOUCH_MOSI`: 32
- `TOUCH_MISO`: 39
- `TOUCH_CLK`:  25



# Materiały
# Kompilacja i Wgrywanie

Projekt jest kompilowany i zarządzany za pomocą PlatformIO.

## Kompilacja

Aby skompilować projekt, potrzebujesz zainstalowanego PlatformIO (rozszerzenie do VS Code lub CLI).

1.  Otwórz projekt w VS Code z zainstalowanym rozszerzeniem PlatformIO.
2.  Użyj opcji "Build" (ikona zaznaczenia) w PlatformIO, aby skompilować kod.
    Spowoduje to wygenerowanie plików binarnych w folderze `.pio/build/esp32dev/`.

## Wgrywanie oprogramowania

Możesz wgrać oprogramowanie na ESP32 na kilka sposobów:

### 1. Wgrywanie za pomocą PlatformIO

Najprostszym sposobem jest użycie funkcji "Upload" (ikona strzałki w prawo) w PlatformIO. PlatformIO automatycznie zajmie się wgrywaniem wszystkich niezbędnych plików (bootloader, partycje, firmware, SPIFFS).

### 2. Wgrywanie przez przeglądarkę (ESP Web Tools)

1.  Otwórz stronę: https://jason2866.github.io/WebSerial_ESPTool/
2.  Wymagana jest przeglądarka Chrome lub Edge (ze względu na obsługę WebSerial).
3.  Podłącz ESP32 do komputera i wybierz odpowiedni port COM na stronie.
4.  Wybierz pliki binarne z folderu `build/esp32.esp32.esp32/` (lub `.pio/build/esp32dev/` w zależności od konfiguracji PlatformIO) i wprowadź następujące adresy (offsety):
    *   `bootloader.bin`: `0x1000`
    *   `partitions.bin`: `0x8000`
    *   `firmware.bin`: `0x10000`
    *   `spiffs.bin`: `0x290000` (lub inny adres, jeśli używasz niestandardowej tabeli partycji - sprawdź plik `partitions.csv` w projekcie, kolumna `Offset` dla partycji `spiffs`).
5.  Kliknij "PROGRAM" i poczekaj na zakończenie wgrywania. Po zakończeniu naciśnij przycisk RESET na płytce ESP32.

### 3. Wgrywanie za pomocą esptool.py (CLI)

Możesz również użyć narzędzia `esptool.py` z linii komend. Upewnij się, że masz zainstalowany `esptool` (`pip install esptool`).

```bash
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 460800 write_flash 0x1000 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin 0x290000 spiffs.bin
```

Pamiętaj, aby dostosować `/dev/ttyUSB0` do portu szeregowego, pod którym jest podłączone Twoje ESP32. Offset dla `spiffs.bin` może wymagać weryfikacji w pliku `partitions.csv`.

# Konfiguracja

Urządzenie można skonfigurować za pomocą wbudowanego interfejsu webowego. Wszystkie ustawienia zapisywane są w pliku `config.json` w pamięci SPIFFS urządzenia.

## Tryb Access Point (AP) - Pierwsza konfiguracja

Jeśli urządzenie nie ma zapisanej konfiguracji sieci WiFi, po uruchomieniu automatycznie przejdzie w tryb punktu dostępowego (Access Point).

1.  Na ekranie wyświetlą się informacje o sieci AP.
2.  Połącz się z siecią WiFi o nazwie (SSID) `HAMCLOCK-XXXXXX` (gdzie XXXXXX to unikalny identyfikator Twojego urządzenia). Sieć jest otwarta i nie wymaga hasła.
3.  Otwórz przeglądarkę internetową i przejdź pod adres `http://192.168.4.1/config`.
4.  Wypełnij formularz konfiguracyjny, podając co najmniej dane Twojej domowej sieci WiFi (SSID i hasło).
5.  Zapisz konfigurację. Urządzenie uruchomi się ponownie i połączy z podaną siecią WiFi.

## Interfejs Webowy (po połączeniu z WiFi)

Gdy urządzenie jest połączone z Twoją siecią WiFi, jego adres IP zostanie wyświetlony w prawym górnym rogu ekranu. Możesz wejść na ten adres w przeglądarce, aby uzyskać dostęp do pełnego interfejsu webowego, w tym strony konfiguracyjnej (`/config`).

Interfejs webowy pozwala na zmianę wszystkich ustawień, takich jak dane sieci WiFi, znak i hasło APRS, filtry i interwały odświeżania dla POTA, WWFF, SondeHub, Twoja lokalizacja oraz ustawienia strefy czasowej.

# Obsługa
 
## Nawigacja

Pomiędzy ekranami można przełączać się na dwa sposoby:
- Przesuwając palcem (gestem) w lewo lub w prawo po ekranie.
- Dotykając odpowiedniej ikony zakładki na dolnym pasku nawigacyjnym.

## Ekran Główny

Centralny ekran aplikacji, wyświetlający najważniejsze informacje w skrócie.

- **Zegar i Data:** Wyświetla aktualny czas i datę. Dotknięcie zegara przełącza widok między czasem **UTC** a **lokalnym** (strefa czasowa jest konfigurowana w interfejsie webowym).
- **Status:** W prawym górnym rogu ekranu widoczny jest **adres IP** urządzenia (gdy jest połączone z WiFi) oraz skonfigurowany **znak APRS**. W tym miejscu pojawia się również powiadomienie, gdy dostępna jest nowa wersja oprogramowania.
- **Podsumowanie danych:** Poniżej zegara znajdują się kafelki z ostatnimi danymi pobranymi z różnych serwisów:
    - **POTA:** Ostatni aktywny spot z `api.pota.app`.
    - **WWFF:** Ostatni aktywny spot z `cqgma.org`.
    - **APRS:** Ostatnia odebrana ramka z serwisu APRS-IS, wraz z symbolem i obliczonym dystansem.
    - **SONDA:** Ostatnia wykryta w pobliżu sonda radiosonde, wraz z jej wysokością.

## Ekran POTA

Wyświetla tabelę z historią 10 ostatnich spotów POTA. Kolumny zawierają: znak aktywatora, częstotliwość, modulację, kraj oraz czas, jaki upłynął od spotu.

## Ekran WWFF

Wyświetla tabelę z historią 10 ostatnich spotów WWFF. Kolumny zawierają: znak aktywatora, referencję parku, częstotliwość, modulację oraz czas, jaki upłynął od spotu.

## Ekran APRS

Wyświetla tabelę z historią 10 ostatnich ramek APRS odebranych z serwisu APRS-IS (dla zdefiniowanego filtra, domyślnie Polska). Kolumny zawierają: znak stacji, symbol APRS, dystans od Twojej lokalizacji, komentarz z ramki oraz czas, jaki upłynął od jej odebrania.

## Ekran Propagacji

Prezentuje warunki propagacyjne dla pasm KF na podstawie danych z `hamqsl.com`. Tabela pokazuje prognozę dla dnia i nocy dla pasm 80m-40m, 30m-20m, 17m-15m oraz 12m-10m. Warunki są oznaczone kolorami: zielony (Dobra), żółty (Średnia), czerwony (Słaba).

## Ekran Sondy

Wyświetla listę 10 ostatnich odczytów pozycji dla sond radiosonde wykrytych w zdefiniowanym w konfiguracji promieniu. Dane pochodzą z `api.v2.sondehub.org`. Tabela zawiera: identyfikator sondy, wysokość (w metrach), szerokość i długość geograficzną oraz czas od ostatniego odczytu.


# Rozwiązywanie problemów

### Urządzenie nie uruchamia się lub ekran jest pusty
- **Sprawdź zasilanie:** Upewnij się, że używasz kabla USB, który przesyła dane, a nie tylko ładuje.
- **Poprawność wgrania:** Zweryfikuj, czy wszystkie pliki binarne (`bootloader`, `partitions`, `firmware`, `spiffs`) zostały wgrane na poprawne adresy (offsety).

### Po skonfigurowaniu WiFi urządzenie nie łączy się z siecią
- **Poprawność danych:** Sprawdź, czy SSID i hasło są na pewno poprawne (wielkość liter ma znaczenie).
- **Pasmo 2.4 GHz:** Upewnij się, że Twoja sieć WiFi działa w paśmie 2.4 GHz. Moduły ESP32 nie obsługują pasma 5 GHz.
- **Wymuszenie trybu AP:** Jeśli chcesz wrócić do ekranu konfiguracji, najprostszym sposobem jest wgranie obrazu SPIFFS (pliku `spiffs.bin`) ponownie, ale tym razem upewniając się, że w folderze `data` nie ma pliku `config.json`. Po wgraniu pustego systemu plików, urządzenie uruchomi się w trybie AP.

### Dane na ekranie (POTA, WWFF, etc.) nie aktualizują się
- **Połączenie z internetem:** Sprawdź, czy w prawym górnym rogu ekranu wyświetla się adres IP. Jego brak oznacza problem z połączeniem WiFi.
- **Logi w konsoli:** Podłącz urządzenie do komputera i otwórz Monitor portu szeregowego (np. w PlatformIO lub Arduino IDE). Zobaczysz tam szczegółowe logi, w tym ewentualne błędy pobierania danych z API (np. `HTTP fetch failed`).

### Dane APRS nie pojawiają się lub są niekompletne
- **Wymagany Passcode:** Do połączenia z siecią APRS-IS i pobierania danych **wymagane jest podanie znaku oraz poprawnego kodu (passcode)** w konfiguracji webowej.
- **Filtr:** Domyślnie oprogramowanie filtruje stacje z prefiksem `SP` (Polska).

### Ekran dotykowy nie reaguje lub działa nieprawidłowo
- **Wersja sprzętowa:** Upewnij się, że używasz oprogramowania przeznaczonego dla Twojej płytki (ESP32-2432S028R). Różne wersje sprzętowe mogą mieć inaczej podłączony kontroler dotyku.


# Licencja
Projekt jest udostępniony na licencji MIT. Zobacz plik LICENSE po szczegóły.
