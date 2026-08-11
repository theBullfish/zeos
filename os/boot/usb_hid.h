/*
 * Zeos -- USB HID (Boot Protocol) class driver
 *
 * Recognises HID-class interfaces (interface class 0x03) operating in
 * the Boot subclass (subclass 0x01) for the two boot protocols:
 *   - Keyboard (protocol 1) -- 8-byte boot reports
 *   - Mouse    (protocol 2) -- 3-4 byte boot reports
 *
 * For each matching device the driver:
 *   1. Parses the configuration descriptor to find the HID interface +
 *      its single interrupt-IN endpoint.
 *   2. SET_CONFIGURATION(1) on the device.
 *   3. SET_PROTOCOL(0) on the interface (boot protocol, not report).
 *   4. SET_IDLE(0, 0) on the interface (only report on state change).
 *   5. Configures the interrupt-IN endpoint via xhci_setup_interrupt_in().
 *
 * On every usb_hid_poll() call, drains pending interrupt reports and
 * dispatches them to keyboard_inject_scancode() / mouse_inject() so the
 * existing PS/2 input pipeline picks them up unchanged.
 */

#ifndef ZEOS_USB_HID_H
#define ZEOS_USB_HID_H

/* Discover and bind every HID boot device on the xHCI bus. Safe to
 * call once after xhci_init(). Returns the number of HID devices that
 * were successfully attached. */
int usb_hid_init(void);

/* Drain interrupt-IN reports on every attached HID device and dispatch
 * to the input pipeline. Non-blocking. Call frequently from idle loops
 * (or from the keyboard/mouse poll path). */
void usb_hid_poll(void);

#endif /* ZEOS_USB_HID_H */
