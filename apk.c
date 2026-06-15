#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <fcntl.h>
#include <errno.h>
#include <modbus.h>

#define GPIO_DE_PIN     "22"
#define GPIO_DIR_PATH   "/sys/class/gpio/gpio22/direction"
#define GPIO_VAL_PATH   "/sys/class/gpio/gpio22/value"

#define UART_PORT       "/dev/ttyAMA0"

#define MODBUS_BAUDRATE 9600
#define MODBUS_PARITY   'N'
#define MODBUS_DATABITS 8
#define MODBUS_STOPBITS 1

#define CAN_ID_PRIORITY(id)      (((id) >> 24) & 0x1F) // Izoluje prvih 5 bita prioriteta
#define CAN_ID_MODBUS_ADDR(id)   (((id) >> 16) & 0xFF) // Izoluje 8 bita slave adrese
#define CAN_ID_FUNCTION_CODE(id) (((id) >>  8) & 0xFF) // Izoluje 8 bita funkcijskog koda
#define CAN_ID_GATEWAY_ID(id)    (((id)       ) & 0xFF) // Izoluje zadnji bajt (Gateway ID)
 
#define MIN_CAN_PAYLOAD 4

/* -----------------------------------------------------------------------
 * Inicijalizacija GPIO pina preko sysfs-a 
 * ----------------------------------------------------------------------- */
static void init_gpio(void)
{
    int fd;

    fd = open("/sys/class/gpio/export", O_WRONLY);
    if (fd >= 0) {
        write(fd, GPIO_DE_PIN, strlen(GPIO_DE_PIN));
        close(fd);
    }
    usleep(50000); 

    fd = open(GPIO_DIR_PATH, O_WRONLY);
    if (fd >= 0) {
        write(fd, "out", 3);
        close(fd);
    }
}

/* -----------------------------------------------------------------------
 * Funkcija za kontrolu smjera RS-485 transivera.
 * Registruje se sa modbus_rtu_set_custom_rts() 
 * ----------------------------------------------------------------------- */
static void custom_set_rts(modbus_t *ctx, int on)
{
    int fd = open(GPIO_VAL_PATH, O_WRONLY);
    if (fd >= 0) {
        write(fd, on ? "1" : "0", 1);
        close(fd);
    }
    usleep(2000); 
}

int main(void)
{
    int can_sock;
    struct sockaddr_can addr;
    struct ifreq ifr;
    struct can_frame frame;
    struct can_filter rfilter[1];

    modbus_t *ctx;

    printf("Inicijalizacija CAN-Modbus RTU Gejtveja...\n");

    init_gpio();

    ctx = modbus_new_rtu(UART_PORT, MODBUS_BAUDRATE, MODBUS_PARITY, MODBUS_DATABITS, MODBUS_STOPBITS);
    
    if (ctx == NULL) {
        fprintf(stderr, "Greska: Nije moguce kreirati libmodbus kontekst!\n");
        return -1;
    }

    modbus_rtu_set_rts(ctx, MODBUS_RTU_RTS_UP);
    modbus_rtu_set_custom_rts(ctx, custom_set_rts);

    if (modbus_connect(ctx) == -1) {
        fprintf(stderr, "Greska: Modbus veza nije uspostavljena: %s\n",
                modbus_strerror(errno));
        modbus_free(ctx);
        return -1;
    }

    can_sock = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (can_sock < 0) {
        perror("Greska pri otvaranju CAN socketa");
        modbus_close(ctx);
        modbus_free(ctx);
        return -1;
    }

    strcpy(ifr.ifr_name, "can0");
    ioctl(can_sock, SIOCGIFINDEX, &ifr);

    addr.can_family  = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (bind(can_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("Greska pri bind-ovanju na can0");
        close(can_sock);
        modbus_close(ctx);
        modbus_free(ctx);
        return -1;
    }

    // Filtriramo samo prosirene EFF okvire
    rfilter[0].can_id   = CAN_EFF_FLAG;
    rfilter[0].can_mask = CAN_EFF_FLAG;
    setsockopt(can_sock, SOL_CAN_RAW, CAN_RAW_FILTER, &rfilter, sizeof(rfilter));

    printf("Gateway uspjesno pokrenut. Slusam saobracaj na can0...\n");

    while (1) {
        int nbytes = read(can_sock, &frame, sizeof(struct can_frame));
        if (nbytes < 0) {
            perror("Greska pri citanju CAN okvira");
            continue;
        }
        if (nbytes < (int)sizeof(struct can_frame)) {
            fprintf(stderr, "Upozorenje: Nepotpun CAN okvir.\n");
            continue;
        }

        // Provjera da li je u pitanju RTR okvir (zahtjev za citanje bez payload-a)
        int is_rtr = (frame.can_id & CAN_RTR_FLAG) ? 1 : 0;

        unsigned int ext_id       = frame.can_id & CAN_EFF_MASK;
        unsigned char modbus_addr = CAN_ID_MODBUS_ADDR(ext_id);
        unsigned char fc          = CAN_ID_FUNCTION_CODE(ext_id);
        unsigned char gw_id       = CAN_ID_GATEWAY_ID(ext_id);

        modbus_set_slave(ctx, modbus_addr);
        int ret = -1;

        if (is_rtr) {
            printf("\n[RTR UPIT] Primljen zahtjev za citanje! Slave=%d FC=0x%02X\n", modbus_addr, fc);
            
            if (fc == 0x01) { // RTR zahtjev za citanje stanja releja (Coils)
                uint8_t bits[4];
                ret = modbus_read_bits(ctx, 0x0000, 4, bits); // Citamo pocetna 4 releja
                if (ret > 0) {
                    struct can_frame resp;
                    resp.can_id = ext_id | CAN_EFF_FLAG; // Vracamo regularan EFF okvir (bez RTR flega)
                    resp.can_dlc = 4;
                    for (int i = 0; i < 4; i++) resp.data[i] = bits[i];
                    write(can_sock, &resp, sizeof(struct can_frame));
                    printf("[RTR ODGOVOR] Poslato trenutno stanje releja na CAN.\n");
                }
            }
            else if (fc == 0x03) { // RTR zahtjev za citanje holding registara
                uint16_t regs[2];
                ret = modbus_read_registers(ctx, 0x0000, 2, regs); // Citamo prva 2 registra
                if (ret > 0) {
                    struct can_frame resp;
                    resp.can_id = ext_id | CAN_EFF_FLAG;
                    resp.can_dlc = 4; // 2 registra * 2 bajta = 4 bajta saobracaja
                    resp.data[0] = (regs[0] >> 8) & 0xFF;
                    resp.data[1] = regs[0] & 0xFF;
                    resp.data[2] = (regs[1] >> 8) & 0xFF;
                    resp.data[3] = regs[1] & 0xFF;
                    write(can_sock, &resp, sizeof(struct can_frame));
                    printf("[RTR ODGOVOR] Poslati holding registri na CAN.\n");
                }
            }
            else {
                fprintf(stderr, "[RTR] Funkcijski kod 0x%02X nije podrzan za RTR.\n", fc);
            }
            
            if (ret < 0) {
                fprintf(stderr, "Greska Modbus RTR komunikacije: %s\n", modbus_strerror(errno));
            }
            continue; // Zavrsavamo obradu RTR-a i idemo na sljedeci CAN okvir
        }

        /* -----------------------------------------------------------------------
         * Standardni rezim: Obrada regularnih CAN okvira sa podacima (Payload)
         * ----------------------------------------------------------------------- */
        if (frame.can_dlc < MIN_CAN_PAYLOAD) {
            fprintf(stderr, "Greska: CAN okvir nema dovoljno bajtova za Modbus PDU (dlc=%d).\n", frame.can_dlc);
            continue;
        }

        int reg_addr  = (frame.data[0] << 8) | frame.data[1];
        int reg_value = (frame.data[2] << 8) | frame.data[3];

        printf("\n[CAN->Modbus] EXT ID=0x%08X | GW=%d Slave=%d FC=0x%02X Reg=0x%04X Val=0x%04X\n",
               ext_id, gw_id, modbus_addr, fc, reg_addr, reg_value);

        switch (fc) {
            case 0x01: {
                /* FC01: Citanje coils (digitalnih izlaza) */
                uint8_t bits[8];
                ret = modbus_read_bits(ctx, reg_addr, reg_value, bits);
                if (ret > 0) {
                    printf("[Modbus] FC01: procitano %d coil(s), prvi=0x%02X\n", ret, bits[0]);
                    struct can_frame resp;
                    resp.can_id  = frame.can_id; 
                    resp.can_dlc = (ret > 8) ? 8 : ret;
                    for (int i = 0; i < resp.can_dlc; i++) resp.data[i] = bits[i];
                    write(can_sock, &resp, sizeof(struct can_frame));
                }
                break;
            }
            case 0x05:
                /* FC05: Pisanje jednog digitalnog izlaza */
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
                    printf("[Modbus] FC03: procitano %d registrar(a), prvi=0x%04X\n", ret, regs[0]);
                    struct can_frame resp;
                    resp.can_id  = frame.can_id;
                    int n = (ret > 4) ? 4 : ret;
                    resp.can_dlc = n * 2;
                    for (int i = 0; i < n; i++) {
                        resp.data[i * 2]     = (regs[i] >> 8) & 0xFF;
                        resp.data[i * 2 + 1] = regs[i] & 0xFF;
                    }
                    write(can_sock, &resp, sizeof(struct can_frame));
                }
                break;
            }

}
            default:
                fprintf(stderr, "Upozorenje: Nepodrzani funkcijski kod FC=0x%02X.\n", fc);
                continue;
        }

        if (ret < 0) {
            fprintf(stderr, "Greska Modbus komunikacije: %s\n", modbus_strerror(errno));
        }
    }

    close(can_sock);
    modbus_close(ctx);
    modbus_free(ctx);
    return 0;
}
