// SPDX-License-Identifier: Apache-2.0

.macro import_extent width, height, target
    ldr \height, [x26, #0x24]
    ldr w8, [x26, #0x10]
    sub w8, w8, #3
    cmp w8, #1
    b.hi \target
    cbz \width, \target
    cbz \height, \target
    ldr w8, [x26, #0xe8]
    cmp w8, #2
    b.ne \target
    ldr w9, [x26, #0x28]
    ldr w10, [x26, #0x2c]
    cmp w9, \width
    b.lo \target
    cmp w10, \height
    b.lo \target
    tst w9, #127
    b.ne \target
    tst w10, #31
    b.ne \target
    ldr w8, [x26, #0x50]
    cmp w8, w9
    b.ne \target
    ldr w8, [x26, #0x54]
    lsr w12, w10, #1
    cmp w8, w12
    b.lo \target
    umull x12, w9, w10
    ldr x13, [x26, #0xf0]
    cmp x13, x12
    b.ne \target
    lsr x12, x12, #1
    ldr x13, [x26, #0xf8]
    cmp x13, x12
    b.lo \target
    mov \width, w9
    mov \height, w10
    b \target
.endm

.text
.org 0x12e94
    b parent_import
parent_return:
.org 0x12f34
    b child_import
child_return:
.org 0x23c40
parent_import:
    import_extent w4, w0, parent_return
.p2align 4
child_import:
    import_extent w1, w3, child_return
