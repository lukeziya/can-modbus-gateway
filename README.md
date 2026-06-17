# Elektrotehnički fakultet Banja Luka
## Industrijske komunikacione mreže

### Projektni zadatak: Realizacija CAN-Modbus RTU gejtveja



## Opis projekta
U modernim industrijskim okruženjima, heterogenost mreža predstavlja jedan od najvećih integracionih izazova. CAN (Controller Area Network) protokol je dominantan u automobilskoj industriji, robotici i automatizaciji robusnih ugrađenih sistema zbog svoje determinističke prirode i visoke otpornosti na smetnje. S druge strane, Modbus RTU (preko RS-485 fizickog sloja) predstavlja zlatni standard za komunikaciju sa industrijskim senzorima, aktuatorima i PLC uređajima zbog svoje jednostavnosti i niske cijene implementacije.

Predmet ovog projektnog zadatka je projektovanje i realizacija pametnog CAN-Modbus RTU gejtveja baziranog na Raspberry Pi platformi. Ovaj uređaj djeluje kao translator protokola u realnom vremenu, omogućavajući čvorovima na CAN magistrali da transparentno upravljaju i čitaju stanja izvršnih Modbus RTU uređaja bez poznavanja detalja Modbus serijske komunikacije.
Cilj projektnog zadatka je projektovanje gateway-a koji prima CAN okvire sa 29-bitnim (EFF) identifikatorom, dekodira Modbus parametre ugrađene u taj identifikator i prosljeđuje odgovarajuće komande Modbus RTU slave uređaju (4-kanalni relejni modul) preko RS-485 magistrale.

## Izazovi 

Glavni izazov projektnog zadatka je mapiranje adresa i funkcijskih kodova Modbus protokola u prošireni (29-bitni) CAN ID. Potrebno je obratiti pažnju na ono što je neophodno. Pošto Modbus RTU podržava do 247 slave uređaja za Modbus adresu potreban je 1 bajt. Takođe, funkcijski kodovi su definisani standardom i kako bi se pokrile sve moguće kombinacije za to polje je potreban 1 bajt. Pitanje je kako raspodijeliti ostalih 14 bitova na polje PRIORITY i GATEWAY ID.
Ova dva polja su fleksibilna po pitanju broja potrebnih bitova. GATEWAY ID predstavlja jedinstveni identifikator samog uređaja u CAN mreži na kojem se vrti data aplikacija. U ovoj realizaciji, kako se bajtovi ne bi "skraćivali", za ovo polje ostavljeno je 8 bitova, što je dovoljno za registrovanje do 256 gateway-a. To je i više nego dovoljan broj za standardnu CAN infrastrukturu. Za polje PRIORITY ostaju rezervisano 5 bitova i omogućeno je da se u isto vrijeme šalju do 32 okvira sa različitim prioritetom. Polja EFF CAN ID-a su raspoređena na sljedeći način:
```
+----------+---------------+---------------+---------------+
|  28  24  |  23       16  |  15        8  |  7         0  |
+----------+---------------+---------------+---------------+
| PRIORITY |  MODBUS ADDR  | FUNCTION CODE |  GATEWAY ID   |
+----------+---------------+---------------+---------------+
|  5 bita  |    8 bita     |    8 bita     |    8 bita     |
+----------+---------------+---------------+---------------+
```
CAN payload (4 bajta) nosi Modbus PDU podatke:

| Bajt | Sadržaj             |
|------|---------------------|
| 0    | Adresa registra Hi  |
| 1    | Adresa registra Lo  |
| 2    | Vrijednost Hi       |
| 3    | Vrijednost Lo       |



## Podržani funkcijski kodovi

| FC   | Modbus funkcija          | libmodbus poziv           |
|------|--------------------------|---------------------------|
| 0x01 | Read Coils               | `modbus_read_bits()`      |
| 0x03 | Read Holding Registers   | `modbus_read_registers()` |
| 0x05 | Write Single Coil        | `modbus_write_bit()`      |
| 0x06 | Write Single Register    | `modbus_write_register()` |
#
## Makroi za dekompoziciju CAN ID-a

```sh
...

#define CAN_ID_PRIORITY(id)      (((id) >> 24) & 0x1F) // Izoluje 5 bita prioriteta
#define CAN_ID_MODBUS_ADDR(id)   (((id) >> 16) & 0xFF) // Izoluje 8 bita slave adrese
#define CAN_ID_FUNCTION_CODE(id) (((id) >>  8) & 0xFF) // Izoluje 8 bita funkcijskog koda
#define CAN_ID_GATEWAY_ID(id)    (((id)       ) & 0xFF) // Izoluje zadnji bajt (Gateway ID)
...
```

Ova četiri makroa obavljaju najvažniju ulogu u aplikaciji. Kako bi iz 29-bitnog CAN ID-a izdvojili prethodno opisana polja potrebno je da ga rasčlanimo na dijelove. Svaki makro izoluje prethodno šiftovan dio CAN ID-a i smiješta ga u prateću promjenljivu koja se koristi za kontrolu Modbus RTU slave uređaja.


## Kontrola smijera RS-485 linije

Pošto je RS-485 poludupleksni medij, softver mora precizno upravljati DE/RE pinovima transivera (preko sysfs GPIO22 interfejsa). Funkcija se registruje unutar libmodbus biblioteke i automatski prebacuje hardver u mod slanja/prijema:

```sh
...

static void custom_set_rts(modbus_t *ctx, int on)
{
    int fd = open("/sys/class/gpio/gpio22/value", O_WRONLY);
    if (fd >= 0) {
        // Ako je on=1 podiže se linija za predaju, ako je on=0 spušta se za prijem
        write(fd, on ? "1" : "0", 1);
        close(fd);
    }
    usleep(2000); 
}
...
```

## Reaktivna petlja i izvršavanje komandi

Srce gateway-a predstavlja `switch-case` arhitektura unutar glavne petlje koja na osnovu dekodiranog funkcijskog koda `fc` poziva odgovarajući libmodbus API:
```sh
...

modbus_set_slave(ctx, modbus_addr);

        int ret = -1;
        switch (fc) {
           case 0x01: {
                uint8_t bits[8];
                ret = modbus_read_bits(ctx, reg_addr, reg_value, bits);
                if (ret > 0) {
                    printf("[Modbus] FC01: procitano %d coil(s), prvi=0x%02X\n", ret, bits[0]);
                    struct can_frame resp;
                    resp.can_id  = frame.can_id; 
                    resp.can_dlc = (ret > 8) ? 8 : ret;
                    int i;
                    for (i = 0; i < resp.can_dlc; i++) resp.data[i] = bits[i];
                    write(can_sock, &resp, sizeof(struct can_frame));
                }
                break;
            }
            case 0x05:
               
                ret = modbus_write_bit(ctx, reg_addr, (reg_value != 0) ? 1 : 0);
                if (ret == 1)
                    printf("[Modbus] FC05: Coil 0x%04X postavljen na %d.\n",
                           reg_addr, (reg_value != 0) ? 1 : 0);
                break;

            case 0x06:
                /* FC06: Pisanje jednog holding registra */
                ret = modbus_write_register(ctx, reg_addr, reg_value);
                if (ret == 1)
                    printf("[Modbus] FC06: Registar 0x%04X postavljen na 0x%04X.\n",
                           reg_addr, reg_value);
                break;

            case 0x03: {
                /* FC03: Citanje holding registara */
                uint16_t regs[8];
                ret = modbus_read_registers(ctx, reg_addr, reg_value, regs);
                if (ret > 0) {
                    printf("[Modbus] FC03: procitano %d registar(a), prvi=0x%04X\n", ret, regs[0]);
                    struct can_frame resp;
                    resp.can_id  = frame.can_id;
                    int n = (ret > 4) ? 4 : ret;
                    resp.can_dlc = n * 2;
                    int i;
                    for (i = 0; i < n; i++) {
                        resp.data[i * 2]     = (regs[i] >> 8) & 0xFF;
                        resp.data[i * 2 + 1] = regs[i] & 0xFF;
                    }
                    write(can_sock, &resp, sizeof(struct can_frame));
                }
                break;
            }
...
```
## Priprema za pokretanje aplikacije

Kako bi sve funkcionisalo, potrebno je povezati dva Raspberry Pi-a na jednu CAN magistralu. U ovom slučaju korišteći RS-485 CAN HAT-ove, gdje su CAN_H i CAN_L linije spojene paralelno. Pošto je u pitanju gateway, on će da radi samo na jednom uređaju te je na njega potrebno spojiti Modbus RTU slave čvor. Njega spajamo na A i B kontakte na RS-485 HAT-u. Dakle sa jedne strane gateway-a je jedan uređaj u CAN magistrali sa kojeg šaljemo CAN okvire, dok je sa druge strane Modbus slave uređaj koji preko gatway-a prima odogovarajuće poruke i izvršava zadate funkcije, te vraća potvrdu istim putem.
## Konfiguracija i podizanje interfejsa

Na ciljnoj platformi potrebno je (uz pretpostavku da je interfejs inicijalizovan) podignuti link za can0 sljedećom komandom:

```sh
sudo ip link set can0 up type can bitrate 125000
```
Nakon toga interfejs je aktivan i može se pristupiti pokretanju aplikacije.

> [!NOTE]
> Moguće da će biti potrebno aplikaciju na ciljnoj platformi pokrenuti sa `sudo` privilegijama kako bi se pristupilo Modbus RTU mreži!

## Očekivani rad aplikacije 

Nakon što je gateway pokrenut i sluša saobraćaj na `can0` i nakon što je hardver spojen, pristupa se slanju sirovih okvira pomoću standardnih `can-utils` alata sa drugog čvora u mreži. Na jednom uređaju preko `candump` alata se može pratiti tok poruka, dok će gateway ispisivati prevod i dijagnostičke informacije.

### Primjer 1: Uključivanje Releja #0
Želimo da pošaljemo komandu Modbus uređaju sa adresom 1, koristeći funkcijski kod `0x05`za upis na prvi registar `0x0000`. Ciljani prioritet je `5` (nije toliko važno jer se šalje samo jedan zahtjev), a Gateway Id je `0x10` (isto nije od pretjerane važnosti jer su samo 2 uređaja na CAN magistrali). CAN ID u završnici ima oblik `0x05010510`. Poruka se šalje sa uređaja koji je samo spojen na magistralu, ne i na slave uređaj.


```sh
cansend can0 05010510#0000FF00

```
Payload, `0000FF00` predstavlja podatke poslate u ovoj poruci i sačinjen je od 2 dijela. Prvi dio `0x0000` je adresa prvog releja na pločici, dok je drugi dio `oxFF00` kombinacija za paljenje adresiranog registra.
Očekivani ispis na gateway terminalu izgleda ovako:

```sh
[CAN -> Modbus] EXT ID=0x05010510 | GW=16 Slave=1 FC=0x05 Reg=0x0000 Val=0xFF00
[Modbus] FC05: Coil 0x0000 postavljen na 1.

```
što nam govori sve potrebne podatke o uspješnosti poruke koja je poslata.

Isključivanje releja funkcioniše na sličan način, gdje se u poruci mijenjaju samo posljednja dva bajta:
```sh
cansend can0 05010510#00000000

```
### Primjer 2: Upis podatka u registar #1
Ukoliko želimo da upišemo neki podatak u registar, mijenja se pored adrese i samog podatka koji se upisuj samo funkcijski kod:
```sh
cansend can0 05010610#00010001

```
Dakle u CAN ID je samo promjenjen funkcijski kod sa `0x05` u `0x06` i podatak koji se upisuje u registar `0x0001` je `0x0001`. Na gateway-u dobija se sljedeći ispis:

```sh
[CAN -> Modbus] EXT ID=0x05010610 | GW=16 Slave=1 FC=0x06 Reg=0x0001 Val=0x0001
[Modbus] FC06: Registar 0x0001 postavljen na 0x0001.

```
### Primjer 3: Slanje RTR (Remote Tansmission Request) zahtjeva
U slučaju prijema RTR okvira `is_rtr == 1`, gateway ne čita payload (jer je prazan), već vrši dekompoziciju 29 bit-nog CAN ID kako bi rekonstruisao Modbus komandu. Na osnovu funkcijskog koda, gateway mapira CAN RTR na odgovarajuću `libmodbus` funkciju. Postoje dva scenarija: `FC=0x01 (Read Coils)` - zahtjev za čitanje stanja releja i `FC=0x03 (Read Holding Register)` - zahtjev za čitanje vrijednosti registra. Primjer komunikacije: Slanje RTR zahtjeva za čitanje 4 bajta sa uređaja (Slave=1, FC=0x01, GW_ID=10):

```sh
cansend can0 05010110#R4

```
Izlaz na candump can0 prikazuje da je gateway uspješno vratio regularni paket sa stanjem gdje je npr. samo treći relej uključen:
```sh
can0  05010110   [4]  00 00 01 00

```
### Primjer 4: Čitanje sistemskih registara
Čitanje registra rezervisanog za sistemsku brzinu (baud rate) `0x0001`:
```sh
cansend can0 05010310#00010001
```
Šaljemo funkcijski kod `0x03` koji gateway-u govori da zavtjevano čitanje registra sa adresom `0x0001` i tražimo čitnaje samo jednog registra. Na `candump` dobijamo sljedeći ispis:
```sh
can0  05010310   [2]  00 00

```
Podaci vraćeni na CAN magistralu ukazuju na to da je Modbus slave uspješno primio zahtjev, obradio ga i poslao nazad vrijednost u registru na zahtjevanoj adresi. Vrijednost `0x0000` označava da je upisana default vrijednost od 9600 bps.

### Primjer 5: Čitanje stanja releja

Ako ipak nije potrebno da pročitamo stanje sva četiri releja (što je moguće slanje RTR zahtjeva), pomoću funkcijskog koda `0x01` možemo zahtjevati stanje samo jednog releja koji adresiramo. Primjer komunikacije je:
```sh
cansend can0 05010110#00010001
```
Šaljemo dati funkcijski kod, adresiramo relej `0x0001` i tražimo samo jedan bajt kao potvrdu. Odgovor:
```sh
can0  05010110   [1]  00 

```
U odogovoru, vrijednost poslanog bajta će biti ili 0x00 (ako je relej ugašen), odnosno 0x01 (ako je upaljen).
Demonstrativni video se nalazi na linku (https://youtu.be/zizHeoMEzKg). 
> [!NOTE]
> Na snimku, lijevi terminal predstavlja uređaj na kojem je pokrenut gateway, te je on takođe spojen sa Modbus RTU slave uređajem. Desna dva terminala su pokrenuta na drugoj ciljnoj platformi koja služi za slanje poruka i praćenje CAN magistrale.








