/* PS/2 Keyboard Driver - Polling Mode (no interrupts) */
#include <embodios/types.h>

#define KEYBOARD_DATA_PORT 0x60
#define KEYBOARD_STATUS_PORT 0x64

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Scancode to ASCII mapping (US layout, simplified) */
static const char scancode_to_ascii[] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, /* Ctrl */
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, /* Left Shift */
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 
    0, /* Right Shift */
    '*',
    0, /* Alt */
    ' ', /* Space */
};

/* Check if keyboard has data */
static int keyboard_has_data(void) {
    return (inb(KEYBOARD_STATUS_PORT) & 0x01);
}

/* Get character from keyboard (polling mode) */
int keyboard_getchar_poll(void) {
    if (!keyboard_has_data()) {
        return -1; /* No data available */
    }
    
    uint8_t scancode = inb(KEYBOARD_DATA_PORT);
    
    /* Ignore key release (bit 7 set) */
    if (scancode & 0x80) {
        return -1;
    }
    
    /* Convert scancode to ASCII */
    if (scancode < sizeof(scancode_to_ascii)) {
        char c = scancode_to_ascii[scancode];
        if (c != 0) {
            return (int)c;
        }
    }
    
    return -1;
}
