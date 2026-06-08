# snd_mcp49x1_gpio

Sterownik ALSA PCM playback dla przetworników Microchip MCP4901, MCP4911 i MCP4921 podłączonych do Luckfox Pico Mini A/B przez bit-bang GPIO.

Sterownik udostępnia normalne urządzenie odtwarzające ALSA dla prostego wyjścia dźwiękowego na Luckfox Pico Mini A/B.  Układ został zaprojektowany z założeniem maksymalnej prostoty sprzętowej: jeden MCP49x1, jeden kondensator sprzęgający i głośnik o wysokiej impedancji.  Celem nie jest jakość Hi-Fi, tylko praktyczne, bardzo małe wyjście audio do sygnałów, powiadomień, prostych efektów i lekkiego odtwarzania.

Luckfox Pico Mini A/B nie ma natywnego wyjścia analogowego audio.  MCP49x1 pełni rolę minimalnego zewnętrznego DAC-a.  Sterownik odbiera normalne próbki PCM z ALSA, przelicza je do wybranej rozdzielczości DAC-a i wysyła 16-bitową ramkę MCP49x1 przez bit-bang GPIO.

To jest celowo prosty sterownik PCM.  Nie jest to ASoC, nie jest to IIO i nie jest to ogólny sterownik SPI.

## Użyty sprzęt

Testowana platforma:
- Luckfox Pico Mini A/B / RV1103
- MCP4901-E/SN, MCP4911-E/SN albo MCP4921-E/SN, obudowa SOIC-8
- kondensator elektrolityczny 220 uF szeregowo z głośnikiem
- głośnik 150 ohm

Układ zakłada logikę 3.3 V i zasilanie DAC-a 3.3 V.

## Dlaczego MCP49x1

Rodzina MCP49x1 została wybrana, ponieważ jest mała, tania i prosta w sterowaniu:
- MCP4901: 8-bitowy DAC z wyjściem napięciowym
- MCP4911: 10-bitowy DAC z wyjściem napięciowym
- MCP4921: 12-bitowy DAC z wyjściem napięciowym
- wejście szeregowe podobne do SPI
- zasilanie od 2.7 V do 5.5 V
- bezpośrednia współpraca z logiką 3.3 V
- nie wymaga dodatkowego mikrokontrolera ani kodeka audio

Kompromisem nadal pozostaje minimalny tor analogowy.  DAC 10- albo 12-bitowy może zmniejszyć szum kwantyzacji i poprawić ciche fragmenty, ale w minimalnym układzie nadal nie ma prawdziwego analogowego filtra rekonstrukcyjnego ani wzmacniacza.

## Wyprowadzenia MCP49x1-E/SN

Widok z góry na obudowę SOIC-8.  Pin 1 znajduje się przy wcięciu / kropce oznaczającej początek układu.

```text
             MCP49x1-E/SN
          widok z góry, SOIC-8

              ┌───────┐
      VDD  1  │●      │  8  VOUT
      CS   2  │       │  7  VSS / GND
      SCK  3  │       │  6  VREF
      SDI  4  │       │  5  LDAC
              └───────┘
```

Znaczenie pinów:

| Pin MCP49x1 | Nazwa | Funkcja w tym projekcie |
|---|---|---|
| 1 | `VDD` | zasilanie 3.3 V |
| 2 | `CS` | chip select z GPIO52 |
| 3 | `SCK` | zegar szeregowy z GPIO42 |
| 4 | `SDI` | dane szeregowe z GPIO43 |
| 5 | `LDAC` | do GND, natychmiastowa aktualizacja DAC |
| 6 | `VREF` | do 3.3 V |
| 7 | `VSS` | masa |
| 8 | `VOUT` | wyjście analogowe DAC przez kondensator 220 uF do głośnika |

## Podłączenie

### Pełne podłączenie

**Luckfox Pico Mini A/B ↔ MCP49x1-E/SN ↔ głośnik**

| Luckfox Pico Mini A/B | MCP49x1-E/SN | Funkcja |
|---|---|---|
| `3V3` | pin 1 `VDD` | zasilanie DAC |
| `3V3` | pin 6 `VREF` | napięcie odniesienia DAC |
| `GND` | pin 7 `VSS` | masa |
| `GND` | pin 5 `LDAC` | natychmiastowa aktualizacja wyjścia |
| GPIO52 / `GPIO1_C4` | pin 2 `CS` | chip select DAC |
| GPIO42 / `GPIO1_B2` | pin 3 `SCK` | zegar szeregowy DAC |
| GPIO43 / `GPIO1_B3` | pin 4 `SDI` | dane szeregowe DAC |
| — | pin 8 `VOUT` | wyjście analogowe DAC |

Podłączenie głośnika:

| Element | Połączenie |
|---|---|
| kondensator 220 uF `+` | MCP49x1 pin 8 `VOUT` |
| kondensator 220 uF `-` | strona `+` głośnika |
| druga strona głośnika | GND |

Zalecany dodatkowy element:

| Element | Połączenie | Cel |
|---|---|---|
| kondensator ceramiczny 100 nF | między pinem 1 `VDD` i pinem 7 `VSS` MCP49x1 | lokalne odsprzęganie zasilania |

### Minimalny schemat

```text
Luckfox Pico Mini A/B                  MCP49x1-E/SN
────────────────────                  ─────────────

3V3  ────────────────────────────────  1 VDD
                                      │
                                      ├── 100 nF ──┐
                                      │            │
3V3  ────────────────────────────────  6 VREF      │
GND  ────────────────────────────────  7 VSS  ─────┘
GND  ────────────────────────────────  5 LDAC

GPIO52 / GPIO1_C4  ─────────────────  2 CS
GPIO42 / GPIO1_B2  ─────────────────  3 SCK
GPIO43 / GPIO1_B3  ─────────────────  4 SDI

                                      8 VOUT ── +│ 220 uF │- ── głośnik ── GND
```

Uwagi:
- `LDAC` jest połączony z GND, więc każda odebrana ramka DAC od razu aktualizuje wyjście.
- `VREF` jest połączony z 3.3 V, więc zakres wyjścia DAC wynosi w przybliżeniu od 0 V do 3.3 V.
- Kondensator ceramiczny 100 nF jest lokalnym kondensatorem odsprzęgającym zasilanie DAC-a MCP49x1. Umieść go możliwie blisko pinów `VDD` i `VSS`.
- Kondensator 220 uF usuwa składową stałą z wyjścia DAC przed głośnikiem.
- Polaryzacja kondensatora 220 uF ma znaczenie: plus idzie do `VOUT`, minus idzie w stronę głośnika.

## Dlaczego układ jest tak prosty

Projekt celowo unika:
- DAC-ów I2S
- zewnętrznych wzmacniaczy audio
- złożonych kodeków
- dodatkowych napięć zasilania
- dużych filtrów analogowych
- dodatkowych magistral sterujących

W tej konfiguracji Luckfox Pico Mini A/B liczba dostępnych pinów była ograniczona.  Sprzętowe SPI nie było dostępne dla tego toru audio, dlatego sterownik wysyła ramkę MCP49x1 przez bit-bang GPIO.

Efektem jest bardzo małe wyjście audio, wystarczające do dźwięków interfejsu, alarmów i prostego odtwarzania.  Nie zastępuje ono prawdziwego kodeka audio i wzmacniacza.

## Architektura sterownika

Ścieżka audio:

```text
aplikacja ALSA
  ↓
ALSA PCM core
  ↓
snd_mcp49x1_gpio
  ↓
MMIO GPIO bit-bang na GPIO52/GPIO42/GPIO43
  ↓
MCP49x1 DAC
  ↓
kondensator sprzęgający 220 uF
  ↓
głośnik 150 ohm
```

Obecna wersja bazowa używa MMIO jako jedynej metody ustawiania GPIO w najbardziej czasokrytycznej części odtwarzania.  Kernelowe GPIO API jest używane do zajęcia i skonfigurowania linii, ale nie do generowania każdej krawędzi sygnału audio.  Podczas odtwarzania sterownik zapisuje bezpośrednio rejestry GPIO dla trzech linii sygnałowych, co daje wystarczający zapas czasowy dla obsługiwanych częstotliwości próbkowania.

Urządzenie ALSA przyjmuje dane PCM i wewnętrznie przelicza je do wybranej rozdzielczości DAC-a.  Każda próbka wyjściowa jest wysyłana jako 16-bitowe słowo MCP49x1:

```text
0 0 1 1  D11 ... D0
│ │ │ │  └── pole danych DAC ─┘
│ │ │ └─ SHDN = 1, DAC aktywny
│ │ └─── GA   = 1, wzmocnienie 1x
│ └───── BUF  = 0
└─────── bit ramki rodziny MCP49x1
```

Do wyciszenia / shutdown sterownik wysyła komendę z `SHDN=0`.

## Obsługiwane parametry ALSA PCM

Obsługiwane formaty:
- `U8`
- `S16_LE`

Obsługiwane kanały:
- `1` mono
- `2` stereo, mieszane wewnętrznie do mono

Obsługiwane częstotliwości próbkowania:
- `8000`
- `11025`
- `16000`
- `22050`

Nieobsługiwane częstotliwości, takie jak `32000` i `44100`, zostały celowo usunięte z obecnej bazy, ponieważ w tym układzie wymagały agresywniejszego timingu albo pogarszały jakość dźwięku.

Sterownik używa ograniczeń ALSA rate constraints.  Jeśli aplikacja zażąda np. `22345 Hz`, ALSA wybiera najbliższą obsługiwaną wartość, zwykle `22050 Hz`.

## Parametry modułu

### `dac_bits`

Wybiera rozdzielczość DAC-a, a tym samym obsługiwany wariant układu.

Wartości:
- `8` — MCP4901, DAC 8-bitowy
- `10` — MCP4911, DAC 10-bitowy
- `12` — MCP4921, DAC 12-bitowy

Domyślnie:
- `dac_bits=8`

Przykładowe komendy, wybierz jedną:

| Układ | Komenda |
| --- | --- |
| MCP4901 | `insmod snd_mcp49x1_gpio.ko dac_bits=8` |
| MCP4911 | `insmod snd_mcp49x1_gpio.ko dac_bits=10` |
| MCP4921 | `insmod snd_mcp49x1_gpio.ko dac_bits=12` |

Układu nie da się wiarygodnie rozpoznać automatycznie, ponieważ to połączenie używa tylko linii `CS`, `SCK` i `SDI`; nie ma linii zwrotnej ani odczytywalnego identyfikatora układu.  Wybrana wartość określa, jak próbki ALSA są skalowane do 16-bitowej ramki MCP49x1:
- MCP4901 używa górnych 8 bitów pola danych.
- MCP4911 używa górnych 10 bitów pola danych.
- MCP4921 używa wszystkich 12 bitów pola danych.


### `dither`

Opcjonalny TPDF dither przed końcową konwersją do 8-bitowego kodu DAC-a.

Działa tylko dla `dac_bits=8` / MCP4901. Dla `dac_bits=10` i `dac_bits=12` jest ignorowany.

Parametr:
- `dither=0` - wyłączony, domyślnie; dotychczasowe zachowanie
- `dither=1..4` - włączony, coraz mocniejszy poziom ditheringu

Przykład:

```sh
insmod snd_mcp49x1_gpio.ko dac_bits=8 dither=1
```


### `gain_percent`

Wewnętrzne wzmocnienie przed końcową konwersją do wybranej rozdzielczości DAC-a.

Domyślnie:
- `gain_percent=100`

Przykładowe komendy, wybierz jedną:

| Ustawienie | Komenda |
| --- | --- |
| Domyślne wzmocnienie | `insmod snd_mcp49x1_gpio.ko gain_percent=100` |
| Większe wzmocnienie | `insmod snd_mcp49x1_gpio.ko gain_percent=120` |

Uwagi:
- większa wartość daje głośniejszy dźwięk
- zbyt duża wartość może zwiększyć zniekształcenia
- normalną głośność użytkownika lepiej regulować przez ALSA `Master Playback Volume`

### `limiter_enable`

Włącza programowy limiter.

Wartości:
- `0` — wyłączony
- `1` — włączony

Domyślnie:
- `limiter_enable=1`

Limiter ogranicza twarde przesterowanie, gdy gain i psychoakustyczne wzmocnienie basu zwiększają poziom sygnału.

### `highpass_enable`

Włącza prosty filtr górnoprzepustowy.

Wartości:
- `0` — wyłączony
- `1` — włączony

Domyślnie:
- `highpass_enable=1`

Pomaga ograniczyć bardzo niskie częstotliwości, których mały głośnik i tak nie odtworzy dobrze.

### `highpass_q15`

Współczynnik filtra high-pass w formacie zbliżonym do Q15.

Domyślnie:
- `highpass_q15=30000`

Przykładowe komendy, wybierz jedną:

| Ustawienie | Komenda |
| --- | --- |
| Mocniejszy filtr high-pass | `insmod snd_mcp49x1_gpio.ko highpass_q15=30000` |
| Łagodniejszy filtr high-pass | `insmod snd_mcp49x1_gpio.ko highpass_q15=31200` |

Niższe wartości oznaczają mocniejsze działanie filtra.  Wyższe wartości oznaczają działanie łagodniejsze.

### `fade_ms`

Czas narastania i opadania dźwięku w milisekundach.

Domyślnie:
- `fade_ms=24`

Ogranicza słyszalne kliknięcia przy starcie i zatrzymaniu odtwarzania.

### `psycho_bass_enable`

Włącza psychoakustyczne wzmocnienie basu.

Wartości:
- `0` — wyłączone
- `1` — włączone

Domyślnie:
- `psycho_bass_enable=1`

Nie tworzy prawdziwego głębokiego basu.  Dodaje kontrolowane wzmocnienie niskiego zakresu, które może sprawić, że mały głośnik brzmi pełniej.

### `psycho_bass_level`

Siła psychoakustycznego wzmocnienia basu.

Domyślnie:
- `psycho_bass_level=60`

Typowy użyteczny zakres:
- `30` do `80`

### `psycho_bass_shift`

Steruje szybkością filtra śledzącego niski zakres używanego przez psychoakustyczne wzmocnienie basu.

Domyślnie:
- `psycho_bass_shift=5`

Znaczenie:
- mniejsza wartość — mocniejszy / szybszy efekt
- większa wartość — subtelniejszy / wolniejszy efekt

### `mmio_gpio1_base`

Fizyczny adres bazowy rejestrów GPIO1.

Domyślnie:
- `mmio_gpio1_base=0xff530000`

Na Luckfox Pico Mini A/B / RV1103 normalnie nie należy tego zmieniać.

## Kontrolka miksera ALSA

Sterownik udostępnia:

```text
Master Playback Volume
```

Przykład:
```sh
amixer -c 1 set Master 80%
```

Ta kontrolka jest przeznaczona do normalnej regulacji głośności.  `gain_percent` jest bardziej parametrem kalibracyjnym.

## Budowanie

```sh
make clean; make
```

Wynikowy moduł:

```text
snd_mcp49x1_gpio.ko
```

## Ładowanie

```sh
insmod ./snd_mcp49x1_gpio.ko
```

Sprawdzenie kart ALSA:

```sh
cat /proc/asound/cards
aplay -l
```

Przykładowa karta:

```text
1 [Audio          ]: MCP49x1GPIO - MCP49x1 GPIO Audio
                      MCP49x1 GPIO bit-bang PCM
```

## Pomocniczy podgląd parametrów

Repozytorium zawiera prosty skrypt shellowy, który pokazuje użyteczne parametry modułu jeden raz i kończy działanie. Skrypt czyta wartości bezpośrednio z:

```text
/sys/module/snd_mcp49x1_gpio/parameters/
```

Przykładowy widok:

```text
MCP49x1 ALSA driver parameters

┌────────────────────────────────────┐
│ gpio_cs            : 52            │
│ gpio_sck           : 42            │
│ gpio_sdi           : 43            │
│ dac_bits           : 8             │
│ dither             : 0             │
│ gain_percent       : 100           │
│ limiter_enable     : 1             │
│ highpass_enable    : 1             │
│ highpass_q15       : 30000         │
│ fade_ms            : 24            │
│ psycho_bass_enable : 1             │
│ psycho_bass_level  : 60            │
│ psycho_bass_shift  : 5             │
│ mmio_gpio1_base    : 0xff530000    │
└────────────────────────────────────┘
```

Uruchomienie:

```sh
chmod +x sndstat
./sndstat
```

## Przykłady odtwarzania

### Natywne odtwarzanie ALSA

```sh
aplay -D hw:1,0 audio.wav
```

Plik WAV musi mieć format i częstotliwość obsługiwaną przez sterownik.

### Odtwarzanie przez ALSA plug

```sh
aplay -D plughw:1,0 audio.wav
```

Pozwala warstwie userspace ALSA konwertować format, liczbę kanałów lub częstotliwość, jeśli odpowiednie pluginy są obecne w rootfs.

### Odtwarzanie MP3 przez mpg123

```sh
mpg123 -a hw:1,0 -r 22050 -m -e s16 file.mp3
```

Znaczenie:
- `-a hw:1,0` — użyj bezpośrednio tej karty ALSA
- `-r 22050` — resampling do 22050 Hz
- `-m` — wyjście mono
- `-e s16` — wyjście signed 16-bit PCM

## Proponowany /etc/asound.conf

Do normalnego użycia wygodnie jest ustawić konwersję ALSA do stabilnego formatu obsługiwanego przez sterownik:

```conf
pcm.!default {
    type plug
    slave {
        pcm "hw:1,0"
        format S16_LE
        channels 1
        rate 22050
    }
}

ctl.!default {
    type hw
    card 1
}
```

Po tym aplikacje używające domyślnego urządzenia ALSA powinny grać przez sterownik MCP49x1 bez ręcznego podawania formatu.

## Sprawdzanie ograniczeń częstotliwości

Przykład:

```sh
aplay -v --disable-resample --disable-format --disable-channels -D hw:1,0 -f U8 -c 1 -r 22345 /dev/zero
```

Oczekiwany wynik:

```text
rate       : 22050
exact rate : 22050 (22050/1)
```

Oznacza to, że ALSA nie użyła `22345 Hz`, tylko wybrała obsługiwane `22050 Hz`.

## Ograniczenia

To wyjście audio jest celowo minimalne.  Spodziewane ograniczenia:
- rozdzielczość DAC zależna od `dac_bits`: 8, 10 albo 12 bitów
- fizyczne wyjście mono
- brak analogowego wzmacniacza
- brak filtra rekonstrukcyjnego
- ograniczona odpowiedź niskich częstotliwości
- słyszalny szum w cichych fragmentach w porównaniu z prawdziwym sprzętem audio

Aby uzyskać lepszą jakość, należy użyć prawdziwego DAC-a audio, wzmacniacza i odpowiedniego głośnika.  Ten sterownik jest przeznaczony do prostego audio w systemie embedded, gdzie minimalny sprzęt jest ważniejszy niż jakość Hi-Fi.

## Domyślne parametry

Sterownik jest ładowany jako moduł kernela.

Domyślne parametry modułu:

```text
gain_percent=100
limiter_enable=1
highpass_enable=1
highpass_q15=30000
fade_ms=24
psycho_bass_enable=1
psycho_bass_level=60
psycho_bass_shift=5
mmio_gpio1_base=0xff530000
dac_bits=8
dither=0
```
