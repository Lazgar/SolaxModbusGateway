# Modbus-RTU to MQTT Gateway 
**for Solar Inverters and other Modbus-RTU Devices**

[![license](https://img.shields.io/badge/Licence-GNU%20v3.0-green)](https://github.com/desktop/desktop/blob/master/LICENSE)
![ESP32 Architecture](https://img.shields.io/badge/Architecture-ESP32-blue)
![GitHub release (latest by date including pre-releases)](https://img.shields.io/github/v/release/tobiasfaust/SolaxModbusGateway?include_prereleases&style=plastic)
![GitHub All Releases](https://img.shields.io/github/downloads/tobiasfaust/SolaxModbusGateway/total?style=plastic)

<aside>
 <form action="https://www.paypal.com/donate" method="post" target="_top">
 <input type="hidden" name="hosted_button_id" value="RYUQVSNNTP3N6" />
 <input type="image" src="https://www.paypalobjects.com/en_US/i/btn/btn_donate_LG.gif" border="0" name="submit" title="PayPal - The safer, easier way to pay online!" alt="Donate with PayPal button" />
 <img alt="" border="0" src="https://www.paypal.com/en_DE/i/scr/pixel.gif" width="1" height="1" />
 </form>
</aside>
  
This project implements a Gateway for Solar Inverters with Modbus-RTU communication to MQTT on ESP32 basis.  
Direkt Communication with [OpenWB](https://openwb.de) is implemented.
Sending "set" commands to inverter are basically implemented too.

### Supported Solar Inverters
Basically, all Inverters with Modbus RS485 RTU communication are supported.  
Currently the following Inverters with their special registers are integrated:
* Solax Hybrid X1
* Solax Hybrid X3 
* Solax MIC
* Growatt SPH
* Sofar-KTL Solarmax-SGA
* Deye Sun SG04LP3
* QVolt-HYP-G3-3P

If your Solar Inverter is not listed, it´s quite simple to add it by yourself. Feel free to add the special registers, please check the [wiki page](https://github.com/tobiasfaust/SolaxModbusGateway/wiki/configuration-register) or contact me by opening a [new issue](https://github.com/tobiasfaust/SolaxModbusGateway/issues) in github.

### What you need
* ESP32, ESP32-C3, ESP-S2 or ESP-S3 NodeMCU
* RS-485 TTL UART Module with MAX485 Semiconductor 

An ESP8266 is actually not sufficient, because Modbus communication works fail-free only with hardwareserial. ESP8266 has only one Hardwareserial port which is used by serial/debug output. ESP32 has 3 Hardewareserial ports and we use one of them. Another reason is available memory for such huge json definition or such large modbus answers.  
Please check also the wiki page, [how to wire the circuit](https://github.com/tobiasfaust/SolaxModbusGateway/wiki/wiring-the-circuit).

### How to start
It´s recommend to start with one example to check wiring works correctly. Both LED´s (TX and RX) on your RS-485 module should blink. If only TX-LED blinks, please check: 
* wiring
* baud rate

The example requests the inverter SerialNumber and if wiring is correct, the inverter will answer with his number or something like this.

<pre>
request: 01 03 00 00 00 07 08 04

Response: 01 03 .....
</pre>

Burn firmware on your device via web-installer: https://tobiasfaust.github.io/SolaxModbusGateway/

## please refer full documentation and How-To´s in our [Wiki](https://github.com/tobiasfaust/SolaxModbusGateway/wiki)
