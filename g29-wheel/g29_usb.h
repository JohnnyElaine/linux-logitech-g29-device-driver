/*
 * Logitech G29 USB/HID Protocol Definitions
 */

#ifndef G29_USB_H
#define G29_USB_H

#include <linux/types.h>

#define USB_VENDOR_ID_LOGITECH      0x046d
#define USB_DEVICE_ID_LOGITECH_G29  0xc24f
#define USB_DEVICE_ID_LOGITECH_G29_ALT 0xc260

#define G29_BTN_X           0x00000010u
#define G29_BTN_SQUARE      0x00000020u
#define G29_BTN_CIRCLE      0x00000040u
#define G29_BTN_TRIANGLE    0x00000080u
#define G29_BTN_R1          0x00000100u
#define G29_BTN_L1          0x00000200u
#define G29_BTN_R2          0x00000400u
#define G29_BTN_L2          0x00000800u
#define G29_BTN_SHARE       0x00001000u
#define G29_BTN_OPTION      0x00002000u
#define G29_BTN_R3          0x00004000u
#define G29_BTN_L3          0x00008000u
#define G29_BTN_GEAR_1      0x00010000u
#define G29_BTN_GEAR_2      0x00020000u
#define G29_BTN_GEAR_3      0x00040000u
#define G29_BTN_GEAR_4      0x00080000u
#define G29_BTN_GEAR_5      0x00100000u
#define G29_BTN_GEAR_6      0x00200000u
#define G29_BTN_GEAR_REV    0x00400000u
#define G29_BTN_PLUS        0x00800000u
#define G29_BTN_MINUS       0x01000000u
#define G29_BTN_RED_CW      0x02000000u
#define G29_BTN_RED_CCW     0x04000000u
#define G29_BTN_RETURN      0x08000000u
#define G29_BTN_PS3_LOGO     0xF0000000u

#define G29_DPAD_MASK       0x0000000Eu
#define G29_DPAD_UP         0x00000000u
#define G29_DPAD_RIGHT      0x00000002u
#define G29_DPAD_DOWN       0x00000004u
#define G29_DPAD_LEFT       0x00000006u
#define G29_DPAD_NONE       0x00000008u

#define G29_WHEEL_MIN_ROTATION           0x0000
#define G29_WHEEL_MAX_ROTATION           0xFFFF

#define G29_PEDAL_RELEASED      0xFF
#define G29_PEDAL_PRESSED       0x00
#define G29_PEDAL_THRESHOLD     0x80

#define G29_GEARSHIFT_X_LEFT    0x30   /* Gears 1-2 */
#define G29_GEARSHIFT_X_CENTER  0x80   /* Gears 3-4 */
#define G29_GEARSHIFT_X_RIGHT   0xB0   /* Gears 5-6-R */
#define G29_GEARSHIFT_Y_TOP     0xD0   /* Gears 1-3-5 */
#define G29_GEARSHIFT_Y_BOTTOM  0x40   /* Gears 2-4-6-R */
#define G29_GEARSHIFT_Z_NEUTRAL 0x9C   /* Neutral position */
#define G29_GEARSHIFT_Z_PRESSED 0xDC   /* Pressed down */

#define WHEEL_CENTER        32768
#define WHEEL_MAX_DIST      32768

struct g29_state {
	u32 buttons_le;  /* Button bitmask (little-endian) */
	u16 rot_le;      /* Wheel rotation (little-endian) */
	u8 gas;          /* Gas pedal (0xFF=up, 0x00=down) */
	u8 brk;          /* Brake pedal (0xFF=up, 0x00=down) */
	u8 clt;          /* Clutch pedal (0xFF=up, 0x00=down) */
	u8 gr_x;         /* Gearshift X-axis */
	u8 gr_y;         /* Gearshift Y-axis */
	u8 gr_z;         /* Gearshift Z-axis */
} __packed;

#endif
