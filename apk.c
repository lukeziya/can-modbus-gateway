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
 * Korisnicka funkcija za kontrolu smjera RS-485 transivera.
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

    ctx = modbus_new_rtu(UART_PORT, MODBUS_BAUDRATE, MODBUS_PARITY,
                         MODBUS_DATABITS, MODBUS_STOPBITS);
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

        if (!(frame.can_id & CAN_EFF_FLAG))
            continue;

        if (frame.can_dlc < MIN_CAN_PAYLOAD) {
            fprintf(stderr, "Greska: CAN okvir nema dovoljno bajtova za Modbus PDU (dlc=%d).\n",
                    frame.can_dlc);
            continue;
        }

        unsigned int ext_id       = frame.can_id & CAN_EFF_MASK;
        unsigned char modbus_addr = CAN_ID_MODBUS_ADDR(ext_id);
        unsigned char fc          = CAN_ID_FUNCTION_CODE(ext_id);
        unsigned char gw_id       = CAN_ID_GATEWAY_ID(ext_id);


        int reg_addr  = (frame.data[0] << 8) | frame.data[1];
        int reg_value = (frame.data[2] << 8) | frame.data[3];

        printf("\n[CAN->Modbus] EXT ID=0x%08X | GW=%d Slave=%d FC=0x%02X Reg=0x%04X Val=0x%04X\n",
               ext_id, gw_id, modbus_addr, fc, reg_addr, reg_value);

        /* ----------------------------------------------------------------
         * Postavljanje adrese slave uredjaja i slanje Modbus komande.
         * libmodbus automatski upravlja CRC-om i RS-485 smerom (custom RTS).
         * ---------------------------------------------------------------- */
        modbus_set_slave(ctx, modbus_addr);

        int ret = -1;
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
                    int i;
                    for (i = 0; i < resp.can_dlc; i++)
                        resp.data[i] = bits[i];
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
