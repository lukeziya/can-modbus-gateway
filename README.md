# Elektrotehnički fakultet Banja Luka
## Industrijske komunikacione mreže

### Projektni zadatak - Tema: Realizacija CAN-Modbus RTU gejtveja na Raspberry Pi platformi

---

## 1. Opis i cilj projektnog zadatka

Cilj ovog projektnog zadatka je projektovanje i implementacija komunikacionog gejtveja (Gateway) zasnovanog na **Raspberry Pi** platformi i programskom jeziku **C**. Gejtvej omogućava transparentno premošćavanje i dvosmjernu razmjenu podataka između dva različita komunikaciona protokola i fizička sloja:
1. **CAN magistrale** (korišćenjem proširenog/Extended 29-bitnog identifikatora na SocketCAN nivou).
2. **Modbus RTU mreže** (korišćenjem RS485 poludupleksne serijske veze na `termios` nivou).

Kao dokaz ispravnosti razvijenog rješenja, gejtvej uspješno mapira CAN okvire u Modbus komande kojima se manipuliše stanjima relejnog izlaznog modula sa laboratorijskih vježbi.

---

## 2. Arhitektura sistema i mapiranje protokola

S obzirom na to da CAN i Modbus RTU koriste fundamentalno različite koncepte adresiranja i formatiranja paketa, definisan je namjenski aplikativni sloj unutar proširenog **29-bitnog CAN identifikatora (Extended CAN ID)**. 

Struktura 29-bitnog ID-a je podijeljena na četiri funkcionalna polja (bita):

| Pozicija bitova | Naziv polja | Opis i funkcionalnost |
| :--- | :--- | :--- |
| **B28 - B24** (5 bita) | `Priority` | Arbitažni prioritet okvira na CAN magistrali (0x00 je najviši prioritet). |
| **B23 - B16** (8 bita) | `Modbus Address` | Adresa ciljnog Modbus RTU čvora na RS485 mreži (opseg od 1 do 247). |
| **B15 - B08** (8 bita) | `Function Code` | Izvorni Modbus funkcijski kod (npr. 0x05 za *Write Single Coil*). |
| **B07 - B00** (8 bita) | `Gateway ID` | Jedinstveni identifikator samog Raspberry Pi gejtveja na mreži. |

### Struktura polja podataka (CAN Data Payload):
Korisni podaci unutar CAN okvira direktno nose parametre Modbus Protokolne Jedinice Podataka (PDU):
* **Bajt 0 i Bajt 1:** Početna adresa registra/koila (High Byte, pa Low Byte).
* **Bajt 2 i Bajt 3:** Vrijednost podatka koji se upisuje (npr. `0xFF00` za aktivaciju releja) ili broj registara za čitanje.

---

## 3. Hardverska konfiguracija i šema veza

Sistemska arhitektura se oslanja na Raspberry Pi na koji su, prateći metodologiju sa laboratorijskih vježbi, povezani namjenski komunikacioni moduli:

1. **MCP2515 CAN kontroler:** Komunicira sa procesorom preko **SPI0** interfejsa.
2. **MAX485 Transiver:** Pretvara standardni TTL UART signal sa maline u diferencijalni RS485 signal. Pinovi **DE (Driver Enable)** i **RE (Receiver Enable)** su kratko spojeni i kontrolišu se preko **GPIO 4** pina radi upravljanja smjerom prenosa (poludupleks).

### Detaljan raspored pinova (Pinout):

* **MCP2515 -> Raspberry Pi:**
  * VCC -> 5V (Pin 2)
  * GND -> GND (Pin 6)
  * CS -> CE0 (GPIO 8 / Pin 24)
  * MISO -> MISO (GPIO 9 / Pin 21)
  * MOSI -> MOSI (GPIO 10 / Pin 19)
  * SCK -> SCLK (GPIO 11 / Pin 23)
  * INT -> GPIO 25 (Pin 22)

* **MAX485 -> Raspberry Pi & Relejni modul:**
  * TXD -> UART TX (GPIO 14 / Pin 8)
  * RXD -> UART RX (GPIO 15 / Pin 10)
  * DE/RE -> GPIO 4 (Pin 7) - Kontrola smjera (1 = Slanje, 0 = Prijem)
  * A -> Linija A (D+) na relejnom modulu
  * B -> Linija B (D-) na relejnom modulu

---

## 4. Struktura softverskog rješenja (`apk.c`)

Program je napisan u čistom **C jeziku** i oslanja se isključivo na sistemske pozive Linux operativnog sistema, prateći strukturu repozitorijuma sa vježbi:
* **SocketCAN interfejs (`<linux/can.h>`)**: Koristi se za otvaranje sirovih (raw) mrežnih utičnica preko kojih se primaju i šalju CAN okviri.
* **Termios struktura (`<termios.h>`)**: Koristi se za direktnu niskonivojsku konfiguraciju serijskog porta (brzina 9600 baud, 8 data bita, bez parnosti, 1 stop bit - 8N1). Implementiran je neblokirajući čitanje sa *timeout*-om od 0.5s preko `VTIME` parametra.
* **Sysfs GPIO kontrola**: Kontrola smjera MAX485 čipa se vrši upisom u sistemske datoteke unutar `/sys/class/gpio/gpio4/`.

---

## 5. Uputstvo za kompajliranje, pokretanje i testiranje

### Korak 1: Inicijalizacija perifernih drajvera na OS nivou
Prije pokretanja programa, potrebno je osigurati da su u `/boot/firmware/config.txt` aktivirani SPI i UART drajveri za MCP2515. Nakon toga, CAN mrežni interfejs se podiže sledećom komandom:
```bash
sudo ip link set can0 up type can bitrate 125000
