#include <stdio.h>   /* Standard input/output definitions */
#include <string.h>  /* String function definitions */
#include <unistd.h>  /* UNIX standard function definitions */
#include <fcntl.h>   /* File control definitions */
#include <errno.h>   /* Error number definitions */
#include <termios.h> /* POSIX terminal control definitions */
#include <stdlib.h>
#include <stdarg.h>

#include "ulcd43.h"

/**
 * Baud rates only include types found in Linux. The device also supports other baud rates.
 */
struct baudtable_t baud_index[] = {
    {0, B110},
    {1, B300},
    {2, B600},
    {3, B1200},
    {4, B2400},
    {5, B4800},
    {6, B9600},
    {8, B19200},
    {10, B38400},
    {12, B57600},
    {13, B115200},
    {18, B500000}
};


/**
 * Global send and receive buffer
 */
char cmdbuf[4096];
char recvbuf[2];


/**
 * Pack an unsigned int into two bytes, little endian.
 */
inline int
pack_uint(char *dest, param_t src)
{
    dest[0] = (src & 0xff00) >> 8;
    dest[1] = (src & 0x00ff);
    return 2;
}


/**
 * Unpack an unsigned int from two bytes, little endian.
 */
inline void
unpack_uint(param_t *dest, const char *src)
{
    *dest = ((src[0] << 8) & 0xff00) | (src[1] & 0x00ff);
}


/**
 * Pack a variable list of unsigned ints into a char pointer.
 */
inline int
pack_uints(char *buffer, int args, ...)
{
    int i;
    va_list lst;

    va_start(lst, args);
    for (i = 0; i < args; i++) {
        pack_uint(buffer, va_arg(lst, param_t));
        buffer += 2;
    }
    va_end(lst);

    return args * 2;
}


/**
 * Debug function
 */
void
print_hex(const char *buffer, int size)
{
    int i = 0;
    printf("%d bytes: ", size);
    for (i = 0; i < size; i++) {
        printf("0x%x ", buffer[i]);
    }
    printf("\n");
}


/**
 * Create a new struct ulcd_t object.
 */
struct ulcd_t *
ulcd_new(void)
{
    struct ulcd_t *ulcd;
    ulcd = malloc(sizeof(struct ulcd_t));
    memset(ulcd, 0, sizeof(struct ulcd_t));
    ulcd->fd = -1;
    return ulcd;
}


/**
 * Delete a ulcd_t object.
 */
void
ulcd_free(struct ulcd_t *ulcd)
{
    if (ulcd->fd != -1) {
        close(ulcd->fd);
    }
    free(ulcd);
}


/**
 * Set an error message
 */
int
ulcd_error(struct ulcd_t *ulcd, int error, const char *err, ...)
{
    va_list args;

    ulcd->error = error;
    if (err == NULL) {
        ulcd->err[0] = '\0';
    } else {
        va_start(args, err);
        vsnprintf(ulcd->err, STRBUFSIZE, err, args);
        va_end(args);
    }

    return error;
}


/**
 * Open the serial device.
 */
int
ulcd_open_serial_device(struct ulcd_t *ulcd)
{
    ulcd->fd = open(ulcd->device, O_RDWR | O_NOCTTY);
    if (ulcd->fd == -1) {
        return ulcd_error(ulcd, errno, "Unable to open serial device: %s", strerror(errno));
    } else {
        fcntl(ulcd->fd, F_SETFL, 0);
    }
    return ERROK;
}


/**
 * Set serial device parameters to the ones used by uLCD-43.
 */
void
ulcd_set_serial_parameters(struct ulcd_t *ulcd)
{
    struct termios options;

    tcgetattr(ulcd->fd, &options);

    /* 115200 baud */
    cfsetispeed(&options, ulcd->baudrate);
    cfsetospeed(&options, ulcd->baudrate);

    /* 8N1 */
    options.c_cflag &= ~PARENB;
    options.c_cflag &= ~CSTOPB;
    options.c_cflag &= ~CSIZE;
    options.c_cflag |= CS8;
    options.c_cflag |= CREAD;

    /* Raw input */
    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);

    /* No flow control */
    options.c_iflag |= IGNPAR;
    options.c_iflag &= ~(IXON | IXOFF | IXANY);

    /* Raw output */
    options.c_oflag &= ~OPOST;

    /* No timeout, wait for 1 byte */
    options.c_cc[VMIN] = 1;
    options.c_cc[VTIME] = 0;

    tcsetattr(ulcd->fd, TCSAFLUSH, &options);
}

int
ulcd_send(struct ulcd_t *ulcd, const char *data, int size)
{
    size_t total = 0;
    size_t sent;
    while (total < size) {
        sent = write(ulcd->fd, data+total, size-total);
        if (sent < 0) {
            return ulcd_error(ulcd, errno, "Unable to send data to device: %s", strerror(errno));
        }
        total += sent;
    }

#ifdef DEBUG_SERIAL
    printf("send: ");
    print_hex(data, size);
#endif

    return ERROK;
}

int
ulcd_recv_ack(struct ulcd_t *ulcd)
{
    char r;
    size_t bytes_read;

    bytes_read = read(ulcd->fd, &r, 1);

#ifdef DEBUG_SERIAL
    printf("read ack: ");
    print_hex(&r, 1);
#endif

    if (r == ACK) {
        return ERROK;
    } else if (r == NAK) {
        return ulcd_error(ulcd, ERRNAK, "Device sent NAK instead of ACK");
    }

    return ulcd_error(ulcd, ERRUNKNOWN, "Device sent unknown reply instead of ACK");
}

int
ulcd_send_recv_ack(struct ulcd_t *ulcd, const char *data, int size)
{
    if (ulcd_send(ulcd, data, size)) {
        return ulcd->error;
    }
    if (ulcd_recv_ack(ulcd)) {
        return ulcd->error;
    }
    return ERROK;
}

int
ulcd_send_recv_ack_data(struct ulcd_t *ulcd, const char *data, int size, void *buffer, int datasize)
{
    size_t bytes_read;
    size_t total = 0;
   
    if (ulcd_send_recv_ack(ulcd, data, size)) {
        return ulcd->error;
    }

    while(total < datasize) {
        bytes_read = read(ulcd->fd, buffer+total, datasize-total);
        if (bytes_read < 0) {
            return ulcd_error(ulcd, errno, "Unable to read data from device: %s", strerror(errno));
        }
        total += bytes_read;
    }

#ifdef DEBUG_SERIAL
    printf("read: ");
    print_hex(buffer, datasize);
#endif

    return ERROK;
}

int
ulcd_send_recv_ack_word(struct ulcd_t *ulcd, const char *data, int size, param_t *param)
{
    char buffer[2];

    if (ulcd_send_recv_ack_data(ulcd, data, size, buffer, 2)) {
        return -1;
    }

    if (param != NULL) {
        unpack_uint(param, buffer);
    }

    return ERROK;
}
