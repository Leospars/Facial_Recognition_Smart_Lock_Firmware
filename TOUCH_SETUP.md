# Touch Screen Setup Guide

This guide explains how to set up and configure the touch screen functionality for the JUPY Smart Lock project.

## Hardware Requirements

- ILI9341 TFT Display with XPT2046 Touch Controller
- ESP32-S3 Development Board
- Proper wiring connections as per the schematic

## Pin Configuration

The touch screen uses the following pins (defined in `include/User_Setup.h`):

- **TOUCH_CS**: GPIO 40 - Touch controller chip select
- **T_IRQ**: GPIO 21 - Touch interrupt pin
- **TFT SPI Pins**:
  - MOSI: GPIO 11
  - MISO: GPIO 13
  - SCLK: GPIO 12
  - TFT_CS: GPIO 10
  - TFT_DC: GPIO 9
  - TFT_RST: -1 (connected to ESP32 RST)

## Software Configuration

### 1. Library Dependencies

The following libraries are required and configured in `platformio.ini`:

- `bodmer/TFT_eSPI@^2.5.43` - TFT display library
- `paulstoffregen/XPT2046_Touchscreen@^1.4.0` - Touch controller library

### 2. TFT_eSPI Configuration

A custom `User_Setup.h` file is provided in the `include/` directory with the correct pin configuration for ESP32-S3 and ILI9341 display.

Key settings:
- Driver: `ILI9341_DRIVER`
- SPI Frequency: 27MHz for display, 2.5MHz for touch
- Touch enabled with TOUCH_CS and T_IRQ pins

### 3. Touch Calibration

The touch screen requires calibration to map touch coordinates to display coordinates.

#### Automatic Calibration
- Uncomment `// touch_calibrate();` in `setup()` function
- Upload and run the code
- Follow on-screen instructions to touch the corners
- Calibration data will be printed to Serial monitor
- Copy the calibration array and update the code

#### Manual Calibration Data
Default calibration data is provided:
```cpp
uint16_t calData[5] = {1, 65024, 8, 64506, 5};
tft.setTouch(calData);
```

## Code Integration

### Touch Handling

The touch functionality is implemented in the `handleTouch()` function:

- Detects touch state changes
- Maps touch coordinates to keypad grid (4x3 layout)
- Processes key presses through `processKeyPress()`

### Keypad Layout

The touch screen implements a 4x3 keypad:
```
1 2 3
4 5 6
7 8 9
x 0 #
```

Where:
- `x`: Clear
- `0`: Number 0
- `#`: Enter/Submit

## Troubleshooting

### Common Issues

1. **Ghost Touches / (0,0) Coordinates**
   - Check TOUCH_CS pin connection
   - Verify T_IRQ pin connection
   - Ensure stable 3.3V power to touch controller
   - Check for SPI bus conflicts

2. **Inconsistent Touch Response**
   - Reduce SPI frequency in User_Setup.h
   - Check for electrical noise
   - Verify touch panel is properly seated

3. **Wrong Coordinates**
   - Run touch calibration
   - Update calibration data in code
   - Check display rotation setting

### Diagnostic Steps

1. Run the `touch_diagnostic.ino` sketch from the `tests/` directory
2. Check Serial output for touch events
3. Verify pin connections with multimeter
4. Test SPI communication

### Hardware Checks

- **Connections**: Ensure all SPI lines and control pins are securely connected
- **Power**: Verify 3.3V supply to touch controller
- **Ground**: Check for proper ground connections
- **Damage**: Inspect touch overlay for physical damage

## Testing

After setup:

1. Upload the code to ESP32-S3
2. Open Serial monitor
3. Touch the screen and observe coordinate output
4. Test keypad functionality by touching number areas
5. Verify PIN entry works

## References

- [TFT_eSPI Library Documentation](https://github.com/Bodmer/TFT_eSPI)
- [XPT2046 Touchscreen Library](https://github.com/PaulStoffregen/XPT2046_Touchscreen)
- [ILI9341 + XPT2046 Troubleshooting Guide](./tests/touch_troubleshooting_guide.md)