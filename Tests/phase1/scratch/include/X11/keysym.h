#ifndef P11_SHADOW_KEYSYM_H
#define P11_SHADOW_KEYSYM_H

#include <X11/Xlib.h>

/*
 * Keypad keysyms used by the input path.  Headless has no event source, so
 * keypad translation is inert; the values only need to be distinct.
 */
#define XK_Insert	0xff63
#define XK_Up		0xff52
#define XK_Down		0xff54
#define XK_Left		0xff51
#define XK_Right	0xff53
#define XK_F1		0xffbe
#define XK_F2		0xffbf
#define XK_F3		0xffc0
#define XK_R1		0xffbe
#define XK_R2		0xffbf
#define XK_R3		0xffc0
#define XK_R4		0xffc1
#define XK_R5		0xffc2
#define XK_R6		0xffc3
#define XK_R7		0xffc4
#define XK_R9		0xffc6
#define XK_R11		0xffc8
#define XK_R13		0xffca
#define XK_R15		0xffcc
#define XK_KP_0		0xffb0
#define XK_KP_1		0xffb1
#define XK_KP_2		0xffb2
#define XK_KP_3		0xffb3
#define XK_KP_4		0xffb4
#define XK_KP_5		0xffb5
#define XK_KP_6		0xffb6
#define XK_KP_7		0xffb7
#define XK_KP_8		0xffb8
#define XK_KP_9		0xffb9
#define XK_KP_Divide	0xffaf
#define XK_KP_Multiply	0xffaa
#define XK_KP_Equal	0xffbd

#endif /* P11_SHADOW_KEYSYM_H */
