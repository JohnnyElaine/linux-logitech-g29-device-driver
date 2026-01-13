
Logitech G29 USB Protocol
=========================

- 12 bytes per event
- Little-endian (= smaller value bytes first)

```
                                          1
  0   1   2   3   4   5   6   7   8   9   0   1
+---+---+---+---+---+---+---+---+---+---+---+---+
|    Buttons    |  Rot  |Gas|Brk|Clt|GrX|GrY|GrZ|
+---+---+---+---+---+---+---+---+---+---+---+---+
```

- `Buttons`: Bitmask of pressed buttons (mask values are in big endian).
    - `0x00000001` - ?
    - `0x0000000E` - Arrow keys
        - `0x00000000` - Up
        - `0x00000002` - Right
        - `0x00000004` - Down
        - `0x00000006` - Left
        - `0x00000008` - No arrow key pressed
    - `0x00000010` - X
    - `0x00000020` - Square
    - `0x00000040` - Circle
    - `0x00000080` - Triangle
    - `0x00000100` - R1
    - `0x00000200` - L1
    - `0x00000400` - R2
    - `0x00000800` - L2
    - `0x00001000` - Share
    - `0x00002000` - Option
    - `0x00004000` - R3
    - `0x00008000` - L3
    - `0x00010000` - Gear 1
    - `0x00020000` - Gear 2
    - `0x00040000` - Gear 3
    - `0x00080000` - Gear 4
    - `0x00100000` - Gear 5
    - `0x00200000` - Gear 6
    - `0x00400000` - Gear reverse
    - `0x00800000` - Plus
    - `0x01000000` - Minus
    - `0x02000000` - Red rotation clockwise
    - `0x04000000` - Red rotation counterclockwise
    - `0x08000000` - Return
    - `0xF0000000` - ?
- `Rot`: Wheel rotation (little-endian). `0x0000` (leftmost) - `0xFFFF` (rightmost).
- `Gas`: Gas pedal. `0xFF` (up, default) - `0x00` (down).
- `Brk`: Brake pedal. `0xFF` (up, default) - `0x00` (down).
- `Clt`: Clutch pedal. `0xFF` (up, default) - `0x00` (down).
- `GrX`: Gearshift X axis. ~`0x30` (gear 1+2), ~`0x80` (gear 3+4), ~`0xB0` (gear 5+6+reverse).
- `GrY`: Gearshift Y axis. ~`0xD0` (gear 1+3+5), ~`0x40` (gear 2+4+6+reverse).
- `GrZ`: Gearshift Z axis. ~`0x9C` (neutral), ~`0xDC` (pressed down).


## Sample event

Sample event:
```
08 00 00 00   00 80   ff ff ff   81 80 9c
```

* No buttons pressed / no gear engaged
* Wheel rotation at `0x8000` ~ center
* Gas pedal up
* Brake pedal up
* Clutch pedal up
* Gearshift neutral
