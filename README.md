# Elektrotehnički fakultet Banja Luka
## Industrijske komunikacione mreže

### Projektni zadatak: Realizacija CAN-Modbus RTU gejtveja



## Opis projekta

Gejtvej koji prima CAN okvire sa 29-bitnim (EFF) identifikatorom, dekodira Modbus parametre ugrađene u taj identifikator i prosljeđuje odgovarajuće komande Modbus RTU slave uređaju (4-kanalni relejni modul) preko RS-485 magistrale. Odgovor slave uređaja se opciono vraća na CAN magistralu.

## Mapiranje CAN ID → Modbus

Koristi se 29-bitni Extended Frame Format (EFF) identifikator, čija su polja raspoređena ovako:

```
 Bit: 28      24 23      16 15       8 7        0
      [PRIORITET] [MODBUS ADR] [FUNK. KOD] [GW ID]
        5 bita      8 bita      8 bita     8 bita
```

CAN payload (4 bajta) nosi Modbus PDU podatke:

| Bajt | Sadržaj             |
|------|---------------------|
| 0    | Adresa registra Hi  |
| 1    | Adresa registra Lo  |
| 2    | Vrijednost Hi       |
| 3    | Vrijednost Lo       |

**Primjer** — uključivanje relejnog izlaza #1 (Modbus slave adresa 1, FC=0x05, registar 0x0000):

```
CAN ID: 0x00010500  (prioritet=0, slave=1, FC=05, GW=0)
Data:   00 00 FF 00  (registar 0x0000, vrijednost 0xFF00 = ON)
```

## Podržani funkcijski kodovi

| FC   | Modbus funkcija          | libmodbus poziv           |
|------|--------------------------|---------------------------|
| 0x01 | Read Coils               | `modbus_read_bits()`      |
| 0x03 | Read Holding Registers   | `modbus_read_registers()` |
| 0x05 | Write Single Coil        | `modbus_write_bit()`      |
| 0x06 | Write Single Register    | `modbus_write_register()` |

## Hardverske pretpostavke

- CAN kontroler: MCP2515 (via SPI), interfejs `can0`
- RS-485 transiver: MAX485, smjer se kontroliše GPIO22 (sysfs)
- UART: `/dev/ttyAMA0`, 9600 8N1

## Kompajliranje

Kroskompajliranje za ARM platformu (pretpostavlja se da je libmodbus kroskompajliran u `./usr` prema uputstvu sa Lab5):

```sh
arm-linux-gnueabihf-gcc apk.c \
    -I./usr/include \
    -L./usr/lib \
    -lmodbus \
    -o can-modbus-gateway
```

Prenos na platformu i pokretanje:

```sh
scp can-modbus-gateway pi@<IP>:~
scp ./usr/lib/libmodbus.so* pi@<IP>:~/lib
ssh pi@<IP>
sudo ip link set can0 up type can bitrate 125000
LD_LIBRARY_PATH=~/lib ./can-modbus-gateway
```

## Testiranje

Sa drugog čvora na CAN mreži, slanje komande za uključivanje Relay #1:

```sh
cansend can0 00010500#0000FF00
```

Slanje komande za isključivanje Relay #1:

```sh
cansend can0 00010500#00000000
```

Praćenje saobraćaja:

```sh
candump can0
```

## Struktura koda

- `init_gpio()` — inicijalizacija GPIO22 kao izlaza (sysfs, prema Lab5/Lab7 pristupu)
- `custom_set_rts()` — callback za libmodbus kojim se kontroliše smjer RS-485 transivera
- `main()` — inicijalizacija SocketCAN + libmodbus konteksta, glavna petlja gejtveja

## Korištene tehnologije

- **SocketCAN / BSD Sockets API** (Lab7) — prijem i slanje CAN okvira
- **libmodbus v3.1.11** (Lab5) — Modbus RTU master komunikacija sa relejnim modulom
- **Linux sysfs GPIO** — kontrola smjera MAX485 transivera
