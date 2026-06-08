# Elektrotehnički fakultet Banja Luka
## Industrijske komunikacione mreže

### Projektni zadatak: Realizacija CAN-Modbus RTU gejtveja primjenom libmodbus biblioteke

---

## 1. Opis projekta i cilj

Cilj ovog projektnog zadatka je realizacija komunikacionog gejtveja (Gateway) koji omogućava transparentno povezivanje i dvosmjernu razmjenu podataka između **CAN magistrale** i **Modbus RTU mreže** korišćenjem Raspberry Pi platforme. 

Program prima proširene (Extended) 29-bitne CAN okvire, iz njihovog identifikatora (ID) izdvaja parametre potrebne za Modbus protokol (adresu uređaja i funkcijski kod), a zatim korišćenjem zvanične **`libmodbus`** biblioteke (obrađene u Laboratorijskoj vježbi 5) izvršava komande nad 4-kanalnim relejnim izlaznim modulom.

---

## 2. Mapiranje CAN i Modbus protokola

S obzirom na to da CAN koristi identifikatore (ID) umjesto adresa uređaja, unutar **29-bitnog CAN ID-a** definisan je aplikativni sloj koji nosi sve informacije za rutiranje prema Modbus mreži:

* **Bitovi B28 - B24 (5 bita):** Prioritet okvira na CAN magistrali.
* **Bitovi B23 - B16 (8 bita):** Modbus adresa slave uređaja (čvora).
* **Bitovi B15 - B08 (8 bita):** Modbus funkcijski kod (FC) (npr. `0x05` za upis stanja releja).
* **Bitovi B07 - B00 (8 bita):** Identifikator samog gejtveja (Gateway ID).

### Sadržaj CAN Data polja (Payload):
* **Bajt 0 i Bajt 1:** Adresa Modbus registra (npr. `0x0000` za Relej 1).
* **Bajt 2 i Bajt 3:** Vrijednost koja se upisuje (`0xFF00` za uključenje, `0x0000` za isključenje).

---

## 3. Objašnjenje ključnih dijelova koda

Program `gateway.c` se oslanja na sistemske pozive Linux-a za CAN magistralu i `libmodbus` API za serijsku komunikaciju:

* **`custom_set_rts()` i `modbus_rtu_set_custom_rts()`**
  Poludupleksni RS485 transiver zahtijeva kontrolu smjera prenosa (slanje/prijem). Prateći tekst Vježbe 5, definisana je korisnička funkcija koja preko Linux `Sysfs` interfejsa kontroliše hardverski RTS pin **GPIO 17**. Kada biblioteka šalje upit, postavlja pin na `1` (slanje), a nakon slanja ga vraća na `0` (prijem).
  
* **`modbus_new_rtu()` i `modbus_connect()`**
  Inicijalizuju Modbus kontekst za serijski port `/dev/ttyAMA0` sa parametrima relejnog modula: brzina 9600 bps, 8 bita podataka, bez parnosti i 1 stop bit (8N1), nakon čega se uspostavlja veza.

* **`read(can_sock, &frame, ...)`**
  Blokirajući sistemski poziv koji drži program u stanju čekanja sve dok na CAN magistralu ne stigne novi okvir.

* **Bit-shifting i maskiranje (`ext_id >> 16 & 0xFF`)**
  Operacije kojima se iz pristiglog 29-bitnog CAN ID-a izoluju pojedinačni bajtovi koji predstavljaju adresu slave uređaja i funkcijski kod.

* **`modbus_set_slave()` i `modbus_write_bit()`**
  Funkcije iz `libmodbus` API-ja. Prva dinamički postavlja adresu uređaja kojem se obraćamo, a druga šalje Modbus komandu (FC=0x05) za izmjenu stanja određenog releja.

---

## 4. Uputstvo za korišćenje i verifikaciju

### Korak 1: Kroskompajliranje projekta
Aplikacija se kroskompajlira na razvojnoj mašini uz linkovanje sa prekompajliranom `libmodbus` verzijom iz `usr` foldera (kreiranog prema uputstvu iz vježbe):
```bash
arm-linux-gnueabihf-gcc gateway.c -o gateway -I./usr/include/modbus -L./usr/lib -lmodbus
