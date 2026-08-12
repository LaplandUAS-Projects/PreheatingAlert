# Kaksikanavainen lämpötilavalvontajärjestelmä

ESP32-pohjainen lämpötilavalvonta- ja hälytysjärjestelmä, jossa on kosketusnäyttökäyttöliittymä, säädettävä hälytysraja sekä ääni- ja visuaalihälytys.

Laite mittaa kahta lämpötilaa K-tyypin termopareilla MAX6675-vahvistinmoduulien kautta ja näyttää arvot reaaliaikaisesti TFT-kosketusnäytöllä. Kun molemmat mitatut lämpötilat ylittävät asetetun hälytysrajan, laite käynnistää äänihälytyksen sekä näyttää vilkkuvan hälytysilmoituksen näytöllä.

---

## Ominaisuudet

- Kahden lämpötila-anturin samanaikainen seuranta
- K-tyypin termoparien tuki MAX6675-moduuleilla
- Säädettävä hälytysraja potentiometrillä
- TFT-kosketusnäyttö käyttöliittymää varten
- Visuaalinen hälytys näytöllä
- Äänimerkki DFPlayer Mini -moduulin kautta
- Hälytyksen kuittaus kosketusnäytöltä
- Ei-estävä (non-blocking) ohjelma-arkkitehtuuri
- ESP32-pohjainen toteutus

---

## Laitteisto

Esimerkkitoteutuksessa käytetyt pääkomponentit:

- FireBeetle 2 ESP32-E
- DFRobot DFR0665 TFT-kosketusnäyttö (ILI9341 + XPT2046)
- 2 × MAX6675 lämpötilamuunnin
- 2 × K-tyypin termopari
- DFPlayer Mini MP3 -moduuli
- Kaiutin
- Potentiometri
- Virtalähde

Täydellinen osaluettelo voidaan lisätä projektin valmistuttua.

---

## Kytkennät

| Toiminto | GPIO |
|-----------|--------|
| TFT DC | GPIO 25 |
| TFT CS | GPIO 14 |
| TFT RST | GPIO 26 |
| TFT Taustavalo | GPIO 12 |
| Kosketuspaneeli CS | GPIO 4 |
| Kosketuspaneeli IRQ | GPIO 16 |
| SD-kortti CS | GPIO 13 |
| MAX6675 SCK | GPIO 22 |
| MAX6675 SO | GPIO 34 |
| MAX6675 Anturi 1 CS | GPIO 21 |
| MAX6675 Anturi 2 CS | GPIO 17 |
| DFPlayer RX | GPIO 35 |
| DFPlayer TX | GPIO 15 |
| Potentiometri | GPIO 36 |

---

## Käyttöliittymä

Näytöllä esitetään:

- Anturi 1 lämpötila
- Anturi 2 lämpötila
- Hälytysrajan asetus
- Laitteen nykyinen tila

### Tilaviestit

| Tila | Selitys |
|--------|----------|
| Lämmitetään... | Tavoitelämpötilaa ei ole vielä saavutettu |
| Valmis | Molemmat lämpötilat ovat normaalilla alueella |
| HÄLYTYS | Hälytys aktiivinen |
| Kuittattu | Käyttäjä on kuitannut hälytyksen |

---

## Hälytyslogiikka

Hälytys aktivoituu, kun:

- Anturi 1 ylittää asetetun hälytysrajan.
- Anturi 2 ylittää asetetun hälytysrajan.

Hälytyksen aikana:

- Näytön yläreunaan ilmestyy vilkkuva punainen hälytyspalkki.
- DFPlayer Mini käynnistää hälytysäänen.
- Käyttäjä voi kuitata hälytyksen kosketusnäytöllä.

Kun lämpötilat palautuvat hälytysrajan alapuolelle, hälytys poistuu automaattisesti.

---

## Käyttö

1. Kytke laite käyttöjännitteeseen.
2. Odota järjestelmän käynnistymistä.
3. Seuraa lämpötiloja näytöltä.
4. Säädä hälytysrajaa potentiometrillä.
5. Hälytyksen aktivoituessa kuittaa se kosketusnäytön painikkeella tarvittaessa.

---

## Ohjelmistoarkkitehtuuri

Ohjelmisto on toteutettu ei-estävällä tilakonearkkitehtuurilla.

Päätoiminnot:

- Kosketusnäytön käsittely
- Lämpötilojen mittaus
- Hälytyslogiikka
- DFPlayer Mini -alustus
- Näytön päivitys

Tämän ansiosta käyttöliittymä pysyy responsiivisena ilman viiveitä tai ohjelman pysähtymisiä.

---

## Tarvittavat kirjastot

Projektissa käytetään muun muassa seuraavia Arduino-kirjastoja:

- DFRobot_GDL
- DFRobotDFPlayerMini
- MAX6675
- SPI

---

## Kääntäminen

### PlatformIO

Esimerkkiasetus:

```ini
[env:esp32]
platform = espressif32
board = esp32dev
framework = arduino
```

### Arduino IDE

1. Asenna tarvittavat kirjastot.
2. Valitse ESP32-kohdelevy.
3. Käännä ja lataa ohjelma laitteeseen.

---

## Kehitysideat

- Bluetooth-äänentoisto
- Datan tallennus SD-kortille
- WiFi-yhteys
- MQTT-etävalvonta
- Selainpohjainen käyttöliittymä
- Lämpötilahistorian tallennus ja trendit

---

## Lisenssi

Lisää projektin lisenssitiedot tähän.
