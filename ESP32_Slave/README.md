### Hardware Connections

Connect your ESP development board to the ST7796 LCD display as shown below. Make sure to power off your board before making any connections.

### Wiring Diagram

```text
ESP Development Board                    ST7796 LCD Display
+----------------------+              +--------------------+
|             GND      +------------->| GND                |
|                      |              |                    |
|             3V3      +------------->| VCC                |
|                      |              |                    |
|             PCLK     +------------->| SCL                |
|                      |              |                    |
|             MOSI     +------------->| MOSI               |
|                      |              |                    |
|             MISO     |<-------------+ MISO               |
|                      |              |                    |
|             RST      +------------->| RES                |
|                      |              |                    |
|             DC       +------------->| DC                 |
|                      |              |                    |
|             LCD CS   +------------->| LCD CS             |
|                      |              |                    |
|             TOUCH CS +------------->| TOUCH CS           |
|                      |              |                    |
|             BK_LIGHT +------------->| BLK                |
+----------------------+              +--------------------+
```
